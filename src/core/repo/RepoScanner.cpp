#include "RepoScanner.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <system_error>

#include "Hashing.h"
#include "Languages.h"
#include "SymbolIndexer.h"
#include "platform/PathEncoding.h"
#include "util/Json.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace localcode::repo {
namespace {

namespace stdfs = std::filesystem;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t file_time_ms(const stdfs::directory_entry& entry) {
    std::error_code ec;
    const auto time = entry.last_write_time(ec);
    if (ec) return 0;
    // file_time_type's epoch is unspecified; converting through the system
    // clock is what makes two runs comparable.
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
    return std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch())
        .count();
}

// A leading path component, or the whole thing when there is only one.
std::string first_segment(const std::string& path) {
    const auto slash = path.find('/');
    return slash == std::string::npos ? path : path.substr(0, slash);
}

std::string directory_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

void push_unique(std::vector<std::string>& out, std::string value, std::size_t limit = 32) {
    if (value.empty() || out.size() >= limit) return;
    if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(std::move(value));
}

// Names that mean "this is where a program starts", per ecosystem.
bool looks_like_entry_point(const std::string& path, const std::string& language) {
    const auto name = file_name_of(path);
    if (name == "main.cpp" || name == "main.c" || name == "main.rs" || name == "main.go" ||
        name == "main.py" || name == "__main__.py" || name == "program.cs" ||
        name == "index.js" || name == "index.ts" || name == "app.py") {
        return true;
    }
    if (language == "rust" && (path == "src/main.rs" || path == "src/lib.rs")) return true;
    return false;
}

// A directory is build output when it holds the marker a build system drops in
// one, whatever the directory is called.
//
// Names alone are not enough. `build` and `out` are caught by name, but a CMake
// build directory can be called anything: a scratch one called `verify` was
// indexed as source, and its generated CMakeFiles/ShowIncludes/main.c was
// offered as an entry point of the project. A marker file is a fact about the
// directory rather than a guess about its name.
bool holds_build_output(const stdfs::path& directory) {
    std::error_code ec;
    // Each of these is written by a build system into its own output directory
    // and means nothing else. compile_commands.json is deliberately not here:
    // it is routinely copied or linked next to the source for editors to find,
    // and excluding a source tree because of it would be far worse than
    // indexing a build directory.
    for (const char* marker : {"CMakeCache.txt", "CMakeFiles", "build.ninja", "meson-info"}) {
        const bool present = stdfs::exists(directory / marker, ec);
        if (!ec && present) return true;
        ec.clear();
    }
    return false;
}

}  // namespace

std::vector<RepoCommand> discover_commands(const stdfs::path& root,
                                           const std::vector<RepoFile>& files,
                                           const std::vector<std::string>& generated_directories) {
    std::vector<RepoCommand> commands;
    auto add = [&](std::string purpose, std::string command, std::string source,
                   Evidence evidence, std::string directory = {}) {
        RepoCommand entry;
        entry.purpose = std::move(purpose);
        entry.command = std::move(command);
        entry.source = std::move(source);
        entry.evidence = evidence;
        entry.working_directory = std::move(directory);
        commands.push_back(std::move(entry));
    };

    const auto has = [&](std::string_view path) {
        return std::any_of(files.begin(), files.end(),
                           [&](const RepoFile& f) { return f.path == path; });
    };

    // Cargo. The manifest is the whole story: if it is there, these work.
    if (has("Cargo.toml")) {
        add("build", "cargo build", "Cargo.toml", Evidence::Structural);
        add("test", "cargo test", "Cargo.toml", Evidence::Structural);
    }

    // CMake. The configure line is genuinely unknowable from the manifest - the
    // generator and the build directory are choices, not facts - so a build
    // directory that already exists is the strongest evidence available, and
    // without one this stays an inference and says so.
    if (has("CMakeLists.txt")) {
        bool found_build = false;
        std::size_t examined = 0;
        const auto inspect_build = [&](const stdfs::path& directory) {
            if (fs::is_link_or_reparse_point(directory)) return;
            const auto cache = directory / "CMakeCache.txt";
            std::error_code error;
            if (!stdfs::is_regular_file(cache, error) || fs::is_link_or_reparse_point(cache))
                return;
            const auto cache_size = stdfs::file_size(cache, error);
            if (error || cache_size > 512ull * 1024) return;
            const auto contents = fs::read_bytes(cache, 512ull * 1024);
            if (!contents) return;
            std::string source_root;
            for (const auto& line : strings::split_lines(*contents)) {
                constexpr std::string_view key = "CMAKE_HOME_DIRECTORY:INTERNAL=";
                if (line.starts_with(key)) source_root = strings::trim(line.substr(key.size()));
            }
            // A copied cache can configure a different project. Its presence
            // alone is not evidence that this workspace can build through it.
            if (source_root.empty() || !stdfs::equivalent(root, path_from_utf8(source_root), error) || error)
                return;
            const auto relative = path_to_utf8(directory.lexically_relative(root));
            if (relative.empty() || relative.starts_with("..")) return;
            const auto argument =
                relative.find(' ') == std::string::npos ? relative : '"' + relative + '"';
            // Configuration changes invalidate successful runs even when no
            // indexed source changed. This bounded cache fingerprint is part
            // of the discovery provenance compared on every refresh.
            const auto source = "an existing " + relative + "/CMakeCache.txt (fingerprint " +
                                fnv1a_hex(*contents) + ")";
            add("build", "cmake --build " + argument, source, Evidence::Structural);
            add("test", "ctest --test-dir " + argument + " --output-on-failure", source,
                Evidence::Structural);
            found_build = true;
        };
        // Generated trees are deliberately excluded from the source index.
        // Inspect only their root and immediate children, bounded independently
        // of their potentially huge object-file trees (e.g. build/gui).
        for (const auto& relative : generated_directories) {
            if (++examined > 128) break;
            const auto directory = root / path_from_utf8(relative);
            if (fs::is_link_or_reparse_point(directory)) continue;
            inspect_build(directory);
            std::error_code error;
            for (stdfs::directory_iterator it(directory, error), end; !error && it != end;
                 it.increment(error)) {
                if (++examined > 128) break;
                if (it->is_directory(error)) inspect_build(it->path());
            }
        }
        if (!found_build) {
            add("configure", "cmake -S . -B build", "CMakeLists.txt", Evidence::Heuristic);
            add("build", "cmake --build build", "CMakeLists.txt", Evidence::Heuristic);
            add("test", "ctest --test-dir build --output-on-failure", "CMakeLists.txt",
                Evidence::Heuristic);
        }
    }

    // package.json scripts are declared, so what they say is structural. What
    // is not knowable is which package manager the user runs.
    if (has("package.json")) {
        std::error_code ec;
        const auto contents = fs::read_bytes(root / "package.json", 512 * 1024);
        if (contents) {
            const auto parsed = Json::parse(*contents, nullptr, false);
            if (!parsed.is_discarded() && parsed.contains("scripts") &&
                parsed["scripts"].is_object()) {
                const std::string runner = stdfs::exists(root / "pnpm-lock.yaml", ec) ? "pnpm"
                                           : stdfs::exists(root / "yarn.lock", ec)    ? "yarn"
                                                                                     : "npm";
                for (const auto& [name, value] : parsed["scripts"].items()) {
                    if (!value.is_string()) continue;
                    std::string purpose = "run";
                    if (name == "build") purpose = "build";
                    else if (name == "test") purpose = "test";
                    else if (name == "lint") purpose = "lint";
                    else if (name == "start" || name == "dev") purpose = "run";
                    add(purpose, runner + " run " + name,
                        "package.json scripts." + name, Evidence::Structural);
                }
            }
        }
    }

    if (has("go.mod")) {
        add("build", "go build ./...", "go.mod", Evidence::Structural);
        add("test", "go test ./...", "go.mod", Evidence::Structural);
    }
    if (has("pyproject.toml") || has("setup.py")) {
        add("test", "pytest", has("pyproject.toml") ? "pyproject.toml" : "setup.py",
            Evidence::Heuristic);
    }
    if (has("Makefile") || has("makefile")) {
        add("build", "make", "Makefile", Evidence::Heuristic);
    }
    if (has("pom.xml")) {
        add("build", "mvn package", "pom.xml", Evidence::Structural);
        add("test", "mvn test", "pom.xml", Evidence::Structural);
    }

    // Visual Studio solutions name themselves.
    for (const auto& file : files) {
        if (extension_of(file.path) == ".sln") {
            add("build", "msbuild " + file.path, file.path, Evidence::Structural);
            break;
        }
    }
    return commands;
}

Result<ScanResult> scan_repository(const stdfs::path& root, const fs::IgnoreRules& ignore,
                                   const ScanLimits& limits, CancellationToken cancel,
                                   const std::function<void(const ScanProgress&)>& on_progress) {
    std::error_code ec;
    if (!stdfs::is_directory(root, ec)) {
        return make_error(ErrorCode::NotFound, "not a directory: " + path_to_utf8(root));
    }
    const auto canonical_root = stdfs::weakly_canonical(root, ec);
    const auto base = ec ? root.lexically_normal() : canonical_root;

    ScanResult result;
    result.overview.root = path_to_utf8(base);
    result.overview.indexed_at_unix_ms = now_ms();

    std::map<std::string, LanguageSummary> languages;
    std::set<std::string> source_roots;
    std::set<std::string> test_roots;
    std::set<std::string> build_systems;
    std::set<std::string> package_managers;
    std::set<std::string> generated_dirs;
    std::set<std::string> vendored_dirs;

    ScanProgress progress;
    const auto report = [&] {
        if (on_progress) on_progress(progress);
    };

    stdfs::recursive_directory_iterator it(
        base, stdfs::directory_options::skip_permission_denied, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot read " + path_to_utf8(base), ec.message());

    const stdfs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            // One unreadable directory must not abandon the whole scan.
            ec.clear();
            continue;
        }
        if (cancel.cancelled()) {
            result.cancelled = true;
            break;
        }

        const auto& entry = *it;
        if (fs::is_link_or_reparse_point(entry.path())) {
            it.disable_recursion_pending();
            continue;
        }
        auto relative = path_to_utf8(stdfs::relative(entry.path(), base, ec));
        if (ec || relative.empty() || relative.rfind("..", 0) == 0) {
            ec.clear();
            continue;
        }

        const auto name = path_to_utf8(entry.path().filename());

        if (entry.is_directory(ec)) {
            // A symlinked directory is not followed. Following one is how an
            // index of a repository becomes an index of the whole disk.
            if (entry.is_symlink(ec)) { it.disable_recursion_pending(); continue; }
            if (static_cast<std::size_t>(it.depth()) >= limits.max_depth) {
                it.disable_recursion_pending();
                result.overview.truncated = true;
                continue;
            }
            if (name == ".git") {
                it.disable_recursion_pending();
                continue;
            }
            if (is_vendored_directory(name)) {
                vendored_dirs.insert(relative);
                it.disable_recursion_pending();
                continue;
            }
            if (is_generated_directory(name) || holds_build_output(entry.path())) {
                generated_dirs.insert(relative);
                it.disable_recursion_pending();
                continue;
            }
            if (ignore.ignores_directory(name)) {
                it.disable_recursion_pending();
                continue;
            }
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (entry.is_symlink(ec)) continue;

        ++progress.files_seen;
        if (ignore.ignores_file(relative)) continue;
        if (result.files.size() >= limits.max_files) {
            result.overview.truncated = true;
            break;
        }

        RepoFile file;
        file.path = relative;
        file.language = language_of(relative);
        file.role = role_of(relative, file.language);
        file.modified_unix_ms = file_time_ms(entry);
        const auto size = entry.file_size(ec);
        file.size_bytes = ec ? 0 : static_cast<std::uint64_t>(size);
        ec.clear();

        if (!is_indexable_text(relative)) {
            file.skipped = true;
            file.skip_reason = "not a text file";
        } else if (file.size_bytes > limits.max_file_bytes) {
            file.skipped = true;
            file.skip_reason = "larger than the " +
                               std::to_string(limits.max_file_bytes / (1024 * 1024)) +
                               " MiB limit";
        }

        if (!file.skipped) {
            auto contents = fs::read_bytes(entry.path(), limits.max_file_bytes);
            if (!contents) {
                file.skipped = true;
                file.skip_reason = "could not be read";
            } else {
                // A file whose first bytes contain a NUL is binary whatever its
                // extension claims, and running a regex over it is pointless.
                if (contents->find('\0') != std::string::npos) {
                    file.skipped = true;
                    file.skip_reason = "binary content";
                } else {
                    file.content_hash = fnv1a_hex(*contents);
                    file.line_count = static_cast<std::size_t>(
                        std::count(contents->begin(), contents->end(), '\n'));
                    if (!contents->empty() && contents->back() != '\n') ++file.line_count;
                    ++progress.files_indexed;
                    result.overview.indexed_bytes += file.size_bytes;
                    result.overview.total_lines += file.line_count;
                }
            }
        }

        if (file.skipped) ++result.overview.skipped_files;

        // What the file tells us about the project.
        if (!file.language.empty() && file.role != FileRole::Generated &&
            file.role != FileRole::Vendored) {
            auto& summary = languages[file.language];
            summary.language = file.language;
            ++summary.files;
            summary.lines += file.line_count;
        }
        if (auto system = build_system_of(relative); !system.empty()) build_systems.insert(system);
        if (auto manager = package_manager_of(relative); !manager.empty()) {
            package_managers.insert(manager);
        }
        if (is_manifest(relative)) push_unique(result.overview.manifests, relative);
        if (file.role == FileRole::Documentation) {
            push_unique(result.overview.documentation, relative, 16);
        }
        if (file.role == FileRole::Source) {
            const auto directory = directory_of(relative);
            if (!directory.empty()) source_roots.insert(first_segment(directory));
            if (looks_like_entry_point(relative, file.language)) {
                push_unique(result.overview.entry_points, relative, 16);
            }
        }
        if (file.role == FileRole::Test) {
            const auto directory = directory_of(relative);
            if (!directory.empty()) test_roots.insert(first_segment(directory));
        }

        result.files.push_back(std::move(file));
        if (progress.files_seen % 256 == 0) {
            progress.current_path = relative;
            report();
        }
    }

    progress.current_path.clear();
    report();

    for (auto& [_, summary] : languages) result.overview.languages.push_back(summary);
    std::sort(result.overview.languages.begin(), result.overview.languages.end(),
              [](const LanguageSummary& a, const LanguageSummary& b) {
                  if (a.lines != b.lines) return a.lines > b.lines;
                  return a.files > b.files;
              });

    result.overview.build_systems.assign(build_systems.begin(), build_systems.end());
    result.overview.package_managers.assign(package_managers.begin(), package_managers.end());
    result.overview.source_roots.assign(source_roots.begin(), source_roots.end());
    result.overview.test_roots.assign(test_roots.begin(), test_roots.end());
    result.overview.generated_directories.assign(generated_dirs.begin(), generated_dirs.end());
    result.overview.vendored_directories.assign(vendored_dirs.begin(), vendored_dirs.end());
    result.overview.indexed_files = result.files.size();

    std::error_code git_ec;
    if (stdfs::exists(base / ".git", git_ec)) {
        result.overview.is_git_repository = true;
        result.overview.git_root = path_to_utf8(base);
    }

    result.commands = discover_commands(base, result.files, result.overview.generated_directories);
    return result;
}

}  // namespace localcode::repo
