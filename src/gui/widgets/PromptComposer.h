#pragma once
// ---------------------------------------------------------------------------
// The prompt input.
//
// This is a plain QPlainTextEdit and that is the point: it is a real editor
// widget, so paste, undo, redo, selection, scrolling and Unicode all work at
// native speed. A 200 KB paste is one document operation, not a keystroke loop.
//
// What is added on top: Ctrl+Enter to send, Enter for a newline, an auto-growing
// height, drag-and-drop of files as workspace-relative paths, and a draft that
// is handed back to the owner so it survives a crash.
// ---------------------------------------------------------------------------
#include <QPlainTextEdit>
#include <QString>
#include <QStringList>

namespace localcode::gui {

class PromptComposer : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit PromptComposer(QWidget* parent = nullptr);

    void setBusy(bool busy);
    [[nodiscard]] bool busy() const { return busy_; }

    // Paths dropped onto the composer are resolved against this directory.
    void setWorkspaceRoot(const QString& root) { workspaceRoot_ = root; }

    [[nodiscard]] QString promptText() const;
    void clearPrompt();

signals:
    void submitted(const QString& text);
    void stopRequested();
    void draftChanged(const QString& text);
    void filesDropped(const QStringList& paths);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateHeight();

private:
    QString workspaceRoot_;
    bool    busy_ = false;
    int     minimumLines_ = 3;
    int     maximumLines_ = 14;
};

}  // namespace localcode::gui
