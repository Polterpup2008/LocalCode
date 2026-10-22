// Whole-window observations use production widgets, persistence and indexing.
// They are opt-in, keep data under the supplied output directory, and do not
// load a model or change user settings. Offscreen runs are local observations,
// not evidence of native desktop visibility or interactive input latency.
#include <QApplication>
#include <QEventLoop>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QSettings>
#include <QStyleFactory>
#include <QTextBrowser>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "MainWindow.h"
#include "Theme.h"
#include "dialogs/SettingsDialog.h"
#include "platform/Paths.h"
#include "repo/RepoTypes.h"
#include "widgets/ChatView.h"
#include "widgets/RepositoryPanel.h"

namespace {
using namespace localcode;
using namespace localcode::gui;
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Json summary(const std::vector<double>& samples) {
    auto ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    const auto percentile = [&](double fraction) {
        return ordered.at(static_cast<std::size_t>(std::ceil(fraction * ordered.size())) - 1);
    };
    return {{"unit", "ms"},
            {"n", samples.size()},
            {"samples", samples},
            {"p50", percentile(.5)},
            {"p95", percentile(.95)}};
}

void require(const Status& status) {
    if (!status) throw std::runtime_error(status.error().message);
}

template <typename T>
void require(const Result<T>& result) {
    if (!result) throw std::runtime_error(result.error().message);
}

void populate(const std::filesystem::path& path, int files) {
    std::filesystem::create_directories(path / "src");
    for (int file = 0; file < files; ++file) {
        std::ofstream stream(path / "src" / ("source" + std::to_string(file) + ".cpp"));
        stream << "int fixture_function_" << file << "() { return " << file << "; }\n";
    }
}
}   // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName("LocalCodeAppPerformance");
    application.setOrganizationName("LocalCodePerformance");
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "offscreen") {
        const auto fonts = qEnvironmentVariable("WINDIR") + "/Fonts/";
        QFontDatabase::addApplicationFont(fonts + "segoeui.ttf");
        QFontDatabase::addApplicationFont(fonts + "consola.ttf");
    }
#endif
    if (auto* fusion = QStyleFactory::create("Fusion")) application.setStyle(fusion);
    Theme::instance().apply();
    try {
        const auto output =
            std::filesystem::absolute(argc > 1 ? argv[1] : "build/performance/app-window.json");
        const int repeats = argc > 2 ? std::clamp(std::stoi(argv[2]), 1, 20) : 3;
        const bool reasoning_heavy = argc > 3 && std::string_view(argv[3]) == "reasoning";
        const auto fixture =
            output.parent_path() /
            ("app-fixture-" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 Clock::now().time_since_epoch())
                                                 .count()));
        const auto first_workspace = fixture / "workspace-500";
        const auto second_workspace = fixture / "workspace-20";
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QString::fromStdString((fixture / "layout-settings").string()));
        populate(first_workspace, 500);
        populate(second_workspace, 20);
        std::vector<double> initialise, large_session_initialise, shell, restored, settings,
            repo_panel, switch_workspace;
        std::vector<double> heartbeat_max_gap;
        for (int repeat = 0; repeat < repeats; ++repeat) {
            app_paths().set_root(fixture / ("home-" + std::to_string(repeat)));
            Application engine;
            auto start = Clock::now();
            require(engine.initialise());
            initialise.push_back(elapsed(start));
            require(engine.open_workspace(first_workspace));
            require(engine.start_conversation("Synthetic 500-message restoration"));
            std::vector<ChatMessage> history;
            for (int message = 0; message < 500; ++message) {
                const auto body = "Message " + std::to_string(message) + "\n\n" +
                                  std::string(2048, static_cast<char>('a' + message % 26));
                history.push_back(message % 2 == 0 ? ChatMessage::user(body)
                                                   : ChatMessage::assistant(body));
                if (reasoning_heavy && message % 2 != 0) {
                    for (int passage = 0; passage < 90; ++passage)
                        history.back().reasoning_content +=
                            "The source location and runtime evidence must be checked before "
                            "reporting completion. ";
                }
            }
            require(engine.persist_conversation(history));
            require(engine.checkpoint_session({}, "Synthetic draft"));
            require(engine.mark_clean_shutdown());
            engine.shutdown();
            start = Clock::now();
            require(engine.initialise());
            large_session_initialise.push_back(elapsed(start));
            {
                start = Clock::now();
                MainWindow window(engine);
                window.resize(1280, 800);
                window.show();
                application.processEvents();
                shell.push_back(elapsed(start));
                auto* chat = window.findChild<ChatView*>();
                auto* repository = window.findChild<RepositoryPanel*>();
                if (!chat || !repository) throw std::runtime_error("production widgets missing");
                double maximum_gap = 0;
                auto last_heartbeat = Clock::now();
                QTimer heartbeat;
                heartbeat.setInterval(16);
                QObject::connect(&heartbeat, &QTimer::timeout, &window, [&] {
                    maximum_gap = std::max(maximum_gap, elapsed(last_heartbeat));
                    last_heartbeat = Clock::now();
                });
                heartbeat.start();
                QEventLoop indexing;
                bool ready = false;
                bool failed = false;
                QObject::connect(repository, &RepositoryPanel::progressArrived, &indexing,
                                 [&](int state, int, int, const QString&) {
                                     if (state == static_cast<int>(repo::IndexState::Ready))
                                         ready = true;
                                     if (state == static_cast<int>(repo::IndexState::Failed))
                                         failed = true;
                                     if (ready || failed) indexing.quit();
                                 });
                QTimer timeout;
                timeout.setSingleShot(true);
                QObject::connect(&timeout, &QTimer::timeout, &indexing, &QEventLoop::quit);
                auto wait_for_index = [&] {
                    if (!ready && !failed) {
                        timeout.start(30000);
                        indexing.exec();
                        timeout.stop();
                    }
                    if (!ready || failed) throw std::runtime_error("fixture index did not finish");
                };
                start = Clock::now();
                window.restoreSession();
                application.processEvents();
                restored.push_back(elapsed(start));
                auto* browser = chat->findChild<QTextBrowser*>();
                if (!browser || !browser->toPlainText().contains("Message 499"))
                    throw std::runtime_error("restored conversation is incomplete");
                if (!chat->toMarkdown().contains("Message 0"))
                    throw std::runtime_error("paged restore lost the earliest message");
                wait_for_index();
                for (int sample = 0; sample < 10; ++sample) {
                    start = Clock::now();
                    repository->workspaceChanged();
                    application.processEvents();
                    repo_panel.push_back(elapsed(start));
                }
                start = Clock::now();
                SettingsDialog dialog(engine, &window);
                dialog.show();
                application.processEvents();
                settings.push_back(elapsed(start));
                dialog.reject();
                ready = false;
                failed = false;
                start = Clock::now();
                window.openProjectFolder(QString::fromStdString(second_workspace.string()));
                application.processEvents();
                switch_workspace.push_back(elapsed(start));
                wait_for_index();
                heartbeat.stop();
                heartbeat_max_gap.push_back(maximum_gap);
                if (repeat == 0) {
                    window.grab().save(QString::fromStdString((fixture / "window.png").string()));
                }
                window.close();
            }
            engine.shutdown();
            std::cerr << "Measured complete application fixture " << repeat << '\n';
        }
        Json report{{"schema", 1},
                    {"platform", QGuiApplication::platformName().toStdString()},
                    {"qt", QT_VERSION_STR},
                    {"fixture_root", fixture.string()},
                    {"engine_initialise", summary(initialise)},
                    {"engine_initialise_500_message_database", summary(large_session_initialise)},
                    {"reasoning_heavy", reasoning_heavy},
                    {"construct_show_process_events", summary(shell)},
                    {"restore_500_2kib_messages", summary(restored)},
                    {"settings_construct_show", summary(settings)},
                    {"repository_panel_refresh", summary(repo_panel)},
                    {"workspace_switch_foreground", summary(switch_workspace)},
                    {"maximum_16ms_heartbeat_gap", summary(heartbeat_max_gap)},
                    {"notes",
                     {"Synthetic persisted data; no model loaded.",
                      "Shell timing excludes OS loader and engine initialization.",
                      "Workspace foreground time excludes background index completion.",
                      "Offscreen timings are not native desktop latency."}}};
        report["notes"].push_back(
            "Paged restore retains all 500 messages; it initially renders the latest 50. Earlier "
            "pages load on demand.");
        std::ofstream file(output, std::ios::binary);
        file << report.dump(2) << '\n';
        if (!file) throw std::runtime_error("cannot save application performance report");
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
