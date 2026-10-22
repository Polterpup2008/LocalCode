#pragma once
// ---------------------------------------------------------------------------
// Git integration.
//
// Read-only by design. LocalCode shows the user what changed and never rewrites
// history on its own: no reset --hard, no clean -fd, no force push. Those are
// classified by CommandPolicy and require a human, because the one thing worse
// than an agent that cannot finish a task is an agent that deletes the work
// somebody did by hand this morning.
//
// git is invoked as a subprocess with the porcelain formats, which are
// documented as stable for machine consumption. That avoids linking libgit2 for
// what is fundamentally a handful of parsers.
// ---------------------------------------------------------------------------
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../util/Cancellation.h"
#include "../util/Result.h"

namespace localcode {

enum class GitChange {
    Unmodified,
    Modified,
    Added,
    Deleted,
    Renamed,
    Copied,
    Untracked,
    Ignored,
    Conflicted,
};

std::string_view to_string(GitChange change) noexcept;

struct GitFileStatus {
    std::string path;             // repository-relative, forward slashes
    std::string original_path;    // set for renames and copies
    GitChange   index_change = GitChange::Unmodified;      // staged
    GitChange   worktree_change = GitChange::Unmodified;   // unstaged
    bool        untracked = false;
    bool        conflicted = false;

    [[nodiscard]] bool staged() const { return index_change != GitChange::Unmodified; }
    [[nodiscard]] bool dirty() const {
        return worktree_change != GitChange::Unmodified || untracked || conflicted;
    }
    [[nodiscard]] std::string describe() const;
};

struct GitStatus {
    bool        is_repository = false;
    std::filesystem::path root;
    std::string branch;
    std::string upstream;
    std::string head_commit;
    int         ahead = 0;
    int         behind = 0;
    bool        detached_head = false;
    bool        clean = true;
    std::vector<GitFileStatus> files;

    [[nodiscard]] const GitFileStatus* file(std::string_view path) const;
    [[nodiscard]] std::vector<std::string> dirty_paths() const;
};

struct GitCommit {
    std::string hash;
    std::string short_hash;
    std::string author_name;
    std::string author_email;
    std::int64_t timestamp_unix = 0;
    std::string subject;
    std::string body;
    std::vector<std::string> parents;
};

struct GitBranch {
    std::string name;
    bool        current = false;
    bool        remote = false;
    std::string upstream;
    std::string head_commit;
};

// One file's worth of a unified diff, already split up for the diff viewer.
struct DiffHunk {
    int old_start = 0, old_count = 0;
    int new_start = 0, new_count = 0;
    std::string header;
    std::vector<std::string> lines;   // including the leading ' ', '+' or '-'
};

struct FileDiff {
    std::string path;
    std::string old_path;
    GitChange   change = GitChange::Modified;
    bool        binary = false;
    int         additions = 0;
    int         deletions = 0;
    std::vector<DiffHunk> hunks;
};

struct DiffResult {
    std::vector<FileDiff> files;
    std::string           raw;      // the unified diff as git produced it
    int                   total_additions = 0;
    int                   total_deletions = 0;
};

class GitService {
public:
    GitService() = default;

    // Locates the git executable once. Returns false when git is not installed,
    // which is a normal condition, not an error - the rest of the application
    // works without it, only the git panel is disabled.
    bool available();
    [[nodiscard]] std::filesystem::path git_path() const { return git_; }
    [[nodiscard]] std::string version();

    // The repository root containing `path`, if any.
    Result<std::filesystem::path> repository_root(const std::filesystem::path& path);

    Result<GitStatus> status(const std::filesystem::path& repository,
                             CancellationToken cancel = {});

    // `paths` empty means the whole tree. `staged` selects --cached.
    Result<DiffResult> diff(const std::filesystem::path& repository,
                            const std::vector<std::string>& paths = {}, bool staged = false,
                            int context_lines = 3, CancellationToken cancel = {});

    // Diff of one file against a revision; used by the diff viewer to show what
    // the agent changed relative to where it started.
    Result<DiffResult> diff_against(const std::filesystem::path& repository,
                                    const std::string& revision,
                                    const std::vector<std::string>& paths = {},
                                    CancellationToken cancel = {});

    Result<std::vector<GitCommit>> log(const std::filesystem::path& repository, int max_count = 30,
                                       const std::string& path_filter = {},
                                       CancellationToken cancel = {});

    Result<std::vector<GitBranch>> branches(const std::filesystem::path& repository,
                                            bool include_remote = false,
                                            CancellationToken cancel = {});

    // File contents at a revision, for side-by-side diffs of tracked files.
    Result<std::string> show_file(const std::filesystem::path& repository,
                                  const std::string& revision, const std::string& path,
                                  CancellationToken cancel = {});

    // The current HEAD commit hash, used to anchor "what changed during this task".
    Result<std::string> head_commit(const std::filesystem::path& repository,
                                    CancellationToken cancel = {});

    // --- Parsers, exposed for testing ------------------------------------
    static GitStatus parse_status_porcelain_v2(std::string_view text);
    static DiffResult parse_unified_diff(std::string_view text);
    static std::vector<GitCommit> parse_log(std::string_view text);

private:
    Result<std::string> run(const std::filesystem::path& repository,
                            std::vector<std::string> arguments, CancellationToken cancel,
                            std::chrono::milliseconds timeout = std::chrono::seconds(60));

    std::filesystem::path git_;
    bool                  looked_up_ = false;
    std::string           version_;
};

}  // namespace localcode
