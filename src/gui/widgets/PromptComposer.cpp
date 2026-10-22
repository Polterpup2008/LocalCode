#include "PromptComposer.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>

#include "../Theme.h"

namespace localcode::gui {

PromptComposer::PromptComposer(QWidget* parent) : QPlainTextEdit(parent) {
    setPlaceholderText(tr("Ask LocalCode to do something.   Ctrl+Enter to send"));
    setAcceptDrops(true);
    setTabChangesFocus(false);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Undo is on by default; the limit keeps a very long session bounded
    // without ever losing what a user is likely to want back.
    document()->setMaximumBlockCount(0);
    document()->setUndoRedoEnabled(true);

    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        updateHeight();
        emit draftChanged(toPlainText());
    });
    updateHeight();
}

void PromptComposer::setBusy(bool busy) {
    busy_ = busy;
    setPlaceholderText(busy ? tr("Working...   Esc or the Stop button to cancel")
                            : tr("Ask LocalCode to do something.   Ctrl+Enter to send"));
    // QPlainTextEdit repaints on a placeholder change only when it decides the
    // document is empty, and in this window it did not: after a run finished the
    // composer went on reading "Working... Esc or the Stop button to cancel"
    // indefinitely, while Send was enabled and the agent was idle. Ask for the
    // repaint rather than relying on that decision.
    viewport()->update();
}

QString PromptComposer::promptText() const { return toPlainText().trimmed(); }

void PromptComposer::clearPrompt() {
    clear();
    updateHeight();
}

void PromptComposer::updateHeight() {
    const QFontMetrics metrics(font());
    const int line = metrics.lineSpacing();
    const int lines = qBound(minimumLines_, document()->blockCount(), maximumLines_);
    const int height = line * lines + 16;
    if (height != minimumHeight()) {
        setMinimumHeight(height);
        setMaximumHeight(height);
    }
}

void PromptComposer::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    updateHeight();
}

void PromptComposer::keyPressEvent(QKeyEvent* event) {
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;

    // Ctrl+Enter sends. Plain Enter inserts a newline, because a prompt to a
    // coding agent is usually more than one line and losing a half-written one
    // to a stray Return is infuriating.
    if (enter && ctrl) {
        const auto text = promptText();
        if (!text.isEmpty() && !busy_) emit submitted(text);
        return;
    }
    if (event->key() == Qt::Key_Escape && busy_) {
        emit stopRequested();
        return;
    }
    // Shift+Enter is the other convention for a newline; accept it too.
    if (enter && shift) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

void PromptComposer::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasText()) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragEnterEvent(event);
}

void PromptComposer::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasText()) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragMoveEvent(event);
}

void PromptComposer::dropEvent(QDropEvent* event) {
    const auto* mime = event->mimeData();
    if (!mime->hasUrls()) {
        QPlainTextEdit::dropEvent(event);
        return;
    }

    QStringList paths;
    QStringList inserted;
    const QDir root(workspaceRoot_);

    for (const auto& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QString absolute = url.toLocalFile();
        paths << absolute;

        // A path inside the workspace is inserted relative, which is what the
        // tools expect and what is readable in the prompt.
        QString shown = absolute;
        if (!workspaceRoot_.isEmpty()) {
            const QString relative = root.relativeFilePath(absolute);
            if (!relative.startsWith("..")) shown = relative;
        }
        inserted << (shown.contains(' ') ? QString("\"%1\"").arg(shown) : shown);
    }

    if (!inserted.isEmpty()) {
        auto cursor = textCursor();
        if (!cursor.atBlockStart() && !toPlainText().isEmpty() &&
            !toPlainText().endsWith(' ')) {
            cursor.insertText(" ");
        }
        cursor.insertText(inserted.join(' '));
        setTextCursor(cursor);
        emit filesDropped(paths);
    }
    event->acceptProposedAction();
}

}  // namespace localcode::gui
