#include "RepoIndex.h"
#include "platform/PathEncoding.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "Hashing.h"
#include "Languages.h"
#include "SymbolIndexer.h"
#include "platform/FileSystem.h"
#include "util/Log.h"

namespace localcode::repo {
namespace {

// Collapses "." and ".." without touching the disk, and keeps the result
// inside the repository: a target that climbs out is not ours to resolve.
std::string normalise_relative(std::string_view text) {
    std::vector<std::string> parts;
    std::string              part;
    for (const char character : text) {
        if (character == '/' || character == '\\') {
            if (!part.empty()) {
                if (part == "..") {
                    if (parts.empty()) return {};   // escapes the root
                    parts.pop_back();
                } else if (part != ".") {
                    parts.push_back(part);
                }
                part.clear();
            }
            continue;
        }
        part.push_back(character);
    }
    if (!part.empty()) {
        if (part == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
        } else if (part != ".") {
            parts.push_back(part);
        }
    }

    std::string out;
    for (const auto& piece : parts) {
        if (!out.empty()) out += '/';
        out += piece;
    }
    return out;
}

// An include is written relative to the file that writes it, so the indexer -
// which reads one file at a time and knows nothing of the tree - stores it
// verbatim. That makes the common case invisible: main.cpp and widget.cpp both
// say `#include "widget.h"`, and a query for src/widget.h matched neither,
// while tests/test_widget.cpp matched only because "../src/widget.h" happens to
// contain a slash. "What includes this header" answered with the one file that
// wrote the path the long way round.
//
// So targets are resolved here, where the whole file list is in hand. Anything
// that does not land on an indexed file is left exactly as written: a system or
// dependency header is not part of this repository and inventing a path for it
// would be worse than leaving it raw.
void resolve_include_targets(std::vector<RepoReference>& references,
                             const std::string&          from_file,
                             const std::unordered_set<std::string>& indexed_paths) {
    const auto slash = from_file.find_last_of('/');
    const std::string directory = slash == std::string::npos ? std::string{}
                                                             : from_file.substr(0, slash);

    for (auto& reference : references) {
        if (reference.kind != ReferenceKind::Include) continue;
        if (reference.to_file.empty()) continue;

        // Relative to the including file first: that is what the compiler does
        // for a quoted include, and it is the case that was being missed.
        if (!directory.empty()) {
            const auto candidate = normalise_relative(directory + "/" + reference.to_file);
            if (!candidate.empty() && indexed_paths.count(candidate) != 0) {
                reference.to_file = candidate;
                continue;
            }
        }
        // Then as a path from the repository root, which is how an include
        // directory on the command line usually resolves.
        const auto from_root = normalise_relative(reference.to_file);
        if (!from_root.empty() && indexed_paths.count(from_root) != 0) {
            reference.to_file = from_root;
            continue;
        }
        // Otherwise leave it. It is external, or generated, or we cannot tell -
        // and a wrong path in the index is worse than an unresolved one.
    }
}

}  // namespace

std::string_view to_string(IndexState state) noexcept {
    switch (state) {
        case IndexState::Empty:    return "empty";
        case IndexState::Indexing: return "indexing";
        case IndexState::Ready:    return "ready";
        case IndexState::Stale:    return "stale";
        case IndexState::Failed:   return "failed";
    }
    return "empty";
}

struct RepoIndex::Impl {
    mutable std::mutex    mutex;
    std::mutex operation_mutex;
    RepoStore             store;
    std::filesystem::path root;
    IndexState            state = IndexState::Empty;
    IndexStats            last;
    ScanLimits            limits;
    std::int64_t          generation = 0;
    std::uint64_t invalidations = 0;
};

RepoIndex::RepoIndex() : impl_(std::make_unique<Impl>()) {}
RepoIndex::~RepoIndex() = default;

Status RepoIndex::open(const std::filesystem::path& index_directory,
                       const std::filesystem::path& workspace_root) {
    std::lock_guard operation(impl_->operation_mutex);
    std::lock_guard lock(impl_->mutex);
    impl_->root = workspace_root;
    if (auto status = impl_->store.open(index_directory, workspace_root); !status) {
        impl_->state = IndexState::Failed;
        return status;
    }

    auto overview = impl_->store.overview();
    if (overview && overview->has_value()) {
        // A complete persisted scan says what was true when the app closed.
        // Files can change while no watcher is alive; validate them before
        // exposing cached symbols and line leads as current evidence.
        impl_->state = IndexState::Stale;
        impl_->generation = (*overview)->generation;
    } else {
        impl_->state = IndexState::Empty;
        impl_->generation = 0;
    }
    return ok_status();
}

void RepoIndex::close() {
    std::lock_guard operation(impl_->operation_mutex);
    std::lock_guard lock(impl_->mutex);
    impl_->store.close();
    impl_->state = IndexState::Empty;
}

bool RepoIndex::is_open() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->store.is_open();
}

IndexState RepoIndex::state() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state;
}

void RepoIndex::invalidate() {
    std::lock_guard lock(impl_->mutex);
    ++impl_->invalidations;
    if (impl_->state == IndexState::Ready) impl_->state = IndexState::Stale;
    if (auto cached = impl_->store.overview(); cached && cached->has_value()) {
        auto incomplete = **cached;
        incomplete.truncated = true;
        if (!impl_->store.put_overview(incomplete)) impl_->state = IndexState::Failed;
    }
}

std::filesystem::path RepoIndex::workspace_root() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->root;
}

RepoStore& RepoIndex::store() { return impl_->store; }
const RepoStore& RepoIndex::store() const { return impl_->store; }

IndexStats RepoIndex::last_stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->last;
}

ScanLimits RepoIndex::limits() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->limits;
}
void RepoIndex::set_limits(ScanLimits limits) {
    std::lock_guard lock(impl_->mutex);
    impl_->limits = limits;
}

Result<std::optional<RepoOverview>> RepoIndex::overview() const {
    return impl_->store.overview();
}

Result<IndexStats> RepoIndex::refresh(const fs::IgnoreRules& ignore, CancellationToken cancel,
                                      bool full,
                                      const std::function<void(const IndexProgress&)>& on_progress) {
    std::unique_lock operation(impl_->operation_mutex, std::try_to_lock);
    if (!operation.owns_lock()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a repository index operation is already running");
    }
    std::filesystem::path root;
    ScanLimits limits;
    std::int64_t generation = 0;
    std::uint64_t invalidations = 0;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->store.is_open()) {
            return make_error(ErrorCode::InvalidArgument, "the repository index is not open");
        }
        impl_->state = IndexState::Indexing;
        root = impl_->root;
        limits = impl_->limits;
        generation = ++impl_->generation;
        invalidations = impl_->invalidations;
    }

    const auto started = std::chrono::steady_clock::now();
    IndexStats stats;
    stats.full = full;

    IndexProgress progress;
    progress.state = IndexState::Indexing;
    bool observer_enabled = true;
    const auto report = [&] {
        if (!on_progress || !observer_enabled) return;
        try {
            on_progress(progress);
        } catch (...) {
            observer_enabled = false;
            log_error(LogChannel::App,
                      "repository progress observer threw; disabled for this refresh");
        }
    };
    const auto fail = [&](const Error& error) -> Result<IndexStats> {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->state = IndexState::Failed;
        }
        progress.state = IndexState::Failed;
        report();
        return error;
    };

    // Persist incompleteness before writes. A crash or failed refresh must not
    // reopen yesterday's summary as a fully refreshed index.
    auto prior_overview = impl_->store.overview();
    if (!prior_overview) return fail(prior_overview.error());
    if (prior_overview->has_value()) {
        auto incomplete = **prior_overview;
        incomplete.truncated = true;
        if (auto status = impl_->store.put_overview(incomplete); !status)
            return fail(status.error());
    }
    report();

    auto scanned = scan_repository(
        root, ignore, limits, cancel, [&](const ScanProgress& scan) {
            progress.files_seen = scan.files_seen;
            progress.files_indexed = scan.files_indexed;
            progress.current_path = scan.current_path;
            report();
        });
    if (!scanned) {
        return fail(scanned.error());
    }

    stats.files_scanned = scanned->files.size();
    stats.cancelled = scanned->cancelled;

    // What is on record now, so the comparison is one pass rather than a query
    // per file.
    std::unordered_map<std::string, RepoFile> known;
    auto existing = impl_->store.files();
    if (!existing) return fail(existing.error());
    for (auto& file : *existing)
        known.emplace(file.path, std::move(file));

    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> changed_paths;
    seen.reserve(scanned->files.size());

    // Every path in the tree, known before any file is parsed, so an include
    // can be resolved against the whole repository rather than against whatever
    // happens to have been indexed already.
    std::unordered_set<std::string> indexed_paths;
    indexed_paths.reserve(scanned->files.size());
    for (const auto& file : scanned->files) indexed_paths.insert(file.path);
    const bool topology_changed =
        indexed_paths.size() != known.size() ||
        std::any_of(indexed_paths.begin(), indexed_paths.end(),
                    [&](const std::string& path) { return !known.contains(path); });

    for (auto& file : scanned->files) {
        if (cancel.cancelled()) {
            stats.cancelled = true;
            break;
        }
        seen.insert(file.path);

        const auto previous = known.find(file.path);
        const bool is_new = previous == known.end();
        // Size and time first because they are free; the fingerprint decides,
        // because an editor rewriting a file to the same length is ordinary.
        //
        // A file that was skipped has no fingerprint to compare - nothing read
        // it. Requiring one anyway made every binary and every oversized file
        // look changed on every pass, which is how a warm refresh of this
        // repository reindexed twenty-one files that nobody had touched.
        const bool same_stat = !is_new && previous->second.size_bytes == file.size_bytes &&
                               previous->second.modified_unix_ms == file.modified_unix_ms;
        const bool unchanged =
            !full && same_stat &&
            (file.skipped ? previous->second.skipped &&
                                previous->second.skip_reason == file.skip_reason
                          : !file.content_hash.empty() &&
                                previous->second.content_hash == file.content_hash);
        if (unchanged) {
            ++stats.files_unchanged;
            // Includes depend on the target tree as well as this file's bytes.
            // Re-resolve existing edges when a target is added, deleted or renamed.
            if (topology_changed && !file.skipped) {
                auto refs = impl_->store.references_from_file(file.path);
                if (!refs) return fail(refs.error());
                if (std::any_of(refs->begin(), refs->end(), [](const RepoReference& r) {
                        return r.kind == ReferenceKind::Include;
                    })) {
                    auto contents = fs::read_bytes(root / path_from_utf8(file.path), limits.max_file_bytes);
                    if (!contents) return fail(contents.error());
                    auto indexed = index_source(file.path, file.language, *contents);
                    resolve_include_targets(indexed.references, file.path, indexed_paths);
                    if (auto status =
                            impl_->store.replace_references(file.path, indexed.references);
                        !status)
                        return fail(status.error());
                }
            }
            continue;
        }

        if (is_new) ++stats.files_added; else ++stats.files_reindexed;
        if (is_new || !same_stat || previous->second.content_hash != file.content_hash ||
            previous->second.skipped != file.skipped ||
            previous->second.skip_reason != file.skip_reason) {
            changed_paths.insert(file.path);
        }

        if (file.skipped) {
            // Still clear anything indexed from a previous version of it: a
            // file that has become too large to read must not keep answering
            // with what it used to contain.
            if (auto status = impl_->store.replace_symbols(file.path, {}); !status)
                return fail(status.error());
            if (auto status = impl_->store.replace_references(file.path, {}); !status)
                return fail(status.error());
            if (auto status = impl_->store.put_file(file); !status) return fail(status.error());
            continue;
        }

        auto contents = fs::read_bytes(root / path_from_utf8(file.path), limits.max_file_bytes);
        if (!contents) {
            if (auto status = impl_->store.replace_symbols(file.path, {}); !status)
                return fail(status.error());
            if (auto status = impl_->store.replace_references(file.path, {}); !status)
                return fail(status.error());
            return fail(contents.error());
        }

        auto indexed = index_source(file.path, file.language, *contents);
        resolve_include_targets(indexed.references, file.path, indexed_paths);
        if (auto status = impl_->store.replace_symbols(file.path, indexed.symbols); !status)
            return fail(status.error());
        if (auto status = impl_->store.replace_references(file.path, indexed.references); !status)
            return fail(status.error());
        // Commit the fingerprint last. A failed symbol/reference write must
        // leave the old fingerprint so that the next refresh retries this file.
        if (auto status = impl_->store.put_file(file); !status) return fail(status.error());
    }

    // Anything that was on record and is no longer in the tree goes, together
    // with its symbols and references.
    if (!stats.cancelled && !scanned->overview.truncated) {
        for (const auto& [path, _] : known) {
            if (seen.count(path) != 0) continue;
            if (auto status = impl_->store.remove_file(path); status) {
                ++stats.files_removed;
                changed_paths.insert(path);
            } else
                return fail(status.error());
        }
    }

    if (!stats.cancelled && !scanned->overview.truncated) {
        if (auto status = impl_->store.replace_commands(scanned->commands, !changed_paths.empty());
            !status) {
            return fail(status.error());
        }
    }

    // A fact stands on the files it was derived from. When one of those changes
    // the fact is no longer established, and keeping it would be exactly the
    // stale phantom this design exists to prevent.
    auto facts = impl_->store.facts();
    if (!facts) return fail(facts.error());
    {
        for (const auto& fact : *facts) {
            const bool depends_on_changed =
                std::any_of(fact.depends_on.begin(), fact.depends_on.end(),
                            [&](const std::string& path) { return changed_paths.count(path) != 0; });
            if (!depends_on_changed) continue;
            if (auto status = impl_->store.remove_fact(fact.key); !status)
                return fail(status.error());
            ++stats.facts_invalidated;
        }
    }

    auto overview = scanned->overview;
    overview.truncated = overview.truncated || stats.cancelled;
    overview.generation = generation;
    auto count = impl_->store.symbol_count();
    if (!count) return fail(count.error());
    overview.symbol_count = *count;
    stats.symbols = overview.symbol_count;

    stats.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();

    {
        std::unique_lock lock(impl_->mutex);
        overview.truncated = overview.truncated || invalidations != impl_->invalidations;
        if (auto status = impl_->store.put_overview(overview); !status) {
            lock.unlock();
            return fail(status.error());
        }
        impl_->state = overview.truncated ? IndexState::Stale : IndexState::Ready;
        impl_->last = stats;
        progress.state = impl_->state;
    }
    progress.current_path.clear();
    report();

    log_info(LogChannel::App, "repository index refreshed",
             {{"files", std::to_string(stats.files_scanned)},
              {"added", std::to_string(stats.files_added)},
              {"reindexed", std::to_string(stats.files_reindexed)},
              {"removed", std::to_string(stats.files_removed)},
              {"unchanged", std::to_string(stats.files_unchanged)},
              {"symbols", std::to_string(stats.symbols)},
              {"ms", std::to_string(stats.duration_ms)},
              {"cancelled", stats.cancelled ? "yes" : "no"}});
    return stats;
}

}  // namespace localcode::repo
