#pragma once
// ---------------------------------------------------------------------------
// Reviewing what changed.
//
// The panel a user should look at before accepting an agent's work. It shows the
// changed files, marks which ones the agent touched during this run, warns when
// one of them already had uncommitted edits, and renders the diff either
// unified or side by side.
// ---------------------------------------------------------------------------
#include <QListWidget>
#include <QSplitter>
#include <QString>
#include <QTextBrowser>
#include <QWidget>

#include "git/GitService.h"

class QComboBox;
class QLabel;

namespace localcode::gui {

class DiffView : public QWidget {
    Q_OBJECT

public:
    explicit DiffView(QWidget* parent = nullptr);

    // Replaces the contents with a parsed diff.
    void setDiff(const DiffResult& diff);
    // Marks which paths the agent changed in this run, and which of those were
    // already modified beforehand.
    void setAgentChanges(const QStringList& changed, const QStringList& conflicting);
    void clear();

    [[nodiscard]] bool sideBySide() const;

signals:
    void fileSelected(const QString& path);
    void openFileRequested(const QString& path, int line);
    void refreshRequested();

private slots:
    void showSelectedFile();
    void modeChanged();

private:
    [[nodiscard]] QString renderUnified(const FileDiff& file) const;
    [[nodiscard]] QString renderSideBySide(const FileDiff& file) const;
    void populateList();

    QListWidget*  files_ = nullptr;
    QTextBrowser* content_ = nullptr;
    QComboBox*    mode_ = nullptr;
    QLabel*       summary_ = nullptr;
    QSplitter*    splitter_ = nullptr;

    DiffResult  diff_;
    QStringList agentChanged_;
    QStringList conflicting_;
};

}  // namespace localcode::gui
