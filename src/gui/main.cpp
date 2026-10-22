// ---------------------------------------------------------------------------
// Entry point.
//
// Brings up the engine first and the window second, so a storage or settings
// failure is reported as a dialog rather than a silent crash at start-up.
// ---------------------------------------------------------------------------
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStyleFactory>
#include <QTextStream>
#include <QTimer>

#include "Application.h"
#include "MainWindow.h"
#include "Theme.h"
#include "plugins/DiscordPlugin.h"
#include "util/Log.h"

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeMenuBar, false);

    QApplication app(argc, argv);
    QApplication::setApplicationName("LocalCode");
    QApplication::setOrganizationName("LocalCode");
    QApplication::setApplicationVersion(LOCALCODE_VERSION);
    QApplication::setWindowIcon(QIcon(":/icons/localcode.ico"));
    const auto isolatedHome = qEnvironmentVariable("LOCALCODE_HOME");
    if (!isolatedHome.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolatedHome);
    }

    // Fusion is the only style that honours a custom palette consistently
    // across platforms; the native styles override half of it.
    if (auto* fusion = QStyleFactory::create("Fusion")) QApplication::setStyle(fusion);

    // `LocalCode .` should open the current directory, the way a developer
    // expects of a tool they launch from a terminal.
    QCommandLineParser parser;
    parser.setApplicationDescription(QObject::tr("A native coding agent for local GGUF models."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QObject::tr("project"), QObject::tr("Project folder to open."));
    QCommandLineOption modelOption(
        {"m", "model"}, QObject::tr("Model profile id to load at start-up."), QObject::tr("id"));
    parser.addOption(modelOption);
    QCommandLineOption pluginsOption("plugins", QObject::tr("Open plugin management."));
    QCommandLineOption snapshotOption("diagnostic-snapshot",
                                      QObject::tr("Render startup to a PNG and exit."), "path");
    snapshotOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(pluginsOption);
    parser.addOption(snapshotOption);
    parser.process(app);

    localcode::Application engine;
    if (auto status = engine.initialise(); !status) {
        QMessageBox::critical(nullptr, QObject::tr("LocalCode cannot start"),
                              QObject::tr("%1\n\n%2")
                                  .arg(QString::fromStdString(status.error().message),
                                       QString::fromStdString(status.error().detail)));
        return 1;
    }

    const auto settings = engine.settings();
    auto& theme = localcode::gui::Theme::instance();
    theme.setMode(settings.theme == "light" ? localcode::gui::ThemeMode::Light
                                            : localcode::gui::ThemeMode::Dark);
    theme.setMonoPointSize(settings.editor_font_size);
    theme.apply();

    // The window borrows the engine and must not outlive it being torn down.
    // Its own scope, so it is fully destroyed before shutdown() runs: the
    // controller unregisters its event sink and confirmation handler from the
    // agent as it goes, and shutdown() destroys that agent. Run the other way
    // round, those unregistrations dereference a destroyed object.
    int result = 0;
    {
        // Application-log lines are mirrored into the window's log view. The
        // sink runs on whichever thread logged, so it only queues a copy.
        localcode::gui::MainWindow window(engine);
        if (!parser.isSet(snapshotOption)) window.show();
        window.restoreSession();
        if (parser.isSet(pluginsOption)) window.showPlugins();

        const auto positional = parser.positionalArguments();
        if (!positional.isEmpty()) {
            const QString requested = QFileInfo(positional.first()).absoluteFilePath();
            if (QFileInfo(requested).isDir()) {
                window.openProjectFolder(requested);
            } else {
                QMessageBox::warning(&window, QObject::tr("Cannot open project"),
                                     QObject::tr("%1 is not a directory.").arg(requested));
            }
        }
        if (parser.isSet(modelOption)) window.loadModelById(parser.value(modelOption));

        if (parser.isSet(snapshotOption)) {
            // Render the real startup UI without taking desktop focus. Useful
            // for testing an installed payload with a clean user home.
            window.setAttribute(Qt::WA_DontShowOnScreen);
            window.resize(1280, 800);
            window.show();   // Run showEvent, including asynchronous plugin initialization.
            auto* worker = window.findChild<localcode::gui::DiscordPlugin*>();
            const bool waitForPlugin = parser.isSet(pluginsOption) && worker && worker->available();
            auto* timer = new QTimer(&window);
            timer->setInterval(100);
            QElapsedTimer started;
            started.start();
            QObject::connect(timer, &QTimer::timeout, &window, [&, timer, started, waitForPlugin] {
                if (started.elapsed() < 300) return;   // Settle posted layout work.
                if (waitForPlugin) {
                    const auto* save = window.findChild<QPushButton*>("SaveDiscordSettings");
                    const auto* stop = window.findChild<QPushButton*>("StopDiscordBot");
                    if (!save || !stop || (!save->isEnabled() && !stop->isEnabled())) {
                        if (started.elapsed() < 20000) return;
                        timer->stop();
                        QTextStream(stdout) << "plugin panel initialization timed out\n";
                        app.exit(4);
                        return;
                    }
                    QTextStream(stdout) << "plugin panel initialized\n";
                }
                timer->stop();
                const bool saved = window.grab().save(parser.value(snapshotOption));
                QTextStream(stdout)
                    << (saved ? "startup snapshot saved\n" : "startup snapshot failed\n");
                app.exit(saved ? 0 : 3);
            });
            timer->start();
        }
        result = app.exec();
    }

    // The engine owns a model server process; shutting it down explicitly means
    // no orphaned llama-server is left holding VRAM.
    engine.shutdown();
    return result;
}
