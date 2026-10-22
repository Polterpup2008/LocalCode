#include "SourceEditor.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QTextBlock>
#include <QVBoxLayout>

#include "../Theme.h"
#include "platform/FileSystem.h"
#include "platform/PathEncoding.h"

namespace localcode::gui {
namespace {

// The gutter is a bare widget that forwards its paint to the editor, which is
// the standard Qt arrangement for line numbers.
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(SourceView* editor) : QWidget(editor), editor_(editor) {}

    QSize sizeHint() const override { return {editor_->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { editor_->paintLineNumbers(event); }

private:
    SourceView* editor_;
};

}   // namespace

void SourceView::refreshTheme() {
    setFont(Theme::instance().monoFont());
    highlighter_->rehighlight();
    updateLineNumberAreaWidth();
    highlightCurrentLine();
    gutter_->update();
}

void SourceEditor::refreshTheme() {
    for (int index = 0; index < tabs_->count(); ++index)
        if (auto* view = viewAt(index)) view->refreshTheme();
}

SourceView::SourceView(QWidget* parent) : QPlainTextEdit(parent) {
    setFont(Theme::instance().monoFont());
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    setFrameShape(QFrame::NoFrame);

    gutter_ = new LineNumberArea(this);
    highlighter_ = new SyntaxHighlighter(document());

    connect(this, &QPlainTextEdit::blockCountChanged, this, &SourceView::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &SourceView::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &SourceView::highlightCurrentLine);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &SourceView::describeCursor);
    connect(document(), &QTextDocument::modificationChanged, this, &SourceView::modifiedChanged);

    updateLineNumberAreaWidth();
    highlightCurrentLine();
}

int SourceView::lineNumberAreaWidth() const {
    int digits = 1;
    int lines = qMax(1, blockCount());
    while (lines >= 10) {
        lines /= 10;
        ++digits;
    }
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * qMax(3, digits);
}

void SourceView::updateLineNumberAreaWidth() {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void SourceView::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy)
        gutter_->scroll(0, dy);
    else
        gutter_->update(0, rect.y(), gutter_->width(), rect.height());
    if (rect.contains(viewport()->rect())) updateLineNumberAreaWidth();
}

void SourceView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    gutter_->setGeometry(QRect(area.left(), area.top(), lineNumberAreaWidth(), area.height()));
}

void SourceView::paintLineNumbers(QPaintEvent* event) {
    const auto& p = colours();
    QPainter painter(gutter_);
    painter.fillRect(event->rect(), p.surface);

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();

    painter.setFont(font());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(number == current ? p.text : p.textFaint);
            painter.drawText(0, top, gutter_->width() - 6, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void SourceView::highlightCurrentLine() {
    QList<QTextEdit::ExtraSelection> selections;
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(colours().surface);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        selections.append(selection);
    }
    setExtraSelections(selections);
}

void SourceView::describeCursor() {
    const auto cursor = textCursor();
    emit cursorPositionDescribed(QString("Ln %1, Col %2   %3   %4")
                                     .arg(cursor.blockNumber() + 1)
                                     .arg(cursor.positionInBlock() + 1)
                                     .arg(encoding_, languageName(highlighter_->language())));
}

bool SourceView::loadFile(const QString& path, QString* error) {
    return readFile(path, error, false);
}

bool SourceView::reloadIfChanged(QString* error) {
    if (error) error->clear();
    if (modified() || path_.isEmpty()) return false;
    return readFile(path_, error, true);
}

bool SourceView::readFile(const QString& path, QString* error, bool skipUnchanged) {
    auto text = fs::read_text(path_from_utf8(path.toStdString()), 16u * 1024 * 1024);
    if (!text) {
        if (error) *error = QString::fromStdString(text.error().message);
        return false;
    }
    if (text->truncated) {
        if (error)
            *error =
                tr("This file exceeds the editor's 16 MiB limit. Open it in an external "
                   "editor. The current document has not been changed.");
        return false;
    }

    const auto hash = QCryptographicHash::hash(
        QByteArrayView(text->content.data(), static_cast<qsizetype>(text->content.size())),
        QCryptographicHash::Sha256);
    const auto encoding = QString::fromStdString(std::string{fs::to_string(text->encoding)});
    const auto ending = QString::fromStdString(std::string{fs::to_string(text->line_ending)});
    // Compare content rather than trusting size/mtime: external tools can
    // replace a file with different bytes while preserving both metadata.
    if (skipUnchanged && hash == diskContentHash_ && encoding == encoding_ && ending == lineEnding_)
        return false;

    path_ = path;
    encoding_ = encoding;
    lineEnding_ = ending;

    setPlainText(QString::fromStdString(text->content));
    document()->setModified(false);
    highlighter_->setLanguage(languageForFile(path));
    diskContentHash_ = hash;
    describeCursor();
    return true;
}

bool SourceView::saveFile(QString* error) {
    if (path_.isEmpty()) {
        if (error) *error = tr("This buffer has no file.");
        return false;
    }

    // The file's original encoding and line endings are preserved, so a hand
    // edit does not rewrite every line of the diff.
    const auto encoding = encoding_ == "utf-8-bom"  ? fs::TextEncoding::Utf8Bom
                          : encoding_ == "utf-16le" ? fs::TextEncoding::Utf16Le
                          : encoding_ == "utf-16be" ? fs::TextEncoding::Utf16Be
                                                    : fs::TextEncoding::Utf8;
    const auto ending = lineEnding_ == "crlf" ? fs::LineEnding::CrLf : fs::LineEnding::Lf;

    const auto content = toPlainText().toStdString();
    auto status =
        fs::write_text_atomic(path_from_utf8(path_.toStdString()), content, encoding, ending);
    if (!status) {
        if (error) *error = QString::fromStdString(status.error().message);
        return false;
    }
    diskContentHash_ = QCryptographicHash::hash(
        QByteArrayView(content.data(), static_cast<qsizetype>(content.size())),
        QCryptographicHash::Sha256);
    encoding_ = QString::fromStdString(std::string{fs::to_string(encoding)});
    lineEnding_ = QString::fromStdString(std::string{
        fs::to_string(content.find('\n') == std::string::npos ? fs::LineEnding::None : ending)});
    document()->setModified(false);
    return true;
}

void SourceView::gotoLine(int line) {
    if (line <= 0) return;
    auto cursor = textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line - 1);
    setTextCursor(cursor);
    centerCursor();
}

void SourceView::findNext(const QString& text, bool backwards) {
    if (text.isEmpty()) return;
    QTextDocument::FindFlags flags;
    if (backwards) flags |= QTextDocument::FindBackward;
    if (!find(text, flags)) {
        // Wrap around rather than silently doing nothing.
        auto cursor = textCursor();
        cursor.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        setTextCursor(cursor);
        find(text, flags);
    }
}

// ---------------------------------------------------------------------------
SourceEditor::SourceEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QWidget(this);
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(6, 4, 6, 4);
    barLayout->setSpacing(8);

    search_ = new QLineEdit(bar);
    search_->setPlaceholderText(tr("Find in file   (Enter for next)"));
    search_->setClearButtonEnabled(true);
    barLayout->addWidget(search_, 1);

    status_ = new QLabel(bar);
    status_->setObjectName("Faint");
    barLayout->addWidget(status_);
    layout->addWidget(bar);

    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    layout->addWidget(tabs_, 1);

    connect(tabs_, &QTabWidget::tabCloseRequested, this, &SourceEditor::closeTab);
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        if (auto* view = currentView()) view->findNext(search_->text());
    });

    auto* save = new QShortcut(QKeySequence::Save, this);
    connect(save, &QShortcut::activated, this, [this] {
        QString error;
        if (saveCurrent(&error))
            emit statusMessage(tr("Saved"));
        else if (!error.isEmpty())
            emit statusMessage(tr("Save failed: %1").arg(error));
    });
}

SourceView* SourceEditor::viewAt(int index) const {
    return qobject_cast<SourceView*>(tabs_->widget(index));
}

SourceView* SourceEditor::currentView() const {
    return qobject_cast<SourceView*>(tabs_->currentWidget());
}

int SourceEditor::indexOfFile(const QString& path) const {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* view = viewAt(i); view && view->filePath() == path) return i;
    }
    return -1;
}

void SourceEditor::openFile(const QString& path, int line) {
    if (const int existing = indexOfFile(path); existing >= 0) {
        tabs_->setCurrentIndex(existing);
        if (line > 0) viewAt(existing)->gotoLine(line);
        return;
    }

    auto* view = new SourceView(this);
    QString error;
    if (!view->loadFile(path, &error)) {
        emit statusMessage(tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), error));
        delete view;
        return;
    }

    const int index = tabs_->addTab(view, QFileInfo(path).fileName());
    tabs_->setTabToolTip(index, path);
    tabs_->setCurrentIndex(index);
    if (line > 0) view->gotoLine(line);

    connect(view, &SourceView::modifiedChanged, this, &SourceEditor::updateTabTitle);
    connect(view, &SourceView::cursorPositionDescribed, status_, &QLabel::setText);
}

void SourceEditor::updateTabTitle() {
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* view = viewAt(i);
        if (!view) continue;
        const QString name = QFileInfo(view->filePath()).fileName();
        tabs_->setTabText(i, view->modified() ? name + " *" : name);
    }
}

bool SourceEditor::closeTab(int index) {
    auto* view = viewAt(index);
    if (view && view->modified()) {
        // Saving beats losing work; the user asked to close, not to discard.
        QString error;
        if (!view->saveFile(&error)) {
            QMessageBox::warning(this, tr("Save failed"), error);
            return false;
        }
    }
    tabs_->removeTab(index);
    if (view) view->deleteLater();
    return true;
}

bool SourceEditor::closeAll() {
    while (tabs_->count() > 0) {
        if (!closeTab(0)) return false;
    }
    return true;
}

bool SourceEditor::saveCurrent(QString* error) {
    auto* view = currentView();
    if (!view) return false;
    return view->saveFile(error);
}

bool SourceEditor::saveAll(QString* error) {
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* view = viewAt(i);
        if (view && view->modified() && !view->saveFile(error)) return false;
    }
    return true;
}

QStringList SourceEditor::openFiles() const {
    QStringList files;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* view = viewAt(i)) files << view->filePath();
    }
    return files;
}

bool SourceEditor::hasUnsavedChanges() const {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* view = viewAt(i); view && view->modified()) return true;
    }
    return false;
}

void SourceEditor::reloadUnmodified() {
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* view = viewAt(i);
        if (!view || view->modified()) continue;   // never discard a hand edit

        const int line = view->textCursor().blockNumber() + 1;
        const int scroll = view->verticalScrollBar()->value();
        QString error;
        if (view->reloadIfChanged(&error)) {
            view->gotoLine(line);
            view->verticalScrollBar()->setValue(scroll);
        }
    }
}

}   // namespace localcode::gui
