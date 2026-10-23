#include "ModelPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <limits>

#include "../Theme.h"
#include "../dialogs/InferenceTuningDialog.h"
#include "../dialogs/ResourceSetupDialog.h"
#include "MarkdownRenderer.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "util/Strings.h"

namespace localcode::gui {

ModelPanel::ModelPanel(Application& application, QWidget* parent)
    : QWidget(parent), application_(application) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The enclosing tab is already labelled "Models".

    auto* splitter = new QSplitter(Qt::Vertical, this);

    list_ = new QListWidget(splitter);
    list_->setFrameShape(QFrame::NoFrame);
    splitter->addWidget(list_);

    detail_ = new QTextBrowser(splitter);
    detail_->setFrameShape(QFrame::NoFrame);
    splitter->addWidget(detail_);

    auto* settings = new QGroupBox(tr("Profile"), splitter);
    auto* form = new QFormLayout(settings);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);

    backend_ = new QComboBox(settings);
    backend_->addItems({tr("Auto"), tr("llama.cpp"), tr("ik_llama.cpp")});
    backend_->setToolTip(
        tr("AUTO picks from the runtimes that are actually installed and "
           "explains its choice. Neither runtime is faster in general."));
    form->addRow(tr("Backend"), backend_);

    context_ = new QSpinBox(settings);
    context_->setObjectName("ModelContextSize");
    context_->setRange(512, std::numeric_limits<int>::max());
    context_->setSingleStep(1024);
    context_->setToolTip(tr("Context window to load the model with. Larger costs more memory."));
    auto* contextRow = new QWidget(settings);
    auto* contextLayout = new QHBoxLayout(contextRow);
    contextLayout->setContentsMargins(0, 0, 0, 0);
    contextLayout->setSpacing(4);
    contextLayout->addWidget(context_, 1);
    auto* presets = new QToolButton(contextRow);
    presets->setText(tr("Presets"));
    presets->setObjectName("ContextPresets");
    presets->setPopupMode(QToolButton::InstantPopup);
    auto* presetMenu = new QMenu(presets);
    for (const int tokens : {4096, 8192, 16384, 32768})
        presetMenu->addAction(tr("%1K (%2 tokens)").arg(tokens / 1024).arg(tokens),
            this, [this, tokens] { context_->setValue(tokens); });
    presets->setMenu(presetMenu);
    contextLayout->addWidget(presets);
    form->addRow(tr("Context size"), contextRow);

    gpuLayers_ = new QSpinBox(settings);
    gpuLayers_->setRange(-1, 999);
    gpuLayers_->setSpecialValueText(tr("all"));
    gpuLayers_->setToolTip(tr("-1 offloads every layer to the GPU; 0 keeps everything on CPU."));
    form->addRow(tr("GPU layers"), gpuLayers_);
    autoGpuLayers_ = new QCheckBox(tr("Fit GPU layers to available memory"), settings);
    autoGpuLayers_->setObjectName("AutoGpuLayers");
    autoGpuLayers_->setToolTip(tr("Compatible runtimes adjust GPU placement while retaining your context size. "
                                "Disable this to choose the layer count manually."));
    form->addRow(QString(), autoGpuLayers_);
    connect(autoGpuLayers_, &QCheckBox::toggled, gpuLayers_, &QWidget::setDisabled);

    flashAttention_ = new QCheckBox(tr("Flash attention"), settings);
    form->addRow(QString(), flashAttention_);

    reasoning_ = new QCheckBox(tr("Enable native reasoning"), settings);
    reasoning_->setToolTip(
        tr("Uses the model's native reasoning. When disabled, compatible managed "
           "servers also receive a zero reasoning budget. Older runtimes or "
           "templates may ignore this request."));
    form->addRow(QString(), reasoning_);

    toolProtocol_ = new QComboBox(settings);
    toolProtocol_->addItems({tr("Native tool calls"), tr("Text protocol fallback"), tr("None")});
    form->addRow(tr("Tool calling"), toolProtocol_);

    tune_ = new QPushButton(tr("Performance settings…"), settings);
    tune_->setObjectName("TuneModelProfile");
    form->addRow(QString(), tune_);
    connect(tune_, &QPushButton::clicked, this, [this] {
        const auto profile = selectedProfile();
        if (!profile) return;
        auto input = tuning_;
        input.insert("contextSize", context_->value());
        input.insert("gpuLayers", gpuLayers_->value());
        input.insert("flash_attention", flashAttention_->isChecked() ? "on" : "off");
        InferenceTuningDialog dialog(input, false, this);
        const bool explicitLocalRuntime =
            !profile->server_binary.empty() && !application_.backends().external_endpoint().enabled;
        const auto suggestedKind = profile->backend == BackendPreference::Auto
                                       ? application_.backends().default_preference()
                                       : profile->backend;
        dialog.setBenchmarkFiles(
            QString::fromStdString(path_to_utf8(profile->model_path)),
            profile->server_binary.empty()
                ? QString::fromStdString(suggestedKind == BackendPreference::IkLlama
                                             ? application_.settings().ik_llama_server_path
                                             : application_.settings().llama_server_path)
                : QString::fromStdString(path_to_utf8(profile->server_binary)),
            explicitLocalRuntime);
        if (dialog.exec() != QDialog::Accepted) return;
        tuning_ = dialog.settings();
        if (tuning_.value("gpuLayers").toInt() != gpuLayers_->value())
            autoGpuLayers_->setChecked(false);
        context_->setValue(tuning_.value("contextSize").toInt());
        gpuLayers_->setValue(tuning_.value("gpuLayers").toInt());
        flashAttention_->setChecked(tuning_.value("flash_attention").toString() == "on");
        applyEdits();
    });

    auto* apply = new QPushButton(tr("Apply"), settings);
    connect(apply, &QPushButton::clicked, this, &ModelPanel::applyEdits);
    form->addRow(QString(), apply);

    splitter->addWidget(settings);
    splitter->setSizes({160, 200, 260});
    layout->addWidget(splitter, 1);

    auto* buttons = new QWidget(this);
    auto* buttonLayout = new QGridLayout(buttons);
    buttonLayout->setContentsMargins(6, 4, 6, 6);
    buttonLayout->setSpacing(6);

    load_ = new QPushButton(tr("Load"), buttons);
    load_->setObjectName("Primary");
    unload_ = new QPushButton(tr("Unload"), buttons);
    auto* import = new QPushButton(tr("Add GGUF..."), buttons);
    auto* setup = new QPushButton(tr("Set up runtime and model…"), this);
    setup->setObjectName("SetupCodingModel");
    auto* remove = new QPushButton(tr("Remove"), buttons);
    remove->setObjectName("Danger");

    buttonLayout->addWidget(load_, 0, 0);
    buttonLayout->addWidget(unload_, 0, 1);
    buttonLayout->addWidget(import, 1, 0);
    buttonLayout->addWidget(remove, 1, 1);
    buttonLayout->setColumnStretch(0, 1);
    buttonLayout->setColumnStretch(1, 1);
    layout->addWidget(buttons);
    layout->addWidget(setup);

    connect(list_, &QListWidget::currentRowChanged, this, &ModelPanel::showSelected);
    connect(load_, &QPushButton::clicked, this, [this] {
        const auto id = selectedModelId();
        if (!id.isEmpty()) emit loadRequested(id);
    });
    connect(unload_, &QPushButton::clicked, this, &ModelPanel::unloadRequested);
    connect(import, &QPushButton::clicked, this, &ModelPanel::importRequested);
    connect(setup, &QPushButton::clicked, this, [this] {
        const auto current = selectedProfile();
#ifdef Q_OS_WIN
        const auto runtimeRoot = QString::fromStdWString(app_paths().runtime_dir().native());
        const auto dataRoot = QString::fromStdWString(app_paths().data_dir().native());
        const auto model =
            current ? QString::fromStdWString(current->model_path.native()) : QString{};
#else
        const auto runtimeRoot=QString::fromStdString(path_to_utf8(app_paths().runtime_dir()));
        const auto dataRoot=QString::fromStdString(path_to_utf8(app_paths().data_dir()));
        const auto model=current?QString::fromStdString(path_to_utf8(current->model_path)):QString{};
#endif
        const auto runtimeSettings = application_.settings();
        const auto bundledRuntime =
            QString::fromStdString(path_to_utf8(AppPaths::bundled_llama_server()));
        ResourceSetupDialog dialog({runtimeSettings.use_bundled_llama
                                        ? bundledRuntime
                                        : QString::fromStdString(runtimeSettings.llama_server_path),
                                    model,
                                    {}},
                                   runtimeRoot, QDir(dataRoot).filePath("models"), this, {}, false);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto selected = dialog.selection();
#ifdef Q_OS_WIN
        const auto path = std::filesystem::path(selected.model.toStdWString());
#else
        const auto path=path_from_utf8(selected.model.toStdString());
#endif
        auto imported = application_.import_model(path);
        if (!imported) {
            emit statusMessage(QString::fromStdString(imported.error().to_string()));
            return;
        }
        imported->backend = BackendPreference::LlamaCpp;
        if (selected.cpuStartingProfile) {
            imported->context_size = 4096;
            imported->gpu_layers = 0;
            imported->auto_gpu_layers = false;
            imported->threads = 0;
            imported->batch_size = 512;
            imported->ubatch_size = 128;
            imported->prompt_cache_mib = 128;
            imported->cache_type_k.clear();
            imported->cache_type_v.clear();
        }
        if (auto saved = application_.save_model_profile(*imported); !saved) {
            emit statusMessage(QString::fromStdString(saved.error().to_string()));
            return;
        }
        auto settings = application_.settings();
        settings.use_bundled_llama = QFileInfo(selected.runtime) == QFileInfo(bundledRuntime);
        if (!settings.use_bundled_llama)
            settings.llama_server_path = selected.runtime.toStdString();
        if (auto applied = application_.apply_settings(settings); !applied) {
            emit statusMessage(tr("Model imported; runtime settings could not be saved: %1")
                                   .arg(QString::fromStdString(applied.error().to_string())));
            refresh();
            return;
        }
        refresh();
        for (int row = 0; row < list_->count(); ++row)
            if (list_->item(row)->data(Qt::UserRole).toString() ==
                QString::fromStdString(imported->id)) {
                list_->setCurrentRow(row);
                break;
            }
        emit statusMessage(tr("Files configured. Load the model to check answers and memory use."));
    });
    connect(remove, &QPushButton::clicked, this, &ModelPanel::removeSelected);

    refresh();
}

void ModelPanel::refresh() {
    const QString previous = selectedModelId();

    auto loaded = application_.model_profiles();
    profiles_ = loaded ? *loaded : std::vector<ModelProfile>{};

    list_->clear();
    for (const auto& profile : profiles_) {
        auto* item = new QListWidgetItem(QString::fromStdString(profile.display_name), list_);
        item->setData(Qt::UserRole, QString::fromStdString(profile.id));
        item->setToolTip(QString::fromStdString(path_to_utf8(profile.model_path)));
        if (QString::fromStdString(profile.id) ==
            QString::fromStdString(application_.active_model_id())) {
            item->setForeground(colours().success);
        }
    }

    if (!previous.isEmpty()) {
        for (int i = 0; i < list_->count(); ++i) {
            if (list_->item(i)->data(Qt::UserRole).toString() == previous) {
                list_->setCurrentRow(i);
                return;
            }
        }
    }
    if (list_->count() > 0) list_->setCurrentRow(0);
    showSelected();
}

QString ModelPanel::selectedModelId() const {
    auto* item = list_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

std::optional<ModelProfile> ModelPanel::selectedProfile() const {
    const auto id = selectedModelId().toStdString();
    for (const auto& profile : profiles_) {
        if (profile.id == id) return profile;
    }
    return std::nullopt;
}

void ModelPanel::showSelected() {
    const auto profile = selectedProfile();
    tune_->setEnabled(profile.has_value());
    if (!profile) {
        detail_->clear();
        load_->setEnabled(false);
        return;
    }
    load_->setEnabled(true);

    backend_->setCurrentIndex(profile->backend == BackendPreference::LlamaCpp  ? 1
                              : profile->backend == BackendPreference::IkLlama ? 2
                                                                               : 0);
    context_->setValue(static_cast<int>(profile->context_size));
    gpuLayers_->setValue(profile->gpu_layers);
    autoGpuLayers_->setChecked(profile->auto_gpu_layers);
    gpuLayers_->setDisabled(profile->auto_gpu_layers);
    flashAttention_->setChecked(profile->flash_attention);
    tuning_ = {{"threads", static_cast<int>(profile->threads)},
               {"threads_batch", static_cast<int>(profile->threads_batch)},
               {"cpu_placement", QString::fromUtf8(to_string(profile->cpu_placement).data())},
               {"batch_size", static_cast<int>(profile->batch_size)},
               {"ubatch_size", static_cast<int>(profile->ubatch_size)},
               {"ngram_speculation", profile->ngram_speculation},
               {"cache_type_k", QString::fromStdString(profile->cache_type_k)},
               {"cache_type_v", QString::fromStdString(profile->cache_type_v)}};
    if (profile->prompt_cache_mib)
        tuning_.insert("prompt_cache_mib", QJsonValue(*profile->prompt_cache_mib));
    tuning_.insert("weight_repacking",
                   profile->weight_repacking == WeightRepacking::RuntimeDefault
                       ? QJsonValue(QJsonValue::Null)
                   : profile->weight_repacking == WeightRepacking::Invalid
                       ? QJsonValue("invalid")
                       : QJsonValue(profile->weight_repacking == WeightRepacking::Enabled));

    // Reasoning controls only make sense for a model that actually has it.
    reasoning_->setEnabled(profile->capabilities.supports_native_reasoning);
    reasoning_->setChecked(profile->reasoning_enabled &&
                           profile->capabilities.supports_native_reasoning);
    reasoning_->setToolTip(
        tr("Uses the model's native reasoning. When disabled, compatible managed "
           "servers also receive a zero reasoning budget. Older runtimes or "
           "templates may ignore this request."));
    if (!profile->capabilities.supports_native_reasoning) {
        reasoning_->setToolTip(
            tr("This model has no native reasoning, so there is nothing to "
               "enable. LocalCode does not simulate one."));
    }

    toolProtocol_->setCurrentIndex(profile->tool_protocol == ToolProtocol::Native         ? 0
                                   : profile->tool_protocol == ToolProtocol::TextFallback ? 1
                                                                                          : 2);

    const auto& p = colours();
    QString html = QString("<body style=\"color:%1;%2\">")
                       .arg(p.text.name(QColor::HexRgb), Theme::instance().richTextStyle());
    html += QString("<div style=\"font-size:11pt;font-weight:600\">%1</div>")
                .arg(MarkdownRenderer::escape(QString::fromStdString(profile->display_name)));
    html += QString("<div style=\"color:%1;font-size:9pt;margin-bottom:8px\">%2</div>")
                .arg(p.textFaint.name(QColor::HexRgb),
                     MarkdownRenderer::escape(
                         QString::fromStdString(path_to_utf8(profile->model_path))));

    auto row = [&](const QString& key, const QString& value) {
        if (value.isEmpty()) return;
        html += QString("<div><span style=\"color:%1\">%2</span> &nbsp; %3</div>")
                    .arg(p.textMuted.name(QColor::HexRgb), MarkdownRenderer::escape(key),
                         MarkdownRenderer::escape(value));
    };

    row(tr("Architecture"), QString::fromStdString(profile->architecture));
    row(tr("Quantization"), QString::fromStdString(profile->quantization));
    row(tr("File size"), QString::fromStdString(strings::human_bytes(profile->file_size)));
    if (profile->trained_context) {
        row(tr("Trained context"), QString::number(profile->trained_context) + " tokens");
    }

    html += QString(
                "<div style=\"color:%1;font-weight:600;font-size:9pt;margin:10px 0 3px 0\">"
                "CAPABILITIES</div>")
                .arg(p.textMuted.name(QColor::HexRgb));
    for (const auto& [key, value] : profile->capabilities.describe()) {
        if (key == "evidence") continue;
        const bool yes = value == "yes";
        html += QString("<div><span style=\"color:%1\">%2</span> %3</div>")
                    .arg((yes ? p.success : p.textFaint).name(QColor::HexRgb), yes ? "✓" : "·",
                         MarkdownRenderer::escape(QString::fromStdString(key)));
    }

    if (!profile->capabilities.evidence.empty()) {
        html += QString("<div style=\"color:%1;font-size:9pt;margin-top:8px\">%2: %3</div>")
                    .arg(p.textFaint.name(QColor::HexRgb), tr("Evidence"),
                         MarkdownRenderer::escape(
                             QString::fromStdString(profile->capabilities.evidence)));
    }
    if (!profile->notes.empty()) {
        html += QString("<div style=\"color:%1;margin-top:8px\">%2</div>")
                    .arg(p.warning.name(QColor::HexRgb),
                         MarkdownRenderer::escape(QString::fromStdString(profile->notes)));
    }
    html += "</body>";
    detail_->setHtml(html);
}

void ModelPanel::applyEdits() {
    auto profile = selectedProfile();
    if (!profile) return;

    profile->backend = backend_->currentIndex() == 1   ? BackendPreference::LlamaCpp
                       : backend_->currentIndex() == 2 ? BackendPreference::IkLlama
                                                       : BackendPreference::Auto;
    profile->context_size = static_cast<std::uint32_t>(context_->value());
    profile->gpu_layers = gpuLayers_->value();
    profile->auto_gpu_layers = autoGpuLayers_->isChecked();
    profile->flash_attention = flashAttention_->isChecked();
    profile->threads = static_cast<std::uint32_t>(tuning_.value("threads").toInt());
    profile->threads_batch = static_cast<std::uint32_t>(tuning_.value("threads_batch").toInt());
    profile->cpu_placement =
        cpu_placement_from_string(tuning_.value("cpu_placement").toString("os").toStdString());
    profile->batch_size = static_cast<std::uint32_t>(tuning_.value("batch_size").toInt());
    profile->ubatch_size = static_cast<std::uint32_t>(tuning_.value("ubatch_size").toInt());
    if (tuning_.contains("prompt_cache_mib"))
        profile->prompt_cache_mib = tuning_.value("prompt_cache_mib").toInteger();
    else
        profile->prompt_cache_mib.reset();
    const auto repack = tuning_.value("weight_repacking");
    profile->weight_repacking = repack.isNull() || repack.isUndefined()
                                    ? WeightRepacking::RuntimeDefault
                                : !repack.isBool() ? WeightRepacking::Invalid
                                : repack.toBool()  ? WeightRepacking::Enabled
                                                   : WeightRepacking::Disabled;
    profile->ngram_speculation = tuning_.value("ngram_speculation").toBool();
    profile->cache_type_k = tuning_.value("cache_type_k").toString().toStdString();
    profile->cache_type_v = tuning_.value("cache_type_v").toString().toStdString();
    profile->reasoning_enabled =
        reasoning_->isChecked() && profile->capabilities.supports_native_reasoning;
    profile->show_reasoning =
        profile->reasoning_enabled && profile->capabilities.supports_reasoning_content;
    profile->tool_protocol = toolProtocol_->currentIndex() == 0   ? ToolProtocol::Native
                             : toolProtocol_->currentIndex() == 1 ? ToolProtocol::TextFallback
                                                                  : ToolProtocol::None;

    auto status = application_.save_model_profile(*profile);
    if (!status) {
        emit statusMessage(tr("Could not save the profile: %1")
                               .arg(QString::fromStdString(status.error().message)));
        return;
    }
    emit statusMessage(tr("Profile saved. Reload the model for it to take effect."));
    refresh();
}

void ModelPanel::removeSelected() {
    const auto profile = selectedProfile();
    if (!profile) return;

    const auto answer = QMessageBox::question(
        this, tr("Remove model"),
        tr("Remove \"%1\" from LocalCode?\n\nThe GGUF file on disk is not deleted.")
            .arg(QString::fromStdString(profile->display_name)));
    if (answer != QMessageBox::Yes) return;

    application_.remove_model_profile(profile->id);
    refresh();
}

}   // namespace localcode::gui
