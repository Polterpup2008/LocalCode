#include "ChatView.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

#include "../Theme.h"
#include "MarkdownRenderer.h"

namespace localcode::gui {
namespace {

// Tokens arrive far faster than a text document can be re-laid out. Flushing on
// this cadence keeps the view readable without spending the whole frame budget
// on layout - the difference between a responsive window and a stuttering one.
constexpr int kFlushIntervalMs = 60;

QString colour(const QColor& c) {
    return c.name(QColor::HexRgb);
}

QString roleLabel(ChatRole role) {
    switch (role) {
        case ChatRole::User: return QStringLiteral("You");
        case ChatRole::Assistant: return QStringLiteral("LocalCode");
        case ChatRole::System: return QStringLiteral("System");
        case ChatRole::Notice: return QStringLiteral("Notice");
        case ChatRole::Error: return QStringLiteral("Error");
    }
    return {};
}

QColor roleColour(ChatRole role) {
    const auto& p = colours();
    switch (role) {
        case ChatRole::User: return p.accent;
        case ChatRole::Assistant: return p.text;
        case ChatRole::System: return p.textMuted;
        case ChatRole::Notice: return p.warning;
        case ChatRole::Error: return p.danger;
    }
    return p.text;
}

// Durations a person reads at a glance: seconds under a minute, then m/s.
QString formatDuration(qint64 milliseconds) {
    if (milliseconds < 1000) return QStringLiteral("%1ms").arg(milliseconds);
    const qint64 seconds = (milliseconds + 500) / 1000;
    if (seconds < 60) return QStringLiteral("%1s").arg(seconds);
    return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
}

}   // namespace

ChatView::ChatView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    browser_ = new QTextBrowser(this);
    browser_->setOpenLinks(false);
    browser_->setOpenExternalLinks(false);
    browser_->setFrameShape(QFrame::NoFrame);
    browser_->setReadOnly(true);
    browser_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard |
                                      Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    browser_->document()->setDocumentMargin(14);
    browser_->document()->setDefaultFont(Theme::instance().uiFont());
    layout->addWidget(browser_);

    latestButton_ = new QPushButton(tr("Jump to latest"), this);
    latestButton_->setObjectName("JumpToLatest");
    latestButton_->setAccessibleName(tr("Jump to the latest message"));
    latestButton_->setToolTip(tr("Return to the latest message and follow new output"));
    latestButton_->hide();
    layout->addWidget(latestButton_, 0, Qt::AlignRight);
    connect(latestButton_, &QPushButton::clicked, this, [this] {
        following_ = true;
        scrollToBottomIfFollowing();
    });

    connect(browser_, &QTextBrowser::anchorClicked, this, &ChatView::handleAnchorClicked);

    // "Following" means the user is at the bottom and wants to stay there; if
    // they scroll up to read, streaming must not yank them back down.
    connect(browser_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        auto* bar = browser_->verticalScrollBar();
        following_ = value >= bar->maximum() - 24;
        updateLatestButton();
    });
    connect(browser_->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { updateLatestButton(); });

    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(kFlushIntervalMs);
    connect(flushTimer_, &QTimer::timeout, this, &ChatView::flushPending);

    // Half a second: fast enough that the indicator is obviously alive, slow
    // enough that it is not redrawing the document for the sake of it.
    thinkingTimer_ = new QTimer(this);
    thinkingTimer_->setInterval(500);
    connect(thinkingTimer_, &QTimer::timeout, this, [this] {
        ++thinkingFrame_;
        refreshTail();
    });

    // Draw once up front, so the pane starts as the empty state rather than as a
    // blank rectangle waiting for a first message.
    rebuild();
}

void ChatView::setShowReasoning(bool show) {
    if (showReasoning_ == show) return;
    showReasoning_ = show;
    refreshTheme();
}

void ChatView::refreshTheme() {
    browser_->document()->setDefaultFont(Theme::instance().uiFont());
    for (const auto& message : messages_)
        message.cachedHtml.clear();
    rebuild();
}

void ChatView::addMessage(ChatRole role, const QString& markdown) {
    if (streaming_) endAssistantMessage();
    Message message;
    message.role = role;
    message.markdown = markdown;
    message.complete = true;
    messages_ << message;
    refreshTail();
}

void ChatView::addMessages(const QList<TranscriptMessage>& messages) {
    if (streaming_) endAssistantMessage();
    for (const auto& restored : messages) {
        Message message;
        message.role = restored.role;
        message.markdown = restored.markdown;
        message.reasoning = restored.reasoning;
        messages_.push_back(std::move(message));
    }
    if (!messages.isEmpty()) refreshTail();
}

void ChatView::restoreMessages(const QList<TranscriptMessage>& messages) {
    clear();
    for (const auto& restored : messages) {
        Message message;
        message.role = restored.role;
        message.markdown = restored.markdown;
        message.reasoning = restored.reasoning;
        messages_.push_back(std::move(message));
    }
    firstVisibleMessage_ = std::max(0, static_cast<int>(messages_.size()) - 50);
    rebuild();
}

void ChatView::beginAssistantMessage() {
    if (streaming_) endAssistantMessage();

    Message message;
    message.role = ChatRole::Assistant;
    message.complete = false;
    messages_ << message;

    pending_.clear();
    pendingReasoning_.clear();
    streaming_ = true;
    flushTimer_->start();
    refreshTail();
}

void ChatView::appendDelta(const QString& text) {
    if (!streaming_) beginAssistantMessage();
    // The first ordinary token means thinking is over.
    if (thinking_ && !text.isEmpty()) {
        thinking_ = false;
        thinkingTimer_->stop();
    }
    pending_ += text;
}

void ChatView::appendReasoning(const QString& text) {
    if (!streaming_) beginAssistantMessage();
    if (!thinking_) {
        thinking_ = true;
        thinkingFrame_ = 0;
        thinkingClock_.start();
        thinkingTimer_->start();
    }
    pendingReasoning_ += text;
}

void ChatView::setTurnStats(const TurnStatsView& stats) {
    if (messages_.isEmpty()) return;

    QStringList parts;

    // --- Reasoning -------------------------------------------------------
    // No runtime reports how long the think block took: it reports one decode
    // phase covering reasoning and the answer together. All that can be seen
    // from here is how long reasoning deltas were arriving, and that is a
    // property of network buffering as much as of the model - a short block
    // arrives inside a single read and spans no time at all.
    //
    // So the size is stated, which is a fact, and a duration only when the
    // observation can support one: several deltas, spread over a period long
    // enough that buffering cannot explain it. Even then it is labelled as
    // streaming rather than thinking.
    if (stats.reasoningCharacters > 0) {
        const bool defensible =
            stats.reasoningDeltaCount >= 3 && stats.observedReasoningStreamMs >= 1000;
        QString reasoning = tr("thought ~%1 tokens").arg(stats.estimatedReasoningTokens);
        if (defensible) {
            reasoning +=
                tr(", streamed over %1").arg(formatDuration(stats.observedReasoningStreamMs));
        }
        parts << reasoning;
    }

    // --- Tokens ----------------------------------------------------------
    if (stats.completionTokens > 0) {
        parts << (stats.usageFromServer ? tr("%1 tokens").arg(stats.completionTokens)
                                        : tr("~%1 tokens").arg(stats.completionTokens));
    }

    // --- Throughput ------------------------------------------------------
    // Decode rate only. The prompt phase is reported separately because mixing
    // them produces a figure that tracks prompt length rather than model speed.
    if (stats.decodeTokensPerSecond > 0.0) {
        parts << (stats.timingsFromServer
                      ? tr("%1 tok/s decode").arg(stats.decodeTokensPerSecond, 0, 'f', 1)
                      : tr("~%1 tok/s decode").arg(stats.decodeTokensPerSecond, 0, 'f', 1));
    }
    if (stats.timingsFromServer && stats.promptTokensPerSecond > 0.0 &&
        stats.promptTokensEvaluated > 0) {
        parts << tr("%1 tok/s prompt (%2)")
                     .arg(stats.promptTokensPerSecond, 0, 'f', 1)
                     .arg(stats.promptTokensEvaluated);
    }

    auto& message = messages_.last();
    message.statsLine = parts.join(QStringLiteral("  ·  "));
    if (stats.timingsFromServer && (stats.promptMs > 0 || stats.decodeMs > 0)) {
        parts << tr("prompt %1 · generation %2")
                     .arg(formatDuration(static_cast<qint64>(stats.promptMs)),
                          formatDuration(static_cast<qint64>(stats.decodeMs)));
        if (stats.cachedPromptTokens > 0)
            parts << tr("%1 prompt tokens reused").arg(stats.cachedPromptTokens);
        message.statsLine = parts.join(QStringLiteral("  ·  "));
    }

    // The header above the reasoning block uses the same rule.
    message.reasoningSummary =
        stats.reasoningCharacters > 0
            ? (stats.reasoningDeltaCount >= 3 && stats.observedReasoningStreamMs >= 1000
                   ? tr("THOUGHT · ~%1 TOKENS · STREAMED OVER %2")
                         .arg(stats.estimatedReasoningTokens)
                         .arg(formatDuration(stats.observedReasoningStreamMs))
                   : tr("THOUGHT · ~%1 TOKENS").arg(stats.estimatedReasoningTokens))
            : QString{};

    message.cachedHtml.clear();
    refreshTail();
}

void ChatView::flushPending() {
    if (!streaming_) return;
    if (pending_.isEmpty() && pendingReasoning_.isEmpty()) return;
    if (messages_.isEmpty()) return;

    messages_.last().markdown += pending_;
    messages_.last().reasoning += pendingReasoning_;
    pending_.clear();
    pendingReasoning_.clear();
    refreshTail();
}

void ChatView::endAssistantMessage() {
    flushTimer_->stop();
    thinkingTimer_->stop();
    thinking_ = false;
    flushPending();
    if (!messages_.isEmpty()) messages_.last().complete = true;
    streaming_ = false;
    activityNote_.clear();
    refreshTail();
}

void ChatView::setFinalMessage(const QString& markdown) {
    if (streaming_) endAssistantMessage();
    if (markdown.isEmpty()) return;
    if (!messages_.isEmpty() && messages_.last().role == ChatRole::Assistant) {
        auto& message = messages_.last();
        const auto current = message.markdown.trimmed();
        const auto final = markdown.trimmed();
        if (current == final) return;
        if (!current.isEmpty() && final.startsWith(current + '\n')) {
            message.markdown = markdown;
            message.cachedHtml.clear();
            refreshTail();
            return;
        }
    }
    addMessage(ChatRole::Assistant, markdown);
}

void ChatView::setActivityNote(const QString& note) {
    if (activityNote_ == note) return;
    activityNote_ = note;
    refreshTail();
}

void ChatView::clearActivityNote() {
    setActivityNote({});
}

void ChatView::clear() {
    flushTimer_->stop();
    thinkingTimer_->stop();
    thinking_ = false;
    thinkingFrame_ = 0;
    messages_.clear();
    firstVisibleMessage_ = 0;
    pending_.clear();
    pendingReasoning_.clear();
    activityNote_.clear();
    streaming_ = false;
    following_ = true;
    rebuild();
}

QString ChatView::renderMessage(const Message& message, int index) const {
    if (message.complete && !message.cachedHtml.isEmpty()) return message.cachedHtml;

    const auto& p = colours();
    const bool is_user = message.role == ChatRole::User;

    QString html;
    // Author line.
    html += QString(
                "<p style=\"color:%1;font-weight:600;font-size:9pt;"
                "margin:18px 0 6px 0\">%2</p>")
                .arg(colour(roleColour(message.role)), roleLabel(message.role));

    // Native reasoning, when the model produced any and the user wants it shown.
    if (showReasoning_ && !message.reasoning.trimmed().isEmpty()) {
        // The header is live while the model is still thinking: a moving counter
        // and a travelling dot. Minutes can pass with no visible output on a
        // local model, and a still screen is indistinguishable from a hang.
        QString header;
        const bool live = thinking_ && !message.complete;
        if (live) {
            // Live, this IS wall time and is honest by construction: the counter
            // is running now and the user is watching it run.
            // QChar by code point, not a literal: a middle dot written into the
            // source is one bad encoding round trip away from rendering as two
            // pieces of rubbish, which is exactly what happened here - the live
            // header read THINKING <mojibake> 506ms in a real session. Nothing
            // about this string now depends on how the file is encoded.
            static const QChar kDot(0x00B7);
            const int frame = thinkingFrame_ % 4;
            const int lead = (frame == 0) ? 0 : (frame == 2 ? 2 : 1);
            const QString marker = QString(lead, QChar(' ')) + kDot + QString(2 - lead, QChar(' '));
            header = QStringLiteral("THINKING %1 %2")
                         .arg(marker, formatDuration(thinkingClock_.elapsed()));
        } else if (!message.reasoningSummary.isEmpty()) {
            header = message.reasoningSummary;
        } else {
            header = QStringLiteral("THOUGHT");
        }

        html += QString(
                    "<p style=\"color:%1;font-size:9pt;margin:0 0 8px 0\">%2 &nbsp; "
                    "<a style=\"color:%1\" href=\"localcode:reasoning/%3\">%4</a> &nbsp; "
                    "<a style=\"color:%1\" href=\"localcode:copy-reasoning/%3\">%5</a></p>")
                    .arg(colour(live ? p.accent : p.textMuted), header)
                    .arg(index)
                    .arg(message.reasoningExpanded ? tr("Hide reasoning") : tr("Show reasoning"),
                         tr("Copy"));
        if (message.reasoningExpanded) {
            html += QString("<p style=\"color:%1;font-size:9pt;margin:0 0 10px 0\">%2</p>")
                        .arg(colour(p.textMuted),
                             MarkdownRenderer::escape(message.reasoning).replace('\n', "<br>"));
        }
    }

    QString body = message.complete ? MarkdownRenderer::toHtml(message.markdown)
                                    : MarkdownRenderer::toHtmlStreaming(message.markdown);
    // Renderer block numbers are local to a message. Preserve that identity
    // rather than interpreting every message's first copy link as block zero
    // of the whole conversation (especially when old pages are not laid out).
    body.replace("href=\"localcode:copy/", QString("href=\"localcode:copy/%1/").arg(index));

    if (is_user) {
        // The user's own words get a quiet surface so the eye can find them when
        // scrolling back through a long session.
        html += QString(
                    "<div style=\"background:%1;border-left:2px solid %2;"
                    "padding:8px 12px;border-radius:0 4px 4px 0\">%3</div>")
                    .arg(colour(p.surface), colour(p.accent), body);
    } else if (message.role == ChatRole::Error) {
        html += QString(
                    "<div style=\"background:%1;border-left:2px solid %2;"
                    "padding:8px 12px;color:%3;border-radius:0 4px 4px 0\">%4</div>")
                    .arg(colour(p.surface), colour(p.danger), colour(p.text), body);
    } else if (message.role == ChatRole::Notice) {
        html += QString(
                    "<div style=\"background:%1;border-left:2px solid %2;"
                    "padding:8px 12px;color:%3;border-radius:0 4px 4px 0\">%4</div>")
                    .arg(colour(p.surface), colour(p.warning), colour(p.textMuted), body);
    } else {
        html += body;
    }

    // A caret while this message is still being written.
    if (!message.complete && index == messages_.size() - 1) {
        html += QString("<span style=\"color:%1\">&#9612;</span>").arg(colour(p.accent));
    }

    // What the turn cost, kept faint: useful when you look for it, invisible
    // when you are reading the answer.
    if (!message.statsLine.isEmpty()) {
        html += QString("<p style=\"color:%1;font-size:9pt;margin:8px 0 12px 0\">%2</p>")
                    .arg(colour(p.textFaint), MarkdownRenderer::escape(message.statsLine));
    }

    if (message.complete) message.cachedHtml = html;
    return html;
}

void ChatView::rebuild() {
    const auto& p = colours();
    auto* bar = browser_->verticalScrollBar();
    const int previous = bar->value();
    const QSignalBlocker scrollSignals(bar);

    QString html = QString("<body style=\"color:%1;%2line-height:140%\">")
                       .arg(colour(p.text), Theme::instance().richTextStyle());

    if (firstVisibleMessage_ > 0) {
        html += QString("<p style=\"color:%1\">%2 &nbsp; <a href=\"localcode:earlier\">%3</a></p>")
                    .arg(colour(p.textMuted),
                         tr("%n earlier message(s)", nullptr, firstVisibleMessage_),
                         tr("Load earlier messages"));
    }

    if (messages_.isEmpty() && activityNote_.isEmpty()) {
        // An empty pane is the first thing anyone sees, and a blank rectangle
        // says nothing about what this is or what it can do. Kept short and
        // quiet: it is a starting point, not a splash screen, and it disappears
        // the moment there is a real conversation to read.
        // Paragraphs, not divs: Qt's rich text engine does not apply margins to
        // nested divs here and ran the label into the sentence after it.
        html += QString(
                    "<div style=\"margin:24px 4px 0 4px;font-size:10pt\">"
                    "<p style=\"color:%2;font-size:9pt;margin:0 0 12px 0\">"
                    "READY</p>"
                    "<p style=\"color:%1;margin:0 0 16px 0\">"
                    "Add a GGUF model from the File menu and choose a working folder. "
                    "Use the project menu to open a folder or work without a project.</p>"
                    "<p style=\"color:%1;margin:0 0 16px 0\">"
                    "Then describe a change: LocalCode can read files, run commands "
                    "and edit the files in your working folder.</p>"
                    "<p style=\"color:%3;margin:0 0 6px 0\">"
                    "Every edit is made on disk and shown under <b>Changes</b>.</p>"
                    "<p style=\"color:%3;margin:0 0 6px 0\">"
                    "Commands run under a policy; destructive ones are refused.</p>"
                    "<p style=\"color:%3;margin:0 0 18px 0\">"
                    "Claims in the final answer are checked against what actually ran.</p>"
                    "<p style=\"color:%3;margin:0\">"
                    "<b>Ctrl+Enter</b> sends &nbsp;·&nbsp; <b>Esc</b> stops</p>"
                    "</div>")
                    .arg(colour(p.textMuted), colour(p.textFaint), colour(p.textFaint));
    }

    html += "</body>";
    browser_->setHtml(html);
    renderedMessages_ = firstVisibleMessage_;
    lastMessageStart_ = 0;
    activityStart_ = -1;

    if (!messages_.isEmpty() || !activityNote_.isEmpty()) refreshTail();

    if (following_) {
        bar->setValue(bar->maximum());
    } else {
        bar->setValue(qMin(previous, bar->maximum()));
    }
    updateLatestButton();
}

void ChatView::refreshTail() {
    if (messages_.isEmpty() && activityNote_.isEmpty()) {
        if (activityStart_ >= 0) rebuild();
        return;
    }
    auto* document = browser_->document();
    auto* bar = browser_->verticalScrollBar();
    const int previous = bar->value();
    const bool follow = following_;
    // Internal range/value changes must not change the user's scroll intent.
    const QSignalBlocker scrollSignals(bar);
    // The edit block already coalesces these changes. Toggling layout off/on
    // asks Qt to invalidate the whole document, defeating tail-only updates.
    QTextCursor cursor(document);
    cursor.beginEditBlock();

    auto eraseTail = [&](int position) {
        cursor.setPosition(position);
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        cursor.setBlockFormat(QTextBlockFormat{});
        cursor.setCharFormat(QTextCharFormat{});
    };
    auto insertMessage = [&](int index) {
        const auto html = QString("<body style=\"color:%1;%3line-height:140%\">%2</body>")
                              .arg(colour(colours().text), renderMessage(messages_[index], index),
                                   Theme::instance().richTextStyle());
        cursor.insertHtml(html);
        QTextCursor author(document);
        author.setPosition(lastMessageStart_);
        auto format = author.blockFormat();
        format.setTopMargin(index > firstVisibleMessage_ ? 18 : 6);
        format.setBottomMargin(6);
        author.setBlockFormat(format);
    };

    if (renderedMessages_ < messages_.size()) {
        if (renderedMessages_ == 0) {
            // Remove the welcome text once, when the first real message arrives.
            eraseTail(0);
        } else if (activityStart_ >= 0) {
            eraseTail(activityStart_);
        }
        for (int index = renderedMessages_; index < messages_.size(); ++index) {
            cursor.movePosition(QTextCursor::End);
            if (index > 0) cursor.insertBlock(QTextBlockFormat{}, QTextCharFormat{});
            lastMessageStart_ = cursor.position();
            insertMessage(index);
        }
        renderedMessages_ = messages_.size();
    } else if (!messages_.isEmpty()) {
        // The finished history remains in Qt's document, including its layout
        // and selection. Only the active/latest message and its activity suffix
        // are replaced. No concatenation, parsing or removal of old messages.
        eraseTail(lastMessageStart_);
        insertMessage(messages_.size() - 1);
    } else {
        eraseTail(0);
    }

    activityStart_ = -1;
    if (!activityNote_.isEmpty()) {
        cursor.movePosition(QTextCursor::End);
        activityStart_ = cursor.position();
        if (!messages_.isEmpty()) cursor.insertBlock(QTextBlockFormat{}, QTextCharFormat{});
        cursor.insertHtml(
            QString("<p style=\"color:%1;font-size:9pt;font-style:italic;"
                    "margin:4px 0\">%2</p>")
                .arg(colour(colours().textFaint), MarkdownRenderer::escape(activityNote_)));
    }
    cursor.endEditBlock();
    following_ = follow;
    bar->setValue(follow ? bar->maximum() : qMin(previous, bar->maximum()));
    updateLatestButton();
}

void ChatView::scrollToBottomIfFollowing() {
    if (!following_) return;
    auto* bar = browser_->verticalScrollBar();
    bar->setValue(bar->maximum());
    updateLatestButton();
}

void ChatView::updateLatestButton() {
    if (!latestButton_) return;
    auto* bar = browser_->verticalScrollBar();
    latestButton_->setVisible(!following_ && bar->maximum() > bar->value() + 24);
}

void ChatView::handleAnchorClicked(const QUrl& url) {
    const QString href = url.toString();

    if (href == "localcode:earlier") {
        if (firstVisibleMessage_ == 0) return;
        firstVisibleMessage_ = std::max(0, firstVisibleMessage_ - 50);
        following_ = false;
        rebuild();
        browser_->verticalScrollBar()->setValue(0);
        return;
    }

    for (const auto& prefix :
         {QStringLiteral("localcode:reasoning/"), QStringLiteral("localcode:copy-reasoning/")}) {
        if (!href.startsWith(prefix)) continue;
        bool ok = false;
        const int index = href.mid(prefix.size()).toInt(&ok);
        if (!ok || index < 0 || index >= messages_.size()) return;
        auto& message = messages_[index];
        if (prefix == QStringLiteral("localcode:copy-reasoning/")) {
            QApplication::clipboard()->setText(message.reasoning);
            emit codeCopied();
        } else {
            message.reasoningExpanded = !message.reasoningExpanded;
            message.cachedHtml.clear();
            if (index == messages_.size() - 1)
                refreshTail();
            else
                rebuild();
        }
        return;
    }

    // Code-block copy links are handled here rather than by the shell.
    if (href.startsWith("localcode:copy/")) {
        const auto parts = href.mid(QStringLiteral("localcode:copy/").size()).split('/');
        if (parts.size() != 2) return;
        bool messageOk = false, blockOk = false;
        const int message = parts[0].toInt(&messageOk);
        const int block = parts[1].toInt(&blockOk);
        if (!messageOk || !blockOk || message < 0 || message >= messages_.size() || block < 0)
            return;
        const auto blocks = MarkdownRenderer::codeBlocks(messages_[message].markdown);
        if (block >= blocks.size()) return;
        QApplication::clipboard()->setText(blocks[block].code);
        emit codeCopied();
        return;
    }

    if (url.scheme() == "http" || url.scheme() == "https") {
        QDesktopServices::openUrl(url);
        return;
    }
    emit linkActivated(href);
}

void ChatView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    scrollToBottomIfFollowing();
    updateLatestButton();
}

QString ChatView::toMarkdown() const {
    QStringList parts;
    for (const auto& message : messages_) {
        parts << QString("## %1\n\n%2").arg(roleLabel(message.role), message.markdown);
    }
    return parts.join("\n\n");
}

}   // namespace localcode::gui
