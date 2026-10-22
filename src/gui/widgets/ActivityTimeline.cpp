#include "ActivityTimeline.h"

#include <QLabel>
#include <QListWidgetItem>
#include <QScrollBar>
#include <QVBoxLayout>

#include "../Theme.h"

namespace localcode::gui {
namespace {

QString marker(TimelineEntry::State state) {
    switch (state) {
        case TimelineEntry::State::Running:   return QStringLiteral("→");   // ->
        case TimelineEntry::State::Succeeded: return QStringLiteral("✓");   // check
        case TimelineEntry::State::Failed:    return QStringLiteral("✗");   // cross
        case TimelineEntry::State::Warning:   return QStringLiteral("!");
        case TimelineEntry::State::Info:      return QStringLiteral("·");   // middle dot
    }
    return {};
}

QColor markerColour(TimelineEntry::State state) {
    const auto& p = colours();
    switch (state) {
        case TimelineEntry::State::Running:   return p.accent;
        case TimelineEntry::State::Succeeded: return p.success;
        case TimelineEntry::State::Failed:    return p.danger;
        case TimelineEntry::State::Warning:   return p.warning;
        case TimelineEntry::State::Info:      return p.textMuted;
    }
    return p.text;
}

}  // namespace

ActivityTimeline::ActivityTimeline(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // No title label: this panel lives in a tab already labelled "Activity", and
    // repeating it costs a row of height to say nothing.

    list_ = new QListWidget(this);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setWordWrap(false);
    list_->setTextElideMode(Qt::ElideMiddle);
    list_->setUniformItemSizes(false);
    layout->addWidget(list_, 1);

    summary_ = new QLabel(this);
    summary_->setObjectName("Faint");
    summary_->setContentsMargins(8, 4, 8, 6);
    layout->addWidget(summary_);

    connect(list_, &QListWidget::itemSelectionChanged, this, &ActivityTimeline::handleSelection);
}

int ActivityTimeline::beginEntry(const QString& title) {
    TimelineEntry entry;
    entry.title = title;
    entry.state = TimelineEntry::State::Running;
    entries_ << entry;

    auto* item = new QListWidgetItem(list_);
    item->setFont(Theme::instance().monoFont(9));
    list_->addItem(item);

    const int row = entries_.size() - 1;
    applyRow(row, entry);

    // Follow the newest entry unless the user has scrolled away to read.
    auto* bar = list_->verticalScrollBar();
    if (bar->value() >= bar->maximum() - 8) list_->scrollToBottom();

    summary_->setText(tr("%1 steps").arg(entries_.size()));
    return row;
}

void ActivityTimeline::completeEntry(int row, bool succeeded, const QString& summary,
                                     const QString& detail, qint64 durationMs,
                                     const QString& logPath) {
    if (row < 0 || row >= entries_.size()) return;

    auto& entry = entries_[row];
    entry.state = succeeded ? TimelineEntry::State::Succeeded : TimelineEntry::State::Failed;
    if (!summary.isEmpty()) entry.title = summary;
    entry.detail = detail;
    entry.durationMs = durationMs;
    entry.logPath = logPath;
    applyRow(row, entry);
}

void ActivityTimeline::addNote(const QString& text, TimelineEntry::State state) {
    TimelineEntry entry;
    entry.title = text;
    entry.state = state;
    entries_ << entry;

    auto* item = new QListWidgetItem(list_);
    item->setFont(Theme::instance().monoFont(9));
    list_->addItem(item);
    applyRow(entries_.size() - 1, entry);

    auto* bar = list_->verticalScrollBar();
    if (bar->value() >= bar->maximum() - 8) list_->scrollToBottom();
}

void ActivityTimeline::applyRow(int row, const TimelineEntry& entry) {
    auto* item = list_->item(row);
    if (!item) return;

    QString text = marker(entry.state) + "  " + entry.title;
    if (entry.durationMs > 1000) {
        text += QString("   %1s").arg(entry.durationMs / 1000.0, 0, 'f', 1);
    }
    item->setText(text);
    item->setForeground(entry.state == TimelineEntry::State::Succeeded ? colours().text
                                                                       : markerColour(entry.state));
    item->setToolTip(entry.detail.isEmpty() ? entry.title : entry.detail.left(2000));
}

void ActivityTimeline::handleSelection() {
    const int row = list_->currentRow();
    if (row < 0 || row >= entries_.size()) return;
    const auto& entry = entries_[row];
    emit entrySelected(entry.title, entry.detail, entry.logPath);
}

void ActivityTimeline::clear() {
    entries_.clear();
    list_->clear();
    summary_->clear();
}

int ActivityTimeline::entryCount() const { return entries_.size(); }

}  // namespace localcode::gui
