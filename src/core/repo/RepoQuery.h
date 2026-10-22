#pragma once
// ---------------------------------------------------------------------------
// Asking the index questions.
//
// One layer, used by the agent's tools and by the interface, so both get the
// same answers. Two rules shape it:
//
// **Ranges, not files.** Handing back a whole file because one line in it
// matched is how a context window fills with material nobody needed. Results
// carry the lines that matter and the numbers to find the rest.
//
// **Every result says why it was chosen.** "Matched the symbol name exactly",
// "the file imports this", "named in the build target" - a ranked list with no
// stated reason is a list the agent has to take on faith, and it will.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "RepoStore.h"
#include "RepoTypes.h"
#include "util/Result.h"

namespace localcode::repo {

// A span of a file worth showing, with enough around it to read.
struct CodeExcerpt {
    std::string file;
    std::size_t first_line = 0;   // 1-based, inclusive
    std::size_t last_line = 0;
    std::string text;
};

struct SearchHit {
    std::string file;
    std::size_t line = 0;
    std::string preview;   // the matching line, trimmed
    // Why this is in the list, in words meant to be read.
    std::string reason;
    Evidence evidence = Evidence::Lexical;
    double score = 0.0;
    // Bounded lexical navigation spans, not verified function boundaries.
    std::size_t end_line = 0;
    std::vector<std::pair<std::size_t, std::size_t>> related_ranges;
};

// What a search actually managed to cover. A result list that stopped early
// looks exactly like a complete one, and an agent told "3 matches" when the
// search gave up two thirds of the way through the repository will conclude
// there are three.
struct SearchStats {
    std::size_t files_considered = 0;
    std::size_t files_read = 0;
    bool hit_file_limit = false;
    bool hit_match_limit = false;

    [[nodiscard]] bool complete() const { return !hit_file_limit && !hit_match_limit; }
};

struct SearchOptions {
    std::size_t max_hits = 20;
    std::size_t max_files = 200;
    bool regex = false;
    bool case_sensitive = false;
    // Restrict to files whose path contains this, when set.
    std::string path_contains;
    // Skip tests, generated and vendored files unless asked for.
    bool include_tests = true;
    bool include_non_source = true;
};

class RepoQuery {
public:
    RepoQuery(const RepoStore& store, std::filesystem::path root);

    // --- Files ------------------------------------------------------------
    // Paths whose name or directory matches. Ranked: an exact file name first,
    // then a path segment, then anything containing it.
    [[nodiscard]] Result<std::vector<SearchHit>> find_files(std::string_view fragment,
                                                            std::size_t limit = 20) const;
    // Compact initial leads for a task, ranked from indexed paths and symbols.
    // Does not scan file contents or treat a lexical lead as verified evidence.
    [[nodiscard]] Result<std::vector<SearchHit>> task_sources(std::string_view objective,
                                                              std::size_t limit = 6) const;

    // --- Text -------------------------------------------------------------
    // Searches the indexed files' contents. Reads from disk: the index records
    // where files are and what is in them structurally, not every line of every
    // one, and keeping a second copy of the tree in SQLite to avoid a read that
    // takes microseconds would be a poor trade.
    [[nodiscard]] Result<std::vector<SearchHit>> search_text(std::string_view needle,
                                                             const SearchOptions& options = {},
                                                             SearchStats* stats = nullptr) const;

    // --- Symbols ----------------------------------------------------------
    [[nodiscard]] Result<std::vector<RepoSymbol>> find_symbol(std::string_view name,
                                                              std::size_t limit = 20) const;

    // Files that name this symbol, other than where it is defined.
    [[nodiscard]] Result<std::vector<SearchHit>> find_references(std::string_view symbol,
                                                                 std::size_t limit = 40) const;

    // --- Relationships ----------------------------------------------------
    // Test files that appear to exercise `path`, by include, by name, or by
    // naming a symbol it defines. Each hit says which of those it was.
    [[nodiscard]] Result<std::vector<SearchHit>> tests_for(std::string_view path,
                                                           std::size_t limit = 20) const;

    // --- Excerpts ---------------------------------------------------------
    [[nodiscard]] Result<CodeExcerpt> excerpt(std::string_view path, std::size_t around_line,
                                              std::size_t context = 8) const;

private:
    [[nodiscard]] Result<std::string> read_file(std::string_view relative,
                                                std::size_t max_bytes = 2 * 1024 * 1024) const;

    const RepoStore* store_;
    std::filesystem::path root_;
    // The canonical root, resolved once. Every file read checks that it is
    // still underneath this, and recomputing it per file made a search of a
    // few hundred files hundreds of filesystem calls slower than it needed to
    // be. The root cannot move while a query object is alive.
    std::filesystem::path canonical_root_;
};

}   // namespace localcode::repo
