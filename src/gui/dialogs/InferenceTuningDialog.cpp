#include "InferenceTuningDialog.h"
#include <QComboBox>
#include <QJsonArray>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include "../ScreenGuard.h"
#include "InferenceBenchmarkDialog.h"
#include "platform/Environment.h"
#include "platform/GpuProbe.h"
#include "util/Strings.h"

namespace localcode::gui {
InferenceTuningDialog::InferenceTuningDialog(const QJsonObject& input, bool plugin, QWidget* parent)
    : QDialog(parent), plugin_(plugin) {
    setWindowTitle(tr("Inference performance"));
    setModal(true);
    resize(600, 620);
    setMinimumSize(380, 340);
    auto* box = new QVBoxLayout(this);
    hardware_ = new QLabel(tr("Detecting CPU, RAM and available GPU memory…"), this);
    hardware_->setObjectName("TuningHardware");
    hardware_->setWordWrap(true);
    hardware_->setTextFormat(Qt::PlainText);
    box->addWidget(hardware_);
    auto* explanation = new QLabel(tr("Compare settings on this PC. More threads can be slower. "
                                      "Larger context and batches use more memory; quantized KV "
                                      "can change answers. Apply settings, then reload the model."),
                                   this);
    explanation->setWordWrap(true);
    box->addWidget(explanation);
    auto* measure = new QPushButton(tr("Compare CPU performance…"), this);
    measure->setObjectName("TuningBenchmark");
    measure->setToolTip(tr("Compare the chosen CPU weight repacking mode. "
                          "An explicit choice requires a benchmark that supports it."));
    box->addWidget(measure);
    connect(measure, &QPushButton::clicked, this, [this] {
        InferenceBenchmarkDialog dialog(benchmarkModel_, benchmarkServer_, this);
        dialog.setWeightRepacking(settings().value("weight_repacking"));
        dialog.setCpuPlacement(settings().value("cpu_placement").toString("os"));
        dialog.setThreadSelectionEnabled(gpu_->value() == 0 && benchmarkThreadTransfer_,
                                         qMin(threads_->maximum(), threadsBatch_->maximum()));
        if (dialog.exec() != QDialog::Accepted) return;
        const auto selection = dialog.selectedThreadSettings();
        if (selection.isEmpty()) return;
        threads_->setValue(selection.value("threads").toInt());
        threadsBatch_->setValue(selection.value("threads_batch").toInt());
    });
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* form = new QFormLayout(content);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(content);
    box->addWidget(scroll, 1);
    auto spin = [&](const QString& label, const char* name, int min, int max, int current) {
        auto* field = new QSpinBox(content);
        field->setObjectName(name);
        field->setRange(min, max);
        field->setValue(current);
        form->addRow(label, field);
        return field;
    };
    context_ = spin(tr("Context tokens"), "TuningContext", plugin ? 2048 : 1024, 2147483647,
                    input.value("contextSize").toInt(4096));
    context_->setSingleStep(1024);
    gpu_ = spin(tr("Layers on GPU (0 = CPU)"), "TuningGpuLayers", plugin ? 0 : -1, 999,
                input.value("gpuLayers").toInt(0));
    if (!plugin) gpu_->setSpecialValueText(tr("All"));
    threads_ =
        spin(tr("CPU generation threads"), "TuningThreads", 0, 256, input.value("threads").toInt());
    threads_->setSpecialValueText(tr("Automatic"));
    threadsBatch_ = spin(tr("CPU prompt threads"), "TuningThreadsBatch", 0, 256,
                         input.value("threads_batch").toInt());
    threadsBatch_->setSpecialValueText(tr("Same as generation"));
    placement_ = new QComboBox(content);
    placement_->setObjectName("TuningCpuPlacement");
    placement_->addItem(tr("System scheduler"), "os");
    const auto modes = input.value("cpuPlacementModes");
    const auto advertised = modes.toArray();
    if (!plugin || !modes.isArray() || advertised.contains("physical_cores"))
        placement_->addItem(tr("Physical cores (prefer faster cores)"), "physical_cores");
    if (!plugin || (modes.isArray() && advertised.contains("physical_pool")))
        placement_->addItem(tr("Physical core pool"), "physical_pool");
    const auto requestedPlacement = input.value("cpu_placement");
    const auto placement = requestedPlacement.isUndefined() || requestedPlacement.isNull() ? QString("os")
                         : requestedPlacement.isString() ? requestedPlacement.toString() : QString("invalid");
    if (placement_->findData(placement) < 0)
        placement_->addItem(tr("Saved placement unavailable; choose an option"), placement);
    placement_->setCurrentIndex(qMax(0, placement_->findData(placement)));
    placementSupported_ = !plugin || input.contains("cpu_placement");
    placement_->setEnabled(placementSupported_);
    placement_->setToolTip(placementSupported_
        ? tr("Physical cores pins each worker and requires at least 2 generation and prompt threads. "
             "Physical core pool lets the system schedule within eligible physical cores and also supports one thread. "
             "Faster cores are preferred. Automatic uses the fastest available core class; explicit counts must fit "
             "available physical cores. Compare speed before saving.")
        : tr("Update the Discord plugin to configure CPU placement."));
    form->addRow(tr("CPU core placement"), placement_);
    batch_ =
        spin(tr("Prompt batch tokens"), "TuningBatch", 0, 32768, input.value("batch_size").toInt());
    batch_->setSpecialValueText(tr("Runtime default"));
    ubatch_ = spin(tr("Prompt microbatch tokens"), "TuningUbatch", 0, 32768,
                   input.value("ubatch_size").toInt());
    ubatch_->setSpecialValueText(tr("Runtime default"));
    ubatch_->setToolTip(
        tr("Limit how many prompt tokens are evaluated together. Compare "
           "memory use and prompt speed after reloading the model."));
    cacheRam_ =
        spin(plugin ? tr("Prompt cache RAM (MiB)") : tr("Prompt cache RAM (MiB; 0 = disabled)"),
             "TuningCacheRam", plugin ? 0 : -1, plugin ? 4096 : 2147483647,
             input.value("prompt_cache_mib").toInt(plugin ? 384 : -1));
    cacheRam_->setSpecialValueText(plugin ? tr("Disabled") : tr("Runtime default"));
    cacheRam_->setToolTip(tr(
        "Limit saved prompt states in host RAM. The active context's KV memory is separate. "
        "A smaller cache can require more prompt processing. Reload after changing this value."));
    repacking_ = new QComboBox(content);
    repacking_->setObjectName("TuningWeightRepacking");
    repacking_->addItem(tr("Runtime default"));
    repacking_->addItem(tr("Enabled"), true);
    repacking_->addItem(tr("Disabled (may use less RAM)"), false);
    const auto repack = input.value("weight_repacking");
    if (repack.isBool()) repacking_->setCurrentIndex(repack.toBool() ? 1 : 2);
    else if (!repack.isNull() && !repack.isUndefined()) {
        repacking_->addItem(tr("Invalid saved value — select a mode"), "invalid");
        repacking_->setCurrentIndex(3);
    }
    repackingSupported_ = !plugin || input.contains("weight_repacking");
    repacking_->setEnabled(repackingSupported_);
    repacking_->setToolTip(repackingSupported_
        ? tr("Allow the runtime to rearrange supported CPU weights for faster calculations. "
             "Disabling can reduce host RAM, slow prompt processing and change replies. "
             "Reload and recheck speed and answers after changing this value.")
        : tr("Update the Discord plugin to configure CPU weight repacking."));
    form->addRow(tr("CPU weight repacking"), repacking_);
    if (!plugin) {
        speculation_ = new QComboBox(content);
        speculation_->setObjectName("TuningSpeculation");
        speculation_->addItem(tr("Disabled"), false);
        speculation_->addItem(tr("Adaptive n-grams (unavailable: incorrect answers)"), true);
        auto* choices = qobject_cast<QStandardItemModel*>(speculation_->model());
        if (choices) choices->item(1)->setEnabled(false);
        speculation_->setCurrentIndex(input.value("ngram_speculation").toBool() ? 1 : 0);
        speculation_->setToolTip(
            tr("Adaptive n-grams failed answer-integrity checks and cannot be selected. "
               "If an existing profile has this enabled, choose Disabled and save "
               "before loading the model."));
        form->addRow(tr("Repetitive text acceleration"), speculation_);
    }
    auto precision = [&](const QString& label, const char* name, const QString& current) {
        auto* field = new QComboBox(content);
        field->setObjectName(name);
        field->addItem(tr("Runtime default"), "");
        field->addItem("FP16", "f16");
        field->addItem("BF16", "bf16");
        field->addItem(tr("Q8 (less memory)"), "q8_0");
        field->addItem(tr("Q4 (check quality)"), "q4_0");
        const int index = field->findData(current);
        if (index < 0) {
            field->addItem(current, current);
            field->setCurrentIndex(field->count() - 1);
        } else
            field->setCurrentIndex(index);
        form->addRow(label, field);
        return field;
    };
    key_ = precision(tr("KV keys"), "TuningKvK", input.value("cache_type_k").toString());
    value_ = precision(tr("KV values"), "TuningKvV", input.value("cache_type_v").toString());
    flash_ = new QComboBox(content);
    flash_->addItem(tr("On"), "on");
    flash_->addItem(tr("Off"), "off");
    if (plugin) flash_->addItem(tr("Automatic"), "auto");
    flash_->setCurrentIndex(
        qMax(0, flash_->findData(input.value("flash_attention").toString("on"))));
    form->addRow(tr("Flash Attention"), flash_);
    auto* warning = new QLabel(tr("The runtime and model must support the selected options. CPU "
                                  "mode does not make a large model fit into insufficient RAM."),
                               content);
    warning->setWordWrap(true);
    form->addRow(warning);
    auto* cpu = new QPushButton(tr("Use a CPU starting profile (4K)"), this);
    cpu->setObjectName("TuningCpuPreset");
    box->addWidget(cpu);
    connect(cpu, &QPushButton::clicked, this, [this] {
        context_->setValue(4096);
        gpu_->setValue(0);
        threads_->setValue(0);
        threadsBatch_->setValue(0);
        placement_->setCurrentIndex(0);
        batch_->setValue(512);
        ubatch_->setValue(128);
        key_->setCurrentIndex(0);
        value_->setCurrentIndex(0);
        cacheRam_->setValue(128);
        if (speculation_) speculation_->setCurrentIndex(0);
        if (plugin_) {
            flash_->setCurrentIndex(flash_->findData("auto"));
        }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (repacking_->currentIndex() > 2) {
            hardware_->setText(tr("Choose Runtime default, Enabled or Disabled for CPU weight repacking."));
            return;
        }
        if (speculation_ && speculation_->currentData().toBool()) {
            hardware_->setText(
                tr("Adaptive n-grams are unavailable. Choose Disabled before saving."));
            return;
        }
        if (batch_->value() > 0 && ubatch_->value() > batch_->value()) {
            hardware_->setText(tr("Microbatch must not exceed the prompt batch."));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    struct Detected {
        OsInfo os;
        std::optional<GpuMemory> gpu;
    };

    auto* watcher = new QFutureWatcher<Detected>(this);
    connect(watcher, &QFutureWatcher<Detected>::finished, this, [this, watcher] {
        const auto detected = watcher->result();
        watcher->deleteLater();
        QString text =
            tr("%1 logical CPUs · %2 RAM")
                .arg(detected.os.logical_cpus)
                .arg(QString::fromStdString(strings::human_bytes(detected.os.total_memory_bytes)));
        if (detected.gpu)
            text +=
                tr("\n%1 · %2 free of %3 VRAM")
                    .arg(QString::fromStdString(detected.gpu->name))
                    .arg(QString::fromStdString(strings::human_bytes(detected.gpu->free_bytes)))
                    .arg(QString::fromStdString(strings::human_bytes(detected.gpu->total_bytes)));
        else
            text += tr("\nGPU memory unavailable; verify the chosen runtime's device support.");
        hardware_->setText(text);
    });
    watcher->setFuture(
        QtConcurrent::run([] { return Detected{probe_os_info(), probe_gpu_memory()}; }));
}

QJsonObject InferenceTuningDialog::settings() const {
    QJsonObject out{{"contextSize", context_->value()},
                    {"gpuLayers", gpu_->value()},
                    {"threads", threads_->value()},
                    {"threads_batch", threadsBatch_->value()},
                    {"batch_size", batch_->value()},
                    {"ubatch_size", ubatch_->value()},
                    {"cache_type_k", key_->currentData().toString()},
                    {"cache_type_v", value_->currentData().toString()},
                    {"flash_attention", flash_->currentData().toString()}};
    if (cacheRam_->value() >= 0) {
        out.insert("prompt_cache_mib", cacheRam_->value());
    }
    if (repackingSupported_) {
        out.insert("weight_repacking", repacking_->currentIndex() == 0 ? QJsonValue(QJsonValue::Null)
                                      : repacking_->currentIndex() > 2 ? QJsonValue("invalid")
                                      : QJsonValue(repacking_->currentData().toBool()));
    }
    if (placementSupported_) out.insert("cpu_placement", placement_->currentData().toString());
    if (speculation_) out.insert("ngram_speculation", speculation_->currentData().toBool());
    return out;
}

void InferenceTuningDialog::setBenchmarkFiles(const QString& model, const QString& server,
                                              bool allowThreadTransfer) {
    benchmarkModel_ = model;
    benchmarkServer_ = server;
    benchmarkThreadTransfer_ = allowThreadTransfer;
}

void InferenceTuningDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    clampToScreen(*this);
}
}   // namespace localcode::gui
