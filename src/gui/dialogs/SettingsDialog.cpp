#include "SettingsDialog.h"
#include <QShowEvent>
#include "../ScreenGuard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "../Theme.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"

namespace localcode::gui {
namespace {

QWidget* pathRow(QLineEdit* edit, QPushButton* button, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(edit, 1);
    layout->addWidget(button);
    return row;
}

QLabel* hint(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName("Faint");
    label->setWordWrap(true);
    return label;
}

}   // namespace

SettingsDialog::SettingsDialog(Application& application, QWidget* parent)
    : QDialog(parent), application_(application) {
    setWindowTitle(tr("Settings"));
    setModal(true);
    setMinimumSize(520, 420);
    resize(880, 680);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto* body = new QHBoxLayout();
    navigation_ = new QListWidget(this);
    navigation_->setObjectName("SettingsNavigation");
    navigation_->setFixedWidth(140);
    pages_ = new QStackedWidget(this);
    auto addPage = [this](const QString& title, QWidget* page) {
        for (auto* form : page->findChildren<QFormLayout*>())
            form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        navigation_->addItem(title);
        auto* scroll = new QScrollArea(pages_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        pages_->addWidget(scroll);
    };
    addPage(tr("General"), buildPathsPage());
    addPage(tr("Models"), buildBackendPage());
    addPage(tr("Agent"), buildAgentPage());
    addPage(tr("Repository"), buildRepositoryPage());
    addPage(tr("Tools"), buildWebPage());
    addPage(tr("Performance"), buildPerformancePage());
    addPage(tr("Appearance"), buildInterfacePage());
    addPage(tr("Advanced"), buildAdvancedPage());
    connect(navigation_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    navigation_->setCurrentRow(0);
    body->addWidget(navigation_);
    body->addWidget(pages_, 1);
    layout->addLayout(body, 1);
    QSettings saved;
    if (saved.contains("settings/geometry"))
        restoreGeometry(saved.value("settings/geometry").toByteArray());

    auto* buttons = new QDialogButtonBox(this);
    auto* ok = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    ok->setObjectName("Primary");
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    load();
}

SettingsDialog::~SettingsDialog() {
    runtimeCancel_.cancel();
    QSettings saved;
    saved.setValue("settings/geometry", saveGeometry());
}

QWidget* SettingsDialog::buildBackendPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    auto* runtimes = new QGroupBox(tr("Inference runtimes"), page);
    auto* form = new QFormLayout(runtimes);

    llamaSource_ = new QComboBox(runtimes);
    llamaSource_->setObjectName("LlamaRuntimeSource");
    llamaSource_->addItems({tr("LocalCode llama.cpp (default)"), tr("Custom llama.cpp")});
    form->addRow(tr("llama.cpp runtime"), llamaSource_);
    form->addRow(
        QString(),
        hint(tr("The default runtime is included with LocalCode. LocalCode "
                "starts and stops it when you load or unload a model. No server path is needed. "
                "Choose Custom llama.cpp only to use a different build."),
             runtimes));

    llamaServer_ = new QLineEdit(runtimes);
    llamaServer_->setObjectName("CustomLlamaServerPath");
    llamaServer_->setPlaceholderText(tr("Found on PATH or under the runtime directory"));
    auto* browseLlama = new QPushButton(tr("Browse..."), runtimes);
    connect(browseLlama, &QPushButton::clicked, this, &SettingsDialog::browseLlamaServer);
    customLlamaRow_ = pathRow(llamaServer_, browseLlama, runtimes);
    form->addRow(tr("Custom server"), customLlamaRow_);
    form->addRow(
        QString(),
        hint(tr("Included runtime: %1")
                 .arg(QString::fromStdString(path_to_utf8(AppPaths::bundled_llama_server()))),
             runtimes));
    connect(llamaSource_, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::updateRuntimeChoice);

    ikLlamaServer_ = new QLineEdit(runtimes);
    ikLlamaServer_->setPlaceholderText(tr("Optional"));
    auto* browseIk = new QPushButton(tr("Browse..."), runtimes);
    connect(browseIk, &QPushButton::clicked, this, &SettingsDialog::browseIkLlamaServer);
    form->addRow(tr("ik_llama server"), pathRow(ikLlamaServer_, browseIk, runtimes));

    defaultBackend_ = new QComboBox(runtimes);
    defaultBackend_->addItems({tr("llama.cpp"), tr("ik_llama.cpp")});
    form->addRow(tr("AUTO prefers"), defaultBackend_);
    form->addRow(QString(),
                 hint(tr("Used only when both runtimes are installed and a model profile is set "
                         "to AUTO. Neither runtime is faster in general - which one wins depends "
                         "on the model, the quantization and your hardware. Switch on a model's "
                         "profile and compare if you want to know."),
                      runtimes));
    layout->addWidget(runtimes);

    auto* discovery = new QGroupBox(tr("Discovered runtimes"), page);
    auto* discoveryLayout = new QVBoxLayout(discovery);
    discovered_ = new QPlainTextEdit(discovery);
    discovered_->setReadOnly(true);
    discovered_->setFont(Theme::instance().monoFont(9));
    discovered_->setMaximumHeight(110);
    discoveryLayout->addWidget(discovered_);

    auto* detect = new QPushButton(tr("Scan again"), discovery);
    connect(detect, &QPushButton::clicked, this, &SettingsDialog::detectRuntimes);
    discoveryLayout->addWidget(detect, 0, Qt::AlignLeft);
    layout->addWidget(discovery);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildAgentPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    auto* security = new QGroupBox(tr("What the agent may do"), page);
    auto* form = new QFormLayout(security);

    autonomy_ = new QComboBox(security);
    autonomy_->addItem(tr("Restricted — ask before anything unrecognised"));
    autonomy_->addItem(tr("Standard — run ordinary development commands"));
    autonomy_->addItem(tr("Full — run anything not explicitly blocked"));
    form->addRow(tr("Autonomy"), autonomy_);
    form->addRow(QString(),
                 hint(tr("This governs commands no rule recognises. Destructive operations - "
                         "formatting a disk, editing boot configuration, extracting credentials, "
                         "shutting the machine down - are blocked at every level and cannot be "
                         "enabled. Operations that destroy work, such as git reset --hard, always "
                         "ask first. File-edit tools check workspace paths. Command processes "
                         "currently run with your account's filesystem access."),
                      security));

    readOnly_ = new QCheckBox(tr("Read-only mode (inspect but never modify or run)"), security);
    form->addRow(readOnly_);
    layout->addWidget(security);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildPerformancePage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    auto* limits = new QGroupBox(tr("Limits"), page);
    auto* limitsForm = new QFormLayout(limits);

    maxIterations_ = new QSpinBox(limits);
    maxIterations_->setRange(1, 500);
    limitsForm->addRow(tr("Maximum steps per request"), maxIterations_);

    timeBudget_ = new QSpinBox(limits);
    timeBudget_->setRange(1, 480);
    timeBudget_->setSuffix(tr(" minutes"));
    limitsForm->addRow(tr("Time budget per request"), timeBudget_);
    limitsForm->addRow(
        QString(),
        hint(tr("A run also stops early when the agent stops making progress."), limits));

    streaming_ = new QCheckBox(tr("Stream responses as they are generated"), limits);
    limitsForm->addRow(streaming_);
    layout->addWidget(limits);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildAdvancedPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    auto* external = new QGroupBox(tr("Attach to a running server"), page);
    auto* externalForm = new QFormLayout(external);

    useExternal_ = new QCheckBox(tr("Use a server I started myself"), external);
    externalForm->addRow(useExternal_);

    externalUrl_ = new QLineEdit(external);
    externalUrl_->setPlaceholderText("http://127.0.0.1:8080");
    externalForm->addRow(tr("Base URL"), externalUrl_);

    externalKey_ = new QLineEdit(external);
    externalKey_->setEchoMode(QLineEdit::Password);
    externalKey_->setPlaceholderText(tr("Only if the server was started with --api-key"));
    externalForm->addRow(tr("API key"), externalKey_);
    externalForm->addRow(
        QString(),
        hint(tr("Leave this unchecked for normal use: LocalCode starts and stops its bundled "
                "or custom runtime for you. Check it only to connect to a server you already "
                "started. LocalCode will not start or stop that external server."),
             external));
    connect(useExternal_, &QCheckBox::toggled, externalUrl_, &QWidget::setEnabled);
    connect(useExternal_, &QCheckBox::toggled, externalKey_, &QWidget::setEnabled);
    layout->addWidget(external);

    auto* group = new QGroupBox(tr("Diagnostics"), page);
    auto* form = new QFormLayout(group);
    logLevel_ = new QComboBox(group);
    logLevel_->addItems({"trace", "debug", "info", "warn", "error"});
    form->addRow(tr("Log level"), logLevel_);
    layout->addWidget(group);
    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildRepositoryPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    auto* title = new QLabel(tr("Repository context"), page);
    title->setObjectName("PanelTitle");
    layout->addWidget(title);
    layout->addWidget(
        hint(tr("The index follows the open project. LocalCode uses it to find files "
                "and definitions, then reads source to check those leads. Index matches "
                "do not establish that a task is correct."),
             page));
    layout->addWidget(hint(tr("Open Repository in the project sidebar to see index state, refresh "
                              "changed files, reindex the project, or stop an active scan."),
                           page));
    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildWebPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    auto* search = new QGroupBox(tr("Search"), page);
    auto* form = new QFormLayout(search);

    webEnabled_ = new QCheckBox(tr("Allow the agent to search and read the web"), search);
    form->addRow(webEnabled_);

    provider_ = new QComboBox(search);
    provider_->addItems({"duckduckgo", "searxng", "brave"});
    provider_->setEditable(true);
    form->addRow(tr("Provider"), provider_);

    endpoint_ = new QLineEdit(search);
    endpoint_->setPlaceholderText(tr("https://your-instance/search?q={query}&format=json"));
    form->addRow(tr("Endpoint"), endpoint_);

    apiKey_ = new QLineEdit(search);
    apiKey_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("API key"), apiKey_);
    form->addRow(QString(),
                 hint(tr("DuckDuckGo needs no key and is used by default. {query} in the "
                         "endpoint is replaced with the URL-encoded search terms."),
                      search));
    layout->addWidget(search);

    auto* trust = new QGroupBox(tr("Trusted download sources"), page);
    auto* trustLayout = new QVBoxLayout(trust);
    trustLayout->addWidget(hint(tr("Executables and archives may only be downloaded from these "
                                   "hosts, and every download is confirmed. One host per line; "
                                   "these are added to the built-in list of official vendor "
                                   "sites, source forges and package registries."),
                                trust));
    trustedHosts_ = new QPlainTextEdit(trust);
    trustedHosts_->setFont(Theme::instance().monoFont(9));
    trustedHosts_->setMaximumHeight(120);
    trustLayout->addWidget(trustedHosts_);
    layout->addWidget(trust);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildInterfacePage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);

    auto* group = new QGroupBox(tr("Appearance"), page);
    auto* form = new QFormLayout(group);

    theme_ = new QComboBox(group);
    theme_->addItems({tr("Dark"), tr("Light")});
    form->addRow(tr("Theme"), theme_);

    fontSize_ = new QSpinBox(group);
    fontSize_->setRange(7, 24);
    fontSize_->setSuffix(" pt");
    form->addRow(tr("Editor font size"), fontSize_);

    showReasoning_ = new QCheckBox(tr("Show model reasoning when a model exposes it"), group);
    form->addRow(showReasoning_);
    form->addRow(QString(),
                 hint(tr("Only models whose chat template has a reasoning section produce this. "
                         "LocalCode does not invent one for models that do not."),
                      group));

    layout->addWidget(group);

    layout->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildPathsPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(10, 10, 10, 10);

    auto* group = new QGroupBox(tr("Storage locations"), page);
    auto* form = new QFormLayout(group);

    runtimeDirectory_ = new QLineEdit(group);
    auto* browse = new QPushButton(tr("Browse..."), group);
    connect(browse, &QPushButton::clicked, this, &SettingsDialog::browseRuntimeDirectory);
    form->addRow(tr("Runtime directory"), pathRow(runtimeDirectory_, browse, group));
    form->addRow(QString(),
                 hint(tr("Where LocalCode looks for managed llama.cpp and ik_llama.cpp builds: "
                         "put a build under <runtime>/llama.cpp/ and it is found automatically."),
                      group));

    dataDirectory_ = new QLineEdit(group);
    dataDirectory_->setReadOnly(true);
    form->addRow(tr("Data"), dataDirectory_);

    cacheDirectory_ = new QLineEdit(group);
    cacheDirectory_->setReadOnly(true);
    form->addRow(tr("Cache"), cacheDirectory_);
    form->addRow(QString(),
                 hint(tr("Set the LOCALCODE_HOME environment variable to relocate all of these "
                         "under one directory. Model files are never copied or moved - LocalCode "
                         "reads them where they are."),
                      group));
    layout->addWidget(group);

    layout->addStretch(1);
    return page;
}

void SettingsDialog::load() {
    const auto settings = application_.settings();

    llamaServer_->setText(QString::fromStdString(settings.llama_server_path));
    llamaSource_->setCurrentIndex(settings.use_bundled_llama ? 0 : 1);
    updateRuntimeChoice();
    ikLlamaServer_->setText(QString::fromStdString(settings.ik_llama_server_path));
    defaultBackend_->setCurrentIndex(settings.default_backend == BackendPreference::IkLlama ? 1
                                                                                            : 0);
    useExternal_->setChecked(settings.use_external_server);
    externalUrl_->setText(QString::fromStdString(settings.external_server_url));
    externalKey_->setText(QString::fromStdString(settings.external_server_api_key));
    externalUrl_->setEnabled(settings.use_external_server);
    externalKey_->setEnabled(settings.use_external_server);

    autonomy_->setCurrentIndex(settings.autonomy == AutonomyLevel::Restricted ? 0
                               : settings.autonomy == AutonomyLevel::Full     ? 2
                                                                              : 1);
    maxIterations_->setValue(settings.max_iterations);
    timeBudget_->setValue(settings.agent_time_budget_minutes);
    readOnly_->setChecked(settings.read_only);
    streaming_->setChecked(settings.stream_responses);

    webEnabled_->setChecked(settings.web_enabled);
    provider_->setCurrentText(QString::fromStdString(settings.search_provider));
    endpoint_->setText(QString::fromStdString(settings.search_endpoint));
    apiKey_->setText(QString::fromStdString(settings.search_api_key));

    QStringList hosts;
    for (const auto& host : settings.extra_trusted_hosts)
        hosts << QString::fromStdString(host);
    trustedHosts_->setPlainText(hosts.join('\n'));

    theme_->setCurrentIndex(settings.theme == "light" ? 1 : 0);
    fontSize_->setValue(settings.editor_font_size);
    showReasoning_->setChecked(settings.show_reasoning);
    logLevel_->setCurrentText(QString::fromStdString(settings.log_level));

    runtimeDirectory_->setText(QString::fromStdString(settings.runtime_directory));
    dataDirectory_->setText(QString::fromStdString(settings.data_directory));
    cacheDirectory_->setText(QString::fromStdString(settings.cache_directory));

    detectRuntimes();
}

void SettingsDialog::detectRuntimes() {
    if (runtimeScan_) return;
    runtimeCancel_.reset();
    discovered_->setPlainText(tr("Scanning..."));
    auto request = application_.backends().runtime_discovery();
    request.use_bundled_llama = llamaSource_->currentIndex() == 0;
    request.llama_binary = path_from_utf8(llamaServer_->text().toStdString());
    request.ik_llama_binary = path_from_utf8(ikLlamaServer_->text().toStdString());
    request.runtime_root = path_from_utf8(runtimeDirectory_->text().toStdString());
    auto* watcher = new QFutureWatcher<std::vector<RuntimeInstall>>(this);
    runtimeScan_ = watcher;
    connect(watcher, &QFutureWatcher<std::vector<RuntimeInstall>>::finished, this, [this, watcher] {
        const auto runtimes = watcher->result();
        runtimeScan_ = nullptr;
        watcher->deleteLater();
        if (runtimes.empty()) {
            discovered_->setPlainText(
                tr("No usable runtime was found. If the included runtime is missing, reinstall "
                   "LocalCode, or choose Custom llama.cpp and select its server file. "
                   "Additional runtime builds may be placed under\n%1")
                    .arg(runtimeDirectory_->text() + "/llama.cpp/"));
            return;
        }

        QStringList lines;
        for (const auto& runtime : runtimes) {
            QStringList features;
            if (runtime.features.jinja) features << "jinja";
            if (runtime.features.reasoning_format) features << "reasoning-format";
            if (runtime.features.flash_attention) features << "flash-attn";
            lines << QString("%1\n    %2\n    features: %3")
                         .arg(QString::fromStdString(runtime.display()),
                              QString::fromStdString(runtime.version),
                              features.isEmpty() ? tr("(none advertised)") : features.join(", "));
        }
        discovered_->setPlainText(lines.join("\n"));
    });
    // Closing this dialog/application cannot leave a worker dereferencing the
    // window or its backend manager; it owns only the snapshot.
    watcher->setFuture(QtConcurrent::run([request, cancel = runtimeCancel_.token()] {
        return BackendManager::discover_runtimes(request, cancel);
    }));
}

void SettingsDialog::browseLlamaServer() {
    const auto file = QFileDialog::getOpenFileName(this, tr("Select llama-server"), QString(),
                                                   tr("Executables (*.exe);;All files (*)"));
    if (!file.isEmpty()) llamaServer_->setText(file);
}

void SettingsDialog::updateRuntimeChoice() {
    customLlamaRow_->setEnabled(llamaSource_->currentIndex() == 1);
}

void SettingsDialog::browseIkLlamaServer() {
    const auto file =
        QFileDialog::getOpenFileName(this, tr("Select the ik_llama.cpp server"), QString(),
                                     tr("Executables (*.exe);;All files (*)"));
    if (!file.isEmpty()) ikLlamaServer_->setText(file);
}

void SettingsDialog::browseRuntimeDirectory() {
    const auto directory = QFileDialog::getExistingDirectory(
        this, tr("Select the runtime directory"), runtimeDirectory_->text());
    if (!directory.isEmpty()) runtimeDirectory_->setText(directory);
}

void SettingsDialog::apply() {
    AppSettings settings = application_.settings();

    settings.llama_server_path = llamaServer_->text().toStdString();
    settings.use_bundled_llama = llamaSource_->currentIndex() == 0;
    settings.ik_llama_server_path = ikLlamaServer_->text().toStdString();
    settings.default_backend = defaultBackend_->currentIndex() == 1 ? BackendPreference::IkLlama
                                                                    : BackendPreference::LlamaCpp;
    settings.use_external_server = useExternal_->isChecked();
    settings.external_server_url = externalUrl_->text().toStdString();
    settings.external_server_api_key = externalKey_->text().toStdString();

    settings.autonomy = autonomy_->currentIndex() == 0   ? AutonomyLevel::Restricted
                        : autonomy_->currentIndex() == 2 ? AutonomyLevel::Full
                                                         : AutonomyLevel::Standard;
    settings.max_iterations = maxIterations_->value();
    settings.agent_time_budget_minutes = timeBudget_->value();
    settings.read_only = readOnly_->isChecked();
    settings.stream_responses = streaming_->isChecked();

    settings.web_enabled = webEnabled_->isChecked();
    settings.search_provider = provider_->currentText().toStdString();
    settings.search_endpoint = endpoint_->text().toStdString();
    settings.search_api_key = apiKey_->text().toStdString();

    settings.extra_trusted_hosts.clear();
    for (const auto& host : trustedHosts_->toPlainText().split('\n', Qt::SkipEmptyParts)) {
        settings.extra_trusted_hosts.push_back(host.trimmed().toStdString());
    }

    settings.theme = theme_->currentIndex() == 1 ? "light" : "dark";
    settings.editor_font_size = fontSize_->value();
    settings.show_reasoning = showReasoning_->isChecked();
    settings.log_level = logLevel_->currentText().toStdString();
    settings.runtime_directory = runtimeDirectory_->text().toStdString();

    if (auto status = application_.apply_settings(settings); !status) {
        QMessageBox::warning(this, tr("Settings could not be saved"),
                             QString::fromStdString(status.error().message));
        return;
    }
    emit settingsApplied();
    accept();
}

void SettingsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Windows finishes native dialog centering after the first queued event.
    // Wait for that placement before fitting the final decorated frame.
    QTimer::singleShot(50, this, [this] { clampToScreen(*this); });
}

}   // namespace localcode::gui
