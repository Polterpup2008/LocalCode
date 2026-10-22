#pragma once
// ---------------------------------------------------------------------------
// The conversation.
//
// Streaming is the constraint that shapes this class. Tokens arrive faster than
// a text document can be re-laid-out, so deltas are accumulated in a buffer and
// flushed on a timer; only the message currently being written is re-rendered,
// and earlier history stays intact when the authoritative final reply arrives.
// ---------------------------------------------------------------------------
#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QTextBrowser>
#include <QTimer>
#include <QWidget>

namespace localcode::gui {

enum class ChatRole { User, Assistant, System, Notice, Error };

class ChatView : public QWidget {
    Q_OBJECT

public:
    explicit ChatView(QWidget* parent = nullptr);

    // Appends a complete message.
    void addMessage(ChatRole role, const QString& markdown);

    struct TranscriptMessage {
        ChatRole role = ChatRole::Assistant;
        QString markdown;
        QString reasoning;
    };

    // Restore persisted messages with one document update, without simulating
    // hundreds of streaming turns or starting their timers.
    void addMessages(const QList<TranscriptMessage>& messages);
    // Keep the full transcript, but initially lay out only its recent page.
    void restoreMessages(const QList<TranscriptMessage>& messages);

    // --- Streaming --------------------------------------------------------
    // Opens an assistant message that will be filled in by appendDelta().
    void beginAssistantMessage();
    void appendDelta(const QString& text);
    void appendReasoning(const QString& text);
    // Closes the streaming message and renders it in its final form.
    void endAssistantMessage();

    // Adds the authoritative final reply, merging an appended application note
    // into the last streamed reply while retaining its reasoning and stats.
    void setFinalMessage(const QString& markdown);

    [[nodiscard]] bool streaming() const { return streaming_; }

    // A one-line status the view shows in place of a message body while the
    // model is processing the prompt.
    void setActivityNote(const QString& note);
    void clearActivityNote();

    // What the last completed turn cost, shown under the message it belongs to.
    // A plain mirror of the controller's UiTurnStats, so this widget does not
    // include engine or controller headers.
    struct TurnStatsView {
        bool usageFromServer = false;
        int completionTokens = 0;
        bool timingsFromServer = false;
        int promptTokensEvaluated = 0;
        double promptTokensPerSecond = 0.0;
        double decodeTokensPerSecond = 0.0;
        int reasoningCharacters = 0;
        int estimatedReasoningTokens = 0;
        qint64 observedReasoningStreamMs = 0;
        int reasoningDeltaCount = 0;
    };

    void setTurnStats(const TurnStatsView& stats);

    void clear();
    void setShowReasoning(bool show);
    void refreshTheme();

    [[nodiscard]] bool showReasoning() const { return showReasoning_; }

    // The whole conversation as markdown, for "copy transcript".
    [[nodiscard]] QString toMarkdown() const;

signals:
    void linkActivated(const QString& href);
    void codeCopied();

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void flushPending();
    void handleAnchorClicked(const QUrl& url);

private:
    struct Message {
        ChatRole role = ChatRole::Assistant;
        QString markdown;
        QString reasoning;
        bool complete = true;
        bool reasoningExpanded = false;
        // Filled in when the turn finishes; rendered as a quiet footer.
        QString statsLine;
        // Header for the reasoning block once the turn is done. Never a bare
        // duration: see setTurnStats for why an observed stream span is not the
        // time the model spent thinking.
        QString reasoningSummary;
        // Rendering markdown and highlighting code is the expensive half of a
        // refresh. A finished message never changes, so its HTML is rendered
        // once and reused for every subsequent streaming flush.
        mutable QString cachedHtml;
    };

    void rebuild();
    void refreshTail();
    [[nodiscard]] QString renderMessage(const Message& message, int index) const;
    void scrollToBottomIfFollowing();

    QTextBrowser* browser_ = nullptr;
    QTimer* flushTimer_ = nullptr;
    // Ticks once a second while the model is thinking so the elapsed count moves.
    // Local reasoning can run for minutes; a static label is indistinguishable
    // from a hang.
    QTimer* thinkingTimer_ = nullptr;
    QElapsedTimer thinkingClock_;
    bool thinking_ = false;
    int thinkingFrame_ = 0;
    QList<Message> messages_;
    int renderedMessages_ = 0;
    int firstVisibleMessage_ = 0;
    int lastMessageStart_ = 0;
    int activityStart_ = -1;
    QString pending_;
    QString pendingReasoning_;
    QString activityNote_;
    bool streaming_ = false;
    bool showReasoning_ = true;
    bool following_ = true;
};

}   // namespace localcode::gui
