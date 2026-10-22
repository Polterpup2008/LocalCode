#include "Paths.h"
#include "PathEncoding.h"

#include <cstdlib>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#include <shlobj.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace localcode {
namespace {

std::string env_or_empty(const char* name) {
#if defined(_WIN32)
    wchar_t* buffer = nullptr;
    std::size_t size = 0;
    const std::wstring wide_name(name, name + std::strlen(name));
    if (_wdupenv_s(&buffer, &size, wide_name.c_str()) != 0 || buffer == nullptr) return {};
    const auto value = path_to_utf8(std::filesystem::path{buffer});
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
#endif
}

std::filesystem::path platform_base(const char* windows_folder_env, const char* xdg_env,
                                    const char* xdg_fallback) {
#if defined(_WIN32)
    (void)xdg_env;
    (void)xdg_fallback;
    auto base = env_or_empty(windows_folder_env);
    if (base.empty()) {
        PWSTR wide = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wide))) {
            std::filesystem::path result{wide};
            CoTaskMemFree(wide);
            return result / "LocalCode";
        }
        return std::filesystem::current_path() / ".localcode";
    }
    return path_from_utf8(base) / "LocalCode";
#else
    (void)windows_folder_env;
    auto base = env_or_empty(xdg_env);
    if (!base.empty()) return std::filesystem::path{base} / "localcode";
    return AppPaths::home_dir() / xdg_fallback / "localcode";
#endif
}

}   // namespace

AppPaths::AppPaths() {
    const auto home = env_or_empty("LOCALCODE_HOME");
    if (!home.empty()) {
        set_root(path_from_utf8(home));
        return;
    }

#if defined(_WIN32)
    const auto base = platform_base("LOCALAPPDATA", nullptr, nullptr);
    data_ = base / "data";
    cache_ = base / "cache";
    logs_ = base / "logs";
    runtimes_ = base / "runtimes";
#else
    data_ = platform_base(nullptr, "XDG_DATA_HOME", ".local/share");
    cache_ = platform_base(nullptr, "XDG_CACHE_HOME", ".cache");
    logs_ = data_ / "logs";
    runtimes_ = data_ / "runtimes";
#endif
}

AppPaths& AppPaths::instance() {
    static AppPaths paths;
    return paths;
}

void AppPaths::set_root(const std::filesystem::path& root) {
    data_ = root / "data";
    cache_ = root / "cache";
    logs_ = root / "logs";
    runtimes_ = root / "runtimes";
}

void AppPaths::set_data_dir(const std::filesystem::path& p) {
    data_ = p;
}

void AppPaths::set_cache_dir(const std::filesystem::path& p) {
    cache_ = p;
}

void AppPaths::set_log_dir(const std::filesystem::path& p) {
    logs_ = p;
}

void AppPaths::set_runtime_dir(const std::filesystem::path& p) {
    runtimes_ = p;
}

std::filesystem::path AppPaths::data_dir() const {
    return data_;
}

std::filesystem::path AppPaths::cache_dir() const {
    return cache_;
}

std::filesystem::path AppPaths::log_dir() const {
    return logs_;
}

std::filesystem::path AppPaths::runtime_dir() const {
    return runtimes_;
}

std::filesystem::path AppPaths::command_log_dir() const {
    return cache_ / "commands";
}

std::filesystem::path AppPaths::database_path() const {
    return data_ / "localcode.sqlite3";
}

std::filesystem::path AppPaths::user_skills_dir() const {
    return data_ / "skills";
}

std::filesystem::path AppPaths::builtin_skills_dir() const {
    // Installed layout puts skills next to the executable; the build tree keeps
    // them in the source directory, which LOCALCODE_SKILLS_DIR points at.
    const auto override_dir = env_or_empty("LOCALCODE_SKILLS_DIR");
    if (!override_dir.empty()) return path_from_utf8(override_dir);

    std::error_code ec;
    const auto next_to_exe = executable_dir() / "skills";
    if (std::filesystem::is_directory(next_to_exe, ec)) return next_to_exe;

    const auto shared = executable_dir() / ".." / "share" / "localcode" / "skills";
    if (std::filesystem::is_directory(shared, ec)) {
        return std::filesystem::weakly_canonical(shared, ec);
    }
    return next_to_exe;
}

Status AppPaths::ensure_directories() const {
    const std::filesystem::path dirs[] = {
        data_, cache_, logs_, runtimes_, command_log_dir(), user_skills_dir()};
    for (const auto& dir : dirs) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec && !std::filesystem::is_directory(dir)) {
            return make_error(ErrorCode::IoError, "cannot create directory " + dir.string(),
                              ec.message());
        }
    }
    return ok_status();
}

std::filesystem::path AppPaths::executable_path() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path{buffer};
        }
        buffer.resize(buffer.size() * 2);   // long path: retry with a bigger buffer
    }
#else
    char buffer[PATH_MAX];
    const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (len <= 0) return {};
    buffer[len] = '\0';
    return std::filesystem::path{buffer};
#endif
}

std::filesystem::path AppPaths::executable_dir() {
    const auto exe = executable_path();
    return exe.empty() ? std::filesystem::current_path() : exe.parent_path();
}

std::filesystem::path AppPaths::bundled_llama_server() {
#if defined(_WIN32)
    return executable_dir() / "llama.cpp.localcode" / "llama-server.exe";
#else
    return executable_dir() / "llama.cpp.localcode" / "llama-server";
#endif
}

std::filesystem::path AppPaths::home_dir() {
#if defined(_WIN32)
    auto profile = env_or_empty("USERPROFILE");
    if (!profile.empty()) return path_from_utf8(profile);
    return std::filesystem::current_path();
#else
    auto home = env_or_empty("HOME");
    if (!home.empty()) return std::filesystem::path{home};
    return std::filesystem::current_path();
#endif
}

std::filesystem::path AppPaths::temp_dir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) return std::filesystem::current_path();
    return dir;
}

}   // namespace localcode
