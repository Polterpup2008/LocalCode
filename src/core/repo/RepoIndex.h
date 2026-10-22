#pragma once
// ---------------------------------------------------------------------------
// The index, kept up to date.
//
// Opening a workspace must not wait for a full scan, and a scan must not redo
// work it already did. So: the stored index is available the moment the store
// opens, and a refresh reads the tree, compares what it finds against what is
// on record, and touches only the files that actually changed.
//
// A file counts as unchanged when its size, modification time and content
// fingerprint all match. Size and time alone are not enough - an editor that
// rewrites a file to the same length is ordinary - and the fingerprint alone
// would mean reading every byte of every file on every refresh, which is the
// cost this exists to avoid.
//
// Deletion is the case that is easiest to get wrong and worst to get wrong: a
// symbol that still answers queries after its file is gone sends the agent to
// open something that does not exist. Anything not seen during a refresh is
// removed, along with everything indexed from it.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "RepoScanner.h"
#include "RepoStore.h"
#include "RepoTypes.h"
#include "util/Cancellation.h"
#include "util/Result.h"

namespace localcode::repo {

enum class IndexState {
    Empty,      // nothing has been indexed yet
    Indexing,
    Ready,
    Stale,      // files are known to have changed since the last pass
    Failed,
};

[[nodiscard]] std::string_view to_string(IndexState state) noexcept;

struct IndexStats {
    std::size_t  files_scanned = 0;
    std::size_t  files_reindexed = 0;
    std::size_t  files_added = 0;
    std::size_t  files_removed = 0;
    std::size_t  files_unchanged = 0;
    std::size_t  symbols = 0;
    std::size_t  facts_invalidated = 0;
    std::int64_t duration_ms = 0;
    bool         cancelled = false;
    bool         full = false;
};

struct IndexProgress {
    IndexState  state = IndexState::Empty;
    std::size_t files_seen = 0;
    std::size_t files_indexed = 0;
    std::string current_path;
};

class RepoIndex {
public:
    RepoIndex();
    ~RepoIndex();

    // Opens the stored index for a workspace. Cheap: it does not scan.
    Status open(const std::filesystem::path& index_directory,
                const std::filesystem::path& workspace_root);
    void   close();
    [[nodiscard]] bool is_open() const;

    [[nodiscard]] IndexState state() const;
    void invalidate();
    [[nodiscard]] std::filesystem::path workspace_root() const;

    // Walks the tree and brings the index into line with it. With `full`, every
    // file is reread and reparsed whatever its fingerprint says - the escape
    // hatch for when the indexer itself has changed.
    Result<IndexStats> refresh(const fs::IgnoreRules& ignore, CancellationToken cancel,
                               bool full = false,
                               const std::function<void(const IndexProgress&)>& on_progress = {});

    [[nodiscard]] RepoStore&       store();
    [[nodiscard]] const RepoStore& store() const;

    [[nodiscard]] Result<std::optional<RepoOverview>> overview() const;
    [[nodiscard]] IndexStats last_stats() const;
    [[nodiscard]] ScanLimits limits() const;
    void set_limits(ScanLimits limits);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace localcode::repo
