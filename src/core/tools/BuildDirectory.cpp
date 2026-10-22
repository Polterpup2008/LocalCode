#include "BuildDirectory.h"

#include <algorithm>
#include <fstream>

#include "../util/Strings.h"

namespace localcode {
namespace {

// A CMakeCache.txt line is  NAME:TYPE=VALUE.  Returns the value for `name`, or
// empty. Read line by line and stopped early, because a cache for a real
// project is large and only two entries are wanted.
std::string cache_value(const std::filesystem::path& cache, std::string_view name) {
    std::ifstream file{cache};
    if (!file) return {};

    const std::string prefix{name};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '/') continue;
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        if (line.size() <= prefix.size() || line[prefix.size()] != ':') continue;
        const auto equals = line.find('=', prefix.size());
        if (equals == std::string::npos) continue;
        return strings::trim(line.substr(equals + 1));
    }
    return {};
}

// Names that CMake and the build tools it drives write into a build tree. A
// directory containing only these has nothing in it that a person authored, so
// saying it can be removed is a statement of fact rather than a gamble.
bool is_generated_entry(const std::filesystem::path& entry, bool directory) {
    const auto name = strings::to_lower(entry.filename().string());
    const auto extension = strings::to_lower(entry.extension().string());

    if (directory) {
        static const char* kDirectories[] = {
            "cmakefiles", "debug", "release", "relwithdebinfo", "minsizerel",
            "x64", "win32", "arm64", "testing", "_deps", ".cmake",
        };
        for (const char* known : kDirectories) {
            if (name == known) return true;
        }
        return name.size() > 4 && name.compare(name.size() - 4, 4, ".dir") == 0;
    }

    static const char* kFiles[] = {
        "cmakecache.txt", "cmake_install.cmake", "ctesttestfile.cmake",
        "install_manifest.txt", "compile_commands.json", "build.ninja",
        "rules.ninja", ".ninja_deps", ".ninja_log", "makefile", "cmake_uninstall.cmake",
        "directory.build.props", "directory.build.targets", "all_build.vcxproj",
        "zero_check.vcxproj",
    };
    for (const char* known : kFiles) {
        if (name == known) return true;
    }

    static const char* kExtensions[] = {".sln", ".vcxproj", ".filters", ".user",
                                        ".ninja", ".cmake", ".tlog", ".suo"};
    for (const char* known : kExtensions) {
        if (extension == known) return true;
    }
    return false;
}

}  // namespace

BuildDirectoryState inspect_build_directory(const std::filesystem::path& path) {
    BuildDirectoryState state;
    state.path = path;
    if (path.empty()) return state;

    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) return state;
    state.exists = true;

    const auto cache = path / "CMakeCache.txt";
    if (std::filesystem::is_regular_file(cache, ec) && !ec) {
        state.configured = true;
        state.generator = cache_value(cache, "CMAKE_GENERATOR");
        state.source_dir = cache_value(cache, "CMAKE_HOME_DIRECTORY");
        state.make_program = cache_value(cache, "CMAKE_MAKE_PROGRAM");
    }

    for (const auto& entry : std::filesystem::directory_iterator{path, ec}) {
        if (ec) break;
        std::error_code entry_ec;
        const bool directory = entry.is_directory(entry_ec);
        if (is_generated_entry(entry.path(), directory)) continue;
        state.unrecognised.push_back(entry.path().filename().string());
        if (state.unrecognised.size() >= 8) break;
    }
    std::sort(state.unrecognised.begin(), state.unrecognised.end());

    // Only a configured tree can be called generated output. An arbitrary
    // directory that happens to be empty, or to be called "build", is not
    // CMake's to remove.
    state.generated_only = state.configured && state.unrecognised.empty();
    return state;
}

std::string requested_generator(const std::vector<std::string>& argv) {
    for (std::size_t i = 1; i < argv.size(); ++i) {
        if (argv[i] == "-G" && i + 1 < argv.size()) return argv[i + 1];
        if (argv[i].rfind("-G", 0) == 0 && argv[i].size() > 2) return argv[i].substr(2);
    }
    return {};
}

std::filesystem::path configure_binary_dir(const std::vector<std::string>& argv,
                                           const std::filesystem::path& working_directory) {
    if (argv.empty()) return {};
    if (strings::to_lower(std::filesystem::path{argv[0]}.stem().string()) != "cmake") return {};

    const auto resolve = [&](const std::string& value) {
        std::filesystem::path candidate{value};
        return candidate.is_absolute() ? candidate : working_directory / candidate;
    };

    for (std::size_t i = 1; i < argv.size(); ++i) {
        // --build and --install drive an existing tree rather than configuring
        // one, and have their own handling.
        if (argv[i] == "--build" || argv[i] == "--install") return {};
        if ((argv[i] == "-B" || argv[i] == "--build-dir") && i + 1 < argv.size()) {
            return resolve(argv[i + 1]);
        }
        if (argv[i].rfind("-B", 0) == 0 && argv[i].size() > 2) return resolve(argv[i].substr(2));
    }
    return {};
}

std::string generator_requires_program(std::string_view generator) {
    const auto lowered = strings::to_lower(generator);
    if (lowered.find("ninja") != std::string::npos) return "ninja";
    if (lowered.find("nmake") != std::string::npos) return "nmake";
    if (lowered.find("mingw") != std::string::npos) return "mingw32-make";
    if (lowered.find("borland") != std::string::npos) return "bcc32";
    if (lowered.find("watcom") != std::string::npos) return "wmake";
    // "Unix Makefiles", "MSYS Makefiles" and the rest of the make family.
    if (lowered.find("makefiles") != std::string::npos) return "make";
    // Visual Studio and Xcode find their own toolchains.
    return {};
}

bool same_generator(std::string_view a, std::string_view b) {
    const auto squash = [](std::string_view text) {
        std::string out;
        bool space = false;
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                space = !out.empty();
                continue;
            }
            if (space) out.push_back(' ');
            space = false;
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };
    return squash(a) == squash(b);
}

}  // namespace localcode
