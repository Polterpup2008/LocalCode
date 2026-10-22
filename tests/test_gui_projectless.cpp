#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>
#include <catch2/catch_test_macros.hpp>

#include "MainWindow.h"
#include "platform/FileSystem.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "security/PathPolicy.h"
#include "widgets/SourceEditor.h"

using namespace localcode;
using namespace localcode::gui;

namespace {

struct RestoreProjectlessDataPath {
    std::filesystem::path previous = app_paths().data_dir();

    ~RestoreProjectlessDataPath() { app_paths().set_data_dir(previous); }
};

QAction* projectlessAction(MainWindow& window) {
    return window.findChild<QAction*>("CreateOutputWorkspace");
}

}   // namespace

TEST_CASE("working without a project creates unique persistent dated workspaces",
          "[gui][workspace][projectless][persistence]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    RestoreProjectlessDataPath restore;
    const auto data = path_from_utf8(directory.filePath("data").toStdString());
    app_paths().set_data_dir(data);
    std::filesystem::path first;
    std::filesystem::path second;
    {
        Application application;
        REQUIRE(application.initialise());
        MainWindow window(application);
        auto* action = projectlessAction(window);
        REQUIRE(action);
        CHECK(action->text() == "Work without a project");
        action->trigger();
        REQUIRE(application.workspace().is_open());
        first = application.workspace().root();
        CHECK(first.parent_path() == PathPolicy::canonicalize(data / "outputs"));
        const auto name = QString::fromStdString(path_to_utf8(first.filename()));
        CHECK(QRegularExpression(
                  "^[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}-[A-Za-z0-9]{6}$")
                  .match(name)
                  .hasMatch());
        CHECK(first != std::filesystem::current_path());
        CHECK(application.workspace().paths().resolve("note.txt", PathAccess::Write));
        CHECK_FALSE(application.workspace().paths().resolve("../outside.txt", PathAccess::Write));
        REQUIRE(fs::write_text_atomic(first / "note.txt", "keep this output\n"));

        action->trigger();
        second = application.workspace().root();
        CHECK(second != first);
        CHECK(second.parent_path() == first.parent_path());
        CHECK(std::filesystem::is_directory(first));
        CHECK(std::filesystem::is_regular_file(first / "note.txt"));
        REQUIRE(application.mark_clean_shutdown());
    }
    // Restore the generated workspace through the ordinary session path.
    {
        Application application;
        REQUIRE(application.initialise());
        MainWindow window(application);
        window.restoreSession();
        CHECK(application.workspace().root() == second);
        const auto retained = fs::read_text(first / "note.txt");
        REQUIRE(retained);
        CHECK(retained->content == "keep this output\n");
    }
}

TEST_CASE("starting a dated workspace saves the previous editor through normal switching",
          "[gui][workspace][projectless][editor]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    RestoreProjectlessDataPath restore;
    app_paths().set_data_dir(path_from_utf8(directory.filePath("data").toStdString()));
    const auto project = directory.filePath("existing-project");
    REQUIRE(QDir().mkpath(project));
    const auto file = project + "/existing.txt";
    REQUIRE(fs::write_text_atomic(path_from_utf8(file.toStdString()), "before\n"));
    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    window.openProjectFolder(project);
    auto* editor = window.findChild<SourceEditor*>();
    REQUIRE(editor);
    editor->openFile(file);
    const auto views = editor->findChildren<SourceView*>();
    REQUIRE(views.size() == 1);
    views.front()->setPlainText("saved before switching\n");
    views.front()->document()->setModified(true);

    auto* action = projectlessAction(window);
    REQUIRE(action);
    action->trigger();
    CHECK(application.workspace().root() != path_from_utf8(project.toStdString()));
    CHECK(editor->openFiles().isEmpty());
    const auto saved = fs::read_text(path_from_utf8(file.toStdString()));
    REQUIRE(saved);
    CHECK(saved->content == "saved before switching\n");
}

TEST_CASE("a failed dated output directory leaves the current workspace intact",
          "[gui][workspace][projectless][error]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    RestoreProjectlessDataPath restore;
    const auto data = path_from_utf8(directory.filePath("data").toStdString());
    app_paths().set_data_dir(data);
    const auto project = directory.filePath("existing-project");
    REQUIRE(QDir().mkpath(project));
    Application application;
    REQUIRE(application.initialise());
    MainWindow window(application);
    window.openProjectFolder(project);
    const auto previous = application.workspace().root();
    REQUIRE(fs::write_text_atomic(data / "outputs", "an existing file must survive\n"));
    bool warningShown = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            warningShown = message->windowTitle() == "Cannot create output folder";
            message->accept();
        }
    });
    auto* action = projectlessAction(window);
    REQUIRE(action);
    action->trigger();
    CHECK(warningShown);
    CHECK(application.workspace().root() == previous);
    const auto existing = fs::read_text(data / "outputs");
    REQUIRE(existing);
    CHECK(existing->content == "an existing file must survive\n");
}
