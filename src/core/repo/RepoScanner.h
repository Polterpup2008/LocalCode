#pragma once
// ---------------------------------------------------------------------------
// Walking a repository once and writing down what is there.
//
// Bounded on purpose. A repository can contain a checked-in node_modules, a
// build directory with fifty thousand object files, or a single generated
// header a hundred megabytes long, and an indexer that treats those as source
// has spent its time on the least useful bytes in the tree. The limits are
// explicit, and when one of them stops the walk the result says so rather than
// reporting a partial count as a total.
//
// It does not escape the workspace. Symlinks pointing outside the root are not
// followed, because a repository that links to C:\ is not an invitation to
// index C:\.
// ---------------------------------------------------------------------------
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "RepoTypes.h"
#include "platform/FileSystem.h"
#include "util/Cancellation.h"
#include "util/Result.h"

namespace localcode::repo {

struct ScanLimits {
    std::size_t   max_files = 40000;
    // Files larger than this are recorded but not read. A minified bundle or a
    // generated table teaches the index nothing and costs the most to parse.
    std::uint64_t max_file_bytes = 2ull * 1024 * 1024;
    std::size_t   max_depth = 24;
};

struct ScanProgress {
    std::size_t files_seen = 0;
    std::size_t files_indexed = 0;
    std::string current_path;
};

struct ScanResult {
    std::vector<RepoFile>    files;
    std::vector<RepoCommand> commands;
    RepoOverview             overview;
    bool                     cancelled = false;
};

// Walks `root`, applying `ignore` plus the built-in generated and vendored
// directory rules. `on_progress` is called on the scanning thread, often; it
// must be cheap and must not block.
[[nodiscard]] Result<ScanResult> scan_repository(
    const std::filesystem::path& root, const fs::IgnoreRules& ignore, const ScanLimits& limits,
    CancellationToken cancel, const std::function<void(const ScanProgress&)>& on_progress = {});

// Reads build and test commands out of the manifests that were found. Exposed
// separately so it can be tested against a fixture without a filesystem walk.
[[nodiscard]] std::vector<RepoCommand> discover_commands(const std::filesystem::path& root,
                                                         const std::vector<RepoFile>& files);

}  // namespace localcode::repo
