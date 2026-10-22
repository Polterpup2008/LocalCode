#pragma once
// ---------------------------------------------------------------------------
// Development environment discovery.
//
// Small local models are very bad at guessing where software is installed, and
// a wrong guess turns into "you need to install CMake" when CMake is sitting in
// Program Files. This service answers that question with facts: it resolves each
// tool, records where it came from, and runs it to read a real version string.
//
// Detection is deliberately more than a PATH lookup. On Windows it also consults
// vswhere for Visual Studio, the Windows Kits registry for SDKs, and the
// well-known install locations that installers use but do not add to PATH.
// ---------------------------------------------------------------------------
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../util/Result.h"

namespace localcode {

struct ToolInfo {
    std::string           id;             // stable key: "cmake", "msbuild", ...
    std::string           display_name;
    bool                  found = false;
    std::filesystem::path path;
    std::string           version;        // first line of --version, trimmed
    std::string           source;         // "PATH", "Visual Studio", "registry", ...
    std::string           note;           // why it was not found, or a caveat
    std::vector<std::filesystem::path> alternatives;   // other copies on PATH
};

struct VisualStudioInstall {
    std::string           display_name;
    std::string           version;
    std::filesystem::path install_path;
    std::filesystem::path vcvars64;
    std::filesystem::path msbuild;
    std::vector<std::string> msvc_toolsets;
    bool                  has_cxx_tools = false;
};

struct WindowsSdkInfo {
    std::string           version;
    std::filesystem::path include_dir;
    std::filesystem::path lib_dir;
};

struct OsInfo {
    std::string name;
    std::string version;
    std::string architecture;
    unsigned    logical_cpus = 0;
    std::uint64_t total_memory_bytes = 0;
};

// Fast OS/CPU/RAM snapshot; no compiler discovery or child process is involved.
[[nodiscard]] OsInfo probe_os_info();

struct EnvironmentReport {
    OsInfo                           os;
    std::vector<ToolInfo>            tools;
    std::vector<VisualStudioInstall> visual_studio;
    std::vector<WindowsSdkInfo>      windows_sdks;
    std::vector<std::filesystem::path> path_entries;
    std::vector<std::pair<std::string, std::string>> notable_variables;
    std::int64_t                     detected_at_unix_ms = 0;

    [[nodiscard]] const ToolInfo* tool(std::string_view id) const;
    [[nodiscard]] bool has(std::string_view id) const;
};

// Finds an executable the way a person would expect: PATH first, then the
// well-known places installers leave things off PATH - including inside a Visual
// Studio installation.
//
// `find_executable` in Process.h is the primitive and searches PATH only; it
// cannot do more without depending on this layer, which runs processes. Anything
// user- or model-facing should call this instead, because "cmake is not
// installed" on a machine that ships CMake inside Visual Studio is a wrong
// answer that ends a task.
[[nodiscard]] std::optional<std::filesystem::path> resolve_executable(
    std::string_view name, const std::filesystem::path& working_directory = {});

// The variables vcvars64.bat sets, so an MSVC tool can actually be run.
//
// Finding cl.exe is not the same as being able to use it: the compiler needs
// INCLUDE, LIB and LIBPATH, and those come from a batch file that sets them in
// a shell and exits. A model cannot carry that across separate commands, and
// watching one try - vcvars64.bat with the build appended, four times, each
// returning zero and doing nothing - is what made this necessary.
//
// Captured once by running the script and reading the resulting environment.
// Empty when there is no Visual Studio, or on any platform that does not need
// this, which is every other platform.
[[nodiscard]] const std::vector<std::pair<std::string, std::string>>& msvc_environment();

// Is this program one that needs the MSVC environment to work?
[[nodiscard]] bool needs_msvc_environment(const std::filesystem::path& executable);

// Does an already-configured build tree drive the MSVC compiler directly?
//
// Read from the CMake cache, not guessed. That distinction is what makes it
// safe to supply the developer environment for `cmake --build`: the toolchain
// has already been chosen and written down, so the variables cannot change
// which compiler is used - they only let the chosen one find its headers.
//
// False for a Visual Studio generator, which starts msbuild and sets up its
// own environment, and false when there is no cache to read.
[[nodiscard]] bool build_tree_drives_msvc_directly(const std::filesystem::path& build_directory);

class EnvironmentService {
public:
    EnvironmentService();

    // Cached; pass refresh=true after the user installs something.
    const EnvironmentReport& report(bool refresh = false);

    // Detects one tool without a full scan.
    [[nodiscard]] ToolInfo detect_tool(std::string_view id) const;

    // The catalogue of tools this service knows how to find.
    [[nodiscard]] static std::vector<std::string> known_tool_ids();

private:
    void run_detection(EnvironmentReport& out) const;
    void detect_visual_studio(EnvironmentReport& out) const;
    void detect_windows_sdks(EnvironmentReport& out) const;

    mutable std::mutex        mutex_;
    EnvironmentReport         cache_;
    bool                      valid_ = false;
};

}  // namespace localcode
