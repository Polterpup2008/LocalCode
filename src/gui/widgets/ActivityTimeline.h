#pragma once
// ---------------------------------------------------------------------------
// What the agent is doing, as it does it.
//
// This is the panel that makes an autonomous run legible: every tool call
// appears the moment it starts, updates when it finishes, and shows its result
// in one line. Clicking an entry opens the detail - the full command output, the
// file that was read, the diff that was applied.
// ---------------------------------------------------------------------------
#include <QListWidget>
#include <QString>
#include <QWidget>

class QLabel;

namespace localcode::gui {

struct TimelineEntry {
    enum class State { Running, Succeeded, Failed, Info, Warning };

    QString  title;
    QString  detail;
    QString  logPath;
    State    state = State::Running;
    qint64   durationMs = 0;
};

class ActivityTimeline : public QWidget {
    Q_OBJECT

public:
    explicit ActivityTimeline(QWidget* parent = nullptr);

    // Adds a running entry and returns its row, so it can be completed later.
    int beginEntry(const QString& title);
    void completeEntry(int row, bool succeeded, const QString& summary, const QString& detail,
                       qint64 durationMs, const QString& logPath = {});
    void addNote(const QString& text, TimelineEntry::State state = TimelineEntry::State::Info);

    void clear();
    [[nodiscard]] int entryCount() const;

signals:
    void entrySelected(const QString& title, const QString& detail, const QString& logPath);

private slots:
    void handleSelection();

private:
    void applyRow(int row, const TimelineEntry& entry);

    QListWidget*         list_ = nullptr;
    QLabel*              summary_ = nullptr;
    QList<TimelineEntry> entries_;
};

}  // namespace localcode::gui
