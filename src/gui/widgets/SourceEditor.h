#pragma once
// ---------------------------------------------------------------------------
// A source viewer and editor.
//
// It is not trying to replace an IDE. It exists so that when the agent says it
// changed render.cpp:112, the user can look at line 112 without leaving the
// window - and fix something by hand if they want to.
// ---------------------------------------------------------------------------
#include <QByteArray>
#include <QPlainTextEdit>
#include <QString>
#include <QTabWidget>
#include <QWidget>

#include "SyntaxHighlighter.h"

class QLabel;
class QLineEdit;

namespace localcode::gui {

// One file. Owns its highlighter and the line-number gutter.
class SourceView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit SourceView(QWidget* parent = nullptr);

    bool loadFile(const QString& path, QString* error);
    bool saveFile(QString* error);
    // Returns true only when new disk content/format was loaded. Unchanged
    // documents keep their selection and undo history; local edits are skipped.
    bool reloadIfChanged(QString* error);

    [[nodiscard]] QString filePath() const { return path_; }

    [[nodiscard]] bool modified() const { return document()->isModified(); }

    [[nodiscard]] QString encodingName() const { return encoding_; }

    [[nodiscard]] Language language() const { return highlighter_->language(); }

    void gotoLine(int line);
    void findNext(const QString& text, bool backwards = false);
    void refreshTheme();

    // Gutter painting is delegated here by the gutter widget.
    void paintLineNumbers(QPaintEvent* event);
    [[nodiscard]] int lineNumberAreaWidth() const;

signals:
    void modifiedChanged(bool modified);
    void cursorPositionDescribed(const QString& text);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightCurrentLine();
    void describeCursor();

private:
    bool readFile(const QString& path, QString* error, bool skipUnchanged);
    QWidget* gutter_ = nullptr;
    SyntaxHighlighter* highlighter_ = nullptr;
    QString path_;
    QString encoding_ = "utf-8";
    QString lineEnding_ = "lf";
    QByteArray diskContentHash_;
};

// A tabbed set of open files.
class SourceEditor : public QWidget {
    Q_OBJECT

public:
    explicit SourceEditor(QWidget* parent = nullptr);

    void openFile(const QString& path, int line = 0);
    bool closeAll();
    bool saveCurrent(QString* error);
    bool saveAll(QString* error);
    [[nodiscard]] QStringList openFiles() const;
    [[nodiscard]] bool hasUnsavedChanges() const;
    // Re-reads any open file that changed on disk and has no local edits, which
    // is what happens constantly while the agent is working.
    void reloadUnmodified();
    void refreshTheme();

signals:
    void statusMessage(const QString& text);

private slots:
    bool closeTab(int index);
    void updateTabTitle();

private:
    [[nodiscard]] SourceView* viewAt(int index) const;
    [[nodiscard]] SourceView* currentView() const;
    [[nodiscard]] int indexOfFile(const QString& path) const;

    QTabWidget* tabs_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* status_ = nullptr;
};

}   // namespace localcode::gui
