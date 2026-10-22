#include "DiffView.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

#include "../Theme.h"
#include "MarkdownRenderer.h"

namespace localcode::gui {
namespace {

QString colour(const QColor& c) { return c.name(QColor::HexRgb); }

QString changeLabel(GitChange change) {
    switch (change) {
        case GitChange::Added:      return QStringLiteral("added");
        case GitChange::Deleted:    return QStringLiteral("deleted");
        case GitChange::Renamed:    return QStringLiteral("renamed");
        case GitChange::Copied:     return QStringLiteral("copied");
        case GitChange::Untracked:  return QStringLiteral("new");
        case GitChange::Conflicted: return QStringLiteral("conflicted");
        default:                    return QStringLiteral("modified");
    }
}

}  // namespace

DiffView::DiffView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QWidget(this);
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(8, 4, 8, 4);
    barLayout->setSpacing(8);

    summary_ = new QLabel(bar);
    summary_->setObjectName("Muted");
    barLayout->addWidget(summary_, 1);

    mode_ = new QComboBox(bar);
    mode_->addItem(tr("Unified"));
    mode_->addItem(tr("Side by side"));
    barLayout->addWidget(mode_);

    auto* refresh = new QPushButton(tr("Refresh"), bar);
    refresh->setObjectName("Flat");
    barLayout->addWidget(refresh);
    layout->addWidget(bar);

    splitter_ = new QSplitter(Qt::Horizontal, this);

    files_ = new QListWidget(splitter_);
    files_->setFrameShape(QFrame::NoFrame);
    files_->setMinimumWidth(180);
    splitter_->addWidget(files_);

    content_ = new QTextBrowser(splitter_);
    content_->setFrameShape(QFrame::NoFrame);
    content_->setOpenLinks(false);
    content_->setLineWrapMode(QTextEdit::NoWrap);
    splitter_->addWidget(content_);

    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({220, 700});
    layout->addWidget(splitter_, 1);

    connect(files_, &QListWidget::currentRowChanged, this, &DiffView::showSelectedFile);
    connect(mode_, &QComboBox::currentIndexChanged, this, &DiffView::modeChanged);
    connect(refresh, &QPushButton::clicked, this, &DiffView::refreshRequested);
    connect(content_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        // Anchors are "line:<n>" so a hunk header can jump into the editor.
        const QString href = url.toString();
        if (!href.startsWith("line:")) return;
        const int row = files_->currentRow();
        if (row < 0 || row >= diff_.files.size()) return;
        emit openFileRequested(QString::fromStdString(diff_.files[row].path),
                               href.mid(5).toInt());
    });
}

bool DiffView::sideBySide() const { return mode_->currentIndex() == 1; }

void DiffView::setAgentChanges(const QStringList& changed, const QStringList& conflicting) {
    agentChanged_ = changed;
    conflicting_ = conflicting;
    populateList();
}

void DiffView::setDiff(const DiffResult& diff) {
    diff_ = diff;
    populateList();

    summary_->setText(tr("%1 file(s)   +%2  -%3")
                          .arg(diff_.files.size())
                          .arg(diff_.total_additions)
                          .arg(diff_.total_deletions));

    if (!diff_.files.empty()) {
        files_->setCurrentRow(0);
    } else {
        content_->setHtml(QString("<div style=\"color:%1;padding:14px\">%2</div>")
                              .arg(colour(colours().textFaint),
                                   tr("No changes in the working tree.")));
    }
}

void DiffView::populateList() {
    const int previous = files_->currentRow();
    files_->clear();

    for (const auto& file : diff_.files) {
        const QString path = QString::fromStdString(file.path);
        auto* item = new QListWidgetItem(path, files_);

        QString tip = QString("%1  +%2 -%3")
                          .arg(changeLabel(file.change))
                          .arg(file.additions)
                          .arg(file.deletions);

        if (conflicting_.contains(path)) {
            item->setForeground(colours().warning);
            tip += "\n" + tr("This file already had uncommitted changes before the agent "
                             "edited it.");
        } else if (agentChanged_.contains(path)) {
            item->setForeground(colours().accent);
            tip += "\n" + tr("Changed by the agent during this run.");
        }
        item->setToolTip(tip);
    }

    if (previous >= 0 && previous < files_->count()) files_->setCurrentRow(previous);
}

void DiffView::modeChanged() { showSelectedFile(); }

void DiffView::showSelectedFile() {
    const int row = files_->currentRow();
    if (row < 0 || row >= static_cast<int>(diff_.files.size())) {
        content_->clear();
        return;
    }
    const auto& file = diff_.files[static_cast<std::size_t>(row)];
    emit fileSelected(QString::fromStdString(file.path));
    content_->setHtml(sideBySide() ? renderSideBySide(file) : renderUnified(file));
}

QString DiffView::renderUnified(const FileDiff& file) const {
    const auto& p = colours();
    const QString mono = Theme::instance().monoFont().family();

    QString html = QString("<body style=\"font-family:'%1';font-size:%2pt\">").arg(mono).arg(
        Theme::instance().monoPointSize());

    html += QString("<div style=\"color:%1;padding:6px 8px;background:%2\">%3 &nbsp; "
                    "<span style=\"color:%4\">+%5</span> "
                    "<span style=\"color:%6\">-%7</span></div>")
                .arg(colour(p.text), colour(p.surface),
                     MarkdownRenderer::escape(QString::fromStdString(file.path)),
                     colour(p.addedText))
                .arg(file.additions)
                .arg(colour(p.removedText))
                .arg(file.deletions);

    if (file.binary) {
        html += QString("<div style=\"color:%1;padding:10px\">%2</div>")
                    .arg(colour(p.textMuted), tr("Binary file; no textual diff."));
        return html + "</body>";
    }

    for (const auto& hunk : file.hunks) {
        html += QString("<div style=\"background:%1;color:%2;padding:3px 8px;margin-top:6px\">"
                        "<a href=\"line:%3\" style=\"color:%2;text-decoration:none\">%4</a></div>")
                    .arg(colour(p.surfaceRaised), colour(p.textMuted))
                    .arg(hunk.new_start)
                    .arg(MarkdownRenderer::escape(QString::fromStdString(hunk.header)));

        int old_line = hunk.old_start;
        int new_line = hunk.new_start;

        for (const auto& raw : hunk.lines) {
            const QString line = QString::fromStdString(raw);
            if (line.isEmpty()) continue;

            const QChar kind = line[0];
            const QString body = MarkdownRenderer::escape(line.mid(1));

            QString background, text_colour, numbers;
            if (kind == '+') {
                background = colour(p.addedBackground);
                text_colour = colour(p.addedText);
                numbers = QString("%1 %2").arg("     ", QString::number(new_line++).rightJustified(5));
            } else if (kind == '-') {
                background = colour(p.removedBackground);
                text_colour = colour(p.removedText);
                numbers = QString("%1 %2").arg(QString::number(old_line++).rightJustified(5), "     ");
            } else {
                background = "transparent";
                text_colour = colour(p.textMuted);
                numbers = QString("%1 %2").arg(QString::number(old_line++).rightJustified(5),
                                               QString::number(new_line++).rightJustified(5));
            }

            html += QString("<div style=\"background:%1;white-space:pre\">"
                            "<span style=\"color:%2\">%3</span> "
                            "<span style=\"color:%4\">%5%6</span></div>")
                        .arg(background, colour(p.textFaint),
                             MarkdownRenderer::escape(numbers), text_colour,
                             MarkdownRenderer::escape(QString(kind)), body);
        }
    }
    return html + "</body>";
}

QString DiffView::renderSideBySide(const FileDiff& file) const {
    const auto& p = colours();
    const QString mono = Theme::instance().monoFont().family();

    QString html = QString("<body style=\"font-family:'%1';font-size:%2pt\">").arg(mono).arg(
        Theme::instance().monoPointSize());
    html += QString("<div style=\"color:%1;padding:6px 8px;background:%2\">%3</div>")
                .arg(colour(p.text), colour(p.surface),
                     MarkdownRenderer::escape(QString::fromStdString(file.path)));

    if (file.binary) {
        return html + QString("<div style=\"color:%1;padding:10px\">%2</div></body>")
                          .arg(colour(p.textMuted), tr("Binary file; no textual diff."));
    }

    html += "<table cellspacing=\"0\" cellpadding=\"2\" width=\"100%\" "
            "style=\"border-collapse:collapse\">";

    for (const auto& hunk : file.hunks) {
        html += QString("<tr><td colspan=\"4\" style=\"background:%1;color:%2;padding:3px 6px\">"
                        "%3</td></tr>")
                    .arg(colour(p.surfaceRaised), colour(p.textMuted),
                         MarkdownRenderer::escape(QString::fromStdString(hunk.header)));

        // Pair removed lines with added lines so a modification reads across.
        QList<QPair<QString, QString>> pairs;
        QStringList removed, added;
        auto flush = [&] {
            const int rows = qMax(removed.size(), added.size());
            for (int i = 0; i < rows; ++i) {
                pairs.append({i < removed.size() ? removed[i] : QString(),
                              i < added.size() ? added[i] : QString()});
            }
            removed.clear();
            added.clear();
        };

        for (const auto& raw : hunk.lines) {
            const QString line = QString::fromStdString(raw);
            if (line.isEmpty()) continue;
            if (line[0] == '-') { removed << line.mid(1); continue; }
            if (line[0] == '+') { added << line.mid(1); continue; }
            flush();
            pairs.append({line.mid(1), line.mid(1)});
        }
        flush();

        int old_line = hunk.old_start;
        int new_line = hunk.new_start;
        for (const auto& [left, right] : pairs) {
            const bool same = left == right;
            const QString left_bg = (same || left.isEmpty()) ? "transparent"
                                                             : colour(p.removedBackground);
            const QString right_bg = (same || right.isEmpty()) ? "transparent"
                                                               : colour(p.addedBackground);

            html += "<tr>";
            html += QString("<td style=\"color:%1;text-align:right;width:38px\">%2</td>")
                        .arg(colour(p.textFaint),
                             left.isEmpty() && !same ? QString() : QString::number(old_line++));
            html += QString("<td style=\"background:%1;color:%2;white-space:pre;width:50%%\">"
                            "%3</td>")
                        .arg(left_bg, colour(same ? p.textMuted : p.removedText),
                             MarkdownRenderer::escape(left));
            html += QString("<td style=\"color:%1;text-align:right;width:38px\">%2</td>")
                        .arg(colour(p.textFaint),
                             right.isEmpty() && !same ? QString() : QString::number(new_line++));
            html += QString("<td style=\"background:%1;color:%2;white-space:pre;width:50%%\">"
                            "%3</td>")
                        .arg(right_bg, colour(same ? p.textMuted : p.addedText),
                             MarkdownRenderer::escape(right));
            html += "</tr>";
        }
    }
    return html + "</table></body>";
}

void DiffView::clear() {
    diff_ = DiffResult{};
    files_->clear();
    content_->clear();
    summary_->clear();
}

}  // namespace localcode::gui
