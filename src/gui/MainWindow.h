#pragma once
// ---------------------------------------------------------------------------
// The main window.
//
// Three columns, which is the shape the work actually takes:
//
//   left    what the project contains
//   centre  the conversation, and the files and diffs under discussion
//   right   what the agent is doing and what it has changed
//
// Everything long-running - loading a model, scanning a repository, running the
// agent - happens off the GUI thread. This class only reacts to signals.
// ---------------------------------------------------------------------------
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <memory>

#include "AgentController.h"
#include "Application.h"

class QAction;
class QComboBox;
class QDialog;
class QLabel;
class QListWidget;
class QPushButton;
class QPlainTextEdit;
class QSplitter;
class QTabWidget;

namespace localcode::gui {

class ActivityTimeline;
class ChatView;
class DiffView;
class ModelPanel;
class OutputPanel;
class ProjectExplorer;
class PromptComposer;
class RepositoryPanel;
class SkillsPanel;
class SourceEditor;
class StatusStrip;
class TaskPanel;
class VerificationPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Application& application, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Restores workspace, conversation, open files and the unsent draft.
    void restoreSession();

    // Entry points for the command line.
    void openProjectFolder(const QString& path);
    void loadModelById(const QString& profileId);
    void showPlugins();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void openWorkspace();
    void createOutputWorkspace();
    void openRecentWorkspace();
    void newConversation();
    void switchConversation();
    void importModel();
    void loadModel(const QString& profileId);
    void unloadModel();
    void showSettings();
    void submitPrompt(const QString& text);
    void stopRun();

    // Agent events.
    void onRunStarted(const QString& message);
    void onPromptProcessing();
    void onContentDelta(const QString& text);
    void onReasoningDelta(const QString& text);
    void onToolStarted(const UiToolEvent& event);
    void onToolFinished(const UiToolEvent& event);
    void onToolOutput(const QString& chunk, bool isStderr);
    void onToolProgress(const QString& note);
    void onContextUpdated(const UiContext& context, const QString& note);
    void onGenerationStats(const UiTurnStats& stats);
    void onProgressWarning(const QString& guidance);
    void onFinalizationStarted(const QString& note, bool retry);
    void onTaskStateUpdated(const QString& block);
    void onRunFinished(const UiOutcome& outcome);
    // Reflects the model server's own state. The window never infers this.
    void onServerStateChanged(ServerState state, const QString& message, int exitCode);
    void onRunFailed(const QString& reason);
    void onRunCancelled();
    void onBusyChanged(bool busy);

    void refreshGitState();
    void openFileFromPath(const QString& path, int line = 0);
    void saveDraft(const QString& text);

private:
    void buildUi();
    void buildMenus();
    void buildToolBar();
    void wireAgent();
    void applyWorkspace(const QString& path);
    void updateModelSelector();
    void updateWindowTitle();
    void refreshConversations();
    void deleteConversation(std::int64_t id);
    void appendCurrentConversation();
    void showToolDetails(const QString& title, const QString& detail, const QString& logPath);
    void saveLayout();
    void restoreLayout();
    // Pulls a restored geometry back inside the visible desktop. A saved size
    // is a promise about a display that may no longer be there.
    void clampToAvailableScreen();
    void updatePanelVisibility();
    bool persistConversation();
    void reportStorageFailure(const QString& operation, const Error& error);
    void persistTerminalNote(const QString& text);
    void announce(const QString& text);
    void selectTaskToolScope(const QString& request);

    Application& application_;
    std::unique_ptr<AgentController> controller_;

    // Panels
    ProjectExplorer* explorer_ = nullptr;
    ChatView* chat_ = nullptr;
    PromptComposer* composer_ = nullptr;
    QComboBox* requestMode_ = nullptr;
    ActivityTimeline* timeline_ = nullptr;
    TaskPanel* task_ = nullptr;
    VerificationPanel* verification_ = nullptr;
    SourceEditor* editor_ = nullptr;
    DiffView* diff_ = nullptr;
    OutputPanel* output_ = nullptr;
    SkillsPanel* skills_ = nullptr;
    RepositoryPanel* repository_ = nullptr;
    ModelPanel* models_ = nullptr;
    StatusStrip* status_ = nullptr;

    QSplitter* outerSplitter_ = nullptr;
    QSplitter* centreSplitter_ = nullptr;
    QSplitter* rightSplitter_ = nullptr;
    QTabWidget* centreTabs_ = nullptr;
    QTabWidget* rightTabs_ = nullptr;
    QTabWidget* projectTabs_ = nullptr;
    QListWidget* conversations_ = nullptr;
    QLabel* taskSummary_ = nullptr;
    QDialog* toolDetails_ = nullptr;
    QPlainTextEdit* toolDetailText_ = nullptr;
    QPushButton* toolLogButton_ = nullptr;
    QString toolDetailLogPath_;

    QComboBox* modelSelector_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLabel* workspaceLabel_ = nullptr;

    QAction* openWorkspaceAction_ = nullptr;
    QAction* outputWorkspaceAction_ = nullptr;
    QAction* newConversationAction_ = nullptr;
    QAction* projectPanelAction_ = nullptr;
    QAction* inspectorPanelAction_ = nullptr;
    QAction* outputPanelAction_ = nullptr;

    QTimer* draftTimer_ = nullptr;
    QTimer* gitTimer_ = nullptr;
    QTimer* closeWaitTimer_ = nullptr;
    bool closePending_ = false;

    // Held so a model load in flight can be cancelled at teardown rather than
    // waited out. Without it, closing during a load blocks for as long as the
    // load would have taken.
    std::shared_ptr<CancellationSource> modelCancel_;

    QString pendingDraft_;
    bool layoutApplied_ = false;
    int runningToolRow_ = -1;
    bool suppressModelSelector_ = false;
    bool switchingModel_ = false;
    bool modelUnloading_ = false;
    bool pluginMode_ = false;
    QByteArray codingOuterState_, codingCentreState_;
    std::uint64_t gitRefreshGeneration_ = 0;
    bool gitRefreshInFlight_ = false;
    bool gitRefreshPending_ = false;
    CancellationSource gitRefreshCancel_;
    bool fileToolsOnlyTask_ = false;
};

}   // namespace localcode::gui
