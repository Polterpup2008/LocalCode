// ---------------------------------------------------------------------------
// Indexing a repository, and keeping it honest afterwards.
//
// The expensive failure for an index is not missing something - it is answering
// with something that is no longer true. A symbol whose file was deleted, a
// path that still exists under its old name after a rename, a fact derived from
// a file that has since changed: each of those sends the agent somewhere that
// is not there any more, and it does so confidently.
//
// So most of what follows is about removal and staleness rather than discovery.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <thread>

#include "repo/RepoIndex.h"
#include "platform/PathEncoding.h"
#include "support/TempDirectory.h"

using namespace localcode;
using namespace localcode::repo;
using localcode::test::TempDirectory;

TEST_CASE("a persisted repository is not current until its offline edits are refreshed",
          "[repo][audit]") {
    TempDirectory work;
    TempDirectory cache;
    work.write("source.py", "def before_restart():\n    return 1\n");
    RepoIndex index;
    REQUIRE(index.open(cache.path(), work.path()));
    fs::IgnoreRules ignore;
    REQUIRE(index.refresh(ignore, {}));
    REQUIRE(index.state() == IndexState::Ready);
    index.close();
    work.write("source.py", "def after_restart():\n    return 2\n");
    REQUIRE(index.open(cache.path(), work.path()));
    CHECK(index.state() == IndexState::Stale);
    REQUIRE(index.refresh(ignore, {}));
    CHECK(index.state() == IndexState::Ready);
    const auto old_symbol = index.store().symbols_named("before_restart");
    REQUIRE(old_symbol);
    CHECK(old_symbol->empty());
    const auto new_symbol = index.store().symbols_named("after_restart");
    REQUIRE(new_symbol);
    CHECK(new_symbol->size() == 1);
}

TEST_CASE("repository indexes preserve Unicode roots filenames and symbols", "[repo][unicode]") {
    TempDirectory work;
    TempDirectory cache;
    std::filesystem::path root;
    SECTION("non-ANSI workspace") {
        root = work.path() / path_from_utf8("projet é 路径 🦍");
    }
    SECTION("ASCII workspace with non-ANSI source") {
        root = work.path() / "project";
    }
    std::filesystem::create_directories(root);
    const auto relative = std::string("calcul é 路径 🦍.py");
    const auto native = root / path_from_utf8(relative);
    { std::ofstream out(native); out << "def unicode_add(a, b):\n    return a + b\n"; }
    RepoIndex index;
    REQUIRE(index.open(cache.path(), root));
    fs::IgnoreRules ignore;
    const auto refreshed = index.refresh(ignore, {});
    REQUIRE(refreshed);
    CHECK(refreshed->files_added == 1);
    const auto stored = index.store().file(relative);
    REQUIRE(stored);
    REQUIRE(stored->has_value());
    CHECK((**stored).path == relative);
    const auto symbols = index.store().symbols_named("unicode_add");
    REQUIRE(symbols);
    REQUIRE(symbols->size() == 1);
    CHECK(symbols->front().file == relative);
    const auto overview = index.overview();
    REQUIRE(overview);
    REQUIRE(overview->has_value());
    CHECK((**overview).root == path_to_utf8(root));
    const auto second = index.refresh(ignore, {});
    REQUIRE(second);
    CHECK(second->files_unchanged == 1);
    CHECK(second->files_reindexed == 0);
    index.close();
    REQUIRE(index.open(cache.path(), root));
    const auto restored = index.store().symbols_named("unicode_add");
    REQUIRE(restored);
    REQUIRE(restored->size() == 1);
    CHECK(restored->front().file == relative);
}

TEST_CASE("a throwing index progress observer cannot leave the index running", "[repo][observer]") {
    TempDirectory work;
    TempDirectory cache;
    work.write("main.cpp", "int main() { return 0; }\n");
    RepoIndex index;
    REQUIRE(index.open(cache.path(), work.path()));
    fs::IgnoreRules ignore;
    int calls = 0;
    const auto indexed = index.refresh(ignore, {}, false, [&](const IndexProgress&) {
        ++calls;
        throw 7;
    });
    REQUIRE(indexed);
    CHECK(calls == 1);
    CHECK(index.state() == IndexState::Ready);
    CHECK(indexed->symbols > 0);
    REQUIRE(index.refresh(ignore, {}));
}

namespace {

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// A small but realistic C++ project with a manifest, a source root and a test.
void build_fixture(const std::filesystem::path& root) {
    write_file(root / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(demo)\n"
               "add_executable(demo src/main.cpp src/engine.cpp)\n");
    write_file(root / "README.md", "# demo\n\nA fixture.\n");
    write_file(root / "src" / "main.cpp",
               "#include \"engine.h\"\n\nint main() { return run_engine(); }\n");
    write_file(root / "src" / "engine.cpp",
               "#include \"engine.h\"\n\nint run_engine() { return 7; }\n");
    write_file(root / "src" / "engine.h", "#pragma once\nint run_engine();\n");
    write_file(root / "tests" / "test_engine.cpp",
               "#include \"../src/engine.h\"\nvoid test_run_engine() {}\n");
    // Things that must never be indexed as if a person wrote them.
    write_file(root / "build" / "CMakeCache.txt",
               "CMAKE_HOME_DIRECTORY:INTERNAL=" + root.generic_string() + "\n");
    write_file(root / "build" / "generated.cpp", "void should_not_be_indexed() {}\n");
    write_file(root / "node_modules" / "left-pad" / "index.js", "function leftPad() {}\n");
}

struct Fixture {
    TempDirectory work;
    TempDirectory index_dir;
    RepoIndex     index;
    fs::IgnoreRules ignore;

    Fixture() {
        build_fixture(work.path());
        REQUIRE(index.open(index_dir.path(), work.path()));
    }

    IndexStats refresh(bool full = false) {
        CancellationSource cancel;
        auto stats = index.refresh(ignore, cancel.token(), full);
        REQUIRE(stats);
        return *stats;
    }

    [[nodiscard]] bool has_symbol(std::string_view name) {
        auto found = index.store().symbols_named(name);
        return found && !found->empty();
    }

    [[nodiscard]] bool has_file(std::string_view path) {
        auto found = index.store().file(path);
        return found && found->has_value();
    }
};

// The filesystem's modification time has coarse granularity on some systems, so
// a file rewritten immediately can carry the same timestamp. The content
// fingerprint is what makes the change visible either way; this just keeps the
// test from depending on which of the two noticed.
void touch_delay() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

}  // namespace

TEST_CASE("a cold index finds the project and ignores what it should", "[repo][index]") {
    Fixture fixture;
    const auto stats = fixture.refresh();

    CHECK(stats.files_added > 0);
    CHECK(stats.files_reindexed == 0);
    CHECK_FALSE(stats.cancelled);

    CHECK(fixture.has_file("src/main.cpp"));
    CHECK(fixture.has_file("CMakeLists.txt"));
    CHECK(fixture.has_symbol("run_engine"));
    CHECK(fixture.has_symbol("demo"));  // the CMake target

    // Build output and dependencies are not this project's source.
    CHECK_FALSE(fixture.has_file("build/generated.cpp"));
    CHECK_FALSE(fixture.has_file("node_modules/left-pad/index.js"));
    CHECK_FALSE(fixture.has_symbol("should_not_be_indexed"));
    CHECK_FALSE(fixture.has_symbol("leftPad"));

    auto overview = fixture.index.overview();
    REQUIRE(overview);
    REQUIRE(overview->has_value());
    const auto& map = **overview;
    CHECK(map.is_git_repository == false);
    CHECK(std::find(map.build_systems.begin(), map.build_systems.end(), "CMake") !=
          map.build_systems.end());
    CHECK(std::find(map.entry_points.begin(), map.entry_points.end(), "src/main.cpp") !=
          map.entry_points.end());
    CHECK(std::find(map.source_roots.begin(), map.source_roots.end(), "src") !=
          map.source_roots.end());
    CHECK(std::find(map.test_roots.begin(), map.test_roots.end(), "tests") !=
          map.test_roots.end());
}

TEST_CASE("a second pass over an unchanged tree reindexes nothing", "[repo][index]") {
    Fixture fixture;
    const auto first = fixture.refresh();
    const auto second = fixture.refresh();

    CHECK(second.files_added == 0);
    CHECK(second.files_reindexed == 0);
    CHECK(second.files_removed == 0);
    CHECK(second.files_unchanged == first.files_added);
    // Still answering, from the cache.
    CHECK(fixture.has_symbol("run_engine"));
}

TEST_CASE("a partial refresh cannot delete files it did not visit", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    const auto before = fixture.index.store().files();
    REQUIRE(before);
    auto limits = fixture.index.limits();
    limits.max_files = 1;
    fixture.index.set_limits(limits);
    const auto stats = fixture.refresh();
    CHECK(stats.files_removed == 0);
    auto after = fixture.index.store().files();
    REQUIRE(after);
    CHECK(after->size() == before->size());
    CHECK(fixture.index.state() == IndexState::Stale);
}

TEST_CASE("deleted manifests remove their discovered commands", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    REQUIRE_FALSE(fixture.index.store().commands()->empty());
    std::filesystem::remove(fixture.work.path() / "CMakeLists.txt");
    fixture.refresh();
    auto commands = fixture.index.store().commands();
    REQUIRE(commands);
    CHECK(commands->empty());
}

TEST_CASE("nested CMake build trees yield commands with actual provenance", "[repo][index]") {
    Fixture fixture;
    std::filesystem::remove(fixture.work.path() / "build" / "CMakeCache.txt");
    write_file(fixture.work.path() / "build" / "gui" / "CMakeCache.txt",
               "CMAKE_HOME_DIRECTORY:INTERNAL=" + fixture.work.path().generic_string() + "\n");
    fixture.refresh();
    auto commands = fixture.index.store().commands();
    REQUIRE(commands);
    const auto build = std::find_if(
        commands->begin(), commands->end(),
        [](const RepoCommand& command) { return command.command == "cmake --build build/gui"; });
    REQUIRE(build != commands->end());
    CHECK(build->source.find("build/gui/CMakeCache.txt") != std::string::npos);
    CHECK(build->evidence == Evidence::Structural);
    CHECK_FALSE(build->verified);
    CHECK(std::none_of(commands->begin(), commands->end(), [](const RepoCommand& command) {
        return command.command == "cmake --build build";
    }));
}

TEST_CASE("CMake cache ownership and configuration changes constrain command proof",
          "[repo][index]") {
    Fixture fixture;
    const auto cache = fixture.work.path() / "build" / "CMakeCache.txt";
    fixture.refresh();
    REQUIRE(fixture.index.store().mark_command_verified("build", "cmake --build build", 0, 123));
    fixture.refresh();
    auto commands = fixture.index.store().commands();
    REQUIRE(commands);
    REQUIRE(std::any_of(commands->begin(), commands->end(), [](const RepoCommand& command) {
        return command.command == "cmake --build build" && command.verified;
    }));
    write_file(cache, "CMAKE_HOME_DIRECTORY:INTERNAL=" + fixture.work.path().generic_string() +
                          "\nCMAKE_BUILD_TYPE:STRING=Debug\n");
    const auto changed = fixture.refresh();
    CHECK(changed.files_reindexed == 0);
    commands = fixture.index.store().commands();
    REQUIRE(commands);
    CHECK(std::none_of(commands->begin(), commands->end(),
                       [](const RepoCommand& command) { return command.verified; }));
    TempDirectory other_project;
    write_file(cache,
               "CMAKE_HOME_DIRECTORY:INTERNAL=" + other_project.path().generic_string() + "\n");
    fixture.refresh();
    commands = fixture.index.store().commands();
    REQUIRE(commands);
    CHECK(std::none_of(commands->begin(), commands->end(), [](const RepoCommand& command) {
        return command.evidence == Evidence::Structural;
    }));
}

#ifdef _WIN32
TEST_CASE("repository and tool walks skip NTFS junctions", "[repo][security][junction]") {
    Fixture fixture;
    TempDirectory outside;
    write_file(outside.path() / "secret.cpp", "int outside_secret() { return 1; }\n");
    const auto link = fixture.work.path() / "linked";
    const auto command = "cmd.exe /d /c mklink /J \"" + link.string() + "\" \"" +
                         outside.path().string() + "\" >nul 2>&1";
    REQUIRE(std::system(command.c_str()) == 0);

    struct RemoveJunction {
        std::filesystem::path path;

        ~RemoveJunction() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{link};

    REQUIRE(fs::is_link_or_reparse_point(link));
    fixture.refresh();
    CHECK_FALSE(fixture.has_symbol("outside_secret"));
    bool escaped = false;
    REQUIRE(fs::walk(fixture.work.path(), {}, [&](const auto& path, const auto&) {
        if (path.generic_string().starts_with("linked/")) escaped = true;
        return true;
    }));
    CHECK_FALSE(escaped);
}
#endif

TEST_CASE("changed package scripts invalidate previous command verification", "[repo][index]") {
    Fixture fixture;
    write_file(fixture.work.path() / "package.json", R"({"scripts":{"test":"node old.js"}})");
    fixture.refresh();
    REQUIRE(fixture.index.store().mark_command_verified("test", "npm run test", 0, 123));
    fixture.refresh();
    auto commands = fixture.index.store().commands();
    REQUIRE(commands);
    const auto npm = std::find_if(commands->begin(), commands->end(),
                                  [](const RepoCommand& c) { return c.command == "npm run test"; });
    REQUIRE(npm != commands->end());
    CHECK(npm->verified);
    CHECK(npm->evidence == Evidence::Verified);
    write_file(fixture.work.path() / "package.json", R"({"scripts":{"test":"node new.js"}})");
    fixture.refresh();
    commands = fixture.index.store().commands();
    REQUIRE(commands);
    for (const auto& command : *commands) {
        if (command.command == "npm run test") {
            CHECK_FALSE(command.verified);
            CHECK(command.evidence == Evidence::Structural);
        }
    }
}

TEST_CASE("a failed rerun revokes verified command evidence", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    REQUIRE(fixture.index.store().mark_command_verified("build", "cmake --build build", 0, 123));
    REQUIRE(fixture.index.store().mark_command_verified("build", "cmake --build build", 1, 124));
    auto commands = fixture.index.store().commands();
    REQUIRE(commands);
    for (const auto& command : *commands) {
        if (command.command == "cmake --build build") {
            CHECK_FALSE(command.verified);
            CHECK(command.evidence != Evidence::Verified);
        }
    }
}

TEST_CASE("an unchanged include resolves when its target is added", "[repo][index]") {
    Fixture fixture;
    write_file(fixture.work.path() / "src" / "main.cpp", "#include \"future.h\"\nint main() {}\n");
    fixture.refresh();
    write_file(fixture.work.path() / "src" / "future.h", "#pragma once\n");
    fixture.refresh();
    const auto refs = fixture.index.store().references_from_file("src/main.cpp");
    REQUIRE(refs);
    CHECK(std::any_of(refs->begin(), refs->end(),
                      [](const RepoReference& r) { return r.to_file == "src/future.h"; }));
}

TEST_CASE("resolved references do not match unrelated headers with the same basename",
          "[repo][index]") {
    Fixture fixture;
    write_file(fixture.work.path() / "other" / "engine.h", "#pragma once\n");
    fixture.refresh();
    auto refs = fixture.index.store().references_to_file("other/engine.h");
    REQUIRE(refs);
    CHECK(refs->empty());
}

TEST_CASE("cancelled cached scans reopen as incomplete", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    CancellationSource cancel;
    cancel.cancel();
    auto result = fixture.index.refresh(fixture.ignore, cancel.token());
    REQUIRE(result);
    REQUIRE(result->cancelled);
    fixture.index.close();
    REQUIRE(fixture.index.open(fixture.index_dir.path(), fixture.work.path()));
    CHECK(fixture.index.state() == IndexState::Stale);
    auto overview = fixture.index.overview();
    REQUIRE(overview);
    REQUIRE(overview->has_value());
    CHECK((**overview).truncated);
    fixture.refresh();
    CHECK(fixture.index.state() == IndexState::Ready);
}

TEST_CASE("a concurrent refresh cannot overwrite the active refresh state", "[repo][index]") {
    Fixture fixture;
    bool attempted = false;
    auto result =
        fixture.index.refresh(fixture.ignore, {}, false, [&](const IndexProgress& progress) {
            if (attempted || progress.state != IndexState::Indexing) return;
            attempted = true;
            CHECK_FALSE(fixture.index.refresh(fixture.ignore, {}));
            CHECK(fixture.index.state() == IndexState::Indexing);
        });
    REQUIRE(result);
    CHECK(attempted);
    CHECK(fixture.index.state() == IndexState::Ready);
}

TEST_CASE("an edit during a refresh leaves the result stale until another pass", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    bool invalidated = false;
    auto result =
        fixture.index.refresh(fixture.ignore, {}, false, [&](const IndexProgress& progress) {
            if (!invalidated && progress.state == IndexState::Indexing) {
                invalidated = true;
                fixture.index.invalidate();
            }
        });
    REQUIRE(result);
    CHECK(fixture.index.state() == IndexState::Stale);
    CHECK((**fixture.index.overview()).truncated);
    fixture.refresh();
    CHECK(fixture.index.state() == IndexState::Ready);
}

TEST_CASE("an index write failure is surfaced and is not Ready", "[repo][index]") {
    Fixture fixture;
    bool closed = false;
    auto stats = fixture.index.refresh(fixture.ignore, {}, false, [&](const IndexProgress&) {
        if (!closed) {
            fixture.index.store().close();
            closed = true;
        }
    });
    CHECK_FALSE(stats);
    CHECK(fixture.index.state() == IndexState::Failed);
}

TEST_CASE("an edited file is reindexed and its old symbols disappear", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    REQUIRE(fixture.has_symbol("run_engine"));

    touch_delay();
    write_file(fixture.work.path() / "src" / "engine.cpp",
               "#include \"engine.h\"\n\nint renamed_engine() { return 8; }\n");

    const auto stats = fixture.refresh();
    CHECK(stats.files_reindexed == 1);
    CHECK(stats.files_added == 0);
    CHECK(fixture.has_symbol("renamed_engine"));
    // The definition that is gone must stop being an answer.
    auto old = fixture.index.store().symbols_named("run_engine");
    REQUIRE(old);
    const bool still_in_engine_cpp =
        std::any_of(old->begin(), old->end(),
                    [](const RepoSymbol& s) { return s.file == "src/engine.cpp"; });
    CHECK_FALSE(still_in_engine_cpp);
}

TEST_CASE("a deleted file takes its symbols with it", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    REQUIRE(fixture.has_file("src/engine.cpp"));

    std::filesystem::remove(fixture.work.path() / "src" / "engine.cpp");
    const auto stats = fixture.refresh();

    CHECK(stats.files_removed == 1);
    CHECK_FALSE(fixture.has_file("src/engine.cpp"));

    auto symbols = fixture.index.store().symbols_named("run_engine");
    REQUIRE(symbols);
    CHECK(std::none_of(symbols->begin(), symbols->end(),
                       [](const RepoSymbol& s) { return s.file == "src/engine.cpp"; }));

    auto references = fixture.index.store().references_from_file("src/engine.cpp");
    REQUIRE(references);
    CHECK(references->empty());
}

TEST_CASE("a rename leaves nothing behind at the old path", "[repo][index]") {
    // The failure this guards against: the agent is told the code is at a path
    // that no longer exists, opens it, finds nothing, and starts guessing.
    Fixture fixture;
    fixture.refresh();

    std::filesystem::rename(fixture.work.path() / "src" / "engine.cpp",
                            fixture.work.path() / "src" / "motor.cpp");
    const auto stats = fixture.refresh();

    CHECK(stats.files_added == 1);
    CHECK(stats.files_removed == 1);
    CHECK_FALSE(fixture.has_file("src/engine.cpp"));
    CHECK(fixture.has_file("src/motor.cpp"));

    auto symbols = fixture.index.store().symbols_named("run_engine");
    REQUIRE(symbols);
    REQUIRE_FALSE(symbols->empty());
    for (const auto& symbol : *symbols) CHECK(symbol.file != "src/engine.cpp");
}

TEST_CASE("the index survives being reopened", "[repo][index]") {
    TempDirectory work;
    TempDirectory index_dir;
    build_fixture(work.path());

    {
        RepoIndex index;
        REQUIRE(index.open(index_dir.path(), work.path()));
        CancellationSource cancel;
        fs::IgnoreRules ignore;
        REQUIRE(index.refresh(ignore, cancel.token()));
        CHECK(index.state() == IndexState::Ready);
    }

    // A new object, the same workspace: persisted data is still there, but
    // offline edits have not been checked until the next refresh.
    RepoIndex reopened;
    REQUIRE(reopened.open(index_dir.path(), work.path()));
    CHECK(reopened.state() == IndexState::Stale);

    auto symbols = reopened.store().symbols_named("run_engine");
    REQUIRE(symbols);
    CHECK_FALSE(symbols->empty());

    auto overview = reopened.overview();
    REQUIRE(overview);
    CHECK(overview->has_value());
}

TEST_CASE("a fact dies with the file it was derived from", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();

    RepoFact fact;
    fact.key = "entry_point";
    fact.value = "src/main.cpp";
    fact.source = "the only main() in the tree";
    fact.evidence = Evidence::Lexical;
    fact.depends_on = {"src/main.cpp"};
    REQUIRE(fixture.index.store().put_fact(fact));

    // Execution evidence is stale when a file it explicitly depends on changes.
    RepoFact ran;
    ran.key = "build.command";
    ran.value = "cmake --build build";
    ran.source = "LocalCode ran it";
    ran.evidence = Evidence::Verified;
    ran.depends_on = {"src/main.cpp"};
    REQUIRE(fixture.index.store().put_fact(ran));

    touch_delay();
    write_file(fixture.work.path() / "src" / "main.cpp",
               "#include \"engine.h\"\n\nint main() { return 0; }\n");
    const auto stats = fixture.refresh();

    CHECK(stats.facts_invalidated == 2);
    auto gone = fixture.index.store().fact("entry_point");
    REQUIRE(gone);
    CHECK_FALSE(gone->has_value());

    auto kept = fixture.index.store().fact("build.command");
    REQUIRE(kept);
    CHECK_FALSE(kept->has_value());
}

TEST_CASE("build and test commands are discovered with their provenance", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();

    auto commands = fixture.index.store().commands();
    REQUIRE(commands);
    REQUIRE_FALSE(commands->empty());

    const auto build = std::find_if(commands->begin(), commands->end(),
                                    [](const RepoCommand& c) { return c.purpose == "build"; });
    REQUIRE(build != commands->end());
    CHECK(build->command.find("cmake") != std::string::npos);
    CHECK_FALSE(build->source.empty());
    // Nothing has been run, so nothing is verified.
    CHECK_FALSE(build->verified);
    // And a configure line that was never observed is an inference, not a fact.
    const auto configure = std::find_if(commands->begin(), commands->end(),
                                        [](const RepoCommand& c) { return c.purpose == "configure"; });
    if (configure != commands->end()) CHECK(configure->evidence == Evidence::Heuristic);
}

TEST_CASE("running a command is what makes it verified", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();

    REQUIRE(fixture.index.store().mark_command_verified("build", "cmake --build build", 0, 1234));
    auto commands = fixture.index.store().commands();
    REQUIRE(commands);
    const auto build = std::find_if(commands->begin(), commands->end(),
                                    [](const RepoCommand& c) { return c.purpose == "build"; });
    REQUIRE(build != commands->end());
    CHECK(build->verified);
    CHECK(build->evidence == Evidence::Verified);
    CHECK(build->verified_at_unix_ms == 1234);

    // A later rescan must not demote what was established by running it.
    fixture.refresh(true);
    auto again = fixture.index.store().commands();
    REQUIRE(again);
    const auto after = std::find_if(again->begin(), again->end(),
                                    [](const RepoCommand& c) { return c.purpose == "build"; });
    REQUIRE(after != again->end());
    CHECK(after->verified);
}

TEST_CASE("cancellation stops the walk and says so", "[repo][index]") {
    Fixture fixture;
    CancellationSource cancel;
    cancel.cancel();

    auto stats = fixture.index.refresh(fixture.ignore, cancel.token());
    REQUIRE(stats);
    CHECK(stats->cancelled);
    // A cancelled pass must not be reported as a finished one.
    CHECK(fixture.index.state() == IndexState::Stale);
    // And it must not have deleted everything it simply did not get to.
    CHECK(stats->files_removed == 0);
}

TEST_CASE("a corrupt index file is replaced rather than fatal", "[repo][index]") {
    TempDirectory work;
    TempDirectory index_dir;
    build_fixture(work.path());

    const auto path = RepoStore::index_path(index_dir.path(), work.path());
    write_file(path, "this is not a database");

    RepoStore store;
    const auto status = store.open(index_dir.path(), work.path());
    REQUIRE(status);
    CHECK_FALSE(store.reset_reason().empty());

    // And it works afterwards.
    RepoFile file;
    file.path = "a.cpp";
    CHECK(store.put_file(file));
}

TEST_CASE("an index from another format version is discarded", "[repo][index]") {
    // Rows written by different extraction rules are not reinterpreted: which
    // of them still hold is not knowable, and guessing is how stale entries
    // survive a version change.
    TempDirectory work;
    TempDirectory index_dir;
    build_fixture(work.path());

    {
        RepoIndex index;
        REQUIRE(index.open(index_dir.path(), work.path()));
        CancellationSource cancel;
        fs::IgnoreRules ignore;
        REQUIRE(index.refresh(ignore, cancel.token()));
    }

    {
        Database db;
        REQUIRE(db.open(RepoStore::index_path(index_dir.path(), work.path())));
        REQUIRE(db.execute("UPDATE repo_meta SET value = '999' WHERE key = 'format'"));
    }

    RepoStore store;
    REQUIRE(store.open(index_dir.path(), work.path()));
    CHECK(store.reset_reason().find("999") != std::string::npos);

    auto symbols = store.symbols_named("run_engine");
    REQUIRE(symbols);
    CHECK(symbols->empty());
}

TEST_CASE("a malformed format cannot retain stale index rows", "[repo][index]") {
    Fixture fixture;
    fixture.refresh();
    fixture.index.close();
    {
        Database db;
        REQUIRE(db.open(RepoStore::index_path(fixture.index_dir.path(), fixture.work.path())));
        REQUIRE(db.execute("UPDATE repo_meta SET value = '3garbage' WHERE key = 'format'"));
    }
    RepoStore store;
    REQUIRE(store.open(fixture.index_dir.path(), fixture.work.path()));
    CHECK(store.reset_reason().find("malformed") != std::string::npos);
    auto symbols = store.symbols_named("run_engine");
    REQUIRE(symbols);
    CHECK(symbols->empty());
}

TEST_CASE("an index that cannot be discarded reports the failure", "[repo][index]") {
    TempDirectory work;
    TempDirectory index_dir;
    const auto path = RepoStore::index_path(index_dir.path(), work.path());
    write_file(path / "keep.txt", "preserve this directory\n");
    RepoStore store;
    const auto status = store.open(index_dir.path(), work.path());
    REQUIRE_FALSE(status);
    CHECK(status.error().message == "could not discard repository index");
    CHECK(store.reset_reason().empty());
    CHECK(std::filesystem::exists(path / "keep.txt"));
}

TEST_CASE("a scan cancelled part way through recovers on the next pass", "[repo][index]") {
    Fixture fixture;

    // Stop after the first file, which is what pressing Stop during a large
    // scan actually does - unlike cancelling before it starts, this leaves the
    // index holding some of the tree and none of the rest.
    CancellationSource cancel;
    std::size_t seen = 0;
    auto stats = fixture.index.refresh(fixture.ignore, cancel.token(), false,
                                       [&](const IndexProgress&) {
                                           if (++seen >= 1) cancel.cancel();
                                       });
    REQUIRE(stats);
    CHECK(stats->cancelled);
    CHECK(fixture.index.state() == IndexState::Stale);
    // Partial, and honest about being partial.
    CHECK(stats->files_removed == 0);

    // The next pass has to finish the job rather than inherit the half-done
    // state as though it were complete.
    CancellationSource second;
    auto finished = fixture.index.refresh(fixture.ignore, second.token());
    REQUIRE(finished);
    CHECK_FALSE(finished->cancelled);
    CHECK(fixture.index.state() == IndexState::Ready);

    // Everything the interrupted pass missed is there now.
    CHECK(fixture.has_symbol("run_engine"));
    CHECK(fixture.has_file("src/main.cpp"));
    CHECK(fixture.has_file("src/engine.cpp"));
    CHECK(fixture.has_file("tests/test_engine.cpp"));
    // And the things that must never be indexed still are not.
    CHECK_FALSE(fixture.has_file("build/generated.cpp"));
    CHECK_FALSE(fixture.has_symbol("should_not_be_indexed"));
}

TEST_CASE("a build directory is recognised by what is in it, not its name",
          "[repo][index]") {
    Fixture fixture;

    // Not called "build", so the name list does not catch it. This is ordinary:
    // a CMake build directory can be called anything, and a scratch one named
    // "verify" was indexed as source, with its generated
    // verify/CMakeFiles/ShowIncludes/main.c offered as an entry point.
    write_file(fixture.work.path() / "verify" / "CMakeCache.txt",
               "CMAKE_PROJECT_NAME:STATIC=demo\n");
    write_file(fixture.work.path() / "verify" / "CMakeFiles" / "ShowIncludes" / "main.c",
               "int main(void) { return 0; }\n");
    write_file(fixture.work.path() / "verify" / "generated.cpp",
               "int generated_by_the_build() { return 0; }\n");

    // And one that only looks like it, to make sure a real source directory is
    // not thrown away for having an awkward name.
    write_file(fixture.work.path() / "verify_src" / "real.cpp",
               "int a_real_function() { return 1; }\n");

    fixture.refresh();

    CHECK_FALSE(fixture.has_file("verify/generated.cpp"));
    CHECK_FALSE(fixture.has_file("verify/CMakeFiles/ShowIncludes/main.c"));
    CHECK_FALSE(fixture.has_symbol("generated_by_the_build"));

    CHECK(fixture.has_file("verify_src/real.cpp"));
    CHECK(fixture.has_symbol("a_real_function"));
}
