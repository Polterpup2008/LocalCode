#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
#endif

#include "Theme.h"
#include "agent/ContextManager.h"
#include "widgets/ChatView.h"
#include "widgets/OutputPanel.h"

namespace {
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

QJsonObject summary(const std::vector<double>& samples) {
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    QJsonArray raw;
    for (auto value : samples)
        raw.append(value);
    auto percentile = [&](double fraction) {
        return sorted.at(static_cast<std::size_t>(std::ceil(fraction * sorted.size())) - 1);
    };
    return {{"unit", "ms"},
            {"n", static_cast<int>(samples.size())},
            {"p50", percentile(.5)},
            {"p95", percentile(.95)},
            {"samples", raw}};
}

QJsonObject memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters)))
        throw std::runtime_error("memory probe failed");
    return {{"working_set_bytes", static_cast<double>(counters.WorkingSetSize)},
            {"private_bytes", static_cast<double>(counters.PrivateUsage)}};
#else
    return {{"available", false}};
#endif
}

const QString kBody = QStringLiteral(
    "A representative coding discussion about `src/core/agent/Agent.cpp`.\n\n"
    "- Preserve cancellation and source provenance.\n"
    "- Select the related test and report its actual result.\n\n"
    "```cpp\nint bounded_sum(int a, int b) {\n    return a + b;\n}\n```\n\n"
    "The implementation and verification are distinct. This is synthetic data.");

QJsonObject chat(int count, int repeats) {
    using namespace localcode::gui;
    ChatView view;
    view.setWindowTitle("LocalCode performance fixture");
    view.resize(1000, 700);
    view.show();
    QApplication::processEvents();
    auto* browser = view.findChild<QTextBrowser*>();
    if (!browser) throw std::runtime_error("production chat browser missing");
    qint64 removed = 0;
    int changes = 0;
    qint64 stream_removed = 0;
    int stream_changes = 0;
    bool measuring_stream = false;
    int thinking_timeouts = 0;
    int flush_timeouts = 0;
    for (auto* timer : view.findChildren<QTimer*>()) {
        QObject::connect(timer, &QTimer::timeout, &view, [&, timer] {
            if (!measuring_stream) return;
            if (timer->interval() >= 500)
                ++thinking_timeouts;
            else
                ++flush_timeouts;
        });
    }
    QObject::connect(browser->document(), &QTextDocument::contentsChange, &view,
                     [&](int, int gone, int) {
                         removed += gone;
                         ++changes;
                     });
    std::vector<double> load, append, flush, resize, scroll, clear, bulk_restore, paged_restore;
    std::vector<double> flush_work, stream_events;
    QJsonObject loaded_memory;
    for (int run = 0; run < repeats; ++run) {
        view.clear();
        auto start = Clock::now();
        for (int index = 0; index < count; ++index) {
            view.addMessage(index % 2 == 0 ? ChatRole::User : ChatRole::Assistant,
                            QStringLiteral("Message %1\n\n").arg(index) + kBody);
        }
        QApplication::processEvents();
        load.push_back(elapsed(start));
        loaded_memory = memory();
        start = Clock::now();
        view.addMessage(ChatRole::User, "One additional bounded change.");
        QApplication::processEvents();
        append.push_back(elapsed(start));
        view.beginAssistantMessage();
        removed = 0;
        changes = 0;
        measuring_stream = true;
        for (int chunk = 0; chunk < 12; ++chunk) {
            view.appendDelta("A small streamed answer fragment. ");
            view.appendReasoning("A short actual reasoning fragment. ");
            start = Clock::now();
            if (!QMetaObject::invokeMethod(&view, "flushPending", Qt::DirectConnection)) {
                throw std::runtime_error("production flush slot unavailable");
            }
            flush_work.push_back(elapsed(start));
            const auto events_start = Clock::now();
            QApplication::processEvents();
            stream_events.push_back(elapsed(events_start));
            flush.push_back(elapsed(start));
        }
        measuring_stream = false;
        stream_removed = removed;
        stream_changes = changes;
        view.endAssistantMessage();
        start = Clock::now();
        view.resize(760, 600);
        QApplication::processEvents();
        resize.push_back(elapsed(start));
        view.resize(1000, 700);
        QApplication::processEvents();
        start = Clock::now();
        browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum() / 2);
        QApplication::processEvents();
        scroll.push_back(elapsed(start));
        start = Clock::now();
        view.clear();
        QApplication::processEvents();
        clear.push_back(elapsed(start));
        QList<ChatView::TranscriptMessage> restored;
        for (int index = 0; index < count; ++index) {
            restored.append({index % 2 == 0 ? ChatRole::User : ChatRole::Assistant,
                             QStringLiteral("Message %1\n\n").arg(index) + kBody,
                             {}});
        }
        start = Clock::now();
        view.addMessages(restored);
        QApplication::processEvents();
        bulk_restore.push_back(elapsed(start));
        if (!browser->toPlainText().contains(QStringLiteral("Message %1").arg(count - 1)))
            throw std::runtime_error("bulk restore dropped the final message");
        view.clear();
        start = Clock::now();
        view.restoreMessages(restored);
        QApplication::processEvents();
        paged_restore.push_back(elapsed(start));
        if (!view.toMarkdown().contains("Message 0") ||
            !browser->toPlainText().contains(QStringLiteral("Message %1").arg(count - 1)))
            throw std::runtime_error("production paged restore lost transcript content");
        view.clear();
    }
    // Reproduce clearing while reasoning is active, independently of wall time.
    view.beginAssistantMessage();
    view.appendReasoning("A bounded synthetic reasoning passage.");
    view.clear();
    int active_timers_after_clear = 0;
    for (auto* timer : view.findChildren<QTimer*>()) {
        if (timer->isActive()) ++active_timers_after_clear;
    }
    return {{"messages", count},
            {"fixture_body_utf16_chars", kBody.size()},
            {"load", summary(load)},
            {"append", summary(append)},
            {"stream_flush", summary(flush)},
            {"stream_flush_work", summary(flush_work)},
            {"stream_process_events", summary(stream_events)},
            {"stream_thinking_timer_timeouts", thinking_timeouts},
            {"stream_flush_timer_timeouts", flush_timeouts},
            {"resize", summary(resize)},
            {"scroll", summary(scroll)},
            {"clear", summary(clear)},
            {"bulk_restore_same_messages", summary(bulk_restore)},
            {"production_paged_restore", summary(paged_restore)},
            {"last_stream_removed_characters", static_cast<double>(stream_removed)},
            {"last_stream_document_changes", stream_changes},
            {"loaded_memory", loaded_memory},
            {"after_clear_memory", memory()},
            {"active_timers_after_reasoning_clear", active_timers_after_clear}};
}

QJsonObject context(int repeats) {
    localcode::ContextManager manager;
    manager.set_context_size(32768);
    manager.set_reserved_output(2048);
    std::vector<localcode::ChatMessage> history;
    const auto body = kBody.toStdString();
    for (int index = 0; index < 500; ++index) {
        history.push_back(index % 2 == 0 ? localcode::ChatMessage::user(body)
                                         : localcode::ChatMessage::assistant(body));
    }
    history.push_back(localcode::ChatMessage::user("Locate the lifecycle implementation."));
    std::vector<double> samples;
    std::size_t bytes = 0;
    int estimated_tokens = 0;
    for (int run = 0; run < repeats * 10; ++run) {
        const auto start = Clock::now();
        auto result =
            manager.build("Preserve source evidence and task boundaries.", "", "", history, {});
        samples.push_back(elapsed(start));
        bytes = 0;
        for (const auto& message : result.messages)
            bytes += message.content.size();
        estimated_tokens = result.budget.used;
    }
    auto result = summary(samples);
    result.insert("input_messages", static_cast<int>(history.size()));
    result.insert("output_content_bytes", static_cast<double>(bytes));
    result.insert("estimated_tokens", estimated_tokens);
    result.insert("runtime_tokenizer", false);
    return result;
}

QJsonObject outputPanel() {
    localcode::gui::OutputPanel output;
    output.setWindowTitle("LocalCode output performance fixture");
    output.resize(1000, 400);
    output.show();
    QApplication::processEvents();
    int idle_flushes = 0;
    for (auto* timer : output.findChildren<QTimer*>())
        QObject::connect(timer, &QTimer::timeout, &output, [&] { ++idle_flushes; });
    QEventLoop idle;
    QTimer::singleShot(2000, &idle, &QEventLoop::quit);
    const auto idle_started = Clock::now();
    idle.exec();
    const auto idle_ms = elapsed(idle_started);
    std::vector<double> flush;
    output.beginCommand("Synthetic bounded output fixture");
    for (int chunk = 0; chunk < 30; ++chunk) {
        output.appendCommandOutput(QString(2048, 'x') + '\n', false);
        const auto start = Clock::now();
        if (!QMetaObject::invokeMethod(&output, "flush", Qt::DirectConnection))
            throw std::runtime_error("output flush slot unavailable");
        QApplication::processEvents();
        flush.push_back(elapsed(start));
    }
    output.endCommand(0, 1);
    return {{"idle_observed_ms", idle_ms},
            {"idle_timer_timeouts", idle_flushes},
            {"output_flush", summary(flush)},
            {"chunk_utf16_chars", 2049}};
}
}   // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setApplicationName("LocalCodePerformance");
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "offscreen") {
        const auto fonts = qEnvironmentVariable("WINDIR") + "/Fonts/";
        if (QFontDatabase::addApplicationFont(fonts + "segoeui.ttf") < 0 ||
            QFontDatabase::addApplicationFont(fonts + "consola.ttf") < 0)
            throw std::runtime_error("cannot load the Windows fonts for the offscreen benchmark");
    }
#endif
    localcode::gui::Theme::instance().apply();
    try {
        const auto arguments = application.arguments();
        const auto output = arguments.value(1, "build/performance/gui-baseline.json");
        const int repeats = std::clamp(arguments.value(2, "3").toInt(), 1, 20);
        const auto initial_memory = memory();
        QJsonArray chats;
        const bool output_only = arguments.value(3) == "output";
        if (!output_only) {
            for (const int count : {100, 500, 1000}) {
                chats.append(chat(count, repeats));
                std::cerr << "Measured " << count << "-message chat\n";
            }
        }
        QJsonObject report{{"schema", 1},
                           {"qt_version", QT_VERSION_STR},
                           {"platform", QGuiApplication::platformName()},
                           {"initial_memory", initial_memory},
                           {"chat", chats},
                           {"context_structural_only", context(repeats)},
                           {"output_panel", outputPanel()}};
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        const auto path = std::filesystem::path(output.toStdString());
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(bytes.constData(), bytes.size());
        if (!file) throw std::runtime_error("cannot write benchmark artifact");
        std::cout << bytes.toStdString();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
