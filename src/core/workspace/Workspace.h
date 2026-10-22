#pragma once
// ---------------------------------------------------------------------------
// The selected project.
//
// A workspace bundles everything that is scoped to one directory: the path and
// command policies, the ignore rules, the git view, and - importantly - the
// record of which files the agent itself touched.
//
// That last part matters. Before an agent run starts, the workspace snapshots
// which files were already dirty. Afterwards it can tell the user "the agent
// changed these four files, and these two were already modified before it
// started", instead of presenting one undifferentiated pile of changes and
// hoping nobody loses work.
// ---------------------------------------------------------------------------
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../git/GitService.h"
#include "../platform/FileSystem.h"
#include "../security/CommandPolicy.h"
#include "../security/PathPolicy.h"
#include "../util/Result.h"

namespace localcode {

enum class AgentChangeKind { Created, Modified, Deleted, Moved };

std::string_view to_string(AgentChangeKind kind) noexcept;

struct AgentChange {
    std::string     relative_path;
    AgentChangeKind kind = AgentChangeKind::Modified;
    std::string     from_path;          // for moves
    std::int64_t    at_unix_ms = 0;
    // Whether this file was already modified before the agent touched it. When
    // true, the UI warns that the agent edited work that was not committed.
    bool            was_dirty_before = false;
    std::size_t     edit_count = 1;
};

struct WorkspaceFile {
    std::string   relative_path;
    std::uint64_t size = 0;
    std::int64_t  modified_unix_ms = 0;
    bool          is_directory = false;
};

struct RepositoryMap {
    std::size_t total_files = 0;
    std::size_t total_directories = 0;
    std::uint64_t total_bytes = 0;
    bool        truncated = false;
    // Extension -> count, most common first.
    std::vector<std::pair<std::string, std::size_t>> languages;
    // Files that identify the project's build system and layout.
    std::vector<std::string> key_files;
    std::vector<std::string> top_level_entries;
    std::string detected_build_system;
};

// A fact LocalCode has already established, addressable by name so nobody has
// to rebuild it from prose.
//
// The first and most valuable kind is the executable a build produced. On
// Windows that is an absolute path full of backslashes, and putting one into a
// JSON string is where four measured runs died: \f became a form feed, \U was
// not a valid escape at all, and the program looked missing when it was
// sitting exactly where the build had left it. The path is known the moment
// the build prints it, so the model should never have to type it.
struct ResolvedResource {
    std::string   id;        // "build:fibonacci" - what the model refers to
    std::string   kind;      // "executable"
    std::string   name;      // "fibonacci"
    std::string   path;      // workspace-relative, forward slashes, always
    std::string   source;    // "MSBuild link line", "Ninja link line"
    std::int64_t  recorded_at_unix_ms = 0;
    // The workspace revision when this was recorded, so a resource can be
    // reported as possibly stale rather than silently trusted.
    std::string   source_revision;
};

class Workspace {
public:
    Workspace();

    // Opens a directory as the workspace. Fails if it does not exist.
    Status open(const std::filesystem::path& root);
    void   close();

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::filesystem::path root() const;
    [[nodiscard]] std::string display_name() const;

    [[nodiscard]] PathPolicy&    paths() { return paths_; }
    [[nodiscard]] const PathPolicy& paths() const { return paths_; }
    [[nodiscard]] CommandPolicy& commands() { return commands_; }
    [[nodiscard]] const CommandPolicy& commands() const { return commands_; }
    [[nodiscard]] fs::IgnoreRules& ignore_rules() { return ignore_; }
    [[nodiscard]] GitService&    git() { return git_; }

    // --- File listing -----------------------------------------------------
    // Immediate children of a directory. Cheap; safe to call from the UI thread
    // for one level at a time, which is how the tree view expands.
    Result<std::vector<WorkspaceFile>> list(const std::string& relative_directory = {}) const;

    // Whole-tree walk with the ignore rules applied. Can be slow on a large
    // repository and must be run off the UI thread.
    Result<std::vector<WorkspaceFile>> all_files(std::size_t max_files = 50000) const;

    // A summary of the project used to orient the agent at the start of a task.
    Result<RepositoryMap> build_repository_map(std::size_t max_files = 50000) const;

    // --- Change tracking --------------------------------------------------
    // Records which files are already modified, so agent edits can be told
    // apart from the user's own uncommitted work.
    Status snapshot_baseline(CancellationToken cancel = {}, bool inspect_git = true);
    [[nodiscard]] std::set<std::string> baseline_dirty_files() const;
    [[nodiscard]] std::string baseline_commit() const;

    void record_agent_change(const std::filesystem::path& absolute_path, AgentChangeKind kind,
                             const std::filesystem::path& from_path = {});
    [[nodiscard]] std::vector<AgentChange> agent_changes() const;
    void clear_agent_changes();

    // Files changed by the agent that were already dirty when it started.
    [[nodiscard]] std::vector<AgentChange> conflicting_changes() const;

    // --- Git --------------------------------------------------------------
    Result<GitStatus> git_status(CancellationToken cancel = {});
    [[nodiscard]] bool is_git_repository();

    // --- Resolved resources -----------------------------------------------
    //
    // See ResolvedResource above.
    //
    // Things LocalCode has already worked out and should not make the model
    // work out again. The executables a build produced are the first and most
    // useful: a model that has just built something has to name the result to
    // run it, and on Windows that means an absolute path full of backslashes
    // going into a JSON string, which is where several runs died.
    //
    // Held per workspace, replaced by each successful build, and offered back
    // when a command cannot find the program it was looking for. Addressable
    // by id, so the model can run one without spelling its path at all.
    void record_resolved_resources(std::vector<ResolvedResource> resources);
    [[nodiscard]] std::vector<ResolvedResource> resolved_resources() const;
    [[nodiscard]] std::optional<ResolvedResource> resolved_resource(std::string_view id) const;
    // Just the paths, in recorded order - the common case.
    [[nodiscard]] std::vector<std::string> produced_binaries() const;

    // --- Relative paths ---------------------------------------------------
    [[nodiscard]] std::string to_relative(const std::filesystem::path& absolute) const;

private:
    mutable std::mutex     mutex_;
    std::filesystem::path  root_;
    PathPolicy             paths_;
    CommandPolicy          commands_;
    fs::IgnoreRules        ignore_;
    GitService             git_;

    std::set<std::string>              baseline_dirty_;
    std::string                        baseline_commit_;
    std::map<std::string, AgentChange> agent_changes_;
    std::vector<ResolvedResource>      resources_;
};

}  // namespace localcode
