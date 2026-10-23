#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include "ScreenGuard.h"
#include "Theme.h"
#include "dialogs/ModelImportDialog.h"
#include "dialogs/SettingsDialog.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "security/TaskPolicy.h"
#include "util/Log.h"
#include "widgets/ActivityTimeline.h"
#include "widgets/ChatView.h"
#include "widgets/DiffView.h"
#include "widgets/ModelPanel.h"
#include "widgets/OutputPanel.h"
#include "widgets/PluginsPanel.h"
#include "widgets/ProjectExplorer.h"
#include "widgets/PromptComposer.h"
#include "widgets/RepositoryPanel.h"
#include "widgets/SkillsPanel.h"
#include "widgets/SourceEditor.h"
#include "widgets/StatusStrip.h"
#include "widgets/TaskPanel.h"
#include "widgets/VerificationPanel.h"

namespace localcode::gui {
void MainWindow::showPlugins() {
    centreTabs_->setCurrentIndex(3);
}

namespace {

// The engine speaks std::string; the interface speaks QString. One converter
// each way, used everywhere, beats scattered inline conversions.
std::vector<std::string> toStdStrings(const QStringList& list) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(list.size()));
    for (const auto& item : list)
        out.push_back(item.toStdString());
    return out;
}

// The result of one background git scan. Named at namespace scope because a
// QFutureWatcher needs the type spelled out.
struct GitSnapshot {
    bool repository = false;
    QString branch;
    int changed = 0;
    bool clean = true;
    DiffResult diff;
    QStringList agentChanged;
    QStringList conflicting;
};

QWidget* panel(QWidget* content, QWidget* parent) {
    auto* frame = new QWidget(parent);
    frame->setObjectName("Panel");
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);
    return frame;
}

}   // namespace

MainWindow::MainWindow(Application& application, QWidget* parent)
    : QMainWindow(parent), application_(application) {
    setWindowTitle(tr("LocalCode"));
    resize(1560, 960);

    controller_ = std::make_unique<AgentController>(application_, this);

    buildUi();
    buildMenus();
    buildToolBar();
    wireAgent();
    restoreLayout();

    // The draft is written on a debounce rather than on every keystroke: it is
    // the thing users are angriest to lose to a crash, and a database write per
    // character would be absurd.
    draftTimer_ = new QTimer(this);
    draftTimer_->setSingleShot(true);
    draftTimer_->setInterval(1200);
    connect(draftTimer_, &QTimer::timeout, this, [this] {
        if (auto status = application_.checkpoint_session(toStdStrings(editor_->openFiles()),
                                                          pendingDraft_.toStdString());
            !status)
            reportStorageFailure(tr("Save draft"), status.error());
    });

    // Git state changes constantly while the agent works; polling it on a slow
    // timer is far cheaper than a filesystem watcher over a whole repository.
    gitTimer_ = new QTimer(this);
    gitTimer_->setInterval(4000);
    connect(gitTimer_, &QTimer::timeout, this, [this] {
        // A timer tick has no new observation to invalidate an in-flight scan.
        // Otherwise any Git command slower than this interval is discarded
        // forever, even while the workspace stays unchanged.
        if (!gitRefreshInFlight_) refreshGitState();
    });

    // The server can die at any moment - not only while a load is in flight -
    // and when it does the status line must stop claiming the model is ready.
    // Installed once, for the life of the window, rather than per load.
    application_.backends().set_state_callback([this](const ServerStatus& status) {
        const QString message = QString::fromStdString(status.message);
        const auto state = status.state;
        const int exitCode = status.exit_code;
        QMetaObject::invokeMethod(
            this,
            [this, message, state, exitCode] {
                if (!message.isEmpty()) output_->appendServerLog(message);
                onServerStateChanged(state, message, exitCode);
            },
            Qt::QueuedConnection);
    });

    // Advice that is not anyone's state - the video memory preflight - reaches
    // the transcript so it is seen before a minute-long load fails.
    application_.backends().set_advisory_callback([this](const std::string& note) {
        const QString text = QString::fromStdString(note);
        QMetaObject::invokeMethod(
            this,
            [this, text] {
                chat_->addMessage(ChatRole::Notice, text);
                output_->appendServerLog(text);
            },
            Qt::QueuedConnection);
    });

    updateModelSelector();
    announce(
        tr("Ready. Open a project folder or choose File > Work without a project, "
           "then load a model."));
}

void MainWindow::onServerStateChanged(ServerState state, const QString& message, int exitCode) {
    // These signals are queued across threads. An old load/recovery event must
    // not revive Ready or report an unexpected crash during an explicit unload.
    if (modelUnloading_ && state != ServerState::Stopping && state != ServerState::Stopped) {
        status_->setModelState(ModelState::Unloading, tr("Stopping the model server"));
        return;
    }
    if (state == ServerState::Ready && !modelCancel_ &&
        !application_.backends().current_backend()) {
        status_->setModelName({}, {});
        status_->setModelState(ModelState::Unloaded, tr("No model loaded"));
        return;
    }
    switch (state) {
        case ServerState::Ready: {
            status_->setModelState(ModelState::Ready, message);
            // Automatic recovery does not run the model-load watcher's completion
            // handler. Restore the identity cleared by Restarting here too. During
            // an initial load the backend may not be published yet; its watcher
            // will populate the strip once activation has completed.
            const auto backend = application_.backends().current_backend();
            const auto profile = application_.backends().current_profile();
            if (backend && !profile.id.empty()) {
                status_->setModelName(QString::fromStdString(profile.display_name),
                                      QString::fromStdString(backend->id()));
                status_->setContext(0, static_cast<int>(profile.context_size), false);
            }
            updateModelSelector();
            break;
        }
        case ServerState::Restarting:
            status_->setModelName({}, {});
            status_->setContext(0, 0, false);
            status_->setModelState(ModelState::Restarting, message.section('\n', 0, 0));
            // This is a historical notice, not a tool row that will be completed.
            // The status strip owns the live recovery state.
            timeline_->addNote(message.section('\n', 0, 0));
            break;
        case ServerState::Starting:
            status_->setModelName({}, {});
            status_->setContext(0, 0, false);
            status_->setModelState(switchingModel_ ? ModelState::Switching : ModelState::Loading,
                                   message);
            break;
        case ServerState::Failed: {
            // Say what happened and what to do about it. "Failed" on its own
            // leaves the user staring at a window that will refuse every prompt
            // without explaining why.
            const QString detail =
                message.isEmpty()
                    ? tr("the model server stopped unexpectedly (exit code %1)").arg(exitCode)
                    : message;
            status_->setModelState(ModelState::Failed, detail);
            chat_->addMessage(ChatRole::Error,
                              tr("**The model server stopped.** %1\n\nLoad the model again from "
                                 "the Models panel before sending another prompt. "
                                 "Open Output (Ctrl+J) for the full error and server log.")
                                  .arg(detail.section('\n', 0, 0)));
            timeline_->addNote(tr("Model server stopped"), TimelineEntry::State::Failed);
            updateModelSelector();
            updateWindowTitle();
            break;
        }
        case ServerState::Stopping:
        case ServerState::Stopped:
            status_->setModelName({}, {});
            status_->setContext(0, 0, false);
            status_->setModelState(modelUnloading_                   ? ModelState::Unloading
                                   : modelCancel_ && switchingModel_ ? ModelState::Switching
                                                                     : ModelState::Unloaded,
                                   message);
            break;
    }
}

MainWindow::~MainWindow() {
    // Background work started here captures `this` and reaches into the engine
    // through it. A QFutureWatcher parented to this window is deleted with the
    // window, but deleting the watcher does not stop the task it was watching -
    // that keeps running on the thread pool, against an object that is halfway
    // through being destroyed.
    //
    // The git refresh is what caught this: it runs on a pool thread, iterates
    // the map returned by Workspace::agent_changes(), and crashed there once in
    // twenty exits while the window came down around it.
    //
    // So: stop anything that would start new work, cancel the one task that can
    // run long, and do not return until the pool is idle.
    if (gitTimer_) gitTimer_->stop();
    if (draftTimer_) draftTimer_->stop();
    if (modelCancel_) modelCancel_->cancel();
    gitRefreshCancel_.cancel();
    controller_.reset();
    QThreadPool::globalInstance()->waitForDone();
    // Join the managed server while its callbacks still have a live window.
    // The engine outlives this window and otherwise retains callbacks to it.
    application_.deactivate_model();
    application_.backends().set_state_callback({});
    application_.backends().set_advisory_callback({});
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    outerSplitter_ = new QSplitter(Qt::Horizontal, central);
    outerSplitter_->setChildrenCollapsible(false);

    // --- Left: the project ------------------------------------------------
    projectTabs_ = new QTabWidget(outerSplitter_);
    projectTabs_->setObjectName("ProjectSidebar");
    projectTabs_->setDocumentMode(true);
    projectTabs_->setMinimumWidth(180);
    explorer_ = new ProjectExplorer(projectTabs_);
    repository_ = new RepositoryPanel(application_, projectTabs_);
    projectTabs_->addTab(explorer_, tr("Project"));
    projectTabs_->addTab(repository_, tr("Repository"));
    auto* historyPage = new QWidget(projectTabs_);
    auto* historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(6, 6, 6, 6);
    conversations_ = new QListWidget(historyPage);
    conversations_->setObjectName("ConversationList");
    conversations_->setTextElideMode(Qt::ElideRight);
    conversations_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(conversations_, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& position) {
                if (controller_->busy() || closePending_) return;
                const auto* item = conversations_->itemAt(position);
                if (!item) return;
                const auto id = item->data(Qt::UserRole).toLongLong();
                auto* menu = new QMenu(conversations_);
                menu->setObjectName("ConversationContextMenu");
                menu->setAttribute(Qt::WA_DeleteOnClose);
                auto* remove = menu->addAction(tr("Delete chat"));
                remove->setObjectName("DeleteChatAction");
                connect(remove, &QAction::triggered, this, [this, id] { deleteConversation(id); });
                menu->popup(conversations_->viewport()->mapToGlobal(position));
            });
    historyLayout->addWidget(conversations_, 1);
    auto* newChat = new QPushButton(tr("New conversation"), historyPage);
    connect(newChat, &QPushButton::clicked, this, &MainWindow::newConversation);
    connect(conversations_, &QListWidget::itemActivated, this, &MainWindow::switchConversation);
    historyLayout->addWidget(newChat);
    projectTabs_->addTab(historyPage, tr("Chats"));
    outerSplitter_->addWidget(projectTabs_);

    // --- Centre: conversation, files, diff --------------------------------
    auto* centre = new QWidget(outerSplitter_);
    auto* centreLayout = new QVBoxLayout(centre);
    centreLayout->setContentsMargins(0, 0, 0, 0);
    centreLayout->setSpacing(0);

    auto* summaryBar = new QWidget(centre);
    summaryBar->setObjectName("Panel");
    auto* summaryLayout = new QHBoxLayout(summaryBar);
    summaryLayout->setContentsMargins(12, 6, 8, 6);
    taskSummary_ = new QLabel(tr("Ready"), summaryBar);
    taskSummary_->setObjectName("TaskSummary");
    taskSummary_->setTextFormat(Qt::PlainText);
    taskSummary_->setWordWrap(true);
    summaryLayout->addWidget(taskSummary_, 1);
    auto* details = new QPushButton(tr("Details"), summaryBar);
    details->setToolTip(tr("Show activity, verification and model details (Ctrl+Shift+I)"));
    connect(details, &QPushButton::clicked, this,
            [this] { inspectorPanelAction_->setChecked(!inspectorPanelAction_->isChecked()); });
    summaryLayout->addWidget(details);
    centreLayout->addWidget(summaryBar);

    centreSplitter_ = new QSplitter(Qt::Vertical, centre);

    centreTabs_ = new QTabWidget(centreSplitter_);
    centreTabs_->setDocumentMode(true);

    chat_ = new ChatView(centreTabs_);
    editor_ = new SourceEditor(centreTabs_);
    diff_ = new DiffView(centreTabs_);

    centreTabs_->addTab(chat_, tr("Chat"));
    centreTabs_->addTab(editor_, tr("Files"));
    centreTabs_->addTab(diff_, tr("Changes"));
    // Long settings forms must scroll instead of imposing their minimum height
    // on every page in the tab stack, including the ordinary chat view.
    auto* pluginsPage = new QScrollArea(centreTabs_);
    pluginsPage->setObjectName("PluginsPageScroll");
    pluginsPage->setWidgetResizable(true);
    pluginsPage->setFrameShape(QFrame::NoFrame);
    auto* plugins = new PluginsPanel(pluginsPage);
    pluginsPage->setWidget(plugins);
    centreTabs_->addTab(pluginsPage, tr("Plugins"));
    centreSplitter_->addWidget(centreTabs_);

    output_ = new OutputPanel(centreSplitter_);
    output_->setObjectName("CommandOutput");
    centreSplitter_->addWidget(output_);
    centreSplitter_->setStretchFactor(0, 3);
    centreSplitter_->setStretchFactor(1, 1);
    centreSplitter_->setSizes({640, 220});
    centreLayout->addWidget(centreSplitter_, 1);

    // --- The composer -----------------------------------------------------
    auto* composerBar = new QWidget(centre);
    composerBar->setObjectName("Panel");
    auto* composerLayout = new QHBoxLayout(composerBar);
    composerLayout->setContentsMargins(8, 8, 8, 8);
    composerLayout->setSpacing(8);

    composer_ = new PromptComposer(composerBar);
    composerLayout->addWidget(composer_, 1);

    // Side by side and bottom-aligned. Stacked, they claimed a tall column beside
    // a short text box and pushed Stop to a place the eye never goes.
    auto* buttons = new QWidget(composerBar);
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(6);

    stopButton_ = new QPushButton(tr("Stop"), buttons);
    stopButton_->setObjectName("Danger");
    stopButton_->setMinimumWidth(70);
    stopButton_->setEnabled(false);
    stopButton_->setToolTip(tr("Cancel generation, the running command and the agent loop"));

    sendButton_ = new QPushButton(tr("Send"), buttons);
    sendButton_->setObjectName("Primary");
    sendButton_->setMinimumWidth(70);
    sendButton_->setToolTip(tr("Ctrl+Enter"));

    auto* modeLayout = new QVBoxLayout();
    modeLayout->setContentsMargins(0, 0, 0, 0);
    requestMode_ = new QComboBox(buttons);
    requestMode_->setObjectName("RequestMode");
    requestMode_->addItem(tr("Auto"), static_cast<int>(RequestMode::Auto));
    requestMode_->addItem(tr("Chat"), static_cast<int>(RequestMode::Chat));
    requestMode_->addItem(tr("Code"), static_cast<int>(RequestMode::Code));
    requestMode_->setToolTip(tr("Auto: greetings use lightweight chat; other requests keep coding tools. "
                               "Chat: conversation without project tools. Code: full coding agent."));
    modeLayout->addWidget(requestMode_);
    auto* sendLayout = new QHBoxLayout();
    sendLayout->setSpacing(6);
    sendLayout->addWidget(stopButton_);
    sendLayout->addWidget(sendButton_);
    modeLayout->addLayout(sendLayout);
    buttonLayout->addLayout(modeLayout);
    composerLayout->addWidget(buttons, 0, Qt::AlignBottom);

    centreLayout->addWidget(composerBar);
    connect(centreTabs_, &QTabWidget::currentChanged, this,
            [composerBar, pluginsPage, this](int index) {
                composerBar->setVisible(centreTabs_->widget(index) != pluginsPage);
                updatePanelVisibility();
            });
    outerSplitter_->addWidget(centre);

    // --- Right: what the agent is doing -----------------------------------
    rightSplitter_ = new QSplitter(Qt::Vertical, outerSplitter_);
    rightSplitter_->setObjectName("InspectorSidebar");

    task_ = new TaskPanel(rightSplitter_);
    rightSplitter_->addWidget(panel(task_, rightSplitter_));

    rightTabs_ = new QTabWidget(rightSplitter_);
    rightTabs_->setDocumentMode(true);

    timeline_ = new ActivityTimeline(rightTabs_);
    verification_ = new VerificationPanel(rightTabs_);
    auto* modelsPage = new QScrollArea(rightTabs_);
    modelsPage->setObjectName("ModelProfileScroll");
    modelsPage->setWidgetResizable(true);
    modelsPage->setFrameShape(QFrame::NoFrame);
    models_ = new ModelPanel(application_, modelsPage);
    modelsPage->setWidget(models_);
    skills_ = new SkillsPanel(application_.skills(), rightTabs_);

    rightTabs_->addTab(timeline_, tr("Activity"));
    rightTabs_->addTab(verification_, tr("Verification"));
    rightTabs_->addTab(modelsPage, tr("Models"));
    rightTabs_->addTab(skills_, tr("Skills"));
    rightSplitter_->addWidget(rightTabs_);
    rightSplitter_->setSizes({320, 520});

    rightSplitter_->setMinimumWidth(260);
    outerSplitter_->addWidget(rightSplitter_);
    // Only the centre absorbs extra width; the side columns keep the size they
    // are given. Their sizes are applied in showEvent(), because setSizes() on a
    // splitter that has not been laid out yet is scaled to its placeholder
    // width and the side panels end up crushed.
    outerSplitter_->setStretchFactor(0, 0);
    outerSplitter_->setStretchFactor(1, 1);
    outerSplitter_->setStretchFactor(2, 0);

    centralLayout->addWidget(outerSplitter_, 1);

    status_ = new StatusStrip(central);
    centralLayout->addWidget(status_);

    setCentralWidget(central);
    statusBar()->setSizeGripEnabled(true);

    // --- Wiring between panels -------------------------------------------
    connect(composer_, &PromptComposer::submitted, this, &MainWindow::submitPrompt);
    connect(composer_, &PromptComposer::stopRequested, this, &MainWindow::stopRun);
    connect(composer_, &PromptComposer::draftChanged, this, &MainWindow::saveDraft);
    connect(sendButton_, &QPushButton::clicked, this,
            [this] { submitPrompt(composer_->promptText()); });
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopRun);

    connect(explorer_, &ProjectExplorer::fileActivated, this,
            [this](const QString& path) { openFileFromPath(path); });
    connect(explorer_, &ProjectExplorer::revealRequested, this, [](const QString& path) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });

    connect(editor_, &SourceEditor::statusMessage, this, &MainWindow::announce);
    connect(diff_, &DiffView::openFileRequested, this, &MainWindow::openFileFromPath);
    connect(diff_, &DiffView::refreshRequested, this, &MainWindow::refreshGitState);
    connect(repository_, &RepositoryPanel::fileActivated, this, &MainWindow::openFileFromPath);
    connect(task_, &TaskPanel::fileActivated, this, [this](const QString& relative) {
        const auto root = application_.workspace().root();
        if (root.empty()) return;
        openFileFromPath(
            QString::fromStdString(path_to_utf8(root / path_from_utf8(relative.toStdString()))));
    });

    connect(models_, &ModelPanel::loadRequested, this, &MainWindow::loadModel);
    connect(models_, &ModelPanel::unloadRequested, this, &MainWindow::unloadModel);
    connect(models_, &ModelPanel::importRequested, this, &MainWindow::importModel);
    connect(models_, &ModelPanel::statusMessage, this, &MainWindow::announce);

    connect(skills_, &SkillsPanel::skillStateChanged, this,
            [](const QString&, bool, bool) { /* persisted by the registry */ });

    connect(chat_, &ChatView::codeCopied, this,
            [this] { announce(tr("Copied to the clipboard.")); });
    connect(timeline_, &ActivityTimeline::entrySelected, this, &MainWindow::showToolDetails);
}

void MainWindow::showToolDetails(const QString& title, const QString& detail,
                                 const QString& logPath) {
    if (!toolDetails_) {
        toolDetails_ = new QDialog(this);
        toolDetails_->setObjectName("ToolDetails");
        toolDetails_->resize(760, 520);
        toolDetails_->setMinimumSize(420, 300);
        auto* layout = new QVBoxLayout(toolDetails_);
        toolDetailText_ = new QPlainTextEdit(toolDetails_);
        toolDetailText_->setReadOnly(true);
        toolDetailText_->setFont(Theme::instance().monoFont(10));
        layout->addWidget(toolDetailText_, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, toolDetails_);
        auto* copy = buttons->addButton(tr("Copy details"), QDialogButtonBox::ActionRole);
        toolLogButton_ = buttons->addButton(tr("Open full log"), QDialogButtonBox::ActionRole);
        connect(copy, &QPushButton::clicked, this,
                [this] { QApplication::clipboard()->setText(toolDetailText_->toPlainText()); });
        connect(toolLogButton_, &QPushButton::clicked, this,
                [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(toolDetailLogPath_)); });
        connect(buttons, &QDialogButtonBox::rejected, toolDetails_, &QDialog::reject);
        layout->addWidget(buttons);
    }
    toolDetails_->setWindowTitle(title.left(120));
    toolDetailText_->setPlainText(detail.isEmpty() ? title : detail);
    toolDetailLogPath_ = logPath;
    toolLogButton_->setVisible(!logPath.isEmpty() && QFileInfo::exists(logPath));
    toolDetails_->show();
    toolDetails_->raise();
    QTimer::singleShot(0, toolDetails_, [this] { clampToScreen(*toolDetails_); });
}

void MainWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    openWorkspaceAction_ =
        fileMenu->addAction(tr("Open project folder..."), this, &MainWindow::openWorkspace);
    openWorkspaceAction_->setShortcut(QKeySequence::Open);

    outputWorkspaceAction_ =
        fileMenu->addAction(tr("Work without a project"), this, &MainWindow::createOutputWorkspace);
    outputWorkspaceAction_->setObjectName("CreateOutputWorkspace");
    outputWorkspaceAction_->setToolTip(
        tr("Create a dated output folder in your LocalCode user data and work there."));

    auto* recentMenu = fileMenu->addMenu(tr("Recent projects"));
    connect(recentMenu, &QMenu::aboutToShow, this, [this, recentMenu] {
        recentMenu->clear();
        auto recent = application_.recent_workspaces();
        if (!recent || recent->empty()) {
            recentMenu->addAction(tr("(none)"))->setEnabled(false);
            return;
        }
        for (const auto& workspace : *recent) {
            const QString path = QString::fromStdString(workspace.path);
            auto* action = recentMenu->addAction(path);
            action->setData(path);
            connect(action, &QAction::triggered, this, &MainWindow::openRecentWorkspace);
        }
    });

    fileMenu->addSeparator();
    newConversationAction_ =
        fileMenu->addAction(tr("New conversation"), this, &MainWindow::newConversation);
    newConversationAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));

    fileMenu->addAction(tr("Add GGUF model..."), this, &MainWindow::importModel);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Settings..."), this, &MainWindow::showSettings)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Quit"), this, &QWidget::close)->setShortcut(QKeySequence::Quit);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(tr("Agent"), this, [this] { centreTabs_->setCurrentIndex(0); })
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    viewMenu->addAction(tr("Files"), this, [this] { centreTabs_->setCurrentIndex(1); })
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    viewMenu
        ->addAction(tr("Changes"), this,
                    [this] {
                        centreTabs_->setCurrentIndex(2);
                        refreshGitState();
                    })
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_3));
    viewMenu->addSeparator();
    viewMenu->addAction(tr("Plugins"), this, [this] { centreTabs_->setCurrentIndex(3); })
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_4));
    auto toggle = [viewMenu, this](const QString& title, QWidget* widget,
                                   const QKeySequence& shortcut, bool visible) {
        auto* action = viewMenu->addAction(title);
        action->setCheckable(true);
        action->setShortcut(shortcut);
        connect(action, &QAction::toggled, this, [this] { updatePanelVisibility(); });
        action->setChecked(visible);
        widget->setVisible(visible);
        return action;
    };
    projectPanelAction_ =
        toggle(tr("Project sidebar"), projectTabs_, QKeySequence(Qt::CTRL | Qt::Key_B), true);
    inspectorPanelAction_ = toggle(tr("Inspector"), rightSplitter_,
                                   QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), false);
    outputPanelAction_ =
        toggle(tr("Command output"), output_, QKeySequence(Qt::CTRL | Qt::Key_J), false);
    viewMenu->addSeparator();
    viewMenu->addAction(tr("Focus the prompt"), this, [this] { composer_->setFocus(); })
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    viewMenu
        ->addAction(tr("Refresh project"), this,
                    [this] {
                        explorer_->refresh();
                        refreshGitState();
                    })
        ->setShortcut(QKeySequence::Refresh);
    viewMenu->addSeparator();
    viewMenu->addAction(tr("Open the log folder"), this, [] {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdString(path_to_utf8(app_paths().log_dir()))));
    });

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("About LocalCode"), this, [this] {
        QMessageBox::about(
            this, tr("About LocalCode"),
            tr("<b>LocalCode %1</b><p>A native C++ coding agent for local GGUF models, "
               "running through llama.cpp or ik_llama.cpp.</p>"
               "<p>Everything runs on this machine. Nothing is sent anywhere except the "
               "web requests you can see in the activity panel.</p>")
                .arg(LOCALCODE_VERSION));
    });
}

void MainWindow::buildToolBar() {
    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName("CodingToolbar");
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(16, 16));

    auto* projectButton = new QToolButton(toolbar);
    projectButton->setObjectName("OpenProjectMenu");
    projectButton->setDefaultAction(openWorkspaceAction_);
    projectButton->setPopupMode(QToolButton::MenuButtonPopup);
    auto* projectMenu = new QMenu(projectButton);
    projectMenu->addAction(openWorkspaceAction_);
    projectMenu->addAction(outputWorkspaceAction_);
    projectButton->setMenu(projectMenu);
    projectButton->setToolTip(
        tr("Open a project folder, or use the arrow to work without a project"));
    toolbar->addWidget(projectButton);
    toolbar->addSeparator();

    toolbar->addWidget(new QLabel(tr("Model:"), toolbar));
    modelSelector_ = new QComboBox(toolbar);
    modelSelector_->setMinimumWidth(150);
    modelSelector_->setMaximumWidth(300);
    modelSelector_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    modelSelector_->setMinimumContentsLength(16);
    modelSelector_->setToolTip(
        tr("Choose a model to load. Loading unloads the previous one "
           "first - two large models will not fit in VRAM together."));
    toolbar->addWidget(modelSelector_);

    connect(modelSelector_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (suppressModelSelector_ || index < 0) return;
        const auto id = modelSelector_->itemData(index).toString();
        if (!id.isEmpty() && id != QString::fromStdString(application_.active_model_id())) {
            loadModel(id);
        }
    });

    toolbar->addSeparator();
    workspaceLabel_ = new QLabel(tr("No project open"), toolbar);
    workspaceLabel_->setObjectName("Muted");
    workspaceLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    workspaceLabel_->setMaximumWidth(200);
    toolbar->addWidget(workspaceLabel_);

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    toolbar->addAction(tr("Settings"), this, &MainWindow::showSettings);
    toolbar->addSeparator();
    toolbar->addAction(projectPanelAction_);
    toolbar->addAction(outputPanelAction_);
}

void MainWindow::wireAgent() {
    auto* c = controller_.get();
    closeWaitTimer_ = new QTimer(this);
    closeWaitTimer_->setObjectName("PendingCloseTimer");
    closeWaitTimer_->setSingleShot(true);
    closeWaitTimer_->setInterval(30000);
    connect(closeWaitTimer_, &QTimer::timeout, this, [this] {
        if (!closePending_) return;
        closePending_ = false;
        onBusyChanged(controller_->busy());
        announce(
            tr("The task has not stopped yet. LocalCode remains open to preserve its "
               "results. Close it again after the task finishes."));
    });
    connect(c, &AgentController::workerFinished, this, [this] {
        if (!closePending_) return;
        closeWaitTimer_->stop();
        // Finish the terminal signal delivery before saving and closing. Keep
        // new requests blocked until this one queued close actually runs.
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (!closePending_) return;
                closePending_ = false;
                if (!close()) onBusyChanged(controller_->busy());
            },
            Qt::QueuedConnection);
    });
    connect(c, &AgentController::runStarted, this, &MainWindow::onRunStarted);
    connect(c, &AgentController::promptProcessing, this, &MainWindow::onPromptProcessing);
    connect(c, &AgentController::contentDelta, this, &MainWindow::onContentDelta);
    connect(c, &AgentController::reasoningDelta, this, &MainWindow::onReasoningDelta);
    connect(c, &AgentController::toolStarted, this, &MainWindow::onToolStarted);
    connect(c, &AgentController::toolFinished, this, &MainWindow::onToolFinished);
    connect(c, &AgentController::toolOutput, this, &MainWindow::onToolOutput);
    connect(c, &AgentController::toolProgress, this, &MainWindow::onToolProgress);
    connect(c, &AgentController::contextUpdated, this, &MainWindow::onContextUpdated);
    connect(c, &AgentController::generationStats, this, &MainWindow::onGenerationStats);
    connect(c, &AgentController::progressWarning, this, &MainWindow::onProgressWarning);
    connect(c, &AgentController::finalizationStarted, this, &MainWindow::onFinalizationStarted);
    connect(c, &AgentController::taskStateUpdated, this, &MainWindow::onTaskStateUpdated);
    connect(c, &AgentController::verificationUpdated, verification_,
            &VerificationPanel::setVerification);
    connect(c, &AgentController::runFinished, this, &MainWindow::onRunFinished);
    connect(c, &AgentController::runFailed, this, &MainWindow::onRunFailed);
    connect(c, &AgentController::runCancelled, this, &MainWindow::onRunCancelled);
    connect(c, &AgentController::busyChanged, this, &MainWindow::onBusyChanged);
}

// ---------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------
void MainWindow::openWorkspace() {
    const auto directory = QFileDialog::getExistingDirectory(
        this, tr("Open a project folder"),
        QString::fromStdString(path_to_utf8(application_.workspace().root())));
    if (!directory.isEmpty()) applyWorkspace(directory);
}

void MainWindow::createOutputWorkspace() {
    if (closePending_) {
        announce(tr("Waiting for the current task to stop before closing."));
        return;
    }
    if (modelCancel_) {
        announce(tr("Wait for the model operation before switching projects."));
        return;
    }
    if (controller_->busy()) {
        announce(tr("Stop the agent before switching projects."));
        return;
    }

    const auto data = QString::fromStdString(path_to_utf8(app_paths().data_dir()));
    const auto outputs = QDir(data).filePath("outputs");
    if (!QDir::isAbsolutePath(data) || !QDir().mkpath(outputs)) {
        QMessageBox::warning(
            this, tr("Cannot create output folder"),
            tr("LocalCode could not create its output directory:\n%1").arg(outputs));
        return;
    }
    const auto name = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + "-XXXXXX";
    QTemporaryDir directory(QDir(outputs).filePath(name));
    if (!directory.isValid()) {
        QMessageBox::warning(this, tr("Cannot create output folder"), directory.errorString());
        return;
    }
    // QTemporaryDir creates a unique directory atomically. This is persistent
    // user output, including if opening it is stopped by an existing save guard.
    directory.setAutoRemove(false);
    applyWorkspace(directory.path());
    if (application_.workspace().root() == path_from_utf8(directory.path().toStdString()))
        announce(tr("Output folder opened: %1. Files created here are kept after closing.")
                     .arg(directory.path()));
}

void MainWindow::openRecentWorkspace() {
    auto* action = qobject_cast<QAction*>(sender());
    if (!action) return;
    applyWorkspace(action->data().toString());
}

void MainWindow::applyWorkspace(const QString& path) {
    if (modelCancel_) {
        announce(tr("Wait for the model operation before switching projects."));
        return;
    }
    if (controller_->busy()) {
        announce(tr("Stop the agent before switching projects."));
        return;
    }
    selectTaskToolScope({});
    if (editor_->hasUnsavedChanges()) {
        QString error;
        if (!editor_->saveAll(&error)) {
            QMessageBox::warning(this, tr("Save failed"), error);
            return;
        }
    }
    if (application_.workspace().is_open()) {
        if (!persistConversation()) return;
        const auto saved = application_.checkpoint_session(toStdStrings(editor_->openFiles()),
                                                           pendingDraft_.toStdString());
        if (!saved) {
            reportStorageFailure(tr("Save session before switching projects"), saved.error());
            return;
        }
    }
    const auto previous_root = application_.workspace().root();
    auto status = application_.open_workspace(path_from_utf8(path.toStdString()));
    if (!status) {
        QMessageBox::warning(this, tr("Cannot open project"),
                             QString::fromStdString(status.error().message));
        return;
    }

    explorer_->setRoot(path);
    composer_->setWorkspaceRoot(path);
    if (!editor_->closeAll()) return;
    if (!previous_root.empty() && previous_root != application_.workspace().root()) {
        chat_->clear();
        timeline_->clear();
        task_->clear();
        verification_->clear();
        status_->clearOutcome();
        output_->clearCommandOutput();
    }
    if (draftTimer_) draftTimer_->stop();
    pendingDraft_.clear();
    diff_->clear();

    workspaceLabel_->setText(workspaceLabel_->fontMetrics().elidedText(QFileInfo(path).fileName(),
                                                                       Qt::ElideMiddle, 200));
    workspaceLabel_->setToolTip(path);
    status_->setWorkspace(path);
    updateWindowTitle();

    // A previous draft for this workspace comes back with it.
    const auto draft =
        application_.draft_repository().load(path_to_utf8(application_.workspace().root()));
    composer_->setPlainText(QString::fromStdString(draft.value_or("")));

    gitTimer_->start();
    refreshGitState();

    // The index for this project is opened by open_workspace and is available
    // immediately, whatever it learned last time. Bringing it into line with the
    // tree happens on its own thread, so opening a project stays instant even
    // when the project is large.
    if (repository_ != nullptr) {
        repository_->workspaceChanged();
        application_.refresh_repository_index(false);
    }

    announce(tr("Project opened: %1").arg(path));
    refreshConversations();
}

void MainWindow::openProjectFolder(const QString& path) {
    applyWorkspace(path);
}

void MainWindow::loadModelById(const QString& profileId) {
    loadModel(profileId);
}

void MainWindow::refreshGitState() {
    auto& workspace = application_.workspace();
    if (!workspace.is_open()) return;

    // Git shells out; doing it on the GUI thread would stutter on a large
    // repository, so it runs on the thread pool and comes back by signal.
    const bool changes_visible = centreTabs_->currentWidget() == diff_;
    const auto root = workspace.root();
    const auto changes = workspace.agent_changes();
    const auto generation = ++gitRefreshGeneration_;
    if (fileToolsOnlyTask_) {
        QStringList changed, conflicting;
        for (const auto& change : changes) {
            changed << QString::fromStdString(change.relative_path);
            if (change.was_dirty_before)
                conflicting << QString::fromStdString(change.relative_path);
        }
        task_->setChangedFiles(changed, conflicting);
        task_->setGitSummary({}, 0, false);
        return;
    }
    // A slow Git command or a burst of file tools must not fill the pool with
    // scans that are obsolete before they start. Keep one scan and remember
    // that its completion needs a fresh snapshot of the latest workspace.
    if (gitRefreshInFlight_) {
        gitRefreshPending_ = true;
        return;
    }
    gitRefreshInFlight_ = true;
    // Each job owns the paths and changes it was asked to inspect. A queued
    // job must not read a different workspace after the user switches projects.
    auto git = workspace.git();
    const auto cancel = gitRefreshCancel_.token();

    auto future =
        QtConcurrent::run([git = std::move(git), root, changes, changes_visible, cancel]() mutable {
            GitSnapshot snapshot;
            auto status = git.status(root, cancel);
            if (status && status->is_repository) {
                snapshot.repository = true;
                snapshot.branch = QString::fromStdString(status->branch);
                snapshot.changed = static_cast<int>(status->files.size());
                snapshot.clean = status->clean;
            }
            for (const auto& change : changes) {
                snapshot.agentChanged << QString::fromStdString(change.relative_path);
                if (change.was_dirty_before) {
                    snapshot.conflicting << QString::fromStdString(change.relative_path);
                }
            }
            if (changes_visible && snapshot.repository) {
                auto diff = git.diff(root, {}, false, 3, cancel);
                if (diff) snapshot.diff = *diff;
            }
            return snapshot;
        });

    auto* watcher = new QFutureWatcher<GitSnapshot>(this);
    connect(
        watcher, &QFutureWatcherBase::finished, this,
        [this, watcher, changes_visible, root, generation] {
            const auto snapshot = watcher->result();
            watcher->deleteLater();
            gitRefreshInFlight_ = false;
            if (generation == gitRefreshGeneration_ && application_.workspace().root() == root) {
                task_->setChangedFiles(snapshot.agentChanged, snapshot.conflicting);
                if (snapshot.repository) {
                    task_->setGitSummary(snapshot.branch, snapshot.changed, snapshot.clean);
                }
                diff_->setAgentChanges(snapshot.agentChanged, snapshot.conflicting);
                if (changes_visible && snapshot.repository) diff_->setDiff(snapshot.diff);

                // Any file the agent rewrote is re-read, unless the user has edited it
                // here - a hand edit is never discarded.
                editor_->reloadUnmodified();
            }
            if (gitRefreshPending_) {
                gitRefreshPending_ = false;
                refreshGitState();
            }
        });
    watcher->setFuture(future);
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------
void MainWindow::importModel() {
    ModelImportDialog dialog(application_, this);
    if (dialog.exec() != QDialog::Accepted) return;

    models_->refresh();
    updateModelSelector();
    announce(tr("Model added. Select it to load."));

    const auto id = dialog.importedProfileId();
    if (!id.isEmpty()) {
        const auto answer =
            QMessageBox::question(this, tr("Load this model?"), tr("Load the model now?"));
        if (answer == QMessageBox::Yes) loadModel(id);
    }
}

void MainWindow::loadModel(const QString& profileId) {
    if (controller_->busy()) {
        announce(tr("Cannot switch models while the agent is working. Stop it first."));
        return;
    }
    if (modelCancel_) {
        announce(tr("A model operation is already in progress."));
        return;
    }

    // Loading a large model takes minutes. It happens on a worker thread with a
    // cancellable progress dialog, never on the GUI thread.
    auto* progress = new QProgressDialog(tr("Loading the model..."), tr("Cancel"), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(200);
    progress->setAutoClose(false);
    progress->setAutoReset(false);

    auto cancel = std::make_shared<CancellationSource>();
    modelCancel_ = cancel;
    connect(progress, &QProgressDialog::canceled, this, [cancel] { cancel->cancel(); });

    switchingModel_ = static_cast<bool>(application_.backends().current_backend());
    status_->setModelState(switchingModel_ ? ModelState::Switching : ModelState::Loading,
                           tr("Loading %1").arg(profileId));
    output_->clearServerLog();

    // Server output already reaches the output panel through the callback
    // installed for the life of the window; replacing it here would drop the
    // state changes that arrive after the load finishes - which is exactly when
    // a server dies.

    auto future = QtConcurrent::run([this, profileId, cancel] {
        return application_.activate_model(profileId.toStdString(), cancel->token());
    });

    auto* watcher = new QFutureWatcher<Status>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, progress, profileId] {
        const auto status = watcher->result();
        modelCancel_.reset();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();

        for (const auto& line : application_.backends().server_log(300)) {
            output_->appendServerLog(QString::fromStdString(line));
        }

        if (!status) {
            status_->setModelName({}, {});
            status_->setModelState(ModelState::Failed,
                                   QString::fromStdString(status.error().to_string()));
            chat_->addMessage(ChatRole::Error,
                              tr("**The model could not be loaded.**\n\n%1\n\n%2")
                                  .arg(QString::fromStdString(status.error().message),
                                       QString::fromStdString(status.error().detail)));
            output_->showServerTab();
            updateModelSelector();
            return;
        }

        const auto profile = application_.backends().current_profile();
        const auto info = application_.backends().current_info();
        status_->setModelState(ModelState::Ready, tr("Model loaded"));
        const auto observed = application_.backends().server_status();
        const auto acceleration = QString::fromStdString(observed.acceleration.describe());
        const auto loadMs = qMax<qint64>(0, observed.ready_unix_ms - observed.started_unix_ms);
        timeline_->addNote(tr("Model startup: %1 s. %2").arg(loadMs / 1000.0, 0, 'f', 2).arg(acceleration),
                           TimelineEntry::State::Info);
        status_->setModelState(ModelState::Ready, acceleration);
        const auto loadedBackend = application_.backends().current_backend();
        auto placement = loadedBackend ? QString::fromStdString(loadedBackend->id()) : QString{};
        if (observed.acceleration.offloaded_layers == 0) placement += tr(" · CPU");
        else if (observed.acceleration.offloaded_layers > 0)
            placement += tr(" · GPU %1/%2").arg(observed.acceleration.offloaded_layers)
                                          .arg(observed.acceleration.total_layers);
        else placement += tr(" · acceleration unconfirmed");
        status_->setModelName(QString::fromStdString(profile.display_name), placement);
        status_->setContext(0, static_cast<int>(profile.context_size), false);

        chat_->setShowReasoning(application_.settings().show_reasoning &&
                                profile.capabilities.supports_reasoning_content &&
                                profile.show_reasoning);

        models_->refresh();
        updateModelSelector();
        updateWindowTitle();

        QString note = tr("Loaded **%1**").arg(QString::fromStdString(profile.display_name));
        note += tr("\n\n%1\n\nModel startup: %2 s.").arg(acceleration).arg(loadMs / 1000.0, 0, 'f', 2);
        if (info.context_size > 0) {
            note += tr(" with a %1-token context").arg(info.context_size);
        }
        if (!profile.capabilities.supports_tools) {
            note +=
                tr("\n\nThis model does not declare native tool calling, so "
                   "LocalCode is using its text protocol instead. That works, but "
                   "native tool calls are more reliable.");
        }
        chat_->addMessage(ChatRole::System, note);
        announce(tr("Model ready."));
    });
    watcher->setFuture(future);
}

void MainWindow::unloadModel() {
    if (controller_->busy() || modelCancel_) {
        announce(tr("Stop the agent or cancel the model load before unloading."));
        return;
    }
    // Process shutdown joins the health/output workers and can take seconds.
    // Use the same lifetime guard as loading, keeping GUI events responsive.
    modelCancel_ = std::make_shared<CancellationSource>();
    modelUnloading_ = true;
    switchingModel_ = false;
    sendButton_->setEnabled(false);
    status_->setModelState(ModelState::Unloading, tr("Stopping the model server"));
    auto* progress = new QProgressDialog(tr("Unloading the model..."), QString(), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    progress->show();
    auto* watcher = new QFutureWatcher<Status>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, progress] {
        const auto result = watcher->result();
        modelUnloading_ = false;
        modelCancel_.reset();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        sendButton_->setEnabled(!controller_->busy());
        status_->setModelName({}, {});
        if (result) {
            status_->setModelState(ModelState::Unloaded, tr("No model loaded"));
            announce(tr("Model unloaded."));
        } else {
            status_->setModelState(ModelState::Failed,
                                   QString::fromStdString(result.error().to_string()));
            chat_->addMessage(ChatRole::Error,
                              tr("The model could not be cleanly unloaded: %1")
                                  .arg(QString::fromStdString(result.error().to_string())));
        }
        models_->refresh();
        updateModelSelector();
        updateWindowTitle();
    });
    watcher->setFuture(QtConcurrent::run([this]() -> Status {
        try {
            application_.deactivate_model();
            return ok_status();
        } catch (const std::exception& error) {
            return make_error(ErrorCode::Internal, "model unload failed", error.what());
        } catch (...) {
            return make_error(ErrorCode::Internal, "model unload failed with an unknown error");
        }
    }));
}

void MainWindow::updateModelSelector() {
    if (!modelSelector_) return;

    suppressModelSelector_ = true;
    modelSelector_->clear();

    auto profiles = application_.model_profiles();
    if (!profiles || profiles->empty()) {
        modelSelector_->addItem(tr("No models added"), QString());
        modelSelector_->setEnabled(false);
        suppressModelSelector_ = false;
        return;
    }

    modelSelector_->setEnabled(true);
    const auto active = QString::fromStdString(application_.active_model_id());

    // With nothing loaded, a combo box showing the first model in the library
    // reads as "this model is active" - and it is the most prominent control
    // in the window, so it wins over the status bar saying "No model". A run
    // sent in that state fails with "No model is loaded", which is correct and
    // arrives as a surprise. The placeholder says what is true.
    if (active.isEmpty()) modelSelector_->addItem(tr("Select a model to load"), QString());

    int activeIndex = -1;
    for (const auto& profile : *profiles) {
        const auto id = QString::fromStdString(profile.id);
        modelSelector_->addItem(QString::fromStdString(profile.display_name), id);
        modelSelector_->setItemData(modelSelector_->count() - 1,
                                    QString::fromStdString(profile.display_name), Qt::ToolTipRole);
        if (id == active) activeIndex = modelSelector_->count() - 1;
    }
    if (activeIndex >= 0) modelSelector_->setCurrentIndex(activeIndex);
    suppressModelSelector_ = false;
}

// ---------------------------------------------------------------------------
// Conversation
// ---------------------------------------------------------------------------
void MainWindow::newConversation() {
    if (controller_->busy()) {
        announce(tr("Stop the agent before starting a new conversation."));
        return;
    }
    if (!persistConversation()) return;
    if (auto created = application_.start_conversation(tr("New conversation").toStdString());
        !created) {
        reportStorageFailure(tr("Start conversation"), created.error());
        return;
    }
    chat_->clear();
    timeline_->clear();
    task_->clear();
    verification_->clear();
    status_->clearOutcome();
    output_->clearCommandOutput();
    application_.workspace().clear_agent_changes();
    selectTaskToolScope({});
    refreshGitState();
    announce(tr("New conversation."));
    taskSummary_->setText(tr("Ready"));
    refreshConversations();
}

void MainWindow::refreshConversations() {
    const QSignalBlocker blocker(conversations_);
    conversations_->clear();
    if (!application_.workspace().is_open()) return;
    const auto workspace = application_.workspace_repository().find_by_path(
        path_to_utf8(application_.workspace().root()));
    if (!workspace || !workspace->has_value()) return;
    const auto recent = application_.conversation_repository().recent((*workspace)->id, 50);
    if (!recent) return;
    for (const auto& record : *recent) {
        auto* item = new QListWidgetItem(QString::fromStdString(record.title), conversations_);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(record.id));
        item->setToolTip(item->text());
        if (record.id == application_.current_conversation()) conversations_->setCurrentItem(item);
    }
}

void MainWindow::deleteConversation(std::int64_t id) {
    if (controller_->busy() || closePending_) {
        announce(tr("Stop the agent before deleting a chat."));
        return;
    }
    const auto record = application_.conversation_repository().find(id);
    if (!record) {
        reportStorageFailure(tr("Delete chat"), record.error());
        return;
    }
    if (!record->has_value()) {
        refreshConversations();
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Delete chat"),
        tr("Permanently delete \"%1\" and its messages? Project files will be kept.")
            .arg(QString::fromStdString((*record)->title)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    // Another UI action can run while the confirmation dialog processes events.
    if (controller_->busy() || closePending_) {
        announce(tr("Stop the agent before deleting a chat."));
        return;
    }
    const bool active = id == application_.current_conversation();
    if (auto deleted = application_.delete_conversation(id); !deleted) {
        reportStorageFailure(tr("Delete chat"), deleted.error());
        return;
    }
    if (active) {
        chat_->clear();
        timeline_->clear();
        task_->clear();
        verification_->clear();
        status_->clearOutcome();
        output_->clearCommandOutput();
        application_.workspace().clear_agent_changes();
        selectTaskToolScope({});
        taskSummary_->setText(tr("Ready"));
    }
    refreshConversations();
    announce(tr("Chat deleted."));
}

void MainWindow::switchConversation() {
    if (controller_->busy()) return;
    auto* selected = conversations_->currentItem();
    if (!selected) return;
    const auto id = selected->data(Qt::UserRole).toLongLong();
    if (id == application_.current_conversation()) return;
    if (!persistConversation()) return;
    if (auto loaded = application_.load_conversation(id); !loaded) {
        reportStorageFailure(tr("Open conversation"), loaded.error());
        refreshConversations();
        return;
    }
    selectTaskToolScope({});
    chat_->clear();
    timeline_->clear();
    task_->clear();
    verification_->clear();
    status_->clearOutcome();
    taskSummary_->setText(tr("Previous conversation"));
    output_->clearCommandOutput();
    appendCurrentConversation();
    refreshConversations();
    centreTabs_->setCurrentWidget(chat_);
}

void MainWindow::submitPrompt(const QString& text) {
    const QString message = text.trimmed();
    if (message.isEmpty()) return;
    if (closePending_) {
        announce(tr("Waiting for the current task to stop before closing."));
        return;
    }
    if (modelCancel_) {
        announce(tr("Wait for the model operation before sending a request."));
        return;
    }

    if (controller_->busy()) {
        announce(tr("Already working. Press Stop to interrupt."));
        return;
    }
    if (!application_.backends().current_backend()) {
        chat_->addMessage(ChatRole::Error,
                          tr("No model is loaded. Choose one from the toolbar, or add a GGUF "
                             "file with **File > Add GGUF model**."));
        return;
    }
    if (!application_.workspace().is_open()) {
        chat_->addMessage(ChatRole::Error,
                          tr("No workspace is open. Use **File > Open project folder** or "
                             "**File > Work without a project** to create a dated output folder."));
        return;
    }

    if (application_.current_conversation() == 0) {
        auto created = application_.start_conversation(message.left(80).toStdString());
        if (!created) {
            reportStorageFailure(tr("Start conversation"), created.error());
            return;
        }
    }

    auto runConfig = application_.agent().config();
    runConfig.request_mode = static_cast<RequestMode>(requestMode_->currentData().toInt());
    application_.agent().set_config(runConfig);
    composer_->clearPrompt();
    pendingDraft_.clear();
    if (auto saved = application_.checkpoint_session(toStdStrings(editor_->openFiles()), {});
        !saved)
        reportStorageFailure(tr("Save draft"), saved.error());

    chat_->addMessage(ChatRole::User, message);
    centreTabs_->setCurrentWidget(chat_);

    selectTaskToolScope(message);
    controller_->send(message);
}

void MainWindow::selectTaskToolScope(const QString& request) {
    fileToolsOnlyTask_ = TaskPolicy::requests_file_tools_only(request.toStdString());
    ++gitRefreshGeneration_;
    gitRefreshCancel_.cancel();
    if (fileToolsOnlyTask_) {
        diff_->clear();
        task_->setGitSummary({}, 0, false);
    } else {
        gitRefreshCancel_.reset();
    }
}

void MainWindow::stopRun() {
    if (!controller_->busy()) return;
    controller_->stop();
    announce(tr("Stopping..."));
}

void MainWindow::saveDraft(const QString& text) {
    pendingDraft_ = text;
    if (draftTimer_) draftTimer_->start();
}

// ---------------------------------------------------------------------------
// Agent events
// ---------------------------------------------------------------------------
void MainWindow::onRunStarted(const QString& message) {
    selectTaskToolScope(message);
    timeline_->addNote(tr("Request started"), TimelineEntry::State::Info);
    status_->setActivity(tr("Working"));
    taskSummary_->setText(tr("Working"));
    persistTerminalNote(tr("Interrupted before a terminal result was recorded."));
}

void MainWindow::onPromptProcessing() {
    chat_->setActivityNote(tr("Processing the prompt..."));
    status_->setActivity(tr("Processing prompt"));
}

void MainWindow::onContentDelta(const QString& text) {
    if (text.isEmpty()) {
        chat_->clearActivityNote();
        status_->setActivity(tr("Generating"));
        return;
    }
    if (!chat_->streaming()) chat_->beginAssistantMessage();
    chat_->appendDelta(text);
}

void MainWindow::onReasoningDelta(const QString& text) {
    if (!chat_->streaming()) chat_->beginAssistantMessage();
    chat_->appendReasoning(text);
}

void MainWindow::onToolStarted(const UiToolEvent& event) {
    if (chat_->streaming()) chat_->endAssistantMessage();
    runningToolRow_ = timeline_->beginEntry(event.name);
    taskSummary_->setText(tr("Working — %1").arg(event.name));

    if (event.name == "run_command" || event.name == "run_artifact") {
        output_->beginCommand(event.arguments);
        outputPanelAction_->setChecked(true);
    }
}

void MainWindow::onToolFinished(const UiToolEvent& event) {
    if (runningToolRow_ >= 0) {
        timeline_->completeEntry(runningToolRow_, event.ok, event.summary, event.detail,
                                 event.durationMs, event.logPath);
        runningToolRow_ = -1;
    }
    if (event.name == "run_command" || event.name == "run_artifact") {
        output_->endCommand(event.exitCode, event.durationMs);
    }
    // Any tool that touches files may have changed the tree.
    static const QStringList kMutating = {"write_file",   "replace_in_file", "apply_patch",
                                          "move_file",    "copy_file",       "create_directory",
                                          "download_file"};
    if (kMutating.contains(event.name)) refreshGitState();
}

void MainWindow::onToolOutput(const QString& chunk, bool isStderr) {
    output_->appendCommandOutput(chunk, isStderr);
}

void MainWindow::onToolProgress(const QString& note) {
    chat_->setActivityNote(note);
    status_->setActivity(note);
}

void MainWindow::onContextUpdated(const UiContext& context, const QString& note) {
    status_->setContext(context.used, context.total, context.exact);
    if (!note.isEmpty()) {
        timeline_->addNote(note, TimelineEntry::State::Warning);
    }
}

void MainWindow::onGenerationStats(const UiTurnStats& stats) {
    ChatView::TurnStatsView view;
    view.usageFromServer = stats.usageFromServer;
    view.completionTokens = stats.completionTokens;
    view.timingsFromServer = stats.timingsFromServer;
    view.promptTokensEvaluated = stats.promptTokensEvaluated;
    view.promptTokensPerSecond = stats.promptTokensPerSecond;
    view.promptMs = stats.promptMs;
    view.decodeMs = stats.decodeMs;
    view.cachedPromptTokens = stats.cachedPromptTokens;
    view.decodeTokensPerSecond = stats.decodeTokensPerSecond;
    view.reasoningCharacters = stats.reasoningCharacters;
    view.estimatedReasoningTokens = stats.estimatedReasoningTokens;
    view.observedReasoningStreamMs = stats.observedReasoningStreamMs;
    view.reasoningDeltaCount = stats.reasoningDeltaCount;
    chat_->setTurnStats(view);

    // The strip shows decode rate, which is the one that describes the model.
    status_->setTokenRate(stats.decodeTokensPerSecond, stats.timingsFromServer);

    if (stats.reasoningCharacters > 0) {
        timeline_->addNote(tr("Reasoned for ~%1 tokens, then answered at %2 tok/s")
                               .arg(stats.estimatedReasoningTokens)
                               .arg(stats.decodeTokensPerSecond, 0, 'f', 1),
                           TimelineEntry::State::Info);
    }
}

void MainWindow::onProgressWarning(const QString& guidance) {
    timeline_->addNote(tr("No progress — redirecting"), TimelineEntry::State::Warning);
    chat_->addMessage(ChatRole::Notice, guidance);
}

void MainWindow::onFinalizationStarted(const QString& note, bool retry) {
    // A status line and a timeline note, not a transcript message. This is the
    // run tidying up after itself; it is worth being able to see, and not worth
    // a paragraph in the conversation.
    status_->setActivity(retry ? tr("Final response retry...") : tr("Finalizing result..."));
    taskSummary_->setText(retry ? tr("Retrying the final response") : tr("Preparing the result"));
    chat_->setActivityNote(retry ? tr("Asking once more for a final answer")
                                 : tr("Writing the final summary"));
    timeline_->addNote(retry ? tr("Finalization retry") : tr("Finalizing"),
                       TimelineEntry::State::Running);
    log_debug(LogChannel::App, "finalization started", {{"note", note.toStdString()}});
}

void MainWindow::onTaskStateUpdated(const QString& block) {
    task_->setTaskBlock(block);
}

void MainWindow::onRunFinished(const UiOutcome& outcome) {
    if (chat_->streaming()) chat_->endAssistantMessage();
    chat_->clearActivityNote();
    status_->setActivity({});
    taskSummary_->setText(
        outcome.objectiveVerified
            ? (outcome.completed ? tr("Verified") : tr("Verified — no summary"))
            : (outcome.completed ? tr("Answered") : tr("Stopped — not verified")));
    taskSummary_->setToolTip(outcome.headline);

    // Three outcomes, and the middle one is why this signal carries a struct
    // rather than a word. Verified engineering whose summary never arrived is
    // not a failed task, and reporting it as one throws away the more useful
    // half of what the run established.
    //
    // The wording comes from the engine's own describe_outcome, so this window
    // and the headless tool cannot drift into calling the same result different
    // things.
    if (outcome.completed) {
        chat_->setFinalMessage(outcome.finalMessage);
        const QString finished =
            outcome.objectiveVerified ? tr("Finished - verified") : tr("Answered");
        timeline_->addNote(outcome.unsupportedClaims.isEmpty()
                               ? finished
                               : tr("%1, with unsupported claims").arg(finished),
                           outcome.unsupportedClaims.isEmpty() ? TimelineEntry::State::Succeeded
                                                               : TimelineEntry::State::Warning);
    } else if (outcome.objectiveVerified) {
        QString note = QString("**%1**").arg(outcome.headline);
        if (outcome.finalizationGatedOff) {
            // Withheld deliberately. Calling this a failure would blame the
            // guard for doing its job.
            note +=
                tr("\n\nLocalCode does not write a summary for work it cannot fully "
                   "stand behind, so the account was withheld rather than guessed at.");
        } else {
            note +=
                tr("\n\nEverything asked for was carried out and checked - the "
                   "Verification panel holds the evidence. What failed is the "
                   "write-up of it.");
            if (outcome.finalizationAttempts > 0) {
                note +=
                    tr("\n\nAsked %n more time(s) for one; the model returned reasoning "
                       "and no answer.",
                       nullptr, outcome.finalizationAttempts);
            }
        }
        if (!outcome.unsettledCriteria.isEmpty()) {
            note += tr("\n\nNever checked:\n-  %1").arg(outcome.unsettledCriteria.join("\n-  "));
        }
        if (!outcome.disputedCriteria.isEmpty()) {
            note += tr("\n\nDisputed by an independent check:\n-  %1")
                        .arg(outcome.disputedCriteria.join("\n-  "));
        }
        chat_->addMessage(ChatRole::Notice, note);
        timeline_->addNote(tr("Verified - no summary"), TimelineEntry::State::Warning);
    } else {
        // The objective was not established. The engine usually already said so
        // as it happened; repeating it turns one problem into two messages.
        if (!outcome.alreadyReported && !outcome.headline.isEmpty()) {
            QString note = outcome.headline;
            if (!outcome.unsettledCriteria.isEmpty()) {
                note +=
                    tr("\n\nStill unchecked:\n-  %1").arg(outcome.unsettledCriteria.join("\n-  "));
            }
            chat_->addMessage(ChatRole::Error, note);
        }
        timeline_->addNote(tr("Stopped - not verified"), TimelineEntry::State::Failed);
    }

    // The engineering verdict is shown either way - that is the point.
    task_->setVerification(outcome.taskVerification, outcome.unsupportedClaims,
                           outcome.objectiveVerified, outcome.hadPlan);
    // A run with no acceptance criteria claimed nothing, so there is no
    // verdict to show. Labelling an ordinary question "not verified" reads as a
    // problem and teaches people to ignore the label when it matters.
    if (outcome.hadPlan || outcome.objectiveVerified || outcome.presentationFailed) {
        status_->setOutcome(outcome.objectiveVerified, outcome.presentation,
                            outcome.presentationFailed);
    } else {
        status_->clearOutcome();
    }
    persistConversation();
    persistTerminalNote(outcome.completed ? tr("Answered.") : outcome.headline);
    refreshGitState();
}

void MainWindow::onRunFailed(const QString& reason) {
    taskSummary_->setText(tr("Stopped"));
    taskSummary_->setToolTip(reason);
    if (chat_->streaming()) chat_->endAssistantMessage();
    chat_->clearActivityNote();
    chat_->addMessage(ChatRole::Error, reason);
    timeline_->addNote(tr("Stopped"), TimelineEntry::State::Failed);
    status_->setActivity({});
    persistConversation();
    persistTerminalNote(tr("Stopped: %1").arg(reason));
}

void MainWindow::onRunCancelled() {
    taskSummary_->setText(tr("Cancelled"));
    if (chat_->streaming()) chat_->endAssistantMessage();
    chat_->clearActivityNote();
    chat_->addMessage(ChatRole::Notice, tr("Cancelled."));
    timeline_->addNote(tr("Cancelled"), TimelineEntry::State::Warning);
    status_->setActivity({});
    persistConversation();
    persistTerminalNote(tr("Cancelled."));
    refreshGitState();
}

void MainWindow::onBusyChanged(bool busy) {
    if (busy) {
        task_->clearVerification();
        status_->clearOutcome();
    }
    // "Stopping..." outlived the stop by eight seconds, so the status line
    // still said it while the transcript said "Cancelled" and the indicator
    // said "Ready". A transient message about work in flight has no business
    // surviving the work.
    if (!busy) statusBar()->clearMessage();
    const bool unavailable = busy || closePending_;
    requestMode_->setEnabled(!unavailable);
    sendButton_->setEnabled(!unavailable && !modelCancel_);
    stopButton_->setEnabled(busy);
    composer_->setBusy(unavailable);
    modelSelector_->setEnabled(!unavailable);
    newConversationAction_->setEnabled(!unavailable);
    outputWorkspaceAction_->setEnabled(!unavailable);
    conversations_->setEnabled(!unavailable);
    if (!busy) status_->setActivity({});
}

bool MainWindow::persistConversation() {
    // The engine holds the authoritative history; mirror it into storage so the
    // conversation survives a crash or a restart.
    //
    // Only the part that is not stored yet. This used to append every message
    // after every turn, so six real messages became fourteen rows and a restored
    // conversation showed its early turns four times over.
    const auto saved = application_.persist_conversation(application_.agent().history());
    if (!saved) {
        reportStorageFailure(tr("Save conversation"), saved.error());
        return false;
    }
    refreshConversations();
    return true;
}

void MainWindow::reportStorageFailure(const QString& operation, const Error& error) {
    const auto note = tr("%1 failed: %2").arg(operation, QString::fromStdString(error.message));
    announce(note);
    log_error(LogChannel::App, note.toStdString(), {{"detail", error.detail}});
    timeline_->addNote(note, TimelineEntry::State::Failed);
}

void MainWindow::persistTerminalNote(const QString& text) {
    const auto conversation = application_.current_conversation();
    if (conversation == 0) return;
    auto status = application_.app_state_repository().set(
        "conversation.terminal." + std::to_string(conversation), text.toStdString());
    if (!status) reportStorageFailure(tr("Save run outcome"), status.error());
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------
void MainWindow::openFileFromPath(const QString& path, int line) {
    QString absolute = path;
    if (QFileInfo(path).isRelative()) {
        const auto root = application_.workspace().root();
        if (!root.empty()) {
            absolute =
                QString::fromStdString(path_to_utf8(root / path_from_utf8(path.toStdString())));
        }
    }
    editor_->openFile(absolute, line);
    centreTabs_->setCurrentWidget(editor_);
}

void MainWindow::showSettings() {
    SettingsDialog dialog(application_, this);
    connect(&dialog, &SettingsDialog::settingsApplied, this, [this] {
        const auto settings = application_.settings();
        Theme::instance().setMode(settings.theme == "light" ? ThemeMode::Light : ThemeMode::Dark);
        Theme::instance().setMonoPointSize(settings.editor_font_size);
        Theme::instance().apply();
        chat_->setShowReasoning(settings.show_reasoning);
        chat_->refreshTheme();
        repository_->refreshDisplay();
        verification_->refreshTheme();
        editor_->refreshTheme();
        models_->refresh();
        announce(tr("Settings saved."));
    });
    dialog.exec();
}

void MainWindow::announce(const QString& text) {
    statusBar()->showMessage(text, 8000);
}

void MainWindow::updateWindowTitle() {
    QStringList parts;
    if (application_.workspace().is_open()) {
        parts << QString::fromStdString(application_.workspace().display_name());
    }
    const auto model = application_.backends().current_profile();
    if (!model.display_name.empty()) parts << QString::fromStdString(model.display_name);
    parts << "LocalCode";
    setWindowTitle(parts.join(" — "));
}

void MainWindow::restoreSession() {
    const auto recovered = application_.recover_session();
    if (!recovered) {
        const auto message = tr("Session restoration failed: %1")
                                 .arg(QString::fromStdString(recovered.error().message));
        chat_->addMessage(ChatRole::Notice, message);
        announce(message);
        return;
    }
    const auto& session = *recovered;

    // A first run has the flag unset too, so the notice is only useful when
    // there was genuinely a previous session to restore.
    const bool had_previous_session =
        !session.workspace_path.empty() || session.conversation_id > 0 || !session.draft.empty();

    if (!session.workspace_path.empty()) {
        const QString path = QString::fromStdString(session.workspace_path);
        if (QFileInfo::exists(path)) applyWorkspace(path);
    }

    if (session.conversation_id > 0) {
        if (application_.load_conversation(session.conversation_id)) {
            appendCurrentConversation();
        }
    }

    for (const auto& file : session.open_files) {
        const QString path = QString::fromStdString(file);
        if (QFileInfo::exists(path)) editor_->openFile(path);
    }

    if (!session.draft.empty()) {
        composer_->setPlainText(QString::fromStdString(session.draft));
    }

    if (session.had_unclean_shutdown && had_previous_session) {
        chat_->addMessage(
            ChatRole::Notice,
            tr("LocalCode did not shut down cleanly last time. Your workspace, conversation and "
               "unsent prompt have been restored."));
    }

    refreshConversations();
    updateModelSelector();
    updateWindowTitle();
}

void MainWindow::appendCurrentConversation() {
    QList<ChatView::TranscriptMessage> transcript;
    for (const auto& message : application_.agent().history()) {
        const auto role = message.is_application_feedback() ? ChatRole::Notice
                          : message.role == Role::User      ? ChatRole::User
                          : message.role == Role::Assistant ? ChatRole::Assistant
                                                            : ChatRole::System;
        if (message.role == Role::Tool) continue;   // tool traffic is not chat
        if (message.content.empty() && message.reasoning_content.empty()) continue;
        transcript.push_back({role, QString::fromStdString(message.content),
                              QString::fromStdString(message.reasoning_content)});
    }
    chat_->restoreMessages(transcript);
    controller_->restoreTaskDisplay();
    if (auto history = application_.tool_history_repository().for_conversation(
            application_.current_conversation())) {
        for (auto entry = history->rbegin(); entry != history->rend(); ++entry) {
            const auto row = timeline_->beginEntry(QString::fromStdString(entry->tool_name));
            timeline_->completeEntry(row, entry->succeeded, QString::fromStdString(entry->summary),
                                     QString::fromStdString(entry->detail), entry->duration_ms);
        }
    }
    const auto terminal = application_.app_state_repository().get(
        "conversation.terminal." + std::to_string(application_.current_conversation()));
    if (terminal && !terminal->empty()) {
        const auto text = tr("Previous run: %1").arg(QString::fromStdString(*terminal));
        chat_->addMessage(ChatRole::Notice, text);
        timeline_->addNote(text, TimelineEntry::State::Info);
    }
}

void MainWindow::saveLayout() {
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    settings.setValue("splitter/outer",
                      pluginMode_ ? codingOuterState_ : outerSplitter_->saveState());
    settings.setValue("splitter/centre",
                      pluginMode_ ? codingCentreState_ : centreSplitter_->saveState());
    settings.setValue("splitter/right", rightSplitter_->saveState());
    settings.setValue("tabs/centre", centreTabs_->currentIndex());
    settings.setValue("tabs/right", rightTabs_->currentIndex());
    settings.setValue("layout/version", 2);
    settings.setValue("panels/project", projectPanelAction_->isChecked());
    settings.setValue("panels/inspector", inspectorPanelAction_->isChecked());
    settings.setValue("panels/output", outputPanelAction_->isChecked());
    settings.setValue("tabs/project", projectTabs_->currentIndex());
}

void MainWindow::clampToAvailableScreen() {
    // A saved geometry is a promise about a display that may not be there
    // any more. The arithmetic is shared with the dialogs and tested in core.
    clampToScreen(*this);
}

void MainWindow::restoreLayout() {
    QSettings settings;
    if (settings.contains("window/geometry")) {
        restoreGeometry(settings.value("window/geometry").toByteArray());
        restoreState(settings.value("window/state").toByteArray());
        clampToAvailableScreen();
    }
    if (settings.value("layout/version", 0).toInt() == 2 && settings.contains("splitter/outer")) {
        outerSplitter_->restoreState(settings.value("splitter/outer").toByteArray());
        centreSplitter_->restoreState(settings.value("splitter/centre").toByteArray());
        rightSplitter_->restoreState(settings.value("splitter/right").toByteArray());
    }
    centreTabs_->setCurrentIndex(settings.value("tabs/centre", 0).toInt());
    rightTabs_->setCurrentIndex(settings.value("tabs/right", 0).toInt());
    projectTabs_->setCurrentIndex(settings.value("tabs/project", 0).toInt());
    projectPanelAction_->setChecked(settings.value("panels/project", true).toBool());
    inspectorPanelAction_->setChecked(settings.value("panels/inspector", false).toBool());
    outputPanelAction_->setChecked(settings.value("panels/output", false).toBool());
    updatePanelVisibility();
}

void MainWindow::updatePanelVisibility() {
    if (!projectPanelAction_ || !inspectorPanelAction_ || !outputPanelAction_) return;
    const bool plugin = centreTabs_->currentIndex() == 3;
    if (plugin && !pluginMode_) {
        codingOuterState_ = outerSplitter_->saveState();
        codingCentreState_ = centreSplitter_->saveState();
    } else if (!plugin && pluginMode_) {
        outerSplitter_->restoreState(codingOuterState_);
        centreSplitter_->restoreState(codingCentreState_);
    }
    pluginMode_ = plugin;
    projectTabs_->setVisible(!plugin && projectPanelAction_->isChecked());
    rightSplitter_->setVisible(!plugin && inspectorPanelAction_->isChecked());
    output_->setVisible(!plugin && outputPanelAction_->isChecked());
    taskSummary_->parentWidget()->setVisible(!plugin);
    projectPanelAction_->setEnabled(!plugin);
    inspectorPanelAction_->setEnabled(!plugin);
    outputPanelAction_->setEnabled(!plugin);
    if (auto* toolbar = findChild<QToolBar*>("CodingToolbar")) toolbar->setVisible(!plugin);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    // Again here, and posted. restoreLayout() clamps in the constructor, but at
    // that point the window has no frame and the layout has not yet grown it to
    // its minimum, so a geometry that fitted when it was checked can be over the
    // edge by the time it is on screen - which is how an 8 px overhang survived
    // a clamp that had already run.
    QTimer::singleShot(0, this, [this] { clampToScreen(*this); });

    if (layoutApplied_) return;
    layoutApplied_ = true;

    QSettings settings;
    if (settings.value("layout/version", 0).toInt() == 2 && settings.contains("splitter/outer"))
        return;

    // Posted rather than applied inline: at showEvent time the splitter has not
    // been given its final geometry, so setSizes() would be scaled against a
    // placeholder width and the side columns would come out wrong.
    QTimer::singleShot(0, this, [this] {
        const int total = outerSplitter_->width();
        if (total < 600) return;
        outerSplitter_->setSizes({220, std::max(300, total - 500), 280});
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (closePending_) {
        event->ignore();
        return;
    }
    if (controller_->busy()) {
        const auto answer = QMessageBox::question(this, tr("The agent is still working"),
                                                  tr("Stop it and close LocalCode?"),
                                                  QMessageBox::Yes | QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        // The task can finish while the confirmation dialog processes events.
        if (controller_->busy()) {
            closePending_ = true;
            closeWaitTimer_->start();
            controller_->stop();
            announce(tr("Stopping the task before saving and closing..."));
            event->ignore();
            return;
        }
    }

    if (editor_->hasUnsavedChanges()) {
        const auto answer = QMessageBox::question(
            this, tr("Unsaved changes"),
            tr("Some open files have unsaved edits. Save them before closing?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save) {
            QString error;
            if (!editor_->saveAll(&error)) {
                QMessageBox::warning(this, tr("Save failed"), error);
                event->ignore();
                return;
            }
        }
    }

    saveLayout();
    if (!persistConversation()) {
        QMessageBox::warning(this, tr("Conversation was not saved"),
                             tr("LocalCode could not save the conversation. The window remains "
                                "open so you can preserve it."));
        event->ignore();
        return;
    }
    const auto saved = application_.checkpoint_session(toStdStrings(editor_->openFiles()),
                                                       pendingDraft_.toStdString());
    if (!saved) {
        QMessageBox::warning(this, tr("Session was not saved"),
                             QString::fromStdString(saved.error().to_string()));
        event->ignore();
        return;
    }
    if (auto clean = application_.mark_clean_shutdown(); !clean) {
        reportStorageFailure(tr("Record clean shutdown"), clean.error());
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

}   // namespace localcode::gui
