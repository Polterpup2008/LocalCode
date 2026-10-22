#include "ProjectExplorer.h"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QUrl>
#include <QVBoxLayout>

#include "../Theme.h"
#include "platform/FileSystem.h"

namespace localcode::gui {

ExplorerFilterModel::ExplorerFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setRecursiveFilteringEnabled(false);
    setDynamicSortFilter(true);
}

void ExplorerFilterModel::setIgnoredDirectories(QStringList names) {
    ignored_ = std::move(names);
    invalidateFilter();
}

void ExplorerFilterModel::setFilterText(const QString& text) {
    filter_ = text.trimmed();
    invalidateFilter();
}

void ExplorerFilterModel::setShowHidden(bool show) {
    showHidden_ = show;
    invalidateFilter();
}

void ExplorerFilterModel::setWorkspaceRoot(const QString& path) {
    workspaceRoot_ = path.isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(path));
    invalidateFilter();
}

bool ExplorerFilterModel::filterAcceptsRow(int row, const QModelIndex& parent) const {
    const auto* source = qobject_cast<QFileSystemModel*>(sourceModel());
    if (!source) return true;

    const auto index = source->index(row, 0, parent);
    if (!index.isValid()) return true;

    const QString name = source->fileName(index);
    const bool is_directory = source->isDir(index);

    // An explicitly opened project can live beneath an ignored directory.
    // Keep its ancestors visible to the proxy so mapping the root succeeds.
    const auto path = QDir::cleanPath(QDir::fromNativeSeparators(source->filePath(index)));
    if (is_directory && !workspaceRoot_.isEmpty() &&
        (workspaceRoot_.compare(path, Qt::CaseInsensitive) == 0 ||
         workspaceRoot_.startsWith(path.endsWith('/') ? path : path + '/', Qt::CaseInsensitive)))
        return true;

    if (!showHidden_ && name.startsWith('.') && name != "..") {
        // .github and .vscode are configuration a developer often wants to see;
        // the rest of the dot entries are noise by default.
        if (name != ".github" && name != ".gitignore" && name != ".editorconfig") return false;
    }
    if (is_directory) {
        for (const auto& ignored : ignored_) {
            if (name.compare(ignored, Qt::CaseInsensitive) == 0) return false;
        }
        // A directory is kept when filtering: its children are filtered instead,
        // so the tree structure stays navigable.
        return true;
    }
    if (!filter_.isEmpty() && !name.contains(filter_, Qt::CaseInsensitive)) return false;
    return true;
}

QVariant ExplorerFilterModel::data(const QModelIndex& index, int role) const {
    // QFileSystemModel hands out the Windows shell icons - the yellow folders and
    // per-extension badges from Explorer. They carry no information a developer
    // needs (the name already says what the file is) and they are the loudest
    // thing on screen. The tree reads as a tree without them.
    if (role == Qt::DecorationRole) return {};

    if (role == Qt::ForegroundRole) {
        const auto* source = qobject_cast<QFileSystemModel*>(sourceModel());
        if (source) {
            // Structure over contents: directories carry the full text colour,
            // files sit one step back, so the shape of the project is legible at
            // a glance without any icons.
            const bool is_directory = source->isDir(mapToSource(index));
            return QBrush(is_directory ? colours().text : colours().textMuted);
        }
    }
    return QSortFilterProxyModel::data(index, role);
}

bool ExplorerFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    const auto* source = qobject_cast<QFileSystemModel*>(sourceModel());
    if (!source) return QSortFilterProxyModel::lessThan(left, right);

    const bool left_dir = source->isDir(left);
    const bool right_dir = source->isDir(right);
    if (left_dir != right_dir) return left_dir;   // directories first
    return source->fileName(left).compare(source->fileName(right), Qt::CaseInsensitive) < 0;
}

// ---------------------------------------------------------------------------
ProjectExplorer::ProjectExplorer(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* title = new QLabel(tr("PROJECT"), this);
    title->setObjectName("PanelTitle");
    layout->addWidget(title);

    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter files"));
    filter_->setClearButtonEnabled(true);
    filter_->setContentsMargins(6, 0, 6, 4);
    layout->addWidget(filter_);

    model_ = new QFileSystemModel(this);
    model_->setReadOnly(true);
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);

    proxy_ = new ExplorerFilterModel(this);
    proxy_->setSourceModel(model_);

    QStringList ignored;
    for (const auto& name : fs::default_ignored_directories()) {
        ignored << QString::fromStdString(name);
    }
    proxy_->setIgnoredDirectories(ignored);

    tree_ = new QTreeView(this);
    tree_->setModel(proxy_);
    tree_->setHeaderHidden(true);
    tree_->setAnimated(false);
    tree_->setIndentation(14);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setUniformRowHeights(true);
    for (int column = 1; column < model_->columnCount(); ++column)
        tree_->hideColumn(column);
    layout->addWidget(tree_, 1);
    tree_->hide();

    connect(model_, &QFileSystemModel::directoryLoaded, this, [this](const QString& path) {
        if (QDir::cleanPath(QDir::fromNativeSeparators(path)).compare(root_, Qt::CaseInsensitive) !=
            0)
            return;
        const auto mapped = proxy_->mapFromSource(model_->index(root_));
        tree_->setRootIndex(mapped);
        tree_->setVisible(mapped.isValid());
    });

    connect(filter_, &QLineEdit::textChanged, proxy_, &ExplorerFilterModel::setFilterText);
    connect(tree_, &QTreeView::activated, this, &ProjectExplorer::handleActivated);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &ProjectExplorer::showContextMenu);
}

void ProjectExplorer::setRoot(const QString& path) {
    root_ = path.isEmpty() ? QString()
                           : QDir::cleanPath(QDir::fromNativeSeparators(QDir(path).absolutePath()));
    proxy_->setWorkspaceRoot(root_);
    if (root_.isEmpty() || !QDir(root_).exists()) {
        tree_->hide();
        tree_->setRootIndex({});
        return;
    }
    const auto source = model_->setRootPath(root_);
    const auto mapped = proxy_->mapFromSource(source);
    tree_->setRootIndex(mapped);
    tree_->setVisible(mapped.isValid());
}

void ProjectExplorer::refresh() {
    const QString path = root_;
    model_->setRootPath(QString());
    setRoot(path);
}

void ProjectExplorer::setShowHidden(bool show) {
    proxy_->setShowHidden(show);
}

QString ProjectExplorer::pathAt(const QModelIndex& index) const {
    if (!index.isValid()) return {};
    return model_->filePath(proxy_->mapToSource(index));
}

void ProjectExplorer::handleActivated(const QModelIndex& index) {
    const auto source = proxy_->mapToSource(index);
    if (model_->isDir(source)) return;
    emit fileActivated(model_->filePath(source));
}

void ProjectExplorer::showContextMenu(const QPoint& point) {
    const auto index = tree_->indexAt(point);
    const QString path = pathAt(index);
    if (path.isEmpty()) return;

    QMenu menu(this);
    const auto source = proxy_->mapToSource(index);
    const bool is_directory = model_->isDir(source);

    if (!is_directory) {
        menu.addAction(tr("Open"), [this, path] { emit fileActivated(path); });
    }
    menu.addAction(tr("Copy path"), [path] { QApplication::clipboard()->setText(path); });
    menu.addAction(tr("Copy relative path"), [this, path] {
        QApplication::clipboard()->setText(QDir(root_).relativeFilePath(path));
    });
    menu.addSeparator();
    menu.addAction(tr("Reveal in file manager"), [this, path] { emit revealRequested(path); });
    menu.addSeparator();
    menu.addAction(tr("Refresh"), this, &ProjectExplorer::refresh);

    menu.exec(tree_->viewport()->mapToGlobal(point));
}

}   // namespace localcode::gui
