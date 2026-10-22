#include "Workspace.h"

#include <algorithm>
#include <map>

#include "../util/Log.h"
#include "../platform/PathEncoding.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

// Files whose presence identifies how a project is built and laid out. Seeing
// these in the repository map lets the agent pick the right build command on its
// first try instead of guessing.
const std::vector<std::pair<const char*, const char*>>& build_markers() {
    static const std::vector<std::pair<const char*, const char*>> kMarkers = {
        {"CMakeLists.txt", "CMake"},
        {"CMakePresets.json", "CMake"},
        {"meson.build", "Meson"},
        {"Makefile", "Make"},
        {"GNUmakefile", "Make"},
        {"build.ninja", "Ninja"},
        {"Cargo.toml", "Cargo"},
        {"package.json", "npm/node"},
        {"pyproject.toml", "Python"},
        {"setup.py", "Python"},
        {"requirements.txt", "Python"},
        {"go.mod", "Go"},
        {"pom.xml", "Maven"},
        {"build.gradle", "Gradle"},
        {"build.gradle.kts", "Gradle"},
        {"BUILD.bazel", "Bazel"},
        {"WORKSPACE", "Bazel"},
        {"vcpkg.json", "vcpkg"},
        {"conanfile.txt", "Conan"},
        {"conanfile.py", "Conan"},
        {"Dockerfile", "Docker"},
        {"*.sln", "MSBuild"},
        {"*.vcxproj", "MSBuild"},
        {"*.csproj", "MSBuild"},
    };
    return kMarkers;
}

const std::vector<const char*>& documentation_markers() {
    static const std::vector<const char*> kFiles = {
        "README.md", "README.rst", "README.txt", "README",
        "CONTRIBUTING.md", "AGENTS.md", "CLAUDE.md", "ARCHITECTURE.md",
        ".gitignore", ".editorconfig", "LICENSE",
    };
    return kFiles;
}

}  // namespace

std::string_view to_string(AgentChangeKind kind) noexcept {
    switch (kind) {
        case AgentChangeKind::Created:  return "created";
        case AgentChangeKind::Modified: return "modified";
        case AgentChangeKind::Deleted:  return "deleted";
        case AgentChangeKind::Moved:    return "moved";
    }
    return "modified";
}

Workspace::Workspace() { commands_.set_path_policy(&paths_); }

Status Workspace::open(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return make_error(ErrorCode::NotFound, "workspace directory does not exist: " +
                                                   path_to_utf8(root));
    }

    const auto canonical = PathPolicy::canonicalize(root);

    std::lock_guard lock(mutex_);
    root_ = canonical;
    paths_.set_workspace_root(canonical);
    ignore_ = fs::IgnoreRules{};

    // Honour the project's own .gitignore on top of the built-in list.
    const auto gitignore = canonical / ".gitignore";
    if (std::filesystem::is_regular_file(gitignore, ec)) ignore_.load_gitignore(gitignore);

    baseline_dirty_.clear();
    baseline_commit_.clear();
    agent_changes_.clear();

    log_info(LogChannel::App, "workspace opened", {{"path", path_to_utf8(canonical)}});
    return ok_status();
}

void Workspace::close() {
    std::lock_guard lock(mutex_);
    root_.clear();
    paths_.set_workspace_root({});
    baseline_dirty_.clear();
    baseline_commit_.clear();
    agent_changes_.clear();
}

bool Workspace::is_open() const {
    std::lock_guard lock(mutex_);
    return !root_.empty();
}

std::filesystem::path Workspace::root() const {
    std::lock_guard lock(mutex_);
    return root_;
}

std::string Workspace::display_name() const {
    std::lock_guard lock(mutex_);
    if (root_.empty()) return {};
    auto name = path_to_utf8(root_.filename());
    return name.empty() ? path_to_utf8(root_) : name;
}

void Workspace::record_resolved_resources(std::vector<ResolvedResource> resources) {
    // An empty list is what a no-op rebuild produces - MSBuild prints no link
    // line when nothing needed linking - and it does not mean the program has
    // stopped existing. Keeping the previous set is the difference between
    // being able to answer "where is it" and having to say "I don't know".
    if (resources.empty()) return;
    std::lock_guard lock(mutex_);
    resources_ = std::move(resources);
}

std::vector<ResolvedResource> Workspace::resolved_resources() const {
    std::lock_guard lock(mutex_);
    return resources_;
}

std::optional<ResolvedResource> Workspace::resolved_resource(std::string_view id) const {
    std::lock_guard lock(mutex_);
    // Exact id first, then the bare name, so both "build:fibonacci" and
    // "fibonacci" reach the same thing.
    for (const auto& resource : resources_) {
        if (resource.id == id) return resource;
    }
    for (const auto& resource : resources_) {
        if (strings::iequals(resource.name, id)) return resource;
    }
    // The recorded path names the same thing. A caller that has just been
    // shown "build/Debug/app.exe" and passes it back has identified the
    // resource exactly, not approximately - so this is a lookup, not a guess.
    // Separators are folded because both spell the same path on Windows.
    std::string wanted{id};
    std::replace(wanted.begin(), wanted.end(), '\\', '/');
    for (const auto& resource : resources_) {
        if (strings::iequals(resource.path, wanted)) return resource;
    }
    return std::nullopt;
}

std::vector<std::string> Workspace::produced_binaries() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> paths;
    paths.reserve(resources_.size());
    for (const auto& resource : resources_) paths.push_back(resource.path);
    return paths;
}

std::string Workspace::to_relative(const std::filesystem::path& absolute) const {
    std::lock_guard lock(mutex_);
    if (root_.empty()) return path_to_utf8(absolute);
    std::error_code ec;
    auto relative = std::filesystem::relative(absolute, root_, ec);
    if (ec || relative.empty()) return path_to_utf8(absolute);
    return path_to_utf8(relative);
}

Result<std::vector<WorkspaceFile>> Workspace::list(const std::string& relative_directory) const {
    const auto base = root();
    if (base.empty()) return make_error(ErrorCode::NotFound, "no workspace is open");

    std::filesystem::path directory = base;
    if (!relative_directory.empty()) {
        auto decision = paths_.evaluate(relative_directory, PathAccess::Read);
        if (!decision.allowed) {
            return make_error(ErrorCode::PermissionDenied, decision.reason);
        }
        directory = decision.canonical;
    }

    auto entries = fs::list_directory(directory, /*include_hidden=*/true);
    if (!entries) return entries.error();

    std::vector<WorkspaceFile> files;
    files.reserve(entries->size());
    for (const auto& entry : *entries) {
        if (entry.is_directory) {
            std::lock_guard lock(mutex_);
            if (ignore_.ignores_directory(entry.name)) continue;
        }
        WorkspaceFile file;
        file.relative_path = relative_directory.empty()
                                 ? entry.name
                                 : relative_directory + "/" + entry.name;
        file.is_directory = entry.is_directory;
        file.size = entry.size;
        file.modified_unix_ms = entry.modified_unix_ms;
        files.push_back(std::move(file));
    }
    return files;
}

Result<std::vector<WorkspaceFile>> Workspace::all_files(std::size_t max_files) const {
    const auto base = root();
    if (base.empty()) return make_error(ErrorCode::NotFound, "no workspace is open");

    std::vector<WorkspaceFile> files;
    files.reserve(std::min<std::size_t>(max_files, 4096));

    fs::WalkOptions options;
    {
        std::lock_guard lock(mutex_);
        options.ignore = &ignore_;
    }
    options.max_entries = max_files * 4;

    auto status = fs::walk(base, options,
                           [&](const std::filesystem::path& relative, const fs::DirEntry& entry) {
                               if (files.size() >= max_files) return false;
                               WorkspaceFile file;
                               file.relative_path = path_to_utf8(relative);
                               file.size = entry.size;
                               file.modified_unix_ms = entry.modified_unix_ms;
                               files.push_back(std::move(file));
                               return true;
                           });
    if (!status) return status.error();
    return files;
}

Result<RepositoryMap> Workspace::build_repository_map(std::size_t max_files) const {
    const auto base = root();
    if (base.empty()) return make_error(ErrorCode::NotFound, "no workspace is open");

    auto files = all_files(max_files);
    if (!files) return files.error();

    RepositoryMap map;
    map.total_files = files->size();
    map.truncated = files->size() >= max_files;

    std::map<std::string, std::size_t> extensions;
    for (const auto& file : *files) {
        map.total_bytes += file.size;
        const auto dot = file.relative_path.find_last_of('.');
        const auto slash = file.relative_path.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            ++extensions[strings::to_lower(file.relative_path.substr(dot))];
        }
    }
    map.languages.assign(extensions.begin(), extensions.end());
    std::sort(map.languages.begin(), map.languages.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (map.languages.size() > 15) map.languages.resize(15);

    // Top-level entries orient the model faster than any listing of leaf files.
    auto top_level = list();
    if (top_level) {
        for (const auto& entry : *top_level) {
            map.top_level_entries.push_back(entry.relative_path +
                                            (entry.is_directory ? "/" : ""));
            if (entry.is_directory) ++map.total_directories;
        }
        std::sort(map.top_level_entries.begin(), map.top_level_entries.end());
    }

    std::error_code ec;
    for (const auto& [marker, system] : build_markers()) {
        const std::string pattern{marker};
        if (pattern.find('*') != std::string::npos) {
            for (const auto& file : *files) {
                if (file.relative_path.find('/') != std::string::npos) continue;
                if (strings::glob_match(pattern, file.relative_path)) {
                    map.key_files.push_back(file.relative_path);
                    if (map.detected_build_system.empty()) map.detected_build_system = system;
                    break;
                }
            }
            continue;
        }
        if (std::filesystem::is_regular_file(base / pattern, ec)) {
            map.key_files.push_back(pattern);
            if (map.detected_build_system.empty()) map.detected_build_system = system;
        }
    }
    for (const char* file : documentation_markers()) {
        if (std::filesystem::is_regular_file(base / file, ec)) map.key_files.emplace_back(file);
    }

    return map;
}

Status Workspace::snapshot_baseline(CancellationToken cancel, bool inspect_git) {
    const auto base = root();
    if (base.empty()) return make_error(ErrorCode::NotFound, "no workspace is open");

    std::set<std::string> dirty;
    std::string commit;

    if (inspect_git && git_.available()) {
        auto status = git_.status(base, cancel);
        if (status && status->is_repository) {
            commit = status->head_commit;
            for (const auto& entry : status->files) {
                if (entry.dirty() || entry.staged()) dirty.insert(entry.path);
            }
        }
    }

    std::lock_guard lock(mutex_);
    baseline_dirty_ = std::move(dirty);
    baseline_commit_ = std::move(commit);
    agent_changes_.clear();

    log_info(LogChannel::Agent, inspect_git ? "workspace baseline recorded"
                                            : "workspace baseline Git inspection skipped",
             {{"dirty_files", std::to_string(baseline_dirty_.size())},
              {"head", baseline_commit_}});
    return ok_status();
}

std::set<std::string> Workspace::baseline_dirty_files() const {
    std::lock_guard lock(mutex_);
    return baseline_dirty_;
}

std::string Workspace::baseline_commit() const {
    std::lock_guard lock(mutex_);
    return baseline_commit_;
}

void Workspace::record_agent_change(const std::filesystem::path& absolute_path,
                                    AgentChangeKind kind,
                                    const std::filesystem::path& from_path) {
    const auto relative = to_relative(absolute_path);

    std::lock_guard lock(mutex_);
    auto it = agent_changes_.find(relative);
    if (it != agent_changes_.end()) {
        ++it->second.edit_count;
        it->second.at_unix_ms = now_unix_ms();
        // A file created and then edited stays "created" from the user's view.
        if (it->second.kind != AgentChangeKind::Created) it->second.kind = kind;
        return;
    }

    AgentChange change;
    change.relative_path = relative;
    change.kind = kind;
    change.at_unix_ms = now_unix_ms();
    change.was_dirty_before = baseline_dirty_.count(relative) > 0;
    if (!from_path.empty()) {
        std::error_code ec;
        auto from_relative = std::filesystem::relative(from_path, root_, ec);
        change.from_path = path_to_utf8(ec ? from_path : from_relative);
    }

    if (change.was_dirty_before) {
        log_warn(LogChannel::Agent, "agent modified a file that already had uncommitted changes",
                 {{"path", relative}});
    }
    agent_changes_.emplace(relative, std::move(change));
}

std::vector<AgentChange> Workspace::agent_changes() const {
    std::lock_guard lock(mutex_);
    std::vector<AgentChange> changes;
    changes.reserve(agent_changes_.size());
    for (const auto& [path, change] : agent_changes_) changes.push_back(change);
    std::sort(changes.begin(), changes.end(), [](const AgentChange& a, const AgentChange& b) {
        return a.at_unix_ms < b.at_unix_ms;
    });
    return changes;
}

std::vector<AgentChange> Workspace::conflicting_changes() const {
    std::vector<AgentChange> conflicts;
    for (const auto& change : agent_changes()) {
        if (change.was_dirty_before) conflicts.push_back(change);
    }
    return conflicts;
}

void Workspace::clear_agent_changes() {
    std::lock_guard lock(mutex_);
    agent_changes_.clear();
}

Result<GitStatus> Workspace::git_status(CancellationToken cancel) {
    const auto base = root();
    if (base.empty()) return make_error(ErrorCode::NotFound, "no workspace is open");
    return git_.status(base, cancel);
}

bool Workspace::is_git_repository() {
    const auto base = root();
    if (base.empty() || !git_.available()) return false;
    return git_.repository_root(base).ok();
}

}  // namespace localcode
