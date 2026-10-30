#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <algorithm>
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

TEST_CASE("Prism runtime path and preferred backend survive reopening settings",
          "[gui][settings][runtime-choice][bonsai][persistence]") {
    RuntimeChoicePaths paths;
    const auto server =
        path_to_utf8(app_paths().runtime_dir() / "prism-custom" / "llama-server.exe");
    {
        Application application;
        REQUIRE(application.initialise());
        SettingsDialog dialog(application);
        auto* prism = dialog.findChild<QLineEdit*>("PrismLlamaServerPath");
        auto* backend = dialog.findChild<QComboBox*>("DefaultRuntimeBackend");
        REQUIRE(prism);
        REQUIRE(backend);
        prism->setText(QString::fromStdString(server));
        backend->setCurrentIndex(2);
        saveRuntimeSettings(dialog);
        CHECK(application.settings().default_backend == BackendPreference::PrismLlama);
    }
    Application reopened;
    REQUIRE(reopened.initialise());
    CHECK(reopened.settings().prism_llama_server_path == server);
    CHECK(reopened.settings().default_backend == BackendPreference::PrismLlama);
    CHECK(path_to_utf8(reopened.backends().configured_binary(BackendPreference::PrismLlama)) ==
          server);
}

TEST_CASE("legacy bundled profiles migrate without changing pinned or alternate runtimes",
          "[gui][settings][runtime-choice][migration]") {
    RuntimeChoicePaths paths;
    std::vector<ModelProfile> originals;
    {
        Application application;
        REQUIRE(application.initialise());
        auto settings = application.settings();
        settings.use_bundled_llama = true;
        REQUIRE(application.apply_settings(settings));
        for (const auto kind : {BackendPreference::LlamaCpp, BackendPreference::Auto,
                                BackendPreference::IkLlama, BackendPreference::PrismLlama}) {
            ModelProfile profile;
            profile.id = std::string(to_string(kind));
            profile.model_path = app_paths().data_dir() / (profile.id + ".gguf");
            profile.backend = kind;
            profile.context_size = 12288;
            profile.gpu_layers = 17;
            REQUIRE(application.save_model_profile(profile));
            originals.push_back(profile);
        }
        auto pinned = originals.front();
        pinned.id = "pinned";
        pinned.server_binary = app_paths().runtime_dir() / "owner-selected.exe";
        REQUIRE(application.save_model_profile(pinned));
        originals.push_back(pinned);
        REQUIRE(application.settings_repository().remove("backend.runtime_choices_version"));
    }
    Application upgraded;
    REQUIRE(upgraded.initialise());
    const auto profiles = upgraded.model_profiles();
    REQUIRE(profiles);
    REQUIRE(profiles->size() == originals.size());
    for (const auto& profile : *profiles) {
        const auto found = std::find_if(originals.begin(), originals.end(),
                                        [&](const auto& old) { return old.id == profile.id; });
        REQUIRE(found != originals.end());
        auto expected = *found;
        if (expected.backend == BackendPreference::LlamaCpp && expected.server_binary.empty())
            expected.backend = BackendPreference::LocalCode;
        CHECK(to_json(profile) == to_json(expected));
    }
    CHECK(upgraded.settings_repository().get_int("backend.runtime_choices_version", 0) == 1);
}

TEST_CASE("runtime migration preserves custom mode and later upstream selections",
          "[gui][settings][runtime-choice][migration]") {
    RuntimeChoicePaths paths;
    {
        Application application;
        REQUIRE(application.initialise());
        auto settings = application.settings();
        settings.use_bundled_llama = false;
        REQUIRE(application.apply_settings(settings));
        ModelProfile profile;
        profile.id = "upstream";
        profile.model_path = app_paths().data_dir() / "model.gguf";
        profile.backend = BackendPreference::LlamaCpp;
        REQUIRE(application.save_model_profile(profile));
        REQUIRE(application.settings_repository().remove("backend.runtime_choices_version"));
    }
    {
        Application upgraded;
        REQUIRE(upgraded.initialise());
        REQUIRE(upgraded.model_profiles());
        CHECK(upgraded.model_profiles()->front().backend == BackendPreference::LlamaCpp);
        auto settings = upgraded.settings();
        settings.use_bundled_llama = true;
        REQUIRE(upgraded.apply_settings(settings));
    }
    Application reopened;
    REQUIRE(reopened.initialise());
    REQUIRE(reopened.model_profiles());
    CHECK(reopened.model_profiles()->front().backend == BackendPreference::LlamaCpp);
}
