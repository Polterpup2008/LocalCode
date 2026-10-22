#pragma once
// ---------------------------------------------------------------------------
// Command output and the application's own logs.
//
// Command output is appended live while a build runs, which means it can arrive
// in a flood - so it is buffered and flushed on a timer, and the buffer is
// bounded so a runaway process cannot exhaust memory.
// ---------------------------------------------------------------------------
#include <QPlainTextEdit>
#include <QString>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

namespace localcode::gui {

class OutputPanel : public QWidget {
    Q_OBJECT

public:
    explicit OutputPanel(QWidget* parent = nullptr);

    void appendCommandOutput(const QString& text, bool isStderr);
    void beginCommand(const QString& command);
    void endCommand(int exitCode, qint64 durationMs);
    void clearCommandOutput();

    void appendLog(const QString& channel, const QString& level, const QString& message);
    void appendServerLog(const QString& text);
    void clearServerLog();

    void showCommandTab();
    void showServerTab();

private slots:
    void flush();

private:
    QTabWidget*     tabs_ = nullptr;
    QPlainTextEdit* command_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPlainTextEdit* server_ = nullptr;
    QTimer*         flushTimer_ = nullptr;
    QString         pendingCommand_;
    QString         pendingServer_;
};

}  // namespace localcode::gui
