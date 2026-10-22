#include "RepositoryPanel.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include "../Theme.h"
#include "repo/RepoQuery.h"
#include "repo/RepoTypes.h"

namespace localcode::gui {
namespace {

QString themedHtml(QString html) {
    return html.replace("#9aa0a6", colours().textMuted.name())
        .replace("#8bc34a", colours().success.name())
        .replace("#e6a23c", colours().warning.name())
        .replace("#e05252", colours().danger.name());
}

QString toQt(const std::string& text) {
    return QString::fromStdString(text);
}

// A list, shortened, with the shortening admitted. A panel that silently shows
// the first six of forty source roots is describing a different project.
QString joined(const std::vector<std::string>& values, int limit = 8) {
    if (values.empty()) return {};
    QStringList parts;
    for (int i = 0; i < static_cast<int>(values.size()) && i < limit; ++i) {
        parts << toQt(values[static_cast<std::size_t>(i)]).toHtmlEscaped();
    }
    QString out = parts.join(", ");
    if (static_cast<int>(values.size()) > limit) {
        out += QStringLiteral(" <i>and %1 more</i>").arg(static_cast<int>(values.size()) - limit);
    }
    return out;
}

QString row(const QString& label, const QString& value) {
    if (value.isEmpty()) return {};
    return QStringLiteral(
               "<tr><td style='padding-right:12px; color:#9aa0a6;'>%1</td>"
               "<td>%2</td></tr>")
        .arg(label, value);
}

// How long ago, in the units a person would use. "3 minutes ago" answers the
// question the panel exists for; a timestamp makes the reader do the work.
QString howLongAgo(std::int64_t unix_ms) {
    if (unix_ms <= 0) return QObject::tr("never");
    const auto when = QDateTime::fromMSecsSinceEpoch(unix_ms);
    const auto seconds = when.secsTo(QDateTime::currentDateTime());
    if (seconds < 0) return when.toString(QStringLiteral("d MMM HH:mm"));
    if (seconds < 60) return QObject::tr("just now");
    if (seconds < 3600) return QObject::tr("%n minute(s) ago", "", static_cast<int>(seconds / 60));
    if (seconds < 86400) return QObject::tr("%n hour(s) ago", "", static_cast<int>(seconds / 3600));
    return when.toString(QStringLiteral("d MMM HH:mm"));
}

}   // namespace

RepositoryPanel::RepositoryPanel(Application& application, QWidget* parent)
    : QWidget(parent), application_(application) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    state_ = new QLabel(this);
    state_->setTextFormat(Qt::RichText);
    layout->addWidget(state_);

    counts_ = new QLabel(this);
    counts_->setObjectName("Muted");
    counts_->setWordWrap(true);
    layout->addWidget(counts_);

    progress_ = new QProgressBar(this);
    progress_->setTextVisible(false);
    progress_->setVisible(false);
    layout->addWidget(progress_);

    activity_ = new QLabel(this);
    activity_->setObjectName("Muted");
    activity_->setVisible(false);
    // A long path must not be allowed to widen the dock.
    activity_->setTextInteractionFlags(Qt::NoTextInteraction);
    layout->addWidget(activity_);

    search_ = new QLineEdit(this);
    search_->setObjectName("RepositorySearch");
    search_->setPlaceholderText(tr("Find a file or symbol — Enter"));
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::returnPressed, this, &RepositoryPanel::searchIndex);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (text.isEmpty()) renderOverview();
    });
    layout->addWidget(search_);
    detail_ = new QTextBrowser(this);
    detail_->setOpenExternalLinks(false);
    detail_->setOpenLinks(false);
    connect(detail_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == "localcode") emit fileActivated(url.path(), url.fragment().toInt());
    });
    detail_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(detail_, 1);

    auto* buttons = new QHBoxLayout();
    refresh_ = new QPushButton(tr("Refresh"), this);
    refresh_->setToolTip(tr("Re-read the files that have changed since the last pass."));
    full_ = new QPushButton(tr("Reindex all"), this);
    full_->setToolTip(tr("Read and reparse every file, whatever its fingerprint says."));
    cancel_ = new QPushButton(tr("Stop"), this);
    cancel_->setVisible(false);
    buttons->addWidget(refresh_);
    buttons->addWidget(full_);
    buttons->addWidget(cancel_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(refresh_, &QPushButton::clicked, this, &RepositoryPanel::startRefresh);
    connect(full_, &QPushButton::clicked, this, &RepositoryPanel::startFullReindex);
    connect(cancel_, &QPushButton::clicked, this, &RepositoryPanel::cancelRefresh);

    // The queued hop. Progress is produced on the indexing thread; this is the
    // only place it crosses, and it carries plain types so moc needs nothing
    // registered.
    connect(this, &RepositoryPanel::progressArrived, this, &RepositoryPanel::onProgress,
            Qt::QueuedConnection);

    application_.set_repository_index_callback([this](const repo::IndexProgress& progress) {
        emit progressArrived(static_cast<int>(progress.state),
                             static_cast<int>(progress.files_seen),
                             static_cast<int>(progress.files_indexed), toQt(progress.current_path));
    });

    refreshDisplay();
}

RepositoryPanel::~RepositoryPanel() {
    // Order matters, and clearing the callback alone is not enough.
    //
    // refresh_repository_index() takes a copy of the callback and moves it into
    // the worker, so a scan already running holds its own and goes on calling
    // it whatever this object does to the one Application keeps. The callback
    // closes over `this`. Cancelling joins the thread, which is what actually
    // guarantees no call is in flight - and it happens here, in the derived
    // destructor's body, while this is still a RepositoryPanel and an emit from
    // the worker's last progress report is still safe. Qt drops the queued
    // event when the object goes.
    //
    // Clearing afterwards stops a later refresh from finding a stale callback.
    application_.cancel_repository_index();
    application_.set_repository_index_callback({});
}

void RepositoryPanel::workspaceChanged() {
    search_->clear();
    setBusy(false);
    refreshDisplay();
}

QString RepositoryPanel::describeState() const {
    if (!application_.is_initialised()) return tr("not ready");
    const auto& index = application_.repository();
    if (!index.is_open()) return tr("unavailable");
    switch (index.state()) {
        case repo::IndexState::Empty: return tr("not indexed");
        case repo::IndexState::Indexing: return tr("indexing");
        case repo::IndexState::Ready: return tr("ready");
        case repo::IndexState::Stale: return tr("stale");
        case repo::IndexState::Failed: return tr("failed");
    }
    return tr("unknown");
}

void RepositoryPanel::setStateLabel(const QString& text) {
    // Colour carries the one thing that matters at a glance: whether what the
    // agent is working from is current.
    QString colour = colours().textMuted.name();
    if (text == tr("ready")) colour = colours().success.name();
    if (text == tr("stale") || text == tr("not indexed")) colour = colours().warning.name();
    if (text == tr("failed") || text == tr("unavailable")) colour = colours().danger.name();

    state_->setText(QStringLiteral("<b>%1</b> &nbsp; <span style='color:%2;'>%3</span>")
                        .arg(tr("Repository index"), colour, text.toHtmlEscaped()));
}

void RepositoryPanel::refreshDisplay() {
    setStateLabel(describeState());
    if (search_->text().trimmed().isEmpty())
        renderOverview();
    else
        searchIndex();
}

void RepositoryPanel::searchIndex() {
    const auto needle = search_->text().trimmed().toStdString();
    if (needle.empty()) {
        renderOverview();
        return;
    }
    if (!application_.repository().is_open()) return;
    repo::RepoQuery query(application_.repository().store(),
                          application_.repository().workspace_root());
    const auto files = query.find_files(needle, 12);
    const auto symbols = query.find_symbol(needle, 12);
    QString html =
        "<p>" + tr("Index matches — open source to check a match.").toHtmlEscaped() + "</p>";
    auto link = [&](const std::string& file, std::size_t line, const QString& name) {
        QUrl url;
        url.setScheme("localcode");
        url.setPath(QString::fromStdString(file));
        url.setFragment(QString::number(line));
        html += QString("<p><a href=\"%1\">%2</a><br><span style=\"color:%3\">%4:%5</span></p>")
                    .arg(url.toString(QUrl::FullyEncoded).toHtmlEscaped(), name.toHtmlEscaped(),
                         colours().textMuted.name(), QString::fromStdString(file).toHtmlEscaped())
                    .arg(line);
    };
    if (files)
        for (const auto& hit : *files)
            link(hit.file, hit.line, QString::fromStdString(hit.file));
    if (symbols)
        for (const auto& symbol : *symbols)
            link(symbol.file, symbol.line, QString::fromStdString(symbol.name));
    if (!files || !symbols)
        html += "<p>" + tr("The index could not be queried. Refresh it and try again.") + "</p>";
    else if (files->empty() && symbols->empty())
        html += "<p>" + tr("No matches in the current index.") + "</p>";
    else
        html += "<p>" + tr("Up to 12 file matches and 12 symbol matches are shown.") + "</p>";
    detail_->setHtml(themedHtml(html));
}

void RepositoryPanel::renderOverview() {
    if (!application_.is_initialised() || !application_.repository().is_open()) {
        counts_->clear();
        detail_->setHtml(themedHtml(QStringLiteral("<p style='color:#9aa0a6;'>%1</p>")
                                        .arg(tr("Open a project to build an index of it."))));
        return;
    }

    auto overview = application_.repository().overview();
    if (!overview || !overview->has_value()) {
        counts_->clear();
        detail_->setHtml(
            themedHtml(QStringLiteral("<p style='color:#9aa0a6;'>%1</p>")
                           .arg(tr("Nothing has been indexed yet. Until it is, the agent reads and "
                                   "searches files directly."))));
        return;
    }
    const auto& map = **overview;

    counts_->setText(tr("%1 files · %2 lines · %3 symbols · indexed %4")
                         .arg(map.indexed_files)
                         .arg(map.total_lines)
                         .arg(map.symbol_count)
                         .arg(howLongAgo(map.indexed_at_unix_ms)));

    QString languages;
    for (std::size_t i = 0; i < map.languages.size() && i < 6; ++i) {
        if (i != 0) languages += QStringLiteral(", ");
        languages += QStringLiteral("%1 <span style='color:#9aa0a6;'>(%2)</span>")
                         .arg(toQt(map.languages[i].language).toHtmlEscaped())
                         .arg(map.languages[i].files);
    }

    QString html = QStringLiteral("<table cellspacing='0' cellpadding='2'>");
    html += row(tr("Languages"), languages);
    html += row(tr("Build"), joined(map.build_systems));
    html += row(tr("Packages"), joined(map.package_managers));
    html += row(tr("Source"), joined(map.source_roots));
    html += row(tr("Tests"), joined(map.test_roots));
    html += row(tr("Entry points"), joined(map.entry_points));
    html += QStringLiteral("</table>");

    if (map.skipped_files != 0) {
        html += QStringLiteral("<p style='color:#9aa0a6;'>%1</p>")
                    .arg(tr("%n file(s) skipped as binary or oversized.", "",
                            static_cast<int>(map.skipped_files)));
    }
    if (map.truncated) {
        html += QStringLiteral("<p style='color:#e6a23c;'>%1</p>")
                    .arg(tr("The scan hit its limit, so these counts are a floor, not a total."));
    }

    if (auto commands = application_.repository().store().commands();
        commands && !commands->empty()) {
        html += QStringLiteral(
                    "<p style='margin-top:10px;'><b>%1</b></p><table cellspacing='0' "
                    "cellpadding='2'>")
                    .arg(tr("Build and test commands"));
        for (const auto& command : *commands) {
            // The distinction the whole design rests on: a command LocalCode has
            // run and watched succeed, against one the project's files merely
            // imply. Showing them alike would make a guess look like a fact.
            const QString mark =
                command.verified
                    ? QStringLiteral("<span style='color:#8bc34a;'>%1</span>").arg(tr("verified"))
                    : QStringLiteral("<span style='color:#9aa0a6;'>%1</span>").arg(tr("untried"));
            html += QStringLiteral(
                        "<tr><td style='padding-right:10px; color:#9aa0a6;'>%1</td>"
                        "<td><code>%2</code><br/><span style='color:#9aa0a6;'>%3 · "
                        "from %4</span></td></tr>")
                        .arg(toQt(command.purpose).toHtmlEscaped(),
                             toQt(command.command).toHtmlEscaped(), mark,
                             toQt(command.source).toHtmlEscaped());
        }
        html += QStringLiteral("</table>");
    }

    detail_->setHtml(themedHtml(html));
}

void RepositoryPanel::setBusy(bool busy) {
    busy_ = busy;
    progress_->setVisible(busy);
    activity_->setVisible(busy);
    cancel_->setVisible(busy);
    refresh_->setEnabled(!busy);
    full_->setEnabled(!busy);
    if (!busy) {
        activity_->clear();
        progress_->setRange(0, 1);
        progress_->setValue(0);
    }
}

void RepositoryPanel::onProgress(int state, int filesSeen, int filesIndexed, const QString& path) {
    const auto indexState = static_cast<repo::IndexState>(state);
    if (indexState == repo::IndexState::Indexing) {
        if (!busy_) setBusy(true);
        // Say so in the headline, not only in the progress bar. Leaving it on
        // the previous pass's green "ready" while a scan runs is the precise
        // thing this panel exists to prevent: an index that looks current while
        // it is being rebuilt underneath the agent. The counts and the overview
        // are deliberately left alone - re-reading them per file would hit the
        // store on every tick while the worker is writing to it.
        setStateLabel(tr("indexing"));
        // The total is not known until the walk finishes, so this is a busy
        // indicator with a count beside it rather than a percentage. A made-up
        // percentage that jumps backwards is worse than none.
        progress_->setRange(0, 0);
        activity_->setText(tr("%1 seen, %2 indexed — %3")
                               .arg(filesSeen)
                               .arg(filesIndexed)
                               .arg(path.isEmpty() ? tr("scanning") : path));
        // Keep the label from stretching the dock on a deep path.
        activity_->setText(activity_->fontMetrics().elidedText(activity_->text(), Qt::ElideMiddle,
                                                               std::max(120, activity_->width())));
        return;
    }

    setBusy(false);
    refreshDisplay();
}

void RepositoryPanel::startRefresh() {
    if (!application_.is_initialised()) return;
    setBusy(true);
    application_.refresh_repository_index(false);
}

void RepositoryPanel::startFullReindex() {
    if (!application_.is_initialised()) return;
    setBusy(true);
    application_.refresh_repository_index(true);
}

void RepositoryPanel::cancelRefresh() {
    application_.cancel_repository_index();
    setBusy(false);
    refreshDisplay();
}

}   // namespace localcode::gui
