#pragma once
// ---------------------------------------------------------------------------
// The project tree.
//
// Backed by QFileSystemModel, which loads lazily and watches for changes, so
// opening a repository with a hundred thousand files does not stall the
// interface: only the directories the user actually expands are ever read.
// ---------------------------------------------------------------------------
#include <QFileSystemModel>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QTreeView>
#include <QWidget>

class QLineEdit;
class QLabel;

namespace localcode::gui {

// Hides the directories that are noise in a source tree, and applies the
// filter box.
class ExplorerFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ExplorerFilterModel(QObject* parent = nullptr);

    void setIgnoredDirectories(QStringList names);
    void setFilterText(const QString& text);
    void setShowHidden(bool show);
    void setWorkspaceRoot(const QString& path);

    // Drops the shell's file-type icons and dims files against directories.
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    QStringList ignored_;
    QString filter_;
    bool showHidden_ = false;
    QString workspaceRoot_;
};

class ProjectExplorer : public QWidget {
    Q_OBJECT

public:
    explicit ProjectExplorer(QWidget* parent = nullptr);

    void setRoot(const QString& path);

    [[nodiscard]] QString root() const { return root_; }

    void refresh();

    void setShowHidden(bool show);

signals:
    void fileActivated(const QString& absolutePath);
    void revealRequested(const QString& absolutePath);

private slots:
    void handleActivated(const QModelIndex& index);
    void showContextMenu(const QPoint& point);

private:
    [[nodiscard]] QString pathAt(const QModelIndex& index) const;

    QFileSystemModel* model_ = nullptr;
    ExplorerFilterModel* proxy_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QString root_;
};

}   // namespace localcode::gui
