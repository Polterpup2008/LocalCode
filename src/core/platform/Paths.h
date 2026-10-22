#pragma once
// ---------------------------------------------------------------------------
// Application storage locations.
//
// Every writable location LocalCode uses is resolved here and every one of them
// is overridable, so the application never scatters large files across a user's
// system. The defaults follow platform conventions:
//
//   Windows: %LOCALAPPDATA%\LocalCode\{data,cache,logs,runtimes}
//   Linux:   $XDG_DATA_HOME/localcode, $XDG_CACHE_HOME/localcode, ...
//
// Overrides come from the LOCALCODE_HOME environment variable (which relocates
// all of them under one root) or from explicit setters used by the settings UI.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <string>

#include "../util/Result.h"

namespace localcode {

class AppPaths {
public:
    static AppPaths& instance();

    // Relocates every default under `root`. Called at start-up when
    // LOCALCODE_HOME is set or the user picks a portable location.
    void set_root(const std::filesystem::path& root);

    void set_data_dir(const std::filesystem::path& p);
    void set_cache_dir(const std::filesystem::path& p);
    void set_log_dir(const std::filesystem::path& p);
    void set_runtime_dir(const std::filesystem::path& p);

    // Databases, model profiles, skills the user has authored.
    [[nodiscard]] std::filesystem::path data_dir() const;
    // Regenerable: repository indexes, GGUF metadata cache, HTTP cache.
    [[nodiscard]] std::filesystem::path cache_dir() const;
    [[nodiscard]] std::filesystem::path log_dir() const;
    // Where managed llama.cpp / ik_llama.cpp binaries live.
    [[nodiscard]] std::filesystem::path runtime_dir() const;
    // Full command output is spooled here so tool results can stay small while
    // the complete log remains one click away.
    [[nodiscard]] std::filesystem::path command_log_dir() const;
    [[nodiscard]] std::filesystem::path database_path() const;
    [[nodiscard]] std::filesystem::path builtin_skills_dir() const;
    [[nodiscard]] std::filesystem::path user_skills_dir() const;

    // Creates every directory above. Returns the first failure.
    Status ensure_directories() const;

    // Directory containing the running executable.
    [[nodiscard]] static std::filesystem::path executable_dir();
    [[nodiscard]] static std::filesystem::path executable_path();
    [[nodiscard]] static std::filesystem::path bundled_llama_server();
    [[nodiscard]] static std::filesystem::path home_dir();
    [[nodiscard]] static std::filesystem::path temp_dir();

private:
    AppPaths();

    std::filesystem::path data_;
    std::filesystem::path cache_;
    std::filesystem::path logs_;
    std::filesystem::path runtimes_;
};

// Convenience accessor.
inline AppPaths& app_paths() {
    return AppPaths::instance();
}

}   // namespace localcode
