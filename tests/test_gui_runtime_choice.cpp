#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <catch2/catch_test_macros.hpp>

#include "Application.h"
#include "dialogs/SettingsDialog.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"

using namespace localcode;
using namespace localcode::gui;

namespace {
struct RuntimeChoicePaths {
    QTemporaryDir directory;
    std::filesystem::path data = app_paths().data_dir();
    std::filesystem::path cache = app_paths().cache_dir();
    std::filesystem::path logs = app_paths().log_dir();
    std::filesystem::path runtimes = app_paths().runtime_dir();

    RuntimeChoicePaths() { app_paths().set_root(path_from_utf8(directory.path().toStdString())); }

    ~RuntimeChoicePaths() {
        app_paths().set_data_dir(data);
        app_paths().set_cache_dir(cache);
        app_paths().set_log_dir(logs);
        app_paths().set_runtime_dir(runtimes);
    }
};

void saveRuntimeSettings(SettingsDialog& dialog) {
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(buttons);
    for (auto* button : buttons->buttons()) {
        if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) {
            button->click();
            REQUIRE(dialog.result() == QDialog::Accepted);
            return;
        }
    }
    FAIL("Settings Save button missing");
}
}   // namespace

TEST_CASE("bundled and custom runtime choices persist without losing the custom path",
          "[gui][settings][runtime-choice][persistence]") {
    RuntimeChoicePaths paths;
    REQUIRE(paths.directory.isValid());
    const std::string custom =
        path_to_utf8(app_paths().runtime_dir() / "custom" / "llama-server.exe");
    {
        Application application;
        REQUIRE(application.initialise());
#if defined(_WIN32)
        REQUIRE(application.settings().use_bundled_llama);
#endif
        CHECK_FALSE(application.settings().use_external_server);
        SettingsDialog dialog(application);
        auto* source = dialog.findChild<QComboBox*>("LlamaRuntimeSource");
        auto* server = dialog.findChild<QLineEdit*>("CustomLlamaServerPath");
        REQUIRE(source);
        REQUIRE(server);
        source->setCurrentIndex(1);
        REQUIRE(server->isEnabled());
        server->setText(QString::fromStdString(custom));
        saveRuntimeSettings(dialog);
        CHECK_FALSE(application.settings().use_bundled_llama);
        CHECK_FALSE(application.backends().use_bundled_llama());
        CHECK(application.backends().configured_binary(BackendPreference::LlamaCpp) ==
              path_from_utf8(custom));
    }
    {
        Application application;
        REQUIRE(application.initialise());
        CHECK_FALSE(application.settings().use_bundled_llama);
        CHECK(application.settings().llama_server_path == custom);
        SettingsDialog dialog(application);
        auto* source = dialog.findChild<QComboBox*>("LlamaRuntimeSource");
        auto* server = dialog.findChild<QLineEdit*>("CustomLlamaServerPath");
        REQUIRE(source);
        REQUIRE(server);
        source->setCurrentIndex(0);
        CHECK_FALSE(server->isEnabled());
        saveRuntimeSettings(dialog);
        CHECK(application.settings().use_bundled_llama);
        CHECK(application.backends().runtime_discovery().use_bundled_llama);
        CHECK(application.settings().llama_server_path == custom);
    }
    Application reopened;
    REQUIRE(reopened.initialise());
    CHECK(reopened.settings().use_bundled_llama);
    CHECK(reopened.settings().llama_server_path == custom);
    CHECK(AppPaths::bundled_llama_server().parent_path() ==
          AppPaths::executable_dir() / "llama.cpp.localcode");
}

TEST_CASE("upgrading a saved explicit runtime preserves its custom selection",
          "[gui][settings][runtime-choice][persistence]") {
    RuntimeChoicePaths paths;
    REQUIRE(paths.directory.isValid());
    const auto custom = path_to_utf8(app_paths().runtime_dir() / "owner-selected.exe");
    {
        Application application;
        REQUIRE(application.initialise());
        REQUIRE(application.settings_repository().set("backend.llama_server", custom));
        REQUIRE(application.settings_repository().remove("backend.bundled_llama"));
    }
    Application application;
    REQUIRE(application.initialise());
    CHECK_FALSE(application.settings().use_bundled_llama);
    CHECK(application.settings().llama_server_path == custom);
    CHECK_FALSE(application.settings().use_external_server);
}
