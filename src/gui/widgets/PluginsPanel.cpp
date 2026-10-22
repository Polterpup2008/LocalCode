#include "PluginsPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include "dialogs/InferenceTuningDialog.h"
#include "dialogs/ResourceSetupDialog.h"
#include "platform/Paths.h"
#include "platform/PathEncoding.h"
#include "plugins/DiscordPlugin.h"

namespace localcode::gui {
PluginsPanel::PluginsPanel(QWidget* parent) : QWidget(parent), plugin_(new DiscordPlugin(this)) {
    setObjectName("PluginsPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12); layout->setSpacing(10);
    auto* heading = new QLabel(tr("LocalCode Discord"), this);
    heading->setObjectName("TaskSummary");
    auto headingFont = heading->font(); headingFont.setPointSize(18); headingFont.setBold(true); heading->setFont(headingFont);
    auto* header = new QHBoxLayout;
    header->addWidget(heading, 1);
    install_ = new QPushButton(tr("Install plugin…"), this);
    install_->setObjectName("InstallDiscordPlugin"); header->addWidget(install_);
    layout->addLayout(header);
    auto* description = new QLabel(tr("Configure a private bot, its local model and its group persona. Each named bot has its own data."), this);
    description->setWordWrap(true); layout->addWidget(description);
    auto* row = new QHBoxLayout;
    instance_ = new QComboBox(this); instance_->setEditable(true);
    instance_->setObjectName("DiscordInstance"); instance_->addItem(plugin_->instanceName());
    select_ = new QPushButton(tr("Open / create bot"), this);
    row->addWidget(new QLabel(tr("Bot"), this)); row->addWidget(instance_, 1); row->addWidget(select_);
    layout->addLayout(row);
    status_ = new QLabel(this); status_->setObjectName("DiscordStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    layout->addWidget(status_);
    home_ = new QLabel(plugin_->home(), this); home_->setWordWrap(true); home_->setTextFormat(Qt::PlainText); home_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(home_);
    tabs_ = new QTabWidget(this); layout->addWidget(tabs_, 1);
    auto page = [this](const QString& title) {
        auto* scroll = new QScrollArea(tabs_); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scroll); auto* form = new QFormLayout(content);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(content); tabs_->addTab(scroll, title); return form;
    };
    auto* connection = page(tr("Connection"));
    auto line = [this](QFormLayout* form, const QString& title, const QString& name) {
        auto* field = new QLineEdit(this); field->setObjectName(name); form->addRow(title, field); return field;
    };
    token_ = line(connection, tr("Bot token"), "DiscordToken"); token_->setEchoMode(QLineEdit::Password); token_->setMaxLength(256);
    token_->setPlaceholderText(tr("Paste once; leave empty to keep the saved token"));
    tokenState_ = new QLabel(this); connection->addRow(tokenState_);
    guilds_ = line(connection, tr("Server IDs"), "DiscordGuilds");
    owners_ = line(connection, tr("Owner IDs"), "DiscordOwners");
    channels_ = line(connection, tr("Channel IDs"), "DiscordChannels");
    for (auto* field : {guilds_, owners_, channels_}) field->setPlaceholderText(tr("Decimal IDs, separated by commas"));
    auto* help = new QLabel(tr("Enable Message Content Intent in the Discord developer portal. Only configured servers and channels are read."), this);
    help->setWordWrap(true); connection->addRow(help);
    auto* portal = new QPushButton(tr("Open Discord developer portal"), this); connection->addRow(portal);
    connect(portal, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl("https://discord.com/developers/applications")); });
    auto* modelForm = page(tr("Model"));
    auto fileField = [this, line, modelForm](const QString& title, const QString& name, const QString& filter) {
        auto* holder = new QWidget(this); auto* box = new QHBoxLayout(holder); box->setContentsMargins(0, 0, 0, 0);
        auto* field = new QLineEdit(holder); field->setObjectName(name);
        auto* browse = new QPushButton(tr("Browse…"), holder); box->addWidget(field, 1); box->addWidget(browse); modelForm->addRow(title, holder);
        connect(browse, &QPushButton::clicked, this, [this, field, filter] {
            const auto path = QFileDialog::getOpenFileName(this, tr("Select local file"), field->text(), filter);
            if (!path.isEmpty()) field->setText(path);
        }); return field;
    };
    runtime_ = fileField(tr("llama-server"), "DiscordRuntime", tr("Runtime (llama-server*);;All files (*)"));
    model_ = fileField(tr("GGUF model"), "DiscordModel", tr("GGUF (*.gguf)"));
    mmproj_ = fileField(tr("Vision projector (optional)"), "DiscordMmproj", tr("GGUF (*.gguf)"));
    auto* setup = new QPushButton(tr("Set up runtime and model…"), this);
    setup->setObjectName("SetupDiscordModel");
    modelForm->addRow(setup);
    connect(setup, &QPushButton::clicked, this, [this] {
#ifdef Q_OS_WIN
        const auto runtimeRoot = QString::fromStdWString(app_paths().runtime_dir().native());
        const auto dataRoot = QString::fromStdWString(app_paths().data_dir().native());
#else
        const auto runtimeRoot=QString::fromStdString(path_to_utf8(app_paths().runtime_dir()));
        const auto dataRoot=QString::fromStdString(path_to_utf8(app_paths().data_dir()));
#endif
        ResourceSetupDialog dialog({runtime_->text(), model_->text(), mmproj_->text()}, runtimeRoot,
                                   QDir(dataRoot).filePath("models"), this);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto selected = dialog.selection();
        runtime_->setText(selected.runtime);
        model_->setText(selected.model);
        mmproj_->setText(selected.projector);
        if (selected.cpuStartingProfile) {
            context_->setValue(4096);
            layers_->setValue(0);
            if (tuningSupported_)
                tuning_ = QJsonObject{
                    {"threads", 0},       {"threads_batch", 0},      {"batch_size", 512},
                    {"ubatch_size", 128}, {"prompt_cache_mib", 128}, {"flash_attention", "auto"},
                    {"cache_type_k", ""}, {"cache_type_v", ""}};
        }
    });
    auto spin = [this, modelForm](const QString& title, const QString& name, int minimum, int maximum) {
        auto* field = new QSpinBox(this); field->setObjectName(name); field->setRange(minimum, maximum); modelForm->addRow(title, field); return field;
    };
    context_ = spin(tr("Context tokens"), "DiscordContext", 2048, 2147483647); context_->setSingleStep(1024);
    layers_ = spin(tr("Layers on GPU (0 = CPU)"), "DiscordLayers", 0, 999);
    reserve_ = spin(tr("Required free VRAM (MiB)"), "DiscordVram", 0, 262144);
    idle_ = spin(tr("Unload after idle seconds (0 = keep)"), "DiscordIdle", 0, 86400);
    preload_ = new QCheckBox(tr("Load model when the bot starts"), this); modelForm->addRow(preload_);
    asleep_ = new QCheckBox(tr("Start asleep; wake with /start"), this); modelForm->addRow(asleep_);
    tune_ = new QPushButton(tr("Performance settings…"), this);
    tune_->setObjectName("TuneDiscordModel");
    modelForm->addRow(tune_);
    connect(tune_, &QPushButton::clicked, this, [this] {
        auto input = tuning_;
        input.insert("contextSize", context_->value());
        input.insert("gpuLayers", layers_->value());
        if (!cpuPlacementModes_.isUndefined()) input.insert("cpuPlacementModes", cpuPlacementModes_);
        InferenceTuningDialog dialog(input, true, this);
        dialog.setBenchmarkFiles(model_->text(), runtime_->text());
        if (dialog.exec() != QDialog::Accepted) return;
        tuning_ = dialog.settings();
        context_->setValue(tuning_.take("contextSize").toInt());
        layers_->setValue(tuning_.take("gpuLayers").toInt());
    });
    auto* guidance = new QLabel(tr("For a small PC, start with a small GGUF and 4K–8K context. GPU layers and runtime must match its hardware. These settings are tuning controls; performance gains still need measurement."), this);
    guidance->setWordWrap(true); modelForm->addRow(guidance);
    auto* conversationPage = new QWidget(tabs_);
    auto* conversationForm = new QFormLayout(conversationPage);
    auto historySpin = [conversationPage, conversationForm](const QString& title, const QString& name, int minimum, int maximum) {
        auto* field = new QSpinBox(conversationPage);
        field->setObjectName(name); field->setRange(minimum, maximum);
        conversationForm->addRow(title, field); return field;
    };
    historyMessages_ = historySpin(tr("Recent messages"), "DiscordHistoryMessages", 1, 4096);
    replyDepth_ = historySpin(tr("Quoted reply depth"), "DiscordReplyDepth", 0, 256);
    historyAge_ = historySpin(tr("History age in seconds"), "DiscordHistoryAge", 0, 31536000);
    historyBudget_ = historySpin(tr("History token budget (0 = automatic)"), "DiscordHistoryBudget", 0, 2147483647);
    auto* contextGuidance = new QLabel(tr("Choose the conversation history sent to the model. Quoted replies keep their authors and surrounding messages. The model window is configured in Model; Persona edits your group knowledge. Save while the bot is stopped, then start it."), conversationPage);
    contextGuidance->setWordWrap(true); conversationForm->addRow(contextGuidance);
    conversationNotes_ = new QPlainTextEdit(conversationPage);
    conversationNotes_->setObjectName("DiscordConversationNotes");
    conversationNotes_->setPlaceholderText(tr("Your editable group context and conversation style notes (up to 4096 characters)."));
    conversationForm->addRow(tr("Your context notes"), conversationNotes_);
    tabs_->addTab(conversationPage, tr("Conversation"));
    auto* personaPage = new QWidget(tabs_); auto* personaLayout = new QVBoxLayout(personaPage);
    language_ = new QComboBox(personaPage); language_->setEditable(true); language_->addItems({"french", "auto", "english"});
    personaLayout->addWidget(new QLabel(tr("Reply language"), personaPage)); personaLayout->addWidget(language_);
    persona_ = new QPlainTextEdit(personaPage); persona_->setObjectName("DiscordPersona"); personaLayout->addWidget(persona_, 1);
    tabs_->addTab(personaPage, tr("Persona"));
    log_ = new QPlainTextEdit(tabs_); log_->setObjectName("DiscordLog"); log_->setReadOnly(true); log_->setMaximumBlockCount(300);
    tabs_->addTab(log_, tr("Log"));
    auto* actions = new QHBoxLayout;
    refresh_ = new QPushButton(tr("Refresh status"), this); actions->addWidget(refresh_); actions->addStretch();
    save_ = new QPushButton(tr("Save settings"), this); save_->setObjectName("SaveDiscordSettings");
    start_ = new QPushButton(tr("Start bot"), this); start_->setObjectName("StartDiscordBot");
    stop_ = new QPushButton(tr("Stop bot"), this); stop_->setObjectName("StopDiscordBot");
    actions->addWidget(save_); actions->addWidget(start_); actions->addWidget(stop_); layout->addLayout(actions);
    auto* lifetime = new QLabel(tr("Closing LocalCode leaves the bot online. Stop it here before changing settings."), this);
    lifetime->setWordWrap(true); layout->addWidget(lifetime);
    poll_ = new QTimer(this); poll_->setInterval(5000);
    connect(poll_, &QTimer::timeout, this, &PluginsPanel::refresh);
    connect(refresh_, &QPushButton::clicked, this, &PluginsPanel::refresh);
    connect(install_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Install Discord plugin"), {}, tr("Plugin manifest (manifest.json)"));
        if (path.isEmpty()) return;
        const auto error = plugin_->installPackage(path);
        if (!error.isEmpty()) { status_->setText(error); return; }
        initialise();
    });
    connect(select_, &QPushButton::clicked, this, [this] {
        const auto error = plugin_->selectInstance(instance_->currentText().trimmed());
        if (!error.isEmpty()) { status_->setText(error); return; }
        loaded_ = false; running_ = false; token_->clear(); log_->clear(); logOffset_ = 0; initialise();
    });
    connect(save_, &QPushButton::clicked, this, [this] { plugin_->request("save", collectSettings()); });
    start_->setToolTip(tr("Save these settings, then connect this bot to Discord."));
    connect(start_, &QPushButton::clicked, this, [this] { startAfterSave_ = true; plugin_->request("save", collectSettings()); });
    for (auto* field : {guilds_, owners_, channels_, token_, runtime_, model_})
        connect(field, &QLineEdit::textChanged, this, [this] { updateControls(); });
    connect(stop_, &QPushButton::clicked, this, [this] { status_->setText(tr("Stopping the bot and releasing its model…")); plugin_->request("stop"); });
    connect(plugin_, &DiscordPlugin::busyChanged, this, [this] { updateControls(); });
    connect(plugin_, &DiscordPlugin::failed, this, [this](const QString& message) { startAfterSave_ = false; status_->setText(message); updateControls(); });
    connect(plugin_, &DiscordPlugin::started, this, [this](qint64 pid) {
        status_->setText(tr("Starting bot (process %1)…").arg(pid)); running_ = true; updateControls();
        QTimer::singleShot(1200, this, &PluginsPanel::refresh);
    });
    connect(plugin_, &DiscordPlugin::completed, this, [this](const QString& command, const QJsonObject& result) {
        if (command == "init") plugin_->request("settings");
        else if (command == "settings") { applySettings(result); refresh(); }
        else if (command == "save") {
            tokenConfigured_ = tokenConfigured_ || !token_->text().trimmed().isEmpty(); token_->clear();
            if (startAfterSave_) { startAfterSave_ = false; plugin_->start(); }
            else { status_->setText(tr("Saved. Start the bot to apply these settings.")); plugin_->request("settings"); }
        }
        else if (command == "stop") { running_ = false; status_->setText(tr("Stopped; owned model processes released.")); updateControls(); readLog(); }
        else if (command == "status") {
            running_ = result.value("running").toBool();
            if (running_) {
                const auto state = result.value("sleeping").toBool() ? tr("Asleep")
                    : (result.value("runtimeReady").toBool() ? tr("Online") : tr("Connecting"));
                status_->setText(tr("%1 · model: %2").arg(state, result.value("model").toString()));
            } else status_->setText(tr("Stopped. Configure and save this bot before starting it."));
            updateControls(); readLog();
        }
    });
    status_->setText(plugin_->available() ? tr("Open this panel to configure your bot.") : tr("Install the Discord plugin to create your first bot."));
    updateControls();
}

void PluginsPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!initialised_ && plugin_->available()) initialise();
    if (plugin_->available()) poll_->start();
}
void PluginsPanel::hideEvent(QHideEvent* event) { poll_->stop(); plugin_->releaseStatusClient(); QWidget::hideEvent(event); }
void PluginsPanel::initialise() {
    initialised_ = true; home_->setText(plugin_->home()); loaded_ = false;
    plugin_->request("init"); if (isVisible()) poll_->start();
}
void PluginsPanel::refresh() { if (loaded_ && !plugin_->busy()) plugin_->request("status"); }
void PluginsPanel::updateControls() {
    const bool free = !plugin_->busy(), available = plugin_->available();
    install_->setEnabled(free); select_->setEnabled(free && available); instance_->setEnabled(free && available);
    save_->setEnabled(free && loaded_ && !running_);
    const bool ready = (tokenConfigured_ || !token_->text().trimmed().isEmpty())
        && !guilds_->text().trimmed().isEmpty() && !owners_->text().trimmed().isEmpty()
        && !channels_->text().trimmed().isEmpty() && QFileInfo(runtime_->text()).isFile()
        && QFileInfo(model_->text()).isFile();
    start_->setEnabled(free && loaded_ && !running_ && ready);
    stop_->setEnabled(free && running_); refresh_->setEnabled(free && loaded_);
    // A read-only background status query must not interrupt typing. Actions
    // remain serialized, and init/settings/save/start/stop still lock the form.
    const bool editable = (free || plugin_->refreshingStatus()) && loaded_ && !running_;
    for (int i = 0; i < tabs_->count() - 1; ++i)
        tabs_->widget(i)->setEnabled(editable);
    // Opening pending settings is read-only with respect to the worker. Keep
    // this available during status polling, like the editable fields above.
    tune_->setEnabled(editable && tuningSupported_);
}
void PluginsPanel::applySettings(const QJsonObject& value) {
    cpuPlacementModes_ = value.value("cpuPlacementModes");
    tuningSupported_ = value.value("tuning").isObject();
    tuning_ = value.value("tuning").toObject();
    conversationSupported_ = value.value("conversation").isObject();
    conversationNotesSupported_ = value.value("conversationNotes").isString();
    conversationNotes_->setEnabled(conversationNotesSupported_);
    conversationNotes_->setPlainText(value.value("conversationNotes").toString());
    const auto conversation = value.value("conversation").toObject();
    historyMessages_->setValue(conversation.value("recentMessages").toInt(16));
    replyDepth_->setValue(conversation.value("replyChainDepth").toInt(6));
    historyAge_->setValue(conversation.value("historyMaxAgeS").toInt(900));
    historyBudget_->setValue(conversation.value("historyBudgetTokens").toInt(0));
    for (auto* field : {historyMessages_, replyDepth_, historyAge_, historyBudget_})
        field->setEnabled(conversationSupported_);
    guilds_->setText(value.value("guildIds").toString()); owners_->setText(value.value("ownerIds").toString()); channels_->setText(value.value("channelIds").toString());
    runtime_->setText(value.value("serverBinary").toString()); model_->setText(value.value("modelPath").toString()); mmproj_->setText(value.value("mmprojPath").toString());
    context_->setValue(value.value("contextSize").toInt()); layers_->setValue(value.value("gpuLayers").toInt()); reserve_->setValue(value.value("minFreeVramMib").toInt()); idle_->setValue(value.value("idleUnloadS").toInt());
    preload_->setChecked(value.value("preloadModel").toBool()); asleep_->setChecked(value.value("startAsleep").toBool());
    language_->setCurrentText(value.value("replyLanguage").toString()); persona_->setPlainText(value.value("persona").toString());
    tokenConfigured_ = value.value("tokenConfigured").toBool();
    tokenState_->setText(tokenConfigured_ ? tr("Token saved locally; its value is hidden.") : tr("This bot has no token yet."));
    loaded_ = true; updateControls();
}
QJsonObject PluginsPanel::collectSettings() const {
    QJsonObject out{{"guildIds", guilds_->text()},
                    {"ownerIds", owners_->text()},
                    {"channelIds", channels_->text()},
                    {"serverBinary", runtime_->text()},
                    {"modelPath", model_->text()},
                    {"mmprojPath", mmproj_->text()},
                    {"contextSize", context_->value()},
                    {"gpuLayers", layers_->value()},
                    {"minFreeVramMib", reserve_->value()},
                    {"idleUnloadS", idle_->value()},
                    {"preloadModel", preload_->isChecked()},
                    {"startAsleep", asleep_->isChecked()},
                    {"replyLanguage", language_->currentText()},
                    {"persona", persona_->toPlainText()},
                    {"token", token_->text()}};
    if (tuningSupported_) out.insert("tuning", tuning_);
    if (conversationSupported_) out.insert("conversation", QJsonObject{
        {"recentMessages", historyMessages_->value()}, {"replyChainDepth", replyDepth_->value()},
        {"historyMaxAgeS", historyAge_->value()}, {"historyBudgetTokens", historyBudget_->value()}});
    if (conversationNotesSupported_) out.insert("conversationNotes", conversationNotes_->toPlainText());
    return out;
}
void PluginsPanel::readLog() {
    QFile file(QDir(plugin_->home()).filePath("data/plugin.log"));
    if (!file.open(QIODevice::ReadOnly)) return;
    if (file.size() < logOffset_) { logOffset_ = 0; log_->clear(); }
    if (file.size() - logOffset_ > 64 * 1024) logOffset_ = file.size() - 64 * 1024;
    file.seek(logOffset_); const auto bytes = file.read(64 * 1024); logOffset_ = file.pos();
    if (!bytes.isEmpty()) log_->appendPlainText(QString::fromUtf8(bytes).trimmed());
}
}
