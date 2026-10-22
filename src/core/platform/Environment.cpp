#include "Environment.h"
#include "../inference/RequestTrace.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <thread>

#include "../util/Json.h"
#include "../util/Log.h"
#include "../util/Strings.h"
#include "Paths.h"
#include "Process.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace localcode {
namespace {

struct ToolSpec {
    const char* id;
    const char* display_name;
    // Executables to try, in order of preference.
    std::vector<const char*> executables;
    // Arguments that make the tool print its version.
    std::vector<const char*> version_args;
};

const std::vector<ToolSpec>& tool_catalogue() {
    static const std::vector<ToolSpec> kTools = {
        {"cmake", "CMake", {"cmake"}, {"--version"}},
        {"ninja", "Ninja", {"ninja"}, {"--version"}},
        {"make", "Make", {"make", "gmake", "nmake"}, {"--version"}},
        {"msbuild", "MSBuild", {"msbuild"}, {"-version", "-nologo"}},
        {"git", "Git", {"git"}, {"--version"}},
        {"python", "Python", {"python", "python3", "py"}, {"--version"}},
        {"pip", "pip", {"pip", "pip3"}, {"--version"}},
        {"node", "Node.js", {"node"}, {"--version"}},
        {"npm", "npm", {"npm"}, {"--version"}},
        {"rustc", "Rust compiler", {"rustc"}, {"--version"}},
        {"cargo", "Cargo", {"cargo"}, {"--version"}},
        {"clang", "Clang", {"clang"}, {"--version"}},
        {"clangxx", "Clang++", {"clang++"}, {"--version"}},
        {"gcc", "GCC", {"gcc"}, {"--version"}},
        {"gxx", "G++", {"g++"}, {"--version"}},
        {"go", "Go", {"go"}, {"version"}},
        {"dotnet", ".NET SDK", {"dotnet"}, {"--version"}},
        {"java", "Java", {"java"}, {"-version"}},
        {"vcpkg", "vcpkg", {"vcpkg"}, {"version"}},
        {"conan", "Conan", {"conan"}, {"--version"}},
        {"ctest", "CTest", {"ctest"}, {"--version"}},
        {"vswhere", "vswhere", {"vswhere"}, {"-help"}},
        {"pwsh", "PowerShell 7+", {"pwsh"}, {"--version"}},
        {"ripgrep", "ripgrep", {"rg"}, {"--version"}},
    };
    return kTools;
}

std::string env_value(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
#endif
}

#if defined(_WIN32)
// The Visual Studio installation roots, asked of vswhere once and remembered.
//
// This exists because Visual Studio ships CMake and Ninja inside its own tree
// and puts neither on PATH. Without this, a machine with a complete C++ toolchain
// reports "cmake: not found", and an agent asked to build something dutifully
// tells the user to go and download CMake - which is precisely the dead end the
// bootstrap ordering is supposed to prevent.
const std::vector<std::filesystem::path>& visual_studio_roots() {
    static const std::vector<std::filesystem::path> roots = [] {
        std::vector<std::filesystem::path> found;

        const auto program_files_x86 = env_value("ProgramFiles(x86)");
        if (program_files_x86.empty()) return found;
        const std::filesystem::path vswhere = std::filesystem::path{program_files_x86} /
                                              "Microsoft Visual Studio" / "Installer" /
                                              "vswhere.exe";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(vswhere, ec)) return found;

        ProcessRequest request;
        request.executable = vswhere;
        request.arguments = {"-all", "-products", "*", "-property", "installationPath", "-utf8"};
        request.timeout = std::chrono::seconds(30);
        auto result = ProcessRunner::run(request);
        if (!result || result->exit_code != 0) return found;

        for (const auto& line : strings::split_lines(result->stdout_text)) {
            const auto trimmed = strings::trim(line);
            if (trimmed.empty()) continue;
            found.emplace_back(trimmed);
        }
        return found;
    }();
    return roots;
}
#endif

// Well-known install locations that installers frequently leave off PATH.
std::vector<std::filesystem::path> extra_locations(std::string_view name) {
    std::vector<std::filesystem::path> candidates;
#if defined(_WIN32)
    // A model asks for "msbuild.exe" as readily as "msbuild"; both must match.
    std::string stem{name};
    for (const char* suffix : {".exe", ".bat", ".cmd", ".com"}) {
        const std::size_t length = std::char_traits<char>::length(suffix);
        if (stem.size() > length &&
            strings::iequals(std::string_view{stem}.substr(stem.size() - length), suffix)) {
            stem.resize(stem.size() - length);
            break;
        }
    }
    const std::string_view executable{stem};
#else
    const std::string_view executable = name;
#endif
#if defined(_WIN32)
    const auto program_files = env_value("ProgramFiles");
    const auto program_files_x86 = env_value("ProgramFiles(x86)");
    const auto local_app_data = env_value("LOCALAPPDATA");

    auto add = [&](const std::filesystem::path& base, std::initializer_list<const char*> tail) {
        if (base.empty()) return;
        std::filesystem::path p = base;
        for (const char* part : tail)
            p /= part;
        candidates.push_back(p);
    };

    // Visual Studio's own copies of the C++ build tools. A stand-alone install
    // wins when there is one - it is usually newer - so these come second.
    const auto vs_bundled = [&](std::initializer_list<const char*> tail) {
        for (const auto& root : visual_studio_roots()) {
            std::filesystem::path p = root;
            for (const char* part : tail)
                p /= part;
            candidates.push_back(std::move(p));
        }
    };
    if (executable == "cmake") {
        add(program_files, {"CMake", "bin", "cmake.exe"});
        add(program_files_x86, {"CMake", "bin", "cmake.exe"});
        vs_bundled({"Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin",
                    "cmake.exe"});
    } else if (executable == "ctest") {
        add(program_files, {"CMake", "bin", "ctest.exe"});
        vs_bundled({"Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin",
                    "ctest.exe"});
    } else if (executable == "ninja") {
        vs_bundled(
            {"Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "Ninja", "ninja.exe"});
    } else if (executable == "msbuild") {
        vs_bundled({"MSBuild", "Current", "Bin", "amd64", "MSBuild.exe"});
        vs_bundled({"MSBuild", "Current", "Bin", "MSBuild.exe"});
    } else if (executable == "vcvars64" || executable == "vcvarsall" || executable == "vcvars32") {
        // The developer-environment scripts. The system prompt tells the model
        // where these are, and asking find_executable for one was answered
        // "not installed" - a flat contradiction of what LocalCode had already
        // printed, and enough to make a small model abandon MSVC entirely.
        const std::string script = std::string{executable} + ".bat";
        for (const auto& root : visual_studio_roots()) {
            candidates.push_back(root / "VC" / "Auxiliary" / "Build" / script);
        }
    } else if (executable == "cl" || executable == "link" || executable == "lib" ||
               executable == "dumpbin" || executable == "rc") {
        // The MSVC toolchain lives under a versioned directory, so the version
        // has to be discovered rather than guessed. Newest first: a machine with
        // several toolsets should get the current one.
        //
        // Note that cl.exe found this way still needs the developer environment
        // for its INCLUDE and LIB paths - finding it is not the same as being
        // able to run it standalone. Reporting "not found" was still wrong.
        const std::string program = std::string{executable} + ".exe";
        for (const auto& root : visual_studio_roots()) {
            const auto toolsets = root / "VC" / "Tools" / "MSVC";
            std::error_code ec;
            if (!std::filesystem::is_directory(toolsets, ec)) continue;

            std::vector<std::filesystem::path> versions;
            for (const auto& entry : std::filesystem::directory_iterator(toolsets, ec)) {
                if (entry.is_directory(ec)) versions.push_back(entry.path());
            }
            std::sort(versions.begin(), versions.end(), std::greater<>());
            for (const auto& version : versions) {
                candidates.push_back(version / "bin" / "Hostx64" / "x64" / program);
                candidates.push_back(version / "bin" / "Hostx64" / "x86" / program);
            }
        }
    } else if (executable == "devenv") {
        vs_bundled({"Common7", "IDE", "devenv.exe"});
    } else if (executable == "git") {
        add(program_files, {"Git", "cmd", "git.exe"});
        add(program_files_x86, {"Git", "cmd", "git.exe"});
    } else if (executable == "vswhere") {
        add(program_files_x86, {"Microsoft Visual Studio", "Installer", "vswhere.exe"});
    } else if (executable == "python" || executable == "python3") {
        add(local_app_data, {"Programs", "Python", "Python312", "python.exe"});
        add(local_app_data, {"Programs", "Python", "Python311", "python.exe"});
        add(local_app_data, {"Programs", "Python", "Python313", "python.exe"});
    } else if (executable == "node" || executable == "npm") {
        add(program_files, {"nodejs", executable == "node" ? "node.exe" : "npm.cmd"});
    } else if (executable == "rg") {
        add(local_app_data, {"Microsoft", "WinGet", "Links", "rg.exe"});
    }
#else
    (void)executable;
#endif
    return candidates;
}

std::string first_line(std::string_view text) {
    auto line = strings::trim(text.substr(0, text.find('\n')));
    return line;
}

std::string probe_version(const std::filesystem::path& exe, const std::vector<const char*>& args) {
    ProcessRequest request;
    request.executable = exe;
    for (const char* a : args)
        request.arguments.emplace_back(a);
    request.timeout = std::chrono::seconds(15);
    request.max_captured_bytes = 16 * 1024;

    auto result = ProcessRunner::run(request);
    if (!result) return {};
    // Several tools (java, msbuild) print their version on stderr.
    auto text = result->stdout_text.empty() ? result->stderr_text : result->stdout_text;
    return first_line(text);
}

}   // namespace

const ToolInfo* EnvironmentReport::tool(std::string_view id) const {
    for (const auto& t : tools) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

bool EnvironmentReport::has(std::string_view id) const {
    const auto* t = tool(id);
    return t && t->found;
}

EnvironmentService::EnvironmentService() = default;

std::vector<std::string> EnvironmentService::known_tool_ids() {
    std::vector<std::string> ids;
    ids.reserve(tool_catalogue().size());
    for (const auto& spec : tool_catalogue())
        ids.emplace_back(spec.id);
    return ids;
}

ToolInfo EnvironmentService::detect_tool(std::string_view id) const {
    ToolInfo info;
    info.id = id;

    const ToolSpec* spec = nullptr;
    for (const auto& candidate : tool_catalogue()) {
        if (candidate.id == id) {
            spec = &candidate;
            break;
        }
    }
    if (!spec) {
        info.note = "unknown tool id";
        return info;
    }
    info.display_name = spec->display_name;

    for (const char* executable : spec->executables) {
        if (auto found = find_executable(executable)) {
            info.found = true;
            info.path = *found;
            info.source = "PATH";
            for (const auto& other : find_all_executables(executable)) {
                if (other != *found) info.alternatives.push_back(other);
            }
            break;
        }
    }

    if (!info.found) {
        for (const char* executable : spec->executables) {
            for (const auto& candidate : extra_locations(executable)) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    info.found = true;
                    info.path = candidate;
                    info.source = "well-known install location (not on PATH)";
                    break;
                }
            }
            if (info.found) break;
        }
    }

    if (!info.found) {
        info.note = "not found on PATH or in the usual install locations";
        return info;
    }

    info.version = probe_version(info.path, spec->version_args);
    if (info.version.empty()) info.note = "found, but did not report a version";
    return info;
}

std::optional<std::filesystem::path> resolve_executable(
    std::string_view name, const std::filesystem::path& working_directory) {
    if (auto found = find_executable(name, working_directory)) return found;

    // An explicit path that did not resolve is simply absent; the well-known
    // locations are about bare names.
    const std::filesystem::path as_path{name};
    if (as_path.is_absolute() || as_path.has_parent_path()) return std::nullopt;

    // A program the agent just built, in the directory it is standing in.
    //
    // find_executable searches PATH, and the working directory is not on it, so
    // `fibonacci.exe` came back "not found" while fibonacci.exe sat in the
    // workspace root - which is exactly where `cl fibonacci.cpp` had put it a
    // moment earlier. cmd.exe finds it; a model reasonably expects the same.
    //
    // PATH is still searched first, so an installed tool of the same name keeps
    // winning and nothing on PATH can be shadowed by a file in the workspace.
    if (!working_directory.empty()) {
        std::error_code ec;
#if defined(_WIN32)
        static const char* kSuffixes[] = {"", ".exe", ".bat", ".cmd", ".com"};
        for (const char* suffix : kSuffixes) {
            const auto candidate = working_directory / (std::string{name} + suffix);
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return std::filesystem::weakly_canonical(candidate, ec);
            }
        }
#else
        const auto candidate = working_directory / std::filesystem::path{name};
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::filesystem::weakly_canonical(candidate, ec);
        }
#endif
    }

    for (const auto& candidate : extra_locations(name)) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::filesystem::weakly_canonical(candidate, ec);
        }
    }
    return std::nullopt;
}

const std::vector<std::pair<std::string, std::string>>& msvc_environment() {
    static const std::vector<std::pair<std::string, std::string>> captured = [] {
        std::vector<std::pair<std::string, std::string>> result;
#if defined(_WIN32)
        auto script = resolve_executable("vcvars64.bat");
        if (!script) {
            log_warn(LogChannel::App,
                     "no vcvars64.bat found; MSVC tools will run without their environment");
            return result;
        }

        // Run the script and print the environment it produced. `set` is a cmd
        // builtin, so this has to go through cmd - which is exactly the thing
        // the model cannot do across two separate commands.
        auto shell = find_executable("cmd");
        if (!shell) {
            log_warn(LogChannel::App, "cmd not found; cannot capture the MSVC environment");
            return result;
        }

        // Written to a file rather than passed as one long argument. The
        // command has to contain a quoted path, and a quoted path inside an
        // argument gets escaped again by the process layer into something cmd
        // does not understand - which is why the first attempt captured nothing
        // while the identical command worked when typed by hand.
        std::error_code ec;
        const auto directory = app_paths().cache_dir();
        std::filesystem::create_directories(directory, ec);
        const auto batch = directory / "capture-msvc-env.bat";
        {
            std::ofstream out(batch, std::ios::binary | std::ios::trunc);
            if (!out) {
                log_warn(LogChannel::App, "could not write the MSVC environment probe",
                         {{"path", batch.string()}});
                return result;
            }
            out << "@echo off\r\n"
                << "call \"" << script->string() << "\" >nul 2>&1\r\n"
                << "set\r\n";
        }

        ProcessRequest request;
        request.executable = *shell;
        request.arguments = {"/d", "/s", "/c", batch.string()};
        request.timeout = std::chrono::seconds(60);
        request.max_captured_bytes = 512 * 1024;

        auto ran = ProcessRunner::run(request);
        if (!ran || ran->exit_code != 0) {
            log_warn(LogChannel::App, "could not capture the MSVC environment",
                     {{"script", script->string()}});
            return result;
        }

        for (const auto& line : strings::split_lines(ran->stdout_text)) {
            const auto equals = line.find('=');
            if (equals == std::string::npos || equals == 0) continue;
            auto name = strings::trim(line.substr(0, equals));
            auto value = strings::trim(line.substr(equals + 1));
            if (name.empty()) continue;
            result.emplace_back(std::move(name), std::move(value));
        }
        log_info(LogChannel::App, "captured the MSVC developer environment",
                 {{"variables", std::to_string(result.size())}, {"from", script->string()}});
#endif
        return result;
    }();
    return captured;
}

bool needs_msvc_environment(const std::filesystem::path& executable) {
#if defined(_WIN32)
    const auto stem = strings::to_lower(executable.stem().string());
    // The compiler and its immediate friends. cmake and ninja are deliberately
    // not here: cmake with the Visual Studio generator finds the toolchain by
    // itself, and adding the variables underneath it would change which
    // compiler a configured project uses.
    static const char* kNeedy[] = {"cl", "link", "lib", "rc", "mt", "dumpbin", "nmake"};
    for (const char* candidate : kNeedy) {
        if (stem == candidate) return true;
    }
    return false;
#else
    (void)executable;
    return false;
#endif
}

bool build_tree_drives_msvc_directly(const std::filesystem::path& build_directory) {
#if defined(_WIN32)
    std::error_code ec;
    const auto cache = build_directory / "CMakeCache.txt";
    if (!std::filesystem::is_regular_file(cache, ec)) return false;

    std::ifstream input(cache);
    if (!input) return false;

    bool ninja_or_makefiles = false;
    bool msvc_compiler = false;
    std::string line;
    while (std::getline(input, line)) {
        // CMAKE_GENERATOR:INTERNAL=Ninja
        if (strings::starts_with(line, "CMAKE_GENERATOR:")) {
            const auto equals = line.find('=');
            if (equals == std::string::npos) continue;
            const auto generator = strings::to_lower(strings::trim(line.substr(equals + 1)));
            // A Visual Studio generator runs msbuild, which arranges its own
            // environment. Only the generators that invoke the compiler
            // directly need this.
            ninja_or_makefiles = generator.find("ninja") != std::string::npos ||
                                 generator.find("makefiles") != std::string::npos;
            continue;
        }
        // CMAKE_CXX_COMPILER:FILEPATH=.../Hostx64/x64/cl.exe
        if (strings::starts_with(line, "CMAKE_CXX_COMPILER:") ||
            strings::starts_with(line, "CMAKE_C_COMPILER:")) {
            const auto equals = line.find('=');
            if (equals == std::string::npos) continue;
            const auto compiler = strings::trim(line.substr(equals + 1));
            const auto stem = strings::to_lower(std::filesystem::path{compiler}.stem().string());
            if (stem == "cl") msvc_compiler = true;
        }
    }
    return ninja_or_makefiles && msvc_compiler;
#else
    (void)build_directory;
    return false;
#endif
}

void EnvironmentService::detect_visual_studio(EnvironmentReport& out) const {
#if defined(_WIN32)
    auto vswhere = find_executable("vswhere");
    if (!vswhere) {
        const auto program_files_x86 = env_value("ProgramFiles(x86)");
        if (!program_files_x86.empty()) {
            const std::filesystem::path candidate = std::filesystem::path{program_files_x86} /
                                                    "Microsoft Visual Studio" / "Installer" /
                                                    "vswhere.exe";
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec)) vswhere = candidate;
        }
    }
    if (!vswhere) return;

    ProcessRequest request;
    request.executable = *vswhere;
    request.arguments = {"-all", "-products", "*", "-format", "json", "-utf8"};
    request.timeout = std::chrono::seconds(30);
    auto result = ProcessRunner::run(request);
    if (!result || result->exit_code != 0) return;

    auto parsed = parse_json(result->stdout_text, "vswhere output");
    if (!parsed || !parsed->is_array()) return;

    for (const auto& entry : *parsed) {
        VisualStudioInstall install;
        install.display_name = json_string(entry, "displayName");
        install.version = json_string(entry, "installationVersion");
        install.install_path = json_string(entry, "installationPath");
        if (install.install_path.empty()) continue;

        const auto vcvars = install.install_path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
        std::error_code ec;
        if (std::filesystem::is_regular_file(vcvars, ec)) install.vcvars64 = vcvars;

        for (const char* relative :
             {"MSBuild/Current/Bin/amd64/MSBuild.exe", "MSBuild/Current/Bin/MSBuild.exe"}) {
            const auto candidate = install.install_path / relative;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                install.msbuild = candidate;
                break;
            }
        }

        const auto toolsets = install.install_path / "VC" / "Tools" / "MSVC";
        if (std::filesystem::is_directory(toolsets, ec)) {
            for (const auto& dir : std::filesystem::directory_iterator(toolsets, ec)) {
                if (dir.is_directory())
                    install.msvc_toolsets.push_back(dir.path().filename().string());
            }
            std::sort(install.msvc_toolsets.begin(), install.msvc_toolsets.end());
        }
        install.has_cxx_tools = !install.msvc_toolsets.empty();
        out.visual_studio.push_back(std::move(install));
    }
#else
    (void)out;
#endif
}

void EnvironmentService::detect_windows_sdks(EnvironmentReport& out) const {
#if defined(_WIN32)
    const auto program_files_x86 = env_value("ProgramFiles(x86)");
    if (program_files_x86.empty()) return;
    const std::filesystem::path kits =
        std::filesystem::path{program_files_x86} / "Windows Kits" / "10";
    std::error_code ec;
    const auto include_root = kits / "Include";
    if (!std::filesystem::is_directory(include_root, ec)) return;

    for (const auto& dir : std::filesystem::directory_iterator(include_root, ec)) {
        if (!dir.is_directory()) continue;
        WindowsSdkInfo sdk;
        sdk.version = dir.path().filename().string();
        sdk.include_dir = dir.path();
        const auto lib = kits / "Lib" / sdk.version;
        if (std::filesystem::is_directory(lib, ec)) sdk.lib_dir = lib;
        out.windows_sdks.push_back(std::move(sdk));
    }
    std::sort(
        out.windows_sdks.begin(), out.windows_sdks.end(),
        [](const WindowsSdkInfo& a, const WindowsSdkInfo& b) { return a.version < b.version; });
#else
    (void)out;
#endif
}

OsInfo probe_os_info() {
    OsInfo out;
    out.logical_cpus = std::thread::hardware_concurrency();
#if defined(_WIN32)
    out.name = "Windows";
    out.architecture = env_value("PROCESSOR_ARCHITECTURE");
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory)) out.total_memory_bytes = memory.ullTotalPhys;
    {
        // RtlGetVersion is the only API that still reports the real build number.
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
            RTL_OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof info;
            if (fn && fn(&info) == 0) {
                out.version = std::to_string(info.dwMajorVersion) + "." +
                              std::to_string(info.dwMinorVersion) + "." +
                              std::to_string(info.dwBuildNumber);
            }
        }
    }
#else
    utsname uts{};
    if (::uname(&uts) == 0) {
        out.name = uts.sysname;
        out.version = uts.release;
        out.architecture = uts.machine;
    }
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long page_size = ::sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        out.total_memory_bytes =
            static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
    }
#endif
    return out;
}

void EnvironmentService::run_detection(EnvironmentReport& out) const {
    out.detected_at_unix_ms = now_unix_ms();
    out.os = probe_os_info();

    // --- PATH -------------------------------------------------------------
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    for (const auto& entry : strings::split(env_value("PATH"), separator, false)) {
        if (!entry.empty()) out.path_entries.emplace_back(entry);
    }

    // --- Tools ------------------------------------------------------------
    const auto& catalogue = tool_catalogue();
    out.tools.resize(catalogue.size());
    std::atomic<std::size_t> next{0};
    auto detect_next = [&] {
        for (;;) {
            const auto index = next.fetch_add(1);
            if (index >= catalogue.size()) return;
            const auto& spec = catalogue[index];
            const auto started = std::chrono::steady_clock::now();
            out.tools[index] = detect_tool(spec.id);
            if (RequestTrace::enabled()) {
                RequestTrace::record_event(
                    "environment_tool_probe",
                    {{"tool", spec.id},
                     {"found", out.tools[index].found},
                     {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - started)
                                        .count()}});
            }
        }
    };
    // PATH searches and independent --version processes do not depend on one
    // another. Bound concurrency and retain catalogue order, rather than making
    // the first Send wait for their cumulative latency. No persistent shells.
    const auto concurrency = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
    std::vector<std::future<void>> probes;
    probes.reserve(concurrency);
    for (unsigned worker = 0; worker < concurrency; ++worker)
        probes.push_back(std::async(std::launch::async, detect_next));
    for (auto& probe : probes)
        probe.get();

    detect_visual_studio(out);
    detect_windows_sdks(out);

    // --- Notable variables -------------------------------------------------
    static const char* kVariables[] = {
        "VCPKG_ROOT",   "CMAKE_PREFIX_PATH", "CMAKE_GENERATOR",     "CC",           "CXX",
        "CFLAGS",       "CXXFLAGS",          "VIRTUAL_ENV",         "CONDA_PREFIX", "JAVA_HOME",
        "GOPATH",       "CARGO_HOME",        "RUSTUP_HOME",         "QT_DIR",       "Qt6_DIR",
        "VSINSTALLDIR", "WindowsSdkDir",     "NUMBER_OF_PROCESSORS"};
    for (const char* name : kVariables) {
        auto value = env_value(name);
        if (!value.empty()) out.notable_variables.emplace_back(name, std::move(value));
    }

    log_info(
        LogChannel::App, "environment detected",
        {{"tools_found", std::to_string(std::count_if(out.tools.begin(), out.tools.end(),
                                                      [](const ToolInfo& t) { return t.found; }))},
         {"visual_studio", std::to_string(out.visual_studio.size())}});
}

const EnvironmentReport& EnvironmentService::report(bool refresh) {
    std::lock_guard lock(mutex_);
    if (!valid_ || refresh) {
        cache_ = EnvironmentReport{};
        run_detection(cache_);
        valid_ = true;
    }
    return cache_;
}

}   // namespace localcode
