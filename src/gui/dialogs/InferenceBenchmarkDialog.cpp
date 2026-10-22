#include "InferenceBenchmarkDialog.h"
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <atomic>
#include <memory>
#include <cmath>
#include "../ScreenGuard.h"
#include "platform/CpuPlacement.h"
#include "platform/PathEncoding.h"

namespace localcode::gui {
namespace {
bool advertises(const QString& help, const QString& flag) {
    return QRegularExpression("(?<![A-Za-z0-9_-])" + QRegularExpression::escape(flag) +
                              "(?![A-Za-z0-9_-])")
        .match(help)
        .hasMatch();
}

QString utcNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QJsonObject fileIdentity(const QString& path) {
    const QFileInfo file(path);
    if (!file.isFile()) return {};
    return {{"path", file.canonicalFilePath()},
            {"size", file.size()},
            {"modifiedMs", file.lastModified().toMSecsSinceEpoch()}};
}

QJsonArray adjacentLibraryIdentity(const QString& executable) {
    QJsonArray result;
    if (!QFileInfo(executable).isFile()) return result;
    const QDir directory = QFileInfo(executable).dir();
    const QRegularExpression library("\\.(dll|dylib|so(?:\\.[0-9]+)*)$",
                                     QRegularExpression::CaseInsensitiveOption);
    for (const auto& file : directory.entryInfoList(QDir::Files, QDir::Name)) {
        if (!library.match(file.fileName()).hasMatch()) continue;
        auto identity = fileIdentity(file.absoluteFilePath());
        identity.insert("name", file.fileName());
        result.append(identity);
    }
    return result;
}
}   // namespace

InferenceBenchmarkDialog::InferenceBenchmarkDialog(const QString& model, const QString& server,
                                                   QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Compare CPU performance"));
    resize(760, 660);
    setMinimumSize(360, 340);
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(
        tr("Measure prompt and generation speed with your model. "
           "Each thread count runs twice in reverse order. This loads a separate CPU copy; "
           "unload other models if RAM is tight. After a complete comparison you can choose "
           "CPU thread counts to try in Performance settings."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("BenchmarkOptions");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* form = new QFormLayout(content);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto file = [&](const QString& title, const char* name, const QString& value) {
        auto* row = new QWidget(content);
        auto* box = new QHBoxLayout(row);
        box->setContentsMargins(0, 0, 0, 0);
        auto* field = new QLineEdit(value, row);
        field->setObjectName(name);
        field->setMinimumWidth(80);
        auto* browse = new QPushButton(tr("Browse…"), row);
        box->addWidget(field, 1);
        box->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this, field, title] {
            const auto chosen = QFileDialog::getOpenFileName(this, title, field->text());
            if (!chosen.isEmpty()) field->setText(chosen);
        });
        form->addRow(title, row);
        return field;
    };
#ifdef Q_OS_WIN
    const QString filename = "llama-bench.exe";
#else
    const QString filename = "llama-bench";
#endif
    const auto suggested =
        server.isEmpty() ? QString{} : QFileInfo(server).dir().filePath(filename);
    targetModel_ = fileIdentity(model);
    targetBenchmark_ = fileIdentity(suggested);
    targetServer_ = fileIdentity(server);
    targetLibraries_ = adjacentLibraryIdentity(server);
    targetServerPath_ = server;
    bench_ = file(tr("Benchmark executable"), "BenchmarkExecutable", suggested);
    model_ = file(tr("GGUF model"), "BenchmarkModel", model);
    repackingNote_ = new QLabel(tr("Runtime default"), content);
    repackingNote_->setObjectName("BenchmarkWeightRepacking");
    repackingNote_->setWordWrap(true);
    form->addRow(tr("CPU weight repacking"), repackingNote_);
    placementNote_ = new QLabel(tr("System scheduler"), content);
    placementNote_->setObjectName("BenchmarkCpuPlacement");
    placementNote_->setWordWrap(true);
    form->addRow(tr("CPU core placement"), placementNote_);
    const int logical = qMax(1, QThread::idealThreadCount());
    QStringList defaults;
    for (int count : {1, qMin(2, logical), qMax(1, logical / 2), logical})
        if (!defaults.contains(QString::number(count))) defaults.append(QString::number(count));
    threads_ = new QLineEdit(defaults.join(","), content);
    threads_->setObjectName("BenchmarkThreads");
    defaultThreadInput_ = threads_->text();
    threads_->setPlaceholderText(tr("For example: 1,2,4"));
    form->addRow(tr("Thread counts (up to 8)"), threads_);
    auto spin = [&](const QString& title, const char* name, int min, int max, int value) {
        auto* field = new QSpinBox(content);
        field->setObjectName(name);
        field->setRange(min, max);
        field->setValue(value);
        form->addRow(title, field);
        return field;
    };
    prompt_ = spin(tr("Prompt tokens"), "BenchmarkPrompt", 32, 8192, 128);
    generated_ = spin(tr("Generated tokens"), "BenchmarkGenerated", 16, 512, 32);
    repetitions_ = spin(tr("Repetitions per trial"), "BenchmarkRepetitions", 2, 20, 3);
    auto* note = new QLabel(
        tr("CPU only, normal priority, fresh context, runtime-default batches and KV precision, "
           "warmup enabled. Background activity affects these measurements. "
           "Each trial is limited to 5 minutes; the whole comparison to 30 minutes."),
        content);
    note->setWordWrap(true);
    form->addRow(note);
    generationChoice_ = new QComboBox(content);
    generationChoice_->setObjectName("BenchmarkGenerationChoice");
    promptChoice_ = new QComboBox(content);
    promptChoice_->setObjectName("BenchmarkPromptChoice");
    for (auto* choice : {generationChoice_, promptChoice_}) {
        choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        choice->setMinimumContentsLength(14);
        connect(choice,qOverload<int>(&QComboBox::currentIndexChanged),this,
                [this] { updateThreadSelection(); });
    }
    form->addRow(tr("Generation threads to try"), generationChoice_);
    form->addRow(tr("Prompt threads to try"), promptChoice_);
    selectionNote_ = new QLabel(content);
    selectionNote_->setObjectName("BenchmarkSelectionNote");
    selectionNote_->setWordWrap(true);
    form->addRow(selectionNote_);
    useThreads_ = new QPushButton(tr("Try these CPU threads"), this);
    useThreads_->setObjectName("BenchmarkUseThreads");
    connect(useThreads_, &QPushButton::clicked, this, [this] {
        updateThreadSelection();
        if (useThreads_->isEnabled()) accept();
    });
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    status_ = new QLabel(tr("Choose files and thread counts, then start."), this);
    status_->setObjectName("BenchmarkStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName("BenchmarkResults");
    table_->setHorizontalHeaderLabels(
        {tr("Trial"), tr("Threads"), tr("Prompt tok/s"), tr("Generation tok/s ± SD")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->hide();
    layout->addWidget(table_, 1);
    layout->addWidget(useThreads_);
    for (auto* field : {bench_, model_, threads_})
        connect(field, &QLineEdit::textChanged, this,
                &InferenceBenchmarkDialog::updateThreadSelection);
    for (auto* field : {prompt_, generated_, repetitions_})
        connect(field, &QSpinBox::valueChanged, this,
                &InferenceBenchmarkDialog::updateThreadSelection);
    updateThreadSelection();
    auto* actions = new QHBoxLayout;
    start_ = new QPushButton(tr("Start comparison"), this);
    start_->setObjectName("BenchmarkStart");
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName("BenchmarkStop");
    stop_->setEnabled(false);
    save_ = new QPushButton(tr("Save results…"), this);
    save_->setObjectName("BenchmarkSave");
    save_->setEnabled(false);
    actions->addWidget(start_);
    actions->addWidget(stop_);
    actions->addWidget(save_);
    layout->addLayout(actions);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &InferenceBenchmarkDialog::reject);
    connect(start_, &QPushButton::clicked, this, &InferenceBenchmarkDialog::start);
    connect(stop_, &QPushButton::clicked, this,
            [this] { stop(tr("Comparison stopped. Partial results retained.")); });
    connect(save_, &QPushButton::clicked, this, &InferenceBenchmarkDialog::saveReport);
    process_ = new QProcess(this);
    process_->setObjectName("BenchmarkProcess");
    connect(process_, &QProcess::started, this, [this] {
        trialProcessId_ = static_cast<std::uint32_t>(process_->processId());
    });
    connect(process_, &QProcess::readyReadStandardOutput, this,
            &InferenceBenchmarkDialog::readOutput);
    connect(process_, &QProcess::readyReadStandardError, this,
            &InferenceBenchmarkDialog::readOutput);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && stage_ != Stage::Idle)
            settle(tr("Could not start benchmark: %1").arg(process_->errorString()));
    });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finished(code, status == QProcess::NormalExit);
    });
    connect(process_, &QProcess::finished, scroll, [this, scroll] {
        if (report_.value("completed").toBool()) scroll->ensureWidgetVisible(generationChoice_);
    });
    deadline_ = new QTimer(this);
    deadline_->setObjectName("BenchmarkDeadline");
    deadline_->setSingleShot(true);
    connect(deadline_, &QTimer::timeout, this,
            [this] { stop(tr("Trial timed out. Partial results retained.")); });
    overallDeadline_ = new QTimer(this);
    overallDeadline_->setSingleShot(true);
    connect(overallDeadline_, &QTimer::timeout, this,
            [this] { stop(tr("Comparison timed out. Partial results retained.")); });
}

InferenceBenchmarkDialog::~InferenceBenchmarkDialog() {
    if (ownedProcess_) {
        ownedCancellation_.cancel();
        ownedProcess_->waitForFinished();
    }
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(1000);
    }
}

QJsonObject InferenceBenchmarkDialog::report() const {
    auto result = report_;
    if (stage_ == Stage::Run) {
        const auto pid = ownedProcess_ && ownedProcessId_ ? ownedProcessId_->load()
                         : static_cast<std::uint32_t>(process_->processId());
        if (pid) result.insert("activeProcessId",static_cast<double>(pid));
    }
    return result;
}

void InferenceBenchmarkDialog::setThreadSelectionEnabled(bool enabled, int maximumThreads) {
    threadSelectionEnabled_ = enabled;
    maximumThreads_ = maximumThreads;
    if (report_.value("completed").toBool()) summarizeThreads();
    updateThreadSelection();
}

void InferenceBenchmarkDialog::setWeightRepacking(const QJsonValue& value) {
    if (stage_ != Stage::Idle) return;
    weightRepacking_ = value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
    repackingNote_->setText(weightRepacking_.isNull() ? tr("Runtime default")
                              : !weightRepacking_.isBool() ? tr("Invalid setting; correct Performance settings")
                              : weightRepacking_.toBool() ? tr("Enabled") : tr("Disabled"));
    updateThreadSelection();
}

void InferenceBenchmarkDialog::setCpuPlacement(const QString& value) {
    if (stage_ != Stage::Idle) return;
    cpuPlacement_ = value;
    placementNote_->setText(value == "os" ? tr("System scheduler")
                           : value == "physical_cores" ? tr("Physical cores (prefer faster cores)")
                           : value == "physical_pool" ? tr("Physical core pool")
                           : tr("Invalid setting; correct Performance settings"));
    if (value == "physical_pool" && threads_->text() == defaultThreadInput_) {
        const auto processors = available_cpu_processors();
        if (processors) {
            const auto automatic = physical_cpu_pool(*processors, 0, 0);
            if (automatic) {
                QStringList choices;
                for (const auto count : {std::uint32_t{1}, std::uint32_t{2}, automatic->threads}) {
                    const auto text = QString::number(count);
                    if (!choices.contains(text) && physical_cpu_pool(*processors,count,count)) choices.append(text);
                }
                threads_->setText(choices.join(','));
            }
        }
    }
    if (value == "physical_cores" && threads_->text() == defaultThreadInput_) {
        const auto processors = available_cpu_processors();
        if (processors) {
            const auto automatic = physical_cpu_placement(*processors, 0, 0);
            if (automatic) {
                QStringList choices;
                for (const auto count : {std::uint32_t{2}, automatic->threads}) {
                    const auto text = QString::number(count);
                    if (!choices.contains(text)) choices.append(text);
                }
                threads_->setText(choices.join(','));
            }
        }
    }
    updateThreadSelection();
}

bool InferenceBenchmarkDialog::threadSelectionMatches() const {
    return threadSelectionEnabled_ && report_.value("completed").toBool() &&
           !targetModel_.isEmpty() && !targetBenchmark_.isEmpty() && !targetServer_.isEmpty() &&
           report_.value("modelIdentity").toObject() == targetModel_ &&
           report_.value("benchmarkIdentity").toObject() == targetBenchmark_ &&
           fileIdentity(model_->text()) == targetModel_ &&
           fileIdentity(bench_->text()) == targetBenchmark_ &&
           fileIdentity(targetServerPath_) == targetServer_ &&
           report_.value("libraryIdentity").toArray() == targetLibraries_ &&
           adjacentLibraryIdentity(bench_->text()) == targetLibraries_ &&
           adjacentLibraryIdentity(targetServerPath_) == targetLibraries_ &&
           report_.value("weightRepacking") == weightRepacking_ &&
           report_.value("cpuPlacement").toString("os") == cpuPlacement_ &&
           threads_->text() == report_.value("threadInput").toString() &&
           prompt_->value() == report_.value("promptTokens").toInt() &&
           generated_->value() == report_.value("generatedTokens").toInt() &&
           repetitions_->value() == report_.value("repetitions").toInt();
}

bool InferenceBenchmarkDialog::measurementInputsUnchanged() const {
    return fileIdentity(program_) == report_.value("benchmarkIdentity").toObject() &&
           fileIdentity(model_->text()) == report_.value("modelIdentity").toObject() &&
           adjacentLibraryIdentity(program_) == report_.value("libraryIdentity").toArray();
}

QJsonObject InferenceBenchmarkDialog::selectedThreadSettings() const {
    if (result() != QDialog::Accepted || !threadSelectionMatches() || !selectedPoolsMatch() ||
        generationChoice_->currentIndex() < 0 || promptChoice_->currentIndex() < 0)
        return {};
    return {{"threads", generationChoice_->currentData().toInt()},
            {"threads_batch", promptChoice_->currentData().toInt()}};
}

bool InferenceBenchmarkDialog::selectedPoolsMatch() const {
    if (cpuPlacement_ != "physical_pool") return true;
    if (generationChoice_->currentIndex() < 0 || promptChoice_->currentIndex() < 0) return false;
    const auto pools = report_.value("cpuPools").toObject();
    const auto generation = pools.value(QString::number(generationChoice_->currentData().toInt())).toArray();
    const auto prompt = pools.value(QString::number(promptChoice_->currentData().toInt())).toArray();
    return !generation.isEmpty() && generation == prompt;
}

void InferenceBenchmarkDialog::updateThreadSelection() {
    const bool enabled =
        threadSelectionMatches() && generationChoice_->count() > 0 && promptChoice_->count() > 0;
    useThreads_->setEnabled(enabled && selectedPoolsMatch());
    generationChoice_->setEnabled(enabled);
    promptChoice_->setEnabled(enabled);
    selectionNote_->setText(
        enabled && !selectedPoolsMatch()
            ? tr("These prompt and generation measurements used different CPU pools. Choose counts measured "
                 "with the same pool, or adjust the threads manually in Performance settings.")
        : enabled
            ? tr("Mean speeds cover both trial orders; ranges are in the tooltips. Highest means "
                 "are selected initially; "
                 "background load, context and batch settings can change the result. Only threads "
                 "will be copied; save Performance settings and reload to try them.")
            : tr("Thread transfer requires a complete comparison for this CPU-only profile's "
                 "model and runtime, within the profile's thread limit. Keep the measured inputs "
                 "or run again; reopen this window if the files have changed."));
}

void InferenceBenchmarkDialog::summarizeThreads() {
    generationChoice_->clear();
    promptChoice_->clear();

    struct Rates {
        QList<double> prompt, generation;
    };

    QMap<int, Rates> rates;
    for (const auto value : report_.value("trials").toArray()) {
        const auto trial = value.toObject();
        auto& rate = rates[trial.value("threads").toInt()];
        for (const auto measurement : trial.value("measurements").toArray()) {
            const auto row = measurement.toObject();
            (row.value("n_gen").toInt() == 0 ? rate.prompt : rate.generation)
                .append(row.value("avg_ts").toDouble());
        }
    }
    QJsonArray summary;
    double bestPrompt = 0, bestGeneration = 0;
    for (auto it = rates.cbegin(); it != rates.cend(); ++it) {
        if (it->prompt.size() != 2 || it->generation.size() != 2) return;
        const auto add = [&](QComboBox* choice, const QList<double>& values, double& best) {
            const double mean = values[0] / 2 + values[1] / 2;
            if (it.key() <= maximumThreads_) {
                choice->addItem(tr("%1 · %2 tok/s").arg(it.key()).arg(mean, 0, 'f', 2), it.key());
                choice->setItemData(choice->count() - 1,
                                    tr("Mean %1 tok/s; range %2–%3 across both trial orders.")
                                        .arg(mean, 0, 'f', 2)
                                        .arg(qMin(values[0], values[1]), 0, 'f', 2)
                                        .arg(qMax(values[0], values[1]), 0, 'f', 2),
                                    Qt::ToolTipRole);
                if (mean > best) {
                    best = mean;
                    choice->setCurrentIndex(choice->count() - 1);
                }
            }
            return QJsonObject{{"mean", mean},
                               {"min", qMin(values[0], values[1])},
                               {"max", qMax(values[0], values[1])}};
        };
        summary.append(
            QJsonObject{{"threads", it.key()},
                        {"prompt", add(promptChoice_, it->prompt, bestPrompt)},
                        {"generation", add(generationChoice_, it->generation, bestGeneration)}});
    }
    report_.insert("threadSummary", summary);
}

void InferenceBenchmarkDialog::reject() {
    stop(tr("Comparison closed. Partial results retained."));
    QDialog::reject();
}

void InferenceBenchmarkDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    clampToScreen(*this);
}

void InferenceBenchmarkDialog::start() {
    if (stage_ != Stage::Idle || ownedProcess_ || process_->state() != QProcess::NotRunning) return;
    if (!weightRepacking_.isNull() && !weightRepacking_.isBool()) {
        status_->setText(tr("Correct the CPU weight repacking setting before comparing."));
        return;
    }
    if (cpuPlacement_ != "os" && cpuPlacement_ != "physical_cores" && cpuPlacement_ != "physical_pool") {
        status_->setText(tr("Correct CPU core placement in Performance settings before comparing."));
        return;
    }
    const QFileInfo executable(bench_->text()), model(model_->text());
    if (!executable.isFile() || !model.isFile()) {
        status_->setText(tr("Choose an existing benchmark executable and model file."));
        return;
    }
    modelName_ = model.fileName();
    if (modelName_.contains(',')) {
        status_->setText(tr(
            "This runtime treats commas as model separators. Choose a filename without commas."));
        return;
    }
    QList<int> counts;
    const int logical = qMax(1, QThread::idealThreadCount());
    for (const auto& text : threads_->text().split(',')) {
        bool ok = false;
        int value = text.trimmed().toInt(&ok);
        if (!ok || value < 1 || value > logical || counts.size() >= 8 || counts.contains(value)) {
            status_->setText(
                tr("Use up to 8 distinct thread counts between 1 and %1, separated by commas.")
                    .arg(logical));
            return;
        }
        counts.append(value);
    }
    placementMasks_ = {};
    placementPools_ = {};
    if (cpuPlacement_ == "physical_cores") {
        const auto processors = available_cpu_processors();
        if (!processors) {
            status_->setText(QString::fromStdString(processors.error().to_string()));
            return;
        }
        for (const auto count : counts) {
            const auto placement = physical_cpu_placement(*processors, static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(count));
            if (!placement) {
                status_->setText(QString::fromStdString(placement.error().to_string()));
                return;
            }
            placementMasks_.insert(QString::number(count), QString::fromStdString(placement->mask));
        }
    } else if (cpuPlacement_ == "physical_pool") {
        const auto processors = available_cpu_processors();
        if (!processors) {
            status_->setText(QString::fromStdString(processors.error().to_string()));
            return;
        }
        for (const auto count : counts) {
            const auto pool = physical_cpu_pool(*processors, static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(count));
            if (!pool) { status_->setText(QString::fromStdString(pool.error().to_string())); return; }
            QJsonArray cpus;
            for (const auto cpu : pool->logical_processors) cpus.append(static_cast<int>(cpu));
            placementPools_.insert(QString::number(count),cpus);
        }
    }
    trials_ = counts;
    for (auto it = counts.crbegin(); it != counts.crend(); ++it)
        trials_.append(*it);
    trial_ = 0;
    ++comparison_;
    table_->setRowCount(0);
    generationChoice_->clear();
    promptChoice_->clear();
    modelDirectory_ = model.absolutePath();
    program_ = executable.absoluteFilePath();
    stopReason_.clear();
    stdout_.clear();
    stderr_.clear();
    report_ = {{"startedUtc", utcNow()},
               {"completed", false},
               {"model", model.absoluteFilePath()},
               {"executable", program_},
               {"modelIdentity", fileIdentity(model.absoluteFilePath())},
               {"benchmarkIdentity", fileIdentity(program_)},
               {"libraryIdentity", adjacentLibraryIdentity(program_)},
               {"weightRepacking", weightRepacking_},
               {"cpuPlacement", cpuPlacement_},
               {"cpuMasks", placementMasks_},
               {"cpuPools", placementPools_},
               {"threadInput", threads_->text()},
               {"logicalCpus", logical},
               {"threadOrder", QJsonArray::fromVariantList([&] {
                    QVariantList values;
                    for (int count : trials_)
                        values.append(count);
                    return values;
                }())},
               {"scope",
                "CPU-only; forward/reverse order; normal priority; warmup; runtime-default "
                "batch/KV; selected weight repacking when explicit; background load uncontrolled; no automatic settings"},
               {"trials", QJsonArray{}},
               {"promptTokens", prompt_->value()},
               {"generatedTokens", generated_->value()},
               {"repetitions", repetitions_->value()}};
    updateThreadSelection();
    for (auto* field : {bench_, model_})
        field->parentWidget()->setEnabled(false);
    threads_->setEnabled(false);
    for (auto* field : {prompt_, generated_, repetitions_})
        field->setEnabled(false);
    start_->setEnabled(false);
    stop_->setEnabled(true);
    save_->setEnabled(false);
    stage_ = Stage::Help;
    status_->setText(tr("Checking benchmark options…"));
    process_->setWorkingDirectory(executable.absolutePath());
    process_->start(program_, {"--help"});
    deadline_->start(20000);
    overallDeadline_->start(30 * 60 * 1000);
}

void InferenceBenchmarkDialog::readOutput() {
    stdout_ += process_->readAllStandardOutput();
    stderr_ += process_->readAllStandardError();
    if (stdout_.size() > 4 * 1024 * 1024 || stderr_.size() > 256 * 1024) {
        stdout_.truncate(4 * 1024 * 1024);
        stderr_.truncate(256 * 1024);
        stop(tr("Benchmark output exceeded its limit. Partial results retained."));
    }
}

void InferenceBenchmarkDialog::stop(const QString& reason) {
    if (stage_ == Stage::Idle) return;
    stopReason_ = reason;
    deadline_->stop();
    overallDeadline_->stop();
    if (ownedProcess_)
        ownedCancellation_.cancel();
    else if (process_->state() != QProcess::NotRunning)
        process_->kill();
    else
        settle(reason);
}

void InferenceBenchmarkDialog::settle(const QString& message) {
    deadline_->stop();
    overallDeadline_->stop();
    stage_ = Stage::Idle;
    report_.insert("finishedUtc", utcNow());
    report_.insert("message", message);
    status_->setText(message);
    for (auto* field : {bench_, model_})
        field->parentWidget()->setEnabled(true);
    threads_->setEnabled(true);
    for (auto* field : {prompt_, generated_, repetitions_})
        field->setEnabled(true);
    start_->setEnabled(true);
    stop_->setEnabled(false);
    save_->setEnabled(!report_.isEmpty());
    updateThreadSelection();
}

void InferenceBenchmarkDialog::next() {
    if (stage_ == Stage::Idle || !stopReason_.isEmpty()) return;
    if (!measurementInputsUnchanged()) {
        settle(tr("Model or runtime files changed during comparison. Reopen this window to measure again."));
        return;
    }
    if (trial_ >= trials_.size()) {
        report_.insert("completed", true);
        summarizeThreads();
        settle(
            tr("Comparison complete. Compare both trials and their variation before choosing "
               "threads in Performance settings."));
        return;
    }
    stdout_.clear();
    stderr_.clear();
    QStringList args{"-m",
                     modelName_,
                     "--device",
                     "none",
                     "--n-gpu-layers",
                     "0",
                     "--no-kv-offload",
                     "1",
                     "-d",
                     "0",
                     "--threads",
                     QString::number(trials_[trial_]),
                     "-p",
                     QString::number(prompt_->value()),
                     "-n",
                     QString::number(generated_->value()),
                     "-r",
                     QString::number(repetitions_->value()),
                     "--prio",
                     "0",
                     "-o",
                     "json"};
    if (advertises(help_, "--no-op-offload")) args.append({"--no-op-offload", "1"});
    if (advertises(help_, "--offline")) args.append("--offline");
    if (weightRepacking_.isBool())
        args.append({"--repack", weightRepacking_.toBool() ? "1" : "0"});
    if (cpuPlacement_ == "physical_cores")
        args.append({"--cpu-mask", placementMasks_.value(QString::number(trials_[trial_])).toString(),
                     "--cpu-strict", "1"});
    stage_ = Stage::Run;
    status_->setText(
        tr("Trial %1 / %2 · %3 threads…").arg(trial_ + 1).arg(trials_.size()).arg(trials_[trial_]));
    process_->setWorkingDirectory(modelDirectory_);
    phaseTime_.start();
    trialArguments_ = args;
    trialProcessId_ = 0;
    if (cpuPlacement_ == "physical_pool") {
        ProcessRequest request;
        request.executable = path_from_utf8(program_.toStdString());
        request.working_directory = path_from_utf8(modelDirectory_.toStdString());
        for (const auto& argument : args) request.arguments.push_back(argument.toStdString());
        for (const auto cpu : placementPools_.value(QString::number(trials_[trial_])).toArray())
            request.cpu_allowance.push_back(static_cast<std::uint32_t>(cpu.toInt()));
        request.timeout = std::chrono::minutes(5);
        request.max_captured_bytes = 4 * 1024 * 1024;
        auto pid = std::make_shared<std::atomic<std::uint32_t>>(0);
        ownedProcessId_ = pid;
        request.on_started = [pid](std::uint32_t value) { pid->store(value); };
        ownedCancellation_ = CancellationSource{};
        auto* watcher = new QFutureWatcher<Result<ProcessResult>>(this);
        watcher->setObjectName("BenchmarkOwnedProcess");
        ownedProcess_ = watcher;
        connect(watcher, &QFutureWatcher<Result<ProcessResult>>::finished, this, [this,watcher,pid] {
            const auto result = watcher->result();
            ownedProcess_ = nullptr;
            watcher->deleteLater();
            trialProcessId_ = pid->load();
            if (!result) {
                report_.insert("runtimeError",QString::fromStdString(result.error().to_string()));
                settle(tr("Could not start the CPU pool benchmark. Save results for details."));
                return;
            }
            stdout_ = QByteArray::fromStdString(result->stdout_text);
            stderr_ = QByteArray::fromStdString(result->stderr_text);
            if (result->stdout_truncated || result->stderr_truncated || result->stderr_total_bytes > 256 * 1024) {
                settle(tr("Benchmark output exceeded its limit. Partial results retained."));
                return;
            }
            if (result->timed_out && stopReason_.isEmpty()) stopReason_ = tr("Trial timed out. Partial results retained.");
            finished(result->exit_code, !result->timed_out && !result->cancelled);
        });
        const auto token = ownedCancellation_.token();
        watcher->setFuture(QtConcurrent::run([request = std::move(request),token] {
            return ProcessRunner::run(request,token);
        }));
    } else {
        process_->start(program_, args);
    }
    deadline_->start(5 * 60 * 1000);
}

void InferenceBenchmarkDialog::finished(int exitCode, bool normalExit) {
    if (stage_ == Stage::Idle) return;
    deadline_->stop();
    readOutput();
    if (!stopReason_.isEmpty()) {
        settle(stopReason_);
        return;
    }
    if (!normalExit || exitCode != 0) {
        report_.insert("runtimeError", QString::fromUtf8(stderr_.right(4096)));
        settle(tr("Benchmark failed (exit %1). Save results for the runtime error.").arg(exitCode));
        return;
    }
    if (stage_ == Stage::Help) {
        help_ = QString::fromUtf8(stdout_ + '\n' + stderr_);
        for (const QString flag : {"--device", "--n-gpu-layers", "--no-kv-offload", "--threads",
                                   "--n-depth", "--output", "--repetitions", "--prio"})
            if (!advertises(help_, flag)) {
                settle(tr("This benchmark does not advertise %1.").arg(flag));
                return;
            }
        report_.insert("runtimeHelp", help_);
#ifdef Q_OS_WIN
        const bool unicodeName = std::any_of(modelName_.cbegin(), modelName_.cend(),
            [](QChar letter) { return letter.unicode() < 32 || letter.unicode() > 126; });
        if (unicodeName && !help_.contains("LocalCode Windows UTF-8 argv v1")) {
            settle(tr("This benchmark does not advertise UTF-8 filenames. Choose a compatible "
                      "benchmark or an ASCII model filename; its folders may contain accents."));
            return;
        }
#endif
        if (cpuPlacement_ == "physical_cores" &&
            (!advertises(help_, "--cpu-mask") || !advertises(help_, "--cpu-strict"))) {
            settle(tr("This benchmark cannot measure physical CPU core placement. Choose system scheduling or another benchmark."));
            return;
        }
        if (weightRepacking_.isBool() &&
            !QRegularExpression("--repack\\s+<0\\|1>").match(help_).hasMatch()) {
            settle(tr("This benchmark cannot measure the selected weight repacking mode. "
                      "Choose a benchmark that advertises --repack <0|1>, or use runtime default in Performance settings."));
            return;
        }
        next();
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(stdout_, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray() ||
        document.array().size() != 2) {
        settle(tr(
            "Benchmark returned invalid measurements. No speed result accepted for this trial."));
        return;
    }
    QJsonObject prompt, generation;
    if (!measurementInputsUnchanged()) {
        settle(tr("Model or runtime files changed during comparison. Reopen this window to measure again."));
        return;
    }
    for (const auto value : document.array()) {
        const auto row = value.toObject();
        if (weightRepacking_.isBool() &&
            (!row.value("repack").isBool() || row.value("repack") != weightRepacking_)) {
            settle(tr("Benchmark did not confirm the selected weight repacking mode."));
            return;
        }
        if (cpuPlacement_ == "physical_cores" &&
            (!row.value("cpu_strict").isBool() || !row.value("cpu_strict").toBool() ||
             row.value("cpu_mask").toString() != placementMasks_.value(QString::number(trials_[trial_])).toString())) {
            settle(tr("Benchmark did not confirm the selected physical CPU placement."));
            return;
        }
        const double rate = row.value("avg_ts").toDouble(-1),
                     deviation = row.value("stddev_ts").toDouble(-1);
        if (row.value("n_threads").toInt(-1) != trials_[trial_] || !std::isfinite(rate) ||
            rate <= 0 || !std::isfinite(deviation) || deviation < 0 ||
            row.value("n_gpu_layers").toInt(-1) != 0 || row.value("n_depth").toInt(-1) != 0 ||
            row.value("devices").toString() != "none" || !row.value("no_kv_offload").toBool() ||
            QFileInfo(row.value("model_filename").toString()).fileName() != modelName_) {
            settle(tr("Benchmark returned mismatched or invalid measurements."));
            return;
        }
        if (row.value("n_prompt").toInt(-1) == prompt_->value() &&
            row.value("n_gen").toInt(-1) == 0 && prompt.isEmpty())
            prompt = row;
        else if (row.value("n_gen").toInt(-1) == generated_->value() &&
                 row.value("n_prompt").toInt(-1) == 0 && generation.isEmpty())
            generation = row;
        else {
            settle(tr("Benchmark returned an unexpected workload."));
            return;
        }
    }
    if (prompt.isEmpty() || generation.isEmpty()) {
        settle(tr("Benchmark omitted a requested measurement."));
        return;
    }
    auto trials = report_.value("trials").toArray();
    trials.append(QJsonObject{{"threads", trials_[trial_]},
                              {"elapsedMs", phaseTime_.elapsed()},
                              {"arguments", QJsonArray::fromStringList(trialArguments_)},
                              {"processId", static_cast<double>(trialProcessId_)},
                              {"cpuAllowance", placementPools_.value(QString::number(trials_[trial_]))},
                              {"measurements", document.array()},
                              {"stderr", QString::fromUtf8(stderr_.right(4096))}});
    report_.insert("trials", trials);
    const int row = table_->rowCount();
    table_->insertRow(row);
    const QStringList cells{QString::number(trial_ + 1), QString::number(trials_[trial_]),
                            QString::number(prompt.value("avg_ts").toDouble(), 'f', 2),
                            tr("%1 ± %2")
                                .arg(generation.value("avg_ts").toDouble(), 0, 'f', 2)
                                .arg(generation.value("stddev_ts").toDouble(), 0, 'f', 2)};
    for (int column = 0; column < cells.size(); ++column)
        table_->setItem(row, column, new QTableWidgetItem(cells[column]));
    ++trial_;
    const auto comparison = comparison_;
    QTimer::singleShot(0, this, [this, comparison] {
        if (comparison_ == comparison) next();
    });
}

void InferenceBenchmarkDialog::saveReport() {
    const auto path = QFileDialog::getSaveFileName(this, tr("Save CPU comparison"),
                                                   "cpu-comparison.json", tr("JSON (*.json)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    const auto bytes = QJsonDocument(report_).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        status_->setText(tr("Could not save results: %1").arg(file.errorString()));
}
}   // namespace localcode::gui
