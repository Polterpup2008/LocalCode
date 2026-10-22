#include "ResourceSetupDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <cmath>
#include "../ScreenGuard.h"
#include "inference/ServerManager.h"
#include "model/GgufMetadata.h"
#include "util/Strings.h"

namespace localcode::gui {
namespace {
// Metadata contains long URLs and digests. Preserve the exact selectable text
// while wrapping inside the page, and let its outer scroll area handle height.
class DownloadDetails final : public QTextBrowser {
public:
    explicit DownloadDetails(QWidget* parent) : QTextBrowser(parent) {
        setFrameShape(QFrame::NoFrame);
        setOpenLinks(false);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setStyleSheet("QTextBrowser { background: transparent; padding: 0px; border: none; }");
        document()->setDocumentMargin(0);
        connect(document(), &QTextDocument::contentsChanged, this, [this] { fitHeight(); });
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        fitHeight();
    }

private:
    void fitHeight() {
        document()->setTextWidth(viewport()->width());
        setFixedHeight(static_cast<int>(std::ceil(document()->size().height())) +
                       2 * frameWidth());
    }
};

std::filesystem::path native(const QString& path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

QString bytes(quint64 value) {
    return QString::fromStdString(strings::human_bytes(value));
}

QString describe(const ResourceAsset& asset) {
    return QObject::tr("%1 · %2\nSource: %3\nRevision: %4\nLicense: %5\nSHA256: %6")
        .arg(asset.name, bytes(asset.bytes), asset.source, asset.revision, asset.license,
             asset.sha256);
}
}   // namespace

ResourceSetupDialog::ResourceSetupDialog(SetupSelection initial, QString runtimeRoot,
                                         QString modelRoot, QWidget* parent,
                                         std::shared_ptr<ResourceInstaller> installer,
                                         bool allowProjector)
    : QDialog(parent),
      installer_(installer ? std::move(installer) : std::make_shared<ResourceInstaller>()),
      runtimeRoot_(std::move(runtimeRoot)),
      modelRoot_(std::move(modelRoot)),
      allowProjector_(allowProjector) {
    setWindowTitle(tr("Set up a local model"));
    setModal(true);
    resize(740, 760);
    setMinimumSize(460, 400);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(
        tr("Choose existing files or download a Windows x64 llama.cpp build and a public Hugging "
           "Face GGUF. Downloads show their source and size, verify SHA256, and stay in your "
           "private resource folder. No model or bot is started here."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    pages_ = new QTabWidget(this);
    layout->addWidget(pages_, 1);
    auto page = [&](const QString& title) {
        auto* scroll = new QScrollArea(pages_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scroll);
        auto* form = new QFormLayout(content);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        scroll->setWidget(content);
        pages_->addTab(scroll, title);
        return form;
    };
    auto path = [&](QFormLayout* form, const QString& title, const char* name,
                    const QString& initialPath, const QString& filter) {
        auto* holder = new QWidget(this);
        auto* row = new QHBoxLayout(holder);
        row->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(initialPath, holder);
        edit->setObjectName(name);
        auto* browse = new QPushButton(tr("Browse…"), holder);
        row->addWidget(edit, 1);
        row->addWidget(browse);
        form->addRow(title, holder);
        connect(browse, &QPushButton::clicked, this, [this, edit, filter] {
            const auto file =
                QFileDialog::getOpenFileName(this, tr("Select local file"), edit->text(), filter);
            if (!file.isEmpty()) edit->setText(file);
        });
        connect(edit, &QLineEdit::textChanged, this, [this] { updateControls(); });
        return edit;
    };
    auto detail = [&] {
        auto* label = new QLabel(this);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    auto* engine = page(tr("1 · Runtime"));
    runtime_ = path(engine, tr("Existing llama-server"), "SetupRuntime", initial.runtime,
                    tr("Runtime (llama-server*);;All files (*)"));
    memory_ = new QCheckBox(tr("Prepare a runtime copy with RAM optimizations"), this);
    memory_->setObjectName("SetupMemoryRuntime");
    memory_->setVisible(installer_->memoryRuntimeAvailable());
    memory_->setToolTip(tr("Available for compatible Windows builds. Can reduce RAM held by unused "
                            "model-file pages after loading. Your model, context and memory-locking "
                            "choices stay in place. The original runtime files are preserved."));
    engine->addRow(memory_);
    reasoning_ = new QCheckBox(tr("Prepare a CPU runtime copy with the reasoning-off repair"), this);
    reasoning_->setObjectName("SetupReasoningRuntime");
    reasoning_->setVisible(installer_->reasoningRuntimeAvailable());
    reasoning_->setToolTip(tr("For the compatible Windows x64 CPU build. Helps models that keep "
                               "thinking when reasoning is disabled. Creates a separate runtime "
                               "copy and preserves your original files. Choose either this repair "
                               "or the RAM copy; their combination is not qualified."));
    engine->addRow(reasoning_);
    connect(reasoning_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) memory_->setChecked(false);
    });
    connect(memory_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) reasoning_->setChecked(false);
    });
    tag_ = new QLineEdit("b10655", this);
    tag_->setObjectName("SetupReleaseTag");
    tag_->setToolTip(tr("A binary build tag such as b10655, or an official version such as v0.6.0."));
    engine->addRow(tr("llama.cpp release tag"), tag_);
    runtimeFetch_ = new QPushButton(tr("Find Windows builds"), this);
    runtimeFetch_->setObjectName("SetupFindRuntimes");
    engine->addRow(runtimeFetch_);
    runtimeBuild_ = new QComboBox(this);
    runtimeBuild_->setObjectName("SetupRuntimeBuild");
    engine->addRow(tr("Build"), runtimeBuild_);
    runtimeDetail_ = new DownloadDetails(this);
    runtimeDetail_->setObjectName("SetupRuntimeDetails");
    engine->addRow(runtimeDetail_);
    runtimeDownload_ = new QPushButton(tr("Download selected runtime"), this);
    runtimeDownload_->setObjectName("SetupDownloadRuntime");
    engine->addRow(runtimeDownload_);
    auto* runtimeHelp = detail();
    runtimeHelp->setText(
        tr("b10655 is the build used for the recorded local benchmarks. Official version tags such "
           "as v0.6.0 also work; the selected build is shown before downloading. CPU requires no GPU "
           "driver. CUDA requires a compatible NVIDIA driver and downloads its CUDA libraries too. "
           "Vulkan requires a working Vulkan driver. A successful download does not establish "
           "model speed or compatibility."));
    engine->addRow(runtimeHelp);
    auto* weights = page(tr("2 · Model"));
    model_ = path(weights, tr("Existing model"), "SetupModel", initial.model, tr("GGUF (*.gguf)"));
    if (allowProjector_)
        projector_ = path(weights, tr("Vision projector (optional)"), "SetupProjector",
                          initial.projector, tr("GGUF (*.gguf)"));
    else {
        projector_ = new QLineEdit(this);
        projector_->hide();
    }
    repository_ = new QLineEdit(this);
    repository_->setPlaceholderText(tr("owner/model-repository"));
    repository_->setObjectName("SetupRepository");
    weights->addRow(tr("Public Hugging Face repository"), repository_);
    modelFetch_ = new QPushButton(tr("List downloadable GGUFs"), this);
    modelFetch_->setObjectName("SetupFindModels");
    weights->addRow(modelFetch_);
    modelFile_ = new QComboBox(this);
    modelFile_->setObjectName("SetupModelFile");
    weights->addRow(tr("File"), modelFile_);
    modelDetail_ = new DownloadDetails(this);
    modelDetail_->setObjectName("SetupModelDetails");
    weights->addRow(modelDetail_);
    modelDownload_ = new QPushButton(tr("Download selected GGUF"), this);
    modelDownload_->setObjectName("SetupDownloadModel");
    weights->addRow(modelDownload_);
    auto* modelHelp = detail();
    modelHelp->setText(
        tr("Select weights for this PC and check their license. A smaller model may change its "
           "answers. File size is only part of the RAM/VRAM needed. Gated repositories and split "
           "models can be supplied as existing files.") +
        (allowProjector_ ? tr(" Select a matching vision projector separately if you need images.")
                         : QString{}));
    weights->addRow(modelHelp);
    cpu_ = new QCheckBox(tr("Use a CPU starting profile with 4K context"), this);
    cpu_->setObjectName("SetupCpuProfile");
    cpu_->setChecked(initial.cpuStartingProfile);
    layout->addWidget(cpu_);
    status_ = detail();
    status_->setObjectName("SetupStatus");
    status_->setText(tr("Choose a runtime and model, then use these files."));
    layout->addWidget(status_);
    bar_ = new QProgressBar(this);
    bar_->setRange(0, 1000);
    bar_->hide();
    layout->addWidget(bar_);
    auto* buttons = new QDialogButtonBox(this);
    use_ = buttons->addButton(tr("Use these files"), QDialogButtonBox::AcceptRole);
    use_->setObjectName("SetupUseFiles");
    cancelButton_ = buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ResourceSetupDialog::validateSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &ResourceSetupDialog::reject);
    connect(runtimeBuild_, &QComboBox::currentIndexChanged, this, [this] {
        showRuntime();
        updateControls();
    });
    connect(modelFile_, &QComboBox::currentIndexChanged, this, [this] {
        showModel();
        updateControls();
    });
    connect(runtimeFetch_, &QPushButton::clicked, this, [this] {
        const auto installer = installer_;
        const auto tag = tag_->text().trimmed();
        job(
            tr("Reading llama.cpp release metadata…"),
            [installer, tag](CancellationToken cancel, ResourceProgress) -> Result<Outcome> {
                const auto result = installer->runtimes(tag, cancel);
                if (!result) return result.error();
                Outcome out;
                out.runtimes = *result;
                return out;
            },
            [this](const Outcome& out) {
                runtimes_ = out.runtimes;
                runtimeBuild_->clear();
                for (const auto& item : runtimes_)
                    runtimeBuild_->addItem(item.tag + " · " + item.flavour + " · " +
                                           bytes(item.bytes()));
                showRuntime();
                status_->setText(tr("Select a build and review its source before downloading."));
            });
    });
    connect(modelFetch_, &QPushButton::clicked, this, [this] {
        const auto installer = installer_;
        const auto repository = repository_->text().trimmed();
        job(
            tr("Reading public GGUF metadata…"),
            [installer, repository](CancellationToken cancel, ResourceProgress) -> Result<Outcome> {
                const auto result = installer->models(repository, cancel);
                if (!result) return result.error();
                Outcome out;
                out.models = *result;
                return out;
            },
            [this](const Outcome& out) {
                models_ = out.models;
                if (!allowProjector_)
                    models_.files.removeIf([](const auto& asset) {
                        return asset.name.contains("mmproj", Qt::CaseInsensitive);
                    });
                modelFile_->clear();
                for (const auto& item : models_.files)
                    modelFile_->addItem(item.name + " · " + bytes(item.bytes));
                showModel();
                status_->setText(
                    tr("Review the selected weights and their license before downloading."));
            });
    });
    connect(runtimeDownload_, &QPushButton::clicked, this, [this] {
        const auto index = runtimeBuild_->currentIndex();
        if (index < 0 || index >= runtimes_.size()) return;
        const auto package = runtimes_[index];
        const auto installer = installer_;
        const auto root = runtimeRoot_;
        job(
            tr("Downloading and verifying runtime…"),
            [installer, package, root](CancellationToken cancel,
                                       ResourceProgress progress) -> Result<Outcome> {
                const auto result =
                    installer->installRuntime(package, root, std::move(progress), cancel);
                if (!result) return result.error();
                Outcome out;
                out.path = *result;
                return out;
            },
            [this, package](const Outcome& out) {
                runtime_->setText(out.path);
                if (package.flavour == "cpu") cpu_->setChecked(true);
                status_->setText(tr("Runtime installed. Choose a GGUF on the Model page."));
                pages_->setCurrentIndex(1);
            });
    });
    connect(modelDownload_, &QPushButton::clicked, this, [this] {
        const auto index = modelFile_->currentIndex();
        if (index < 0 || index >= models_.files.size()) return;
        const auto asset = models_.files[index];
        const auto installer = installer_;
        const auto root = modelRoot_;
        job(
            tr("Downloading and verifying GGUF…"),
            [installer, asset, root](CancellationToken cancel,
                                     ResourceProgress progress) -> Result<Outcome> {
                const auto result =
                    installer->installModel(asset, root, std::move(progress), cancel);
                if (!result) return result.error();
                Outcome out;
                out.path = *result;
                return out;
            },
            [this, asset](const Outcome& out) {
                if (asset.name.contains("mmproj", Qt::CaseInsensitive))
                    projector_->setText(out.path);
                else
                    model_->setText(out.path);
                status_->setText(
                    tr("GGUF verified and installed. Use these files to validate the runtime and "
                       "save your selection."));
            });
    });
    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(150);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        if (!progress_) return;
        const auto total = progress_->total.load();
        const auto received = progress_->received.load();
        if (total == 0) {
            bar_->setRange(0, 0);
            return;
        }
        bar_->setRange(0, 1000);
        bar_->setValue(static_cast<int>(qMin<quint64>(1000, received * 1000 / total)));
        bar_->setFormat(bytes(received) + " / " + bytes(total));
    });
    updateControls();
}

ResourceSetupDialog::~ResourceSetupDialog() {
    cancel_.cancel();
}

SetupSelection ResourceSetupDialog::selection() const {
    return {runtime_->text().trimmed(), model_->text().trimmed(), projector_->text().trimmed(),
            cpu_->isChecked()};
}

void ResourceSetupDialog::reject() {
    if (busy_) {
        closeAfterCancel_ = true;
        cancel_.cancel();
        cancelButton_->setEnabled(false);
        status_->setText(tr("Cancelling and cleaning partial files…"));
        return;
    }
    QDialog::reject();
}

void ResourceSetupDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    clampToScreen(*this);
}

void ResourceSetupDialog::updateControls() {
    pages_->setEnabled(!busy_);
    cpu_->setEnabled(!busy_);
    use_->setEnabled(!busy_ && QFileInfo(runtime_->text().trimmed()).isFile() &&
                     QFileInfo(model_->text().trimmed()).isFile());
    runtimeDownload_->setEnabled(!busy_ && runtimeBuild_->currentIndex() >= 0 &&
                                 runtimeBuild_->currentIndex() < runtimes_.size());
    modelDownload_->setEnabled(!busy_ && modelFile_->currentIndex() >= 0 &&
                               modelFile_->currentIndex() < models_.files.size());
#ifndef Q_OS_WIN
    runtimeFetch_->setEnabled(false);
    runtimeDownload_->setEnabled(false);
#endif
}

void ResourceSetupDialog::showRuntime() {
    const auto index = runtimeBuild_->currentIndex();
    if (index < 0 || index >= runtimes_.size()) {
        runtimeDetail_->clear();
        return;
    }
    QStringList parts;
    for (const auto& asset : runtimes_[index].assets)
        parts << describe(asset);
    runtimeDetail_->setPlainText(parts.join("\n\n"));
}

void ResourceSetupDialog::showModel() {
    const auto index = modelFile_->currentIndex();
    modelDetail_->setPlainText(
        index >= 0 && index < models_.files.size() ? describe(models_.files[index]) : QString{});
}

void ResourceSetupDialog::job(const QString& label, Work work,
                              std::function<void(const Outcome&)> finished) {
    if (busy_) return;
    busy_ = true;
    cancel_.reset();
    progress_ = std::make_shared<Progress>();
    status_->setText(label);
    bar_->setRange(0, 0);
    bar_->show();
    progressTimer_->start();
    updateControls();
    const auto cancel = cancel_.token();
    const auto progress = progress_;
    auto* watcher = new QFutureWatcher<Result<Outcome>>(this);
    connect(watcher, &QFutureWatcher<Result<Outcome>>::finished, this,
            [this, watcher, finished = std::move(finished)] {
                const auto result = watcher->result();
                watcher->deleteLater();
                busy_ = false;
                progressTimer_->stop();
                bar_->hide();
                if (closeAfterCancel_) {
                    QDialog::reject();
                    return;
                }
                if (result)
                    finished(*result);
                else
                    status_->setText(QString::fromStdString(result.error().to_string()));
                updateControls();
            });
    watcher->setFuture(QtConcurrent::run([work = std::move(work), cancel,
                                          progress]() -> Result<Outcome> {
        try {
            return work(cancel, [progress](quint64 received, quint64 total) {
                progress->received.store(received);
                progress->total.store(total);
            });
        } catch (const std::exception& error) {
            return make_error(ErrorCode::Internal, "Setup operation failed", error.what());
        } catch (...) {
            return make_error(ErrorCode::Internal, "Setup operation failed with an unknown error.");
        }
    }));
}

void ResourceSetupDialog::validateSelection() {
    const auto chosen = selection();
    const auto memory = memory_->isChecked();
    const auto reasoning = reasoning_->isChecked();
    const auto installer = installer_;
    const auto runtimeRoot = runtimeRoot_;
    job(
        reasoning ? tr("Checking files and preparing the reasoning runtime…")
        : memory ? tr("Checking files and preparing the RAM runtime…")
               : tr("Checking runtime and GGUF headers…"),
        [chosen, memory, reasoning, installer, runtimeRoot](CancellationToken cancel,
                                                ResourceProgress progress) -> Result<Outcome> {
            const auto features = ModelServer::probe_binary(native(chosen.runtime), cancel);
            if (!features) return features.error();
            const auto model = read_gguf_metadata(native(chosen.model));
            if (!model) return model.error();
            if (!chosen.projector.isEmpty()) {
                const auto projector = read_gguf_metadata(native(chosen.projector));
                if (!projector) return projector.error();
            }
            if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Setup cancelled.");
            Outcome out;
            out.path = chosen.runtime;
            if (memory || reasoning) {
                const auto copy = reasoning
                    ? installer->prepareReasoningRuntime(chosen.runtime, runtimeRoot, progress, cancel)
                    : installer->prepareMemoryRuntime(chosen.runtime, runtimeRoot, progress, cancel);
                if (!copy) return copy.error();
                const auto verified = ModelServer::probe_binary(native(*copy), cancel);
                if (!verified) return verified.error();
                out.path = *copy;
                out.model = QFileInfo(chosen.model).absoluteFilePath();
                if (!chosen.projector.isEmpty())
                    out.projector = QFileInfo(chosen.projector).absoluteFilePath();
            }
            return out;
        },
        [this](const Outcome& out) {
            runtime_->setText(out.path);
            if (!out.model.isEmpty()) model_->setText(out.model);
            if (!out.projector.isEmpty()) projector_->setText(out.projector);
            accept();
        });
}
}   // namespace localcode::gui
