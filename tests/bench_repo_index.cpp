// ---------------------------------------------------------------------------
// Indexing this repository, for real, and timing it.
//
// Tagged [.bench] so it does not run in the ordinary suite: it walks a real
// tree, and a test that takes seconds on somebody else's machine is a test they
// will start skipping. Run it with:
//
//     localcode_tests.exe "[bench]"
//
// It measures rather than asserts, except for the one thing worth failing on:
// a query that is no longer effectively immediate.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>

#include "repo/RepoIndex.h"
#include "support/TempDirectory.h"

using namespace localcode;
using namespace localcode::repo;
using localcode::test::TempDirectory;

namespace {

std::int64_t ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

std::int64_t us_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

}  // namespace

TEST_CASE("indexing this repository", "[bench][.bench]") {
    const std::filesystem::path root = LOCALCODE_TEST_SOURCE_DIR;
    TempDirectory index_dir;

    fs::IgnoreRules ignore;
    ignore.load_gitignore(root / ".gitignore");

    RepoIndex index;
    REQUIRE(index.open(index_dir.path(), root));
    CancellationSource cancel;

    // --- cold -------------------------------------------------------------
    auto start = std::chrono::steady_clock::now();
    auto cold = index.refresh(ignore, cancel.token());
    REQUIRE(cold);
    const auto cold_ms = ms_since(start);

    // --- warm: nothing changed -------------------------------------------
    start = std::chrono::steady_clock::now();
    auto warm = index.refresh(ignore, cancel.token());
    REQUIRE(warm);
    const auto warm_ms = ms_since(start);

    // --- one file added ---------------------------------------------------
    // Inside the tree being scanned, or it measures nothing. Removed again
    // immediately, whatever happens, and never reaches a commit.
    const auto scratch = root / ".localcode-bench-scratch.cpp";
    {
        std::ofstream out(scratch);
        out << "void bench_only_scratch_symbol() {}\n";
    }
    start = std::chrono::steady_clock::now();
    auto incremental = index.refresh(ignore, cancel.token());
    const auto incremental_ms = ms_since(start);
    std::error_code remove_ec;
    std::filesystem::remove(scratch, remove_ec);
    REQUIRE(incremental);
    CHECK(incremental->files_added == 1);

    // And its removal is noticed on the next pass.
    auto after_delete = index.refresh(ignore, cancel.token());
    REQUIRE(after_delete);
    CHECK(after_delete->files_removed == 1);

    // --- queries ----------------------------------------------------------
    start = std::chrono::steady_clock::now();
    auto exact = index.store().symbols_named("describe_outcome");
    const auto exact_us = us_since(start);
    REQUIRE(exact);

    start = std::chrono::steady_clock::now();
    auto fuzzy = index.store().symbols_like("presentation");
    const auto fuzzy_us = us_since(start);
    REQUIRE(fuzzy);

    start = std::chrono::steady_clock::now();
    auto refs = index.store().references_to_file("src/core/agent/Outcome.h");
    const auto refs_us = us_since(start);
    REQUIRE(refs);

    const auto size = index.store().size_on_disk();
    const auto overview = index.overview();
    REQUIRE(overview);
    REQUIRE(overview->has_value());

    std::printf("\n=== repository index, LocalCode ===\n");
    std::printf("  files indexed        : %zu (%zu skipped)\n", (*overview)->indexed_files,
                (*overview)->skipped_files);
    std::printf("  lines                : %zu\n", (*overview)->total_lines);
    std::printf("  symbols              : %zu\n", (*overview)->symbol_count);
    std::printf("  cold index           : %lld ms\n", static_cast<long long>(cold_ms));
    std::printf("  warm, no change      : %lld ms (%zu unchanged)\n",
                static_cast<long long>(warm_ms), warm->files_unchanged);
    std::printf("  after one new file   : %lld ms (%zu added, %zu reindexed)\n",
                static_cast<long long>(incremental_ms), incremental->files_added,
                incremental->files_reindexed);
    std::printf("  index on disk        : %.2f MiB\n",
                size ? static_cast<double>(*size) / (1024.0 * 1024.0) : 0.0);
    std::printf("  query, exact symbol  : %lld us (%zu hits)\n",
                static_cast<long long>(exact_us), exact->size());
    std::printf("  query, fuzzy symbol  : %lld us (%zu hits)\n",
                static_cast<long long>(fuzzy_us), fuzzy->size());
    std::printf("  query, references    : %lld us (%zu hits)\n",
                static_cast<long long>(refs_us), refs->size());
    std::printf("  languages            :");
    for (std::size_t i = 0; i < (*overview)->languages.size() && i < 5; ++i) {
        std::printf(" %s(%zu)", (*overview)->languages[i].language.c_str(),
                    (*overview)->languages[i].files);
    }
    std::printf("\n\n");

    // The index has to be worth querying. Anything approaching a tenth of a
    // second stops being something the agent can ask casually.
    CHECK(exact_us < 100000);
    CHECK(fuzzy_us < 100000);
    CHECK(refs_us < 100000);
    // A warm pass over an unchanged tree must not redo the work.
    CHECK(warm->files_reindexed == 0);
    CHECK(warm->files_unchanged > 0);
}
