#pragma once
// ---------------------------------------------------------------------------
// What LocalCode knows about the open project.
//
// The index is the agent's, not the user's - it exists so the model can ask
// where a symbol is instead of grepping for it. This panel is the window onto
// it, and it earns its place by answering one question: is what the agent is
// working from actually current?
//
// So state and recency are the headline, not the file count. A stale index that
// looks healthy is worse than no panel at all, because the agent is confidently
// working from something that has moved.
//
// The scan runs on its own thread in Application. Progress arrives from that
// thread and is re-emitted as a queued signal, so nothing here touches a core
// object across a thread boundary and the window keeps painting while a large
// repository is read.
// ---------------------------------------------------------------------------
#include <QString>
#include <QWidget>

#include "Application.h"
#include "repo/RepoIndex.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTextBrowser;

namespace localcode::gui {

class RepositoryPanel : public QWidget {
    Q_OBJECT

public:
    explicit RepositoryPanel(Application& application, QWidget* parent = nullptr);
    ~RepositoryPanel() override;

    // Reads the stored overview and redraws. Cheap: it does not scan.
    void refreshDisplay();

    // Called when a project is opened, so the panel starts from that project's
    // index rather than the previous one's.
    void workspaceChanged();

signals:
    // Progress crosses from the indexing thread on this signal and nowhere
    // else.
    void progressArrived(int state, int filesSeen, int filesIndexed, const QString& path);
    void fileActivated(const QString& path, int line);

private slots:
    void onProgress(int state, int filesSeen, int filesIndexed, const QString& path);
    void startRefresh();
    void startFullReindex();
    void cancelRefresh();
    void searchIndex();

private:
    void setBusy(bool busy);
    void setStateLabel(const QString& text);
    void renderOverview();
    [[nodiscard]] QString describeState() const;

    Application& application_;
    QLabel* state_ = nullptr;
    QLabel* counts_ = nullptr;
    QLabel* activity_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* full_ = nullptr;
    QPushButton* cancel_ = nullptr;
    bool busy_ = false;
};

}   // namespace localcode::gui
