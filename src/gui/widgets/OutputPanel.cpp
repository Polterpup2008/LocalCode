#include "OutputPanel.h"

#include <QScrollBar>
#include <QVBoxLayout>

#include "../Theme.h"

namespace localcode::gui {
namespace {

// A build can emit tens of thousands of lines. Keeping the view bounded costs
// nothing that matters - the complete log is on disk - and keeps the widget
// responsive.
constexpr int kMaxBlocks = 8000;
constexpr int kFlushIntervalMs = 80;

QPlainTextEdit* makeView(QWidget* parent) {
    auto* view = new QPlainTextEdit(parent);
    view->setReadOnly(true);
    view->setFrameShape(QFrame::NoFrame);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setMaximumBlockCount(kMaxBlocks);
    view->setFont(Theme::instance().monoFont(9));
    return view;
}

void appendKeepingScroll(QPlainTextEdit* view, const QString& text) {
    if (text.isEmpty()) return;
    auto* bar = view->verticalScrollBar();
    const bool following = bar->value() >= bar->maximum() - 4;

    auto cursor = view->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);

    if (following) bar->setValue(bar->maximum());
}

}   // namespace

OutputPanel::OutputPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);

    command_ = makeView(tabs_);
    log_ = makeView(tabs_);
    server_ = makeView(tabs_);

    tabs_->addTab(command_, tr("Command output"));
    tabs_->addTab(server_, tr("Model server"));
    tabs_->addTab(log_, tr("Application log"));
    layout->addWidget(tabs_);

    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(kFlushIntervalMs);
    flushTimer_->setSingleShot(true);
    connect(flushTimer_, &QTimer::timeout, this, &OutputPanel::flush);
}

void OutputPanel::appendCommandOutput(const QString& text, bool isStderr) {
    Q_UNUSED(isStderr);
    pendingCommand_ += text;
    // Bound the buffer even between flushes, in case a process floods faster
    // than the timer fires.
    if (pendingCommand_.size() > 512 * 1024) {
        pendingCommand_ = pendingCommand_.right(256 * 1024);
    }
    if (!pendingCommand_.isEmpty() && !flushTimer_->isActive()) flushTimer_->start();
}

void OutputPanel::appendServerLog(const QString& text) {
    pendingServer_ += text;
    if (!pendingServer_.endsWith('\n')) pendingServer_ += '\n';
    if (pendingServer_.size() > 256 * 1024) {
        pendingServer_ = pendingServer_.right(128 * 1024);
    }
    if (!pendingServer_.isEmpty() && !flushTimer_->isActive()) flushTimer_->start();
}

void OutputPanel::flush() {
    flushTimer_->stop();
    if (!pendingCommand_.isEmpty()) {
        appendKeepingScroll(command_, pendingCommand_);
        pendingCommand_.clear();
    }
    if (!pendingServer_.isEmpty()) {
        appendKeepingScroll(server_, pendingServer_);
        pendingServer_.clear();
    }
}

void OutputPanel::beginCommand(const QString& command) {
    appendKeepingScroll(command_, QString("\n$ %1\n").arg(command));
    tabs_->setCurrentWidget(command_);
}

void OutputPanel::endCommand(int exitCode, qint64 durationMs) {
    flush();
    appendKeepingScroll(command_, QString("[exit %1 in %2 ms]\n").arg(exitCode).arg(durationMs));
}

void OutputPanel::clearCommandOutput() {
    pendingCommand_.clear();
    if (pendingServer_.isEmpty()) flushTimer_->stop();
    command_->clear();
}

void OutputPanel::clearServerLog() {
    pendingServer_.clear();
    if (pendingCommand_.isEmpty()) flushTimer_->stop();
    server_->clear();
}

void OutputPanel::appendLog(const QString& channel, const QString& level, const QString& message) {
    appendKeepingScroll(
        log_,
        QString("%1  %2  %3\n").arg(level.leftJustified(5), channel.leftJustified(9), message));
}

void OutputPanel::showCommandTab() {
    tabs_->setCurrentWidget(command_);
}

void OutputPanel::showServerTab() {
    tabs_->setCurrentWidget(server_);
}

}   // namespace localcode::gui
