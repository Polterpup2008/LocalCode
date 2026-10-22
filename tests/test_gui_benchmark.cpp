#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <set>
#include "dialogs/InferenceBenchmarkDialog.h"
#include "dialogs/InferenceTuningDialog.h"
#include "platform/CpuPlacement.h"
#if defined(Q_OS_WIN)
#include <windows.h>
#endif

using namespace localcode::gui;

TEST_CASE("CPU placement is portable profile data and older plugins receive no new field",
          "[gui][tuning][placement]") {
    for (const bool plugin : {false,true}) {
        InferenceTuningDialog dialog({{"cpu_placement",QJsonValue(QJsonValue::Null)}},plugin);
        auto* choice = dialog.findChild<QComboBox*>("TuningCpuPlacement");
        REQUIRE(choice);
        REQUIRE(choice->isEnabled());
        CHECK(dialog.settings().value("cpu_placement").toString() == "os");
        auto before = dialog.settings(); before.remove("cpu_placement");
        choice->setCurrentIndex(choice->findData("physical_cores"));
        auto after = dialog.settings();
        CHECK(after.value("cpu_placement").toString() == "physical_cores");
        after.remove("cpu_placement");
        CHECK(after == before);
        const auto screenshot = qEnvironmentVariable("LOCALCODE_CPU_PLACEMENT_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            dialog.show();
            QElapsedTimer ready; ready.start();
            while (dialog.findChild<QLabel*>("TuningHardware")->text().startsWith("Detecting") && ready.elapsed()<10000) {
                QApplication::processEvents(); QThread::msleep(10);
            }
            QApplication::processEvents(); QApplication::processEvents();
            const auto name=screenshot+(plugin?"-plugin.png":"-native.png");
            CHECK(dialog.grab().save(name));
            dialog.resize(420,480);
            dialog.findChild<QScrollArea*>()->ensureWidgetVisible(choice);
            QApplication::processEvents(); QApplication::processEvents();
            CHECK(dialog.grab().save(name+".small.png"));
        }
        dialog.findChild<QPushButton*>("TuningCpuPreset")->click();
        CHECK(dialog.settings().value("cpu_placement").toString() == "os");
    }
    InferenceTuningDialog older({},true);
    CHECK_FALSE(older.findChild<QComboBox*>("TuningCpuPlacement")->isEnabled());
    CHECK_FALSE(older.settings().contains("cpu_placement"));
    InferenceTuningDialog invalid({{"cpu_placement",true}},false);
    CHECK(invalid.settings().value("cpu_placement").toString() == "invalid");
}

TEST_CASE("weight repacking is explicit and legacy plugins receive no unknown field",
          "[gui][tuning][repacking]") {
    for (const bool plugin : {false, true}) {
        InferenceTuningDialog dialog({{"weight_repacking", QJsonValue(QJsonValue::Null)}}, plugin);
        auto* choice = dialog.findChild<QComboBox*>("TuningWeightRepacking");
        REQUIRE(choice);
        CHECK(choice->isEnabled());
        CHECK(dialog.settings().value("weight_repacking").isNull());
        CHECK(choice->toolTip().contains("change replies"));
        choice->setCurrentIndex(2);
        CHECK(dialog.settings().value("weight_repacking").isBool());
        CHECK_FALSE(dialog.settings().value("weight_repacking").toBool());
        dialog.findChild<QPushButton*>("TuningCpuPreset")->click();
        CHECK(choice->currentIndex() == 2);
        dialog.setEnabled(false);
        CHECK(dialog.settings().contains("weight_repacking"));
        CHECK_FALSE(dialog.settings().value("weight_repacking").toBool());
        dialog.setEnabled(true);
        choice->setCurrentIndex(1);
        CHECK(dialog.settings().value("weight_repacking").toBool());
        choice->setCurrentIndex(0);
        CHECK(dialog.settings().value("weight_repacking").isNull());
        const auto screenshot = qEnvironmentVariable("LOCALCODE_REPACK_SCREENSHOT_BASE");
        if (!screenshot.isEmpty()) {
            choice->setCurrentIndex(2);
            dialog.show();
            QElapsedTimer ready; ready.start();
            while (dialog.findChild<QLabel*>("TuningHardware")->text().startsWith("Detecting") && ready.elapsed() < 10000) {
                QApplication::processEvents(); QThread::msleep(10);
            }
            QApplication::processEvents(); QApplication::processEvents();
            const auto name = screenshot + (plugin ? "-plugin.png" : "-native.png");
            REQUIRE(dialog.grab().save(name));
            dialog.resize(420, 480);
            dialog.findChild<QScrollArea*>()->ensureWidgetVisible(choice);
            QApplication::processEvents(); QApplication::processEvents();
            CHECK(dialog.width() <= 420);
            REQUIRE(dialog.grab().save(name + ".small.png"));
        }
    }
    InferenceTuningDialog legacy({}, true);
    CHECK_FALSE(legacy.findChild<QComboBox*>("TuningWeightRepacking")->isEnabled());
    CHECK_FALSE(legacy.settings().contains("weight_repacking"));
    InferenceTuningDialog invalid({{"weight_repacking", "invalid"}}, false);
    auto* buttons = invalid.findChild<QDialogButtonBox*>();
    REQUIRE(buttons);
    buttons->button(QDialogButtonBox::Ok)->click();
    CHECK(invalid.result() != QDialog::Accepted);
    invalid.findChild<QComboBox*>("TuningWeightRepacking")->setCurrentIndex(0);
    buttons->button(QDialogButtonBox::Ok)->click();
    CHECK(invalid.result() == QDialog::Accepted);
    CHECK(invalid.settings().value("weight_repacking").isNull());
}

TEST_CASE("CPU pool choice follows explicit plugin capabilities and saves only the policy",
          "[gui][tuning][placement][pool]") {
    for (const bool plugin : {false,true}) {
        const QJsonObject input{{"cpu_placement",QJsonValue(QJsonValue::Null)},
            {"cpuPlacementModes",QJsonArray{"os","physical_cores","physical_pool"}}};
        InferenceTuningDialog dialog(input,plugin);
        auto* choice=dialog.findChild<QComboBox*>("TuningCpuPlacement");
        REQUIRE(choice);
        REQUIRE(choice->findData("physical_pool")>=0);
        auto before=dialog.settings();before.remove("cpu_placement");
        choice->setCurrentIndex(choice->findData("physical_pool"));
        auto after=dialog.settings();
        CHECK(after.value("cpu_placement").toString()=="physical_pool");
        after.remove("cpu_placement");
        CHECK(after==before);
        CHECK_FALSE(after.contains("cpuPlacementModes"));
        CHECK_FALSE(after.contains("cpu_allowance"));
    }
    InferenceTuningDialog legacy({{"cpu_placement",QJsonValue(QJsonValue::Null)}},true);
    CHECK(legacy.findChild<QComboBox*>("TuningCpuPlacement")->findData("physical_pool")==-1);
    InferenceTuningDialog systemOnly({{"cpu_placement",QJsonValue(QJsonValue::Null)},
        {"cpuPlacementModes",QJsonArray{"os"}}},true);
    CHECK(systemOnly.findChild<QComboBox*>("TuningCpuPlacement")->count()==1);
}

TEST_CASE("native cache RAM controls distinguish default disabled and bounded",
          "[gui][tuning][cache-ram]") {
    InferenceTuningDialog dialog({}, false);
    auto* cache = dialog.findChild<QSpinBox*>("TuningCacheRam");
    REQUIRE(cache);
    CHECK(cache->value() == -1);
    CHECK_FALSE(dialog.settings().contains("prompt_cache_mib"));
    for (int value : {0, 64, 8192}) {
        cache->setValue(value);
        REQUIRE(dialog.settings().contains("prompt_cache_mib"));
        CHECK(dialog.settings().value("prompt_cache_mib").toInt() == value);
    }
    cache->setValue(-1);
    CHECK_FALSE(dialog.settings().contains("prompt_cache_mib"));
    const auto screenshot = qEnvironmentVariable("LOCALCODE_CACHE_RAM_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        dialog.show();
        QElapsedTimer ready;
        ready.start();
        while (dialog.findChild<QLabel*>("TuningHardware")->text().startsWith("Detecting") &&
               ready.elapsed() < 10000) {
            QApplication::processEvents();
            QThread::msleep(10);
        }
        // Hardware completion posts another layout event. Capture a settled
        // frame, rather than the transient text update before that event.
        QApplication::processEvents();
        QApplication::processEvents();
        REQUIRE(dialog.grab().save(screenshot));
        dialog.resize(420, 480);
        QApplication::processEvents();
        QApplication::processEvents();
        REQUIRE(dialog.grab().save(screenshot + ".small.png"));
    }
}

namespace {
struct FixtureEnvironment {
    QByteArray original = qgetenv("LOCALCODE_BENCH_FIXTURE_MODE");
    bool existed = qEnvironmentVariableIsSet("LOCALCODE_BENCH_FIXTURE_MODE");

    explicit FixtureEnvironment(const char* mode) { qputenv("LOCALCODE_BENCH_FIXTURE_MODE", mode); }

    ~FixtureEnvironment() {
        if (existed)
            qputenv("LOCALCODE_BENCH_FIXTURE_MODE", original);
        else
            qunsetenv("LOCALCODE_BENCH_FIXTURE_MODE");
    }
};

QString fixture() {
#ifdef Q_OS_WIN
    return QDir(QApplication::applicationDirPath()).filePath("localcode_benchmark_fixture.exe");
#else
    return QDir(QApplication::applicationDirPath()).filePath("localcode_benchmark_fixture");
#endif
}

bool until(const std::function<bool()>& predicate, int limit = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < limit) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    return predicate();
}

QString makeModel(const QString& root) {
    const auto directory = QDir(root).filePath(QString::fromUtf8("modèles avec espaces"));
    QDir().mkpath(directory);
    const auto path = QDir(directory).filePath("model space.gguf");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write("fixture");
    return path;
}

QString makeRuntime(const QString& root) {
#ifdef Q_OS_WIN
    const QString serverName = "llama-server.exe", benchName = "llama-bench.exe";
#else
    const QString serverName = "llama-server", benchName = "llama-bench";
#endif
    const auto server = QDir(root).filePath(serverName);
    QFile file(server);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write("runtime identity fixture");
    if (!QFile::copy(fixture(), QDir(root).filePath(benchName))) return {};
    return server;
}

void prepare(InferenceBenchmarkDialog& dialog, const QString& program,
             const QString& counts = "1") {
    dialog.findChild<QLineEdit*>("BenchmarkExecutable")->setText(program);
    dialog.findChild<QLineEdit*>("BenchmarkThreads")->setText(counts);
}
}   // namespace

TEST_CASE("CPU pool benchmark applies the native process allowance without worker mask flags",
          "[gui][benchmark][pool]") {
    if (!localcode::physical_cpu_pool(1,1)) SKIP("Physical CPU pool unavailable on this host.");
    FixtureEnvironment environment("normal");
    QTemporaryDir directory;
    const auto model=makeModel(directory.path()), server=makeRuntime(directory.path());
    REQUIRE_FALSE(server.isEmpty());
    InferenceBenchmarkDialog dialog(model,server);
    dialog.setCpuPlacement("physical_pool");
    dialog.setThreadSelectionEnabled(true);
    prepare(dialog,QFileInfo(server).dir().filePath(QFileInfo(fixture()).fileName()=="localcode_benchmark_fixture.exe"
            ? "llama-bench.exe" : "llama-bench"),"1");
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&]{return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled();}));
    INFO(dialog.findChild<QLabel*>("BenchmarkStatus")->text().toStdString());
    REQUIRE(dialog.report().value("completed").toBool());
    const auto expected=dialog.report().value("cpuPools").toObject().value("1").toArray();
    REQUIRE_FALSE(expected.isEmpty());
    REQUIRE(dialog.report().value("trials").toArray().size()==2);
    for (const auto trial:dialog.report().value("trials").toArray()) {
        const auto row=trial.toObject();
        CHECK(row.value("cpuAllowance").toArray()==expected);
        CHECK(row.value("processId").toDouble()>0);
        CHECK_FALSE(row.value("arguments").toArray().contains("--cpu-mask"));
        for (const auto measurement:row.value("measurements").toArray())
            CHECK(measurement.toObject().value("process_cpu_allowance").toArray()==expected);
    }
    REQUIRE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
    dialog.findChild<QPushButton*>("BenchmarkUseThreads")->click();
    CHECK(dialog.selectedThreadSettings()==QJsonObject{{"threads",1},{"threads_batch",1}});
    dialog.setCpuPlacement("os");
    CHECK(dialog.selectedThreadSettings().isEmpty());
}

TEST_CASE("CPU pool benchmark stop timeout close and destruction reap the owned child",
          "[gui][benchmark][pool][lifecycle]") {
    if (!localcode::physical_cpu_pool(1,1)) SKIP("Physical CPU pool unavailable on this host.");
    FixtureEnvironment environment("hang");
    QTemporaryDir directory;
    const auto model=makeModel(directory.path());
    auto dialog=std::make_unique<InferenceBenchmarkDialog>(model,QString{});
    dialog->setCpuPlacement("physical_pool");
    prepare(*dialog,fixture(),"1");
    dialog->findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&]{return dialog->report().value("activeProcessId").toDouble()>0;}));
#if defined(Q_OS_WIN)
    const auto pid=static_cast<DWORD>(dialog->report().value("activeProcessId").toDouble());
    const auto handle=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    REQUIRE(handle!=nullptr);
    struct CloseNativeHandle { HANDLE value; ~CloseNativeHandle(){CloseHandle(value);} } observed{handle};
    REQUIRE(WaitForSingleObject(handle,0)==WAIT_TIMEOUT);
#endif
    QElapsedTimer elapsed;elapsed.start();
    SECTION("stop") { dialog->findChild<QPushButton*>("BenchmarkStop")->click(); }
    SECTION("timeout") { QMetaObject::invokeMethod(dialog->findChild<QTimer*>("BenchmarkDeadline"),"timeout",Qt::DirectConnection); }
    SECTION("close") { dialog->reject(); }
    SECTION("destroy") { dialog.reset(); }
    if (dialog) {
        REQUIRE(until([&]{return dialog->findChild<QPushButton*>("BenchmarkStart")->isEnabled();}));
        CHECK_FALSE(dialog->report().value("completed").toBool());
        CHECK(dialog->report().value("trials").toArray().isEmpty());
    }
#if defined(Q_OS_WIN)
    CHECK(WaitForSingleObject(handle,1000)==WAIT_OBJECT_0);
#endif
    CHECK(elapsed.elapsed()<4000);
}

TEST_CASE("closing between owned CPU pool trials cannot launch the queued next child",
          "[gui][benchmark][pool][lifecycle]") {
    if (!localcode::physical_cpu_pool(1,1)) SKIP("Physical CPU pool unavailable on this host.");
    FixtureEnvironment environment("delay");
    QTemporaryDir directory;
    InferenceBenchmarkDialog dialog(makeModel(directory.path()),QString{});
    dialog.setCpuPlacement("physical_pool");
    prepare(dialog,fixture(),"1");
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&]{return dialog.findChild<QFutureWatcherBase*>("BenchmarkOwnedProcess")!=nullptr;}));
    auto* owned=dialog.findChild<QFutureWatcherBase*>("BenchmarkOwnedProcess");
    QObject::connect(owned,&QFutureWatcherBase::finished,&dialog,[&] {
        if (dialog.report().value("trials").toArray().size()==1) dialog.reject();
    });
    REQUIRE(until([&]{return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled();}));
    QApplication::processEvents();QThread::msleep(20);QApplication::processEvents();
    CHECK_FALSE(dialog.report().value("completed").toBool());
    CHECK(dialog.report().value("trials").toArray().size()==1);
    CHECK(dialog.report().value("activeProcessId").toInt()==0);
}

TEST_CASE("CPU pool thread transfer rejects prompt and generation measured on different pools",
          "[gui][benchmark][pool]") {
    const auto cpus=localcode::available_cpu_processors();
    if (!cpus) SKIP("Physical CPU pool unavailable on this host.");
    const auto automatic=localcode::physical_cpu_pool(*cpus,0,0);
    REQUIRE(automatic);
    std::set<std::uint64_t> physical;
    for (const auto& cpu:*cpus) physical.insert(cpu.physical_id);
    const auto larger=static_cast<int>(automatic->logical_processors.size())+1;
    if (larger>256 || static_cast<std::size_t>(larger)>physical.size())
        SKIP("No distinct fast and expanded pools on this host's current eligible topology.");
    FixtureEnvironment environment("split");
    QTemporaryDir directory;
    const auto model=makeModel(directory.path()), server=makeRuntime(directory.path());
    REQUIRE_FALSE(server.isEmpty());
    InferenceBenchmarkDialog dialog(model,server);
    dialog.setCpuPlacement("physical_pool");
    dialog.setThreadSelectionEnabled(true);
    prepare(dialog,QFileInfo(server).dir().filePath(QFileInfo(fixture()).fileName()=="localcode_benchmark_fixture.exe"
            ? "llama-bench.exe" : "llama-bench"),QString("1,%1").arg(larger));
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&]{return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled();}));
    INFO(dialog.findChild<QLabel*>("BenchmarkStatus")->text().toStdString());
    REQUIRE(dialog.report().value("completed").toBool());
    auto* generation=dialog.findChild<QComboBox*>("BenchmarkGenerationChoice");
    auto* prompt=dialog.findChild<QComboBox*>("BenchmarkPromptChoice");
    CHECK(generation->currentData().toInt()==1);
    CHECK(prompt->currentData().toInt()==larger);
    CHECK_FALSE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
    prompt->setCurrentIndex(prompt->findData(1));
    CHECK(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
}

TEST_CASE("CPU comparison confirms physical masks and invalidates transfers when placement changes",
          "[gui][benchmark][placement]") {
    if (!localcode::available_cpu_processors()) SKIP("Physical topology API unavailable on this test host.");
    const char* mode = "placement";
    bool completes = true;
    SECTION("matching tool") {}
    SECTION("unsupported tool") { mode="placement-unsupported"; completes=false; }
    SECTION("wrong mask") { mode="placement-mask"; completes=false; }
    SECTION("missing confirmation") { mode="placement-missing"; completes=false; }
    SECTION("non-boolean confirmation") { mode="placement-numeric"; completes=false; }
    SECTION("strict placement disabled") { mode="placement-false"; completes=false; }
    FixtureEnvironment environment(mode);
    QTemporaryDir directory;
    const auto model=makeModel(directory.path()), server=makeRuntime(directory.path());
    REQUIRE_FALSE(server.isEmpty());
    InferenceBenchmarkDialog dialog(model,server);
    dialog.setCpuPlacement("physical_cores");
    dialog.setThreadSelectionEnabled(true);
    prepare(dialog,QFileInfo(server).dir().filePath(
#ifdef Q_OS_WIN
        "llama-bench.exe"
#else
        "llama-bench"
#endif
    ), "2");
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&]{return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled();}));
    CHECK(dialog.report().value("completed").toBool() == completes);
    CHECK(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled() == completes);
    CHECK(dialog.report().value("cpuPlacement").toString() == "physical_cores");
    if (completes) {
        const auto mask=dialog.report().value("cpuMasks").toObject().value("2").toString();
        REQUIRE_FALSE(mask.isEmpty());
        for (const auto trial : dialog.report().value("trials").toArray())
            for (const auto row : trial.toObject().value("measurements").toArray()) {
                CHECK(row.toObject().value("cpu_mask").toString() == mask);
                CHECK(row.toObject().value("cpu_strict").toBool());
            }
        dialog.setCpuPlacement("os");
        CHECK_FALSE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
    } else CHECK(dialog.report().value("trials").toArray().isEmpty());
}

TEST_CASE("CPU comparison executes both orders with fragmented output and accented directories",
          "[gui][benchmark]") {
    FixtureEnvironment environment("normal");
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto model = makeModel(directory.path());
    REQUIRE_FALSE(model.isEmpty());
    InferenceBenchmarkDialog dialog(model, {});
    const bool two = QThread::idealThreadCount() >= 2;
    prepare(dialog, fixture(), two ? "1,2" : "1");
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    const auto report = dialog.report();
    REQUIRE(report.value("completed").toBool());
    const auto trials = report.value("trials").toArray();
    REQUIRE(trials.size() == (two ? 4 : 2));
    CHECK(trials.first().toObject().value("threads").toInt() == 1);
    CHECK(trials.last().toObject().value("threads").toInt() == 1);
    if (two) {
        CHECK(trials[1].toObject().value("threads").toInt() == 2);
        CHECK(trials[2].toObject().value("threads").toInt() == 2);
    }
    CHECK(report.value("model").toString() == model);
    CHECK(dialog.findChild<QTableWidget*>("BenchmarkResults")->rowCount() == trials.size());
    CHECK(dialog.findChild<QProcess*>("BenchmarkProcess")->state() == QProcess::NotRunning);
    CHECK(dialog.findChild<QPushButton*>("BenchmarkSave")->isEnabled());
    CHECK(report.value("threadSummary").toArray().size() == (two ? 2 : 1));
    const auto image = qEnvironmentVariable("LOCALCODE_BENCHMARK_SCREENSHOT");
    if (!image.isEmpty()) {
        dialog.show();
        QApplication::processEvents();
        REQUIRE(dialog.grab().save(image));
        dialog.resize(410, 480);
        QApplication::processEvents();
        REQUIRE(dialog.grab().save(image + ".small.png"));
    }
}

TEST_CASE("CPU comparison rejects invalid results without accepting a speed", "[gui][benchmark]") {
    const char* mode = "bad-json";
    SECTION("Malformed JSON") {}
    SECTION("Mismatched thread count") {
        mode = "mismatch";
    }
    SECTION("Runtime error") {
        mode = "error";
    }
    SECTION("Bounded output") {
        mode = "huge";
    }
    FixtureEnvironment environment(mode);
    QTemporaryDir directory;
    InferenceBenchmarkDialog dialog(makeModel(directory.path()), {});
    prepare(dialog, fixture());
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    CHECK_FALSE(dialog.report().value("completed").toBool());
    CHECK(dialog.report().value("trials").toArray().isEmpty());
    CHECK(dialog.findChild<QProcess*>("BenchmarkProcess")->state() == QProcess::NotRunning);
    CHECK(dialog.findChild<QPushButton*>("BenchmarkSave")->isEnabled());
    CHECK_FALSE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
    CHECK(dialog.selectedThreadSettings().isEmpty());
}

TEST_CASE("CPU comparison stop timeout and close terminate only its process", "[gui][benchmark]") {
    FixtureEnvironment environment("hang");
    QTemporaryDir directory;
    InferenceBenchmarkDialog dialog(makeModel(directory.path()), {});
    prepare(dialog, fixture());
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] {
        return dialog.findChild<QLabel*>("BenchmarkStatus")->text().startsWith("Trial ") &&
               dialog.findChild<QProcess*>("BenchmarkProcess")->state() == QProcess::Running;
    }));
    SECTION("Stop") {
        dialog.findChild<QPushButton*>("BenchmarkStop")->click();
    }
    SECTION("Timeout") {
        QMetaObject::invokeMethod(dialog.findChild<QTimer*>("BenchmarkDeadline"), "timeout",
                                  Qt::DirectConnection);
    }
    SECTION("Close") {
        dialog.reject();
    }
    REQUIRE(until([&] {
        return dialog.findChild<QProcess*>("BenchmarkProcess")->state() == QProcess::NotRunning;
    }));
    CHECK_FALSE(dialog.report().value("completed").toBool());
    CHECK(dialog.report().value("trials").toArray().isEmpty());
    CHECK(dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled());
}

TEST_CASE("closing between CPU trials cannot launch the queued next process", "[gui][benchmark]") {
    FixtureEnvironment environment("normal");
    QTemporaryDir directory;
    InferenceBenchmarkDialog dialog(makeModel(directory.path()), {});
    prepare(dialog, fixture());
    auto* process = dialog.findChild<QProcess*>("BenchmarkProcess");
    QObject::connect(process, &QProcess::finished, &dialog, [&] {
        if (!dialog.report().value("trials").toArray().isEmpty()) dialog.reject();
    });
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    for (int i = 0; i < 15; ++i) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    CHECK(dialog.report().value("trials").toArray().size() == 1);
    CHECK_FALSE(dialog.report().value("completed").toBool());
    CHECK(process->state() == QProcess::NotRunning);
}

TEST_CASE("restarting a stopped comparison discards the old queued trial", "[gui][benchmark]") {
    FixtureEnvironment environment("normal");
    QTemporaryDir directory;
    InferenceBenchmarkDialog dialog(makeModel(directory.path()), {});
    prepare(dialog, fixture());
    auto* process = dialog.findChild<QProcess*>("BenchmarkProcess");
    bool restarted = false;
    QObject::connect(process, &QProcess::finished, &dialog, [&] {
        if (restarted || dialog.report().value("trials").toArray().isEmpty()) return;
        restarted = true;
        dialog.findChild<QPushButton*>("BenchmarkStop")->click();
        dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    });
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    REQUIRE(restarted);
    CHECK(dialog.report().value("completed").toBool());
    CHECK(dialog.report().value("trials").toArray().size() == 2);
    CHECK(process->state() == QProcess::NotRunning);
}

TEST_CASE("UTF-8 benchmark filenames use the matching launcher and retain the original identity",
          "[gui][benchmark][unicode]") {
    FixtureEnvironment environment("normal");
    QTemporaryDir directory;
    const auto model = directory.filePath(QString::fromUtf8("modèle 樹 🦍.gguf"));
    QFile file(model);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("model fixture");
    file.close();
    InferenceBenchmarkDialog dialog(model, {});
#ifdef Q_OS_WIN
    const QString program = "localcode_benchmark_utf8_fixture.exe";
#else
    const QString program = "localcode_benchmark_utf8_fixture";
#endif
    prepare(dialog, QDir(QCoreApplication::applicationDirPath()).filePath(program));
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    INFO(dialog.findChild<QLabel*>("BenchmarkStatus")->text().toStdString());
    REQUIRE(dialog.report().value("completed").toBool());
    CHECK(dialog.report().value("model").toString() == model);
    REQUIRE(dialog.report().value("trials").toArray().size() == 2);
    for (const auto trial : dialog.report().value("trials").toArray()) {
        const auto row = trial.toObject();
        REQUIRE(row.value("measurements").toArray().size() == 2);
        for (const auto value : row.value("measurements").toArray())
            CHECK(value.toObject().value("model_filename").toString() == QFileInfo(model).fileName());
    }
}

#ifdef Q_OS_WIN
TEST_CASE("legacy Windows benchmarks reject Unicode names after capability detection before loading",
          "[gui][benchmark][unicode]") {
    FixtureEnvironment environment("normal");
    QTemporaryDir directory;
    const auto model = directory.filePath(QString::fromUtf8("modèle.gguf"));
    QFile file(model);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("model fixture");
    file.close();
    InferenceBenchmarkDialog dialog(model, {});
    prepare(dialog, fixture());
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    CHECK_FALSE(dialog.report().value("completed").toBool());
    CHECK(dialog.report().value("trials").toArray().isEmpty());
    CHECK(dialog.report().value("runtimeHelp").isString());
    CHECK(dialog.findChild<QLabel*>("BenchmarkStatus")->text().contains("UTF-8 filenames"));
    CHECK(dialog.findChild<QProcess*>("BenchmarkProcess")->state() == QProcess::NotRunning);
}
#endif

TEST_CASE("invalid thread input does not launch a CPU benchmark", "[gui][benchmark]") {
    QTemporaryDir directory;
    InferenceBenchmarkDialog dialog(makeModel(directory.path()), {});
    for (const QString input : {"", "0", "1,1", "1,no", "999999", "1,,2"}) {
        prepare(dialog, fixture(), input);
        dialog.findChild<QPushButton*>("BenchmarkStart")->click();
        CHECK(dialog.findChild<QProcess*>("BenchmarkProcess")->state() == QProcess::NotRunning);
        CHECK(dialog.report().isEmpty());
        CHECK(dialog.findChild<QLabel*>("BenchmarkStatus")
                  ->text()
                  .contains("distinct thread counts"));
    }
}

TEST_CASE("CPU measurement access preserves every tuning choice", "[gui][benchmark]") {
    for (const bool plugin : {false, true}) {
        InferenceTuningDialog tuning({{"contextSize", 2097152},
                                      {"threads", 7},
                                      {"threads_batch", 6},
                                      {"gpuLayers", 3},
                                      {"cache_type_k", "q8_0"}},
                                     plugin);
        tuning.setBenchmarkFiles("owner-model.gguf", "owner-runtime/llama-server");
        const auto before = tuning.settings();
        bool opened = false;
        QTimer::singleShot(0, &tuning, [&] {
            auto* dialog =
                qobject_cast<InferenceBenchmarkDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            opened = dialog->findChild<QLineEdit*>("BenchmarkModel")->text() == "owner-model.gguf";
            dialog->reject();
        });
        tuning.findChild<QPushButton*>("TuningBenchmark")->click();
        REQUIRE(opened);
        CHECK(tuning.settings() == before);
    }
}

TEST_CASE("measured CPU thread selection is explicit bounded and bound to the input files",
          "[gui][benchmark][thread-selection]") {
    if (QThread::idealThreadCount() < 2) SKIP("Two logical CPUs are needed for split choices.");
    FixtureEnvironment environment("split");
    QTemporaryDir directory;
    const auto model = makeModel(directory.path());
    const auto server = makeRuntime(directory.path());
    REQUIRE_FALSE(server.isEmpty());
    InferenceBenchmarkDialog dialog(model, server);
    dialog.setThreadSelectionEnabled(true);
    auto* button = dialog.findChild<QPushButton*>("BenchmarkUseThreads");
    auto* generation = dialog.findChild<QComboBox*>("BenchmarkGenerationChoice");
    auto* prompt = dialog.findChild<QComboBox*>("BenchmarkPromptChoice");
    REQUIRE_FALSE(button->isEnabled());
    prepare(dialog, dialog.findChild<QLineEdit*>("BenchmarkExecutable")->text(), "1,2");
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    REQUIRE(dialog.report().value("completed").toBool());
    REQUIRE(button->isEnabled());
    CHECK(generation->currentData().toInt() == 1);
    CHECK(prompt->currentData().toInt() == 2);
    CHECK(dialog.selectedThreadSettings().isEmpty());
    const auto summary = dialog.report().value("threadSummary").toArray();
    REQUIRE(summary.size() == 2);
    CHECK(summary[1].toObject().value("prompt").toObject().value("mean").toDouble() == 200.0);
    CHECK(summary[1].toObject().value("generation").toObject().value("min").toDouble() == 20.0);
    const auto image = qEnvironmentVariable("LOCALCODE_BENCHMARK_SCREENSHOT");
    if (!image.isEmpty()) {
        dialog.show();
        QApplication::processEvents();
        auto* scroll = dialog.findChild<QScrollArea*>("BenchmarkOptions");
        scroll->ensureWidgetVisible(generation);
        QApplication::processEvents();
        REQUIRE(dialog.grab().save(image + ".selection.png"));
        dialog.resize(410, 480);
        QApplication::processEvents();
        scroll->ensureWidgetVisible(generation);
        QApplication::processEvents();
        CHECK(dialog.width() == 410);
        CHECK(dialog.height() == 480);
        REQUIRE(dialog.grab().save(image + ".selection.small.png"));
        CHECK(dialog.rect().contains(button->mapTo(&dialog, button->rect().center())));
        REQUIRE(dialog.grab().save(image + ".selection.action.png"));
    }
    SECTION("Explicit selection can differ from highest means") {
        generation->setCurrentIndex(generation->findData(2));
        prompt->setCurrentIndex(prompt->findData(1));
        button->click();
        CHECK(dialog.result() == QDialog::Accepted);
        CHECK(dialog.selectedThreadSettings() == QJsonObject{{"threads", 2}, {"threads_batch", 1}});
    }
    SECTION("Editing workload invalidates transfer until restored") {
        auto* field = dialog.findChild<QSpinBox*>("BenchmarkGenerated");
        const int original = field->value();
        field->setValue(original + 1);
        CHECK_FALSE(button->isEnabled());
        field->setValue(original);
        CHECK(button->isEnabled());
    }
    SECTION("Choosing another model disables transfer") {
        dialog.findChild<QLineEdit*>("BenchmarkModel")->setText(server);
        CHECK_FALSE(button->isEnabled());
    }
    SECTION("Choosing another benchmark disables transfer") {
        dialog.findChild<QLineEdit*>("BenchmarkExecutable")->setText(fixture());
        CHECK_FALSE(button->isEnabled());
    }
    SECTION("Changed model benchmark or server is rechecked at click time") {
        const QString* path = &model;
        const auto benchmark = dialog.findChild<QLineEdit*>("BenchmarkExecutable")->text();
        SECTION("Model replaced") {}
        SECTION("Server replaced") {
            path = &server;
        }
        SECTION("Benchmark replaced") {
            path = &benchmark;
        }
        QFile file(*path);
        REQUIRE(file.open(QIODevice::Append));
        file.write("changed");
        file.close();
        button->click();
        CHECK_FALSE(button->isEnabled());
        CHECK(dialog.result() != QDialog::Accepted);
        CHECK(dialog.selectedThreadSettings().isEmpty());
    }
    SECTION("GPU profile cannot receive CPU measurements") {
        dialog.setThreadSelectionEnabled(false);
        CHECK_FALSE(button->isEnabled());
        CHECK(dialog.selectedThreadSettings().isEmpty());
    }
    SECTION("Profile limits cannot silently clamp a measured choice") {
        dialog.setThreadSelectionEnabled(true, 1);
        CHECK(generation->count() == 1);
        CHECK(prompt->count() == 1);
        button->click();
        CHECK(dialog.selectedThreadSettings() == QJsonObject{{"threads", 1}, {"threads_batch", 1}});
    }
    SECTION("Cancellation transfers nothing") {
        dialog.reject();
        CHECK(dialog.selectedThreadSettings().isEmpty());
    }
}

TEST_CASE("measured threads update only pending generation and prompt fields in both interfaces",
          "[gui][benchmark][thread-selection]") {
    if (QThread::idealThreadCount() < 2) SKIP("Two logical CPUs are needed for split choices.");
    FixtureEnvironment environment("split");
    QTemporaryDir directory;
    const auto model = makeModel(directory.path());
    const auto server = makeRuntime(directory.path());
    REQUIRE_FALSE(server.isEmpty());
    bool allowTransfer = true;
    SECTION("Explicit runtime") {}
    SECTION("Unresolved or external runtime") {
        allowTransfer = false;
    }
    bool explicitRepacking = false;
    bool repackingSupported = false;
    SECTION("An old benchmark cannot measure an explicit repacking mode") {
        explicitRepacking = true;
        allowTransfer = false;
    }
    SECTION("A capable benchmark measures the requested layout before thread transfer") {
        explicitRepacking = true;
        repackingSupported = true;
        qputenv("LOCALCODE_BENCH_FIXTURE_MODE", "repack-split");
    }
    for (const bool plugin : {false, true}) {
        InferenceTuningDialog tuning({{"contextSize", 2097152},
                                      {"gpuLayers", 0},
                                      {"threads", 7},
                                      {"threads_batch", 6},
                                      {"cache_type_k", "q8_0"},
                                      {"batch_size", 512},
                                      {"ubatch_size", 64},
                                      {"prompt_cache_mib", 64},
                                      {"weight_repacking", QJsonValue(QJsonValue::Null)}},
                                     plugin);
        tuning.setBenchmarkFiles(model, server, allowTransfer);
        if (explicitRepacking) {
            auto* choice = tuning.findChild<QComboBox*>("TuningWeightRepacking");
            choice->setCurrentIndex(2);
            tuning.setBenchmarkFiles(model, server, true);
        }
        auto expected = tuning.settings();
        if (allowTransfer) {
            expected.insert("threads", 1);
            expected.insert("threads_batch", 2);
        }
        bool copied = false;
        QTimer::singleShot(0, &tuning, [&] {
            auto* dialog =
                qobject_cast<InferenceBenchmarkDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->findChild<QLineEdit*>("BenchmarkThreads")->setText("1,2");
            dialog->findChild<QPushButton*>("BenchmarkStart")->click();
            copied = until([&] {
                         return dialog->findChild<QPushButton*>("BenchmarkStart")->isEnabled();
                     }) &&
                     dialog->report().value("completed").toBool() ==
                         (!explicitRepacking || repackingSupported) &&
                     dialog->findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled() ==
                         allowTransfer;
            if (copied && allowTransfer)
                dialog->findChild<QPushButton*>("BenchmarkUseThreads")->click();
            else
                dialog->reject();
        });
        tuning.findChild<QPushButton*>("TuningBenchmark")->click();
        REQUIRE(copied);
        CHECK(tuning.settings() == expected);
        CHECK(tuning.result() != QDialog::Accepted);
    }
}

TEST_CASE("CPU comparison honors explicit repacking and rejects unsupported or unconfirmed modes",
          "[gui][benchmark][repacking]") {
    const char* mode = "repack";
    bool completes = true;
    SECTION("Supported tool") {}
    SECTION("Old tool") { mode = "normal"; completes = false; }
    SECTION("Unrecognized bare-switch grammar") { mode = "repack-bare"; completes = false; }
    SECTION("Tool reports the opposite mode") { mode = "repack-mismatch"; completes = false; }
    SECTION("Tool omits confirmation") { mode = "repack-missing"; completes = false; }
    SECTION("Tool reports a number instead of a boolean") { mode = "repack-numeric"; completes = false; }
    FixtureEnvironment environment(mode);
    for (const bool enabled : {false, true}) {
        QTemporaryDir directory;
        const auto model = makeModel(directory.path()), server = makeRuntime(directory.path());
        REQUIRE_FALSE(server.isEmpty());
        InferenceBenchmarkDialog dialog(model, server);
        dialog.setWeightRepacking(enabled);
        dialog.setThreadSelectionEnabled(true);
        prepare(dialog, QFileInfo(server).dir().filePath(
#ifdef Q_OS_WIN
                              "llama-bench.exe"
#else
                              "llama-bench"
#endif
                              ));
        dialog.findChild<QPushButton*>("BenchmarkStart")->click();
        REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
        CHECK(dialog.report().value("completed").toBool() == completes);
        CHECK(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled() == completes);
        CHECK(dialog.report().value("weightRepacking") == QJsonValue(enabled));
        if (completes) {
            for (const auto value : dialog.report().value("trials").toArray()) {
                const auto trial = value.toObject();
                QStringList arguments;
                for (const auto argument : trial.value("arguments").toArray())
                    arguments.append(argument.toString());
                const auto index = arguments.indexOf("--repack");
                REQUIRE(index >= 0);
                CHECK(arguments[index + 1] == (enabled ? "1" : "0"));
                for (const auto row : trial.value("measurements").toArray())
                    CHECK(row.toObject().value("repack") == QJsonValue(enabled));
            }
            dialog.setWeightRepacking(!enabled);
            CHECK_FALSE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
        } else {
            CHECK(dialog.report().value("trials").toArray().isEmpty());
        }
    }
}

TEST_CASE("CPU thread transfer tracks adjacent runtime library changes",
          "[gui][benchmark][thread-selection][libraries]") {
    FixtureEnvironment environment("normal");
    QTemporaryDir directory;
    const auto model = makeModel(directory.path()), server = makeRuntime(directory.path());
    REQUIRE_FALSE(server.isEmpty());
    const auto library = QDir(directory.path()).filePath("runtime-identity-fixture.dll");
    { QFile file(library); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write("original") == 8); }
    InferenceBenchmarkDialog dialog(model, server);
    dialog.setThreadSelectionEnabled(true);
    prepare(dialog, QFileInfo(server).dir().filePath(
#ifdef Q_OS_WIN
        "llama-bench.exe"
#else
        "llama-bench"
#endif
    ));
    bool changesDuringRun = false;
    SECTION("Changed after measurement") {}
    SECTION("Changed between forward and reverse trials") { changesDuringRun = true; }
    bool changed = false;
    auto mutate = [&] {
        QFile file(library);
        REQUIRE(file.open(QIODevice::Append));
        REQUIRE(file.write("-changed") == 8);
        changed = true;
    };
    QObject::connect(dialog.findChild<QProcess*>("BenchmarkProcess"), &QProcess::finished,
                     &dialog, [&](int, QProcess::ExitStatus) {
        if (changesDuringRun && !changed && dialog.report().value("trials").toArray().size() == 1)
            mutate();
    });
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); }));
    if (!changesDuringRun) {
        REQUIRE(dialog.report().value("completed").toBool());
        REQUIRE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
        mutate();
    } else {
        REQUIRE_FALSE(dialog.report().value("completed").toBool());
        CHECK(dialog.report().value("trials").toArray().size() == 1);
    }
    REQUIRE(changed);
    CHECK(dialog.selectedThreadSettings().isEmpty());
    dialog.setThreadSelectionEnabled(true);
    CHECK_FALSE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
}

TEST_CASE("native CPU comparison completes with real benchmark results and responsive UI",
          "[.][gui][benchmark][native]") {
    const auto model = qEnvironmentVariable("LOCALCODE_BENCHMARK_NATIVE_MODEL");
    const auto binary = qEnvironmentVariable("LOCALCODE_BENCHMARK_NATIVE_BINARY");
    if (model.isEmpty() || binary.isEmpty())
        SKIP("Set explicit native benchmark model and binary paths.");
#ifdef Q_OS_WIN
    const auto server = QFileInfo(binary).dir().filePath("llama-server.exe");
#else
    const auto server = QFileInfo(binary).dir().filePath("llama-server");
#endif
    REQUIRE(QFileInfo(server).isFile());
    InferenceBenchmarkDialog dialog(model, server);
    dialog.setThreadSelectionEnabled(true);
    const auto placement = qEnvironmentVariable("LOCALCODE_BENCHMARK_NATIVE_PLACEMENT", "os");
    dialog.setCpuPlacement(placement);
    QStringList nativeCounts;
    if (placement == "physical_cores") {
        const auto automatic=localcode::physical_cpu_placement(0,0);
        if (!automatic) SKIP("Strict physical placement unavailable on this host.");
        nativeCounts={"2",QString::number(automatic->threads)};
        nativeCounts.removeDuplicates();
    } else {
        nativeCounts={"1"};
        if (QThread::idealThreadCount()>=2 &&
            (placement!="physical_pool" || localcode::physical_cpu_pool(2,2))) nativeCounts.append("2");
    }
    prepare(dialog,binary,nativeCounts.join(','));
    const auto repack = qEnvironmentVariable("LOCALCODE_BENCHMARK_NATIVE_REPACK");
    if (!repack.isEmpty()) {
        REQUIRE((repack == "0" || repack == "1"));
        dialog.setWeightRepacking(repack == "1");
    }
    dialog.findChild<QSpinBox*>("BenchmarkPrompt")->setValue(32);
    dialog.findChild<QSpinBox*>("BenchmarkGenerated")->setValue(16);
    dialog.findChild<QSpinBox*>("BenchmarkRepetitions")->setValue(2);
    int pulses = 0;
    QTimer heartbeat;
    heartbeat.setInterval(20);
    QObject::connect(&heartbeat, &QTimer::timeout, &dialog, [&] { ++pulses; });
    heartbeat.start();
    dialog.findChild<QPushButton*>("BenchmarkStart")->click();
    REQUIRE(until([&] { return dialog.findChild<QPushButton*>("BenchmarkStart")->isEnabled(); },
                  300000));
    INFO(dialog.findChild<QLabel*>("BenchmarkStatus")->text().toStdString());
    REQUIRE(dialog.report().value("completed").toBool());
    CHECK(dialog.report().value("cpuPlacement").toString() == placement);
    if (!repack.isEmpty()) {
        REQUIRE(dialog.report().value("weightRepacking").toBool() == (repack == "1"));
        for (const auto& trial : dialog.report().value("trials").toArray()) {
            const auto rows = trial.toObject().value("measurements").toArray();
            REQUIRE(rows.size() == 2);
            for (const auto& row : rows)
                CHECK(row.toObject().value("repack").toBool() == (repack == "1"));
        }
    }
    CHECK(pulses > 10);
    CHECK(dialog.findChild<QProcess*>("BenchmarkProcess")->state() == QProcess::NotRunning);
    if (placement=="physical_pool" && !dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled()) {
        auto* generation=dialog.findChild<QComboBox*>("BenchmarkGenerationChoice");
        auto* prompt=dialog.findChild<QComboBox*>("BenchmarkPromptChoice");
        generation->setCurrentIndex(0);
        prompt->setCurrentIndex(prompt->findData(generation->currentData()));
    }
    REQUIRE(dialog.findChild<QPushButton*>("BenchmarkUseThreads")->isEnabled());
    CHECK(dialog.report().value("threadSummary").toArray().size() == nativeCounts.size());
    const auto output = qEnvironmentVariable("LOCALCODE_BENCHMARK_NATIVE_REPORT");
    if (!output.isEmpty()) {
        QFile file(output);
        REQUIRE(file.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(dialog.report()).toJson();
        REQUIRE(file.write(bytes) == bytes.size());
        REQUIRE(dialog.grab().save(output + ".png"));
    }
    dialog.findChild<QPushButton*>("BenchmarkUseThreads")->click();
    CHECK_FALSE(dialog.selectedThreadSettings().isEmpty());
}
