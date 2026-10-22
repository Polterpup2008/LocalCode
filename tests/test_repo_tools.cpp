// ---------------------------------------------------------------------------
// The repository tools, as the agent actually calls them.
//
// test_repo_index.cpp covers what the index knows. This covers what the model
// is told, which is a different question and fails differently. The worst
// outcome here is not a missing result - it is a confident empty one. "No
// matches" out of an index that was never built reads as "that is not in this
// project", and the model acts on it by writing the thing again.
//
// So the cases below lean on the shapes of failure: no index, empty index, a
// name that is genuinely absent, a path that points outside the workspace, and
// a fact recorded without saying where it came from.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>

#include "platform/Environment.h"
#include "repo/RepoIndex.h"
#include "repo/RepoQuery.h"
#include "support/TempDirectory.h"
#include "tools/ToolRegistry.h"
#include "workspace/Workspace.h"

using namespace localcode;
using localcode::test::TempDirectory;

namespace {

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// A project with enough shape to answer every tool: a build system, a source
// root, a test root, and a symbol that is defined in one file and used in
// another.
void build_project(const std::filesystem::path& root) {
    write_file(root / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(widget)\n"
               "add_executable(widget src/main.cpp src/widget.cpp)\n"
               "enable_testing()\n");
    write_file(root / "README.md", "# widget\n\nBuild with cmake.\n");
    write_file(root / "src" / "widget.h", "#pragma once\nint spin_widget(int turns);\n");
    write_file(root / "src" / "widget.cpp",
               "#include \"widget.h\"\n"
               "#include <cstdio>\n"
               "\n"
               "int spin_widget(int turns) {\n"
               "    return turns * 3;\n"
               "}\n");
    write_file(root / "src" / "main.cpp",
               "#include \"widget.h\"\n"
               "\n"
               "int main() {\n"
               "    return spin_widget(4);\n"
               "}\n");
    write_file(root / "tests" / "test_widget.cpp",
               "#include \"../src/widget.h\"\n"
               "\n"
               "void check_spin() {\n"
               "    spin_widget(1);\n"
               "}\n");
    write_file(root / "build" / "generated.cpp", "int must_not_be_indexed() { return 0; }\n");
}

// The registry, a real workspace on disk, and a real index over it.
struct RepoToolFixture {
    TempDirectory work;
    TempDirectory index_dir;
    Workspace workspace;
    repo::RepoIndex index;
    ToolRegistry registry;
    EnvironmentService environment;
    ToolContext context;
    fs::IgnoreRules ignore;

    explicit RepoToolFixture(bool index_it = true) {
        build_project(work.path());
        REQUIRE(workspace.open(work.path()).ok());
        REQUIRE(index.open(index_dir.path(), work.path()));
        if (index_it) refresh();

        registry.register_builtin_tools();
        context.workspace = &workspace;
        context.environment = &environment;
        context.repo = &index;
    }

    void refresh(bool full = false) {
        CancellationSource cancel;
        auto stats = index.refresh(ignore, cancel.token(), full);
        REQUIRE(stats);
    }

    ToolResult call(const std::string& name, Json arguments = Json::object()) {
        ToolInvocation invocation;
        invocation.tool_name = name;
        invocation.arguments = std::move(arguments);
        invocation.call_id = "test";
        return registry.invoke(invocation, context);
    }
};

// Case-insensitive contains, because these are prose answers and the point is
// the substance, not the capitalisation.
bool mentions(const std::string& haystack, std::string_view needle) {
    auto lower = haystack;
    for (auto& character : lower)
        character = static_cast<char>(std::tolower(character));
    std::string lowered{needle};
    for (auto& character : lowered)
        character = static_cast<char>(std::tolower(character));
    return lower.find(lowered) != std::string::npos;
}

}   // namespace

TEST_CASE("initial repository leads find symbols and respect index invalidation", "[repo][leads]") {
    RepoToolFixture fixture{false};
    write_file(fixture.work.path() / "src" / "lifecycle.cpp",
               "// process ownership\n\nint start_model_server() { return 1; }\n"
               "int stop_model_server() { return 0; }\n");
    write_file(fixture.work.path() / "src" / "strings.cpp",
               "bool starts_with() { return true; }\n");
    fixture.refresh();
    repo::RepoQuery query{fixture.index.store(), fixture.work.path()};
    auto leads = query.task_sources("Locate start_model_server and explain its source", 2);
    REQUIRE(leads);
    REQUIRE_FALSE(leads->empty());
    CHECK(leads->front().file == "src/lifecycle.cpp");
    CHECK(leads->front().line == 3);
    CHECK(leads->front().evidence == repo::Evidence::Lexical);
    CHECK(leads->size() <= 2);
    REQUIRE(query.task_sources("model", 0));
    CHECK(query.task_sources("model", 0)->empty());
    auto arithmetic = query.task_sources("What is 17 multiplied by 23? Give only the number.");
    REQUIRE(arithmetic);
    CHECK(arithmetic->empty());
    auto generated = query.task_sources("Locate must_not_be_indexed");
    REQUIRE(generated);
    CHECK(generated->empty());
    auto broad = query.task_sources("Find which functions starts and stops model server");
    REQUIRE(broad);
    CHECK(std::none_of(broad->begin(), broad->end(),
                       [](const auto& hit) { return hit.file == "src/strings.cpp"; }));

    std::filesystem::remove(fixture.work.path() / "src" / "lifecycle.cpp");
    fixture.refresh();
    auto deleted = query.task_sources("Locate start_model_server");
    REQUIRE(deleted);
    CHECK(deleted->empty());
}

TEST_CASE("owner leads choose requested lifecycle definitions rather than the constructor",
          "[repo][leads]") {
    RepoToolFixture fixture{false};
    write_file(fixture.work.path() / "src" / "managed_server.cpp",
               "class ManagedServer {};\n\n"
               "ManagedServer::ManagedServer() {}\n\n"
               "void ManagedServer::start() {}\n\n"
               "void ManagedServer::stop() {}\n");
    fixture.refresh();
    repo::RepoQuery query{fixture.index.store(), fixture.work.path()};
    const auto leads =
        query.task_sources("Locate the managed server implementation that starts and stops", 1);
    REQUIRE(leads);
    REQUIRE(leads->size() == 1);
    CHECK(leads->front().file == "src/managed_server.cpp");
    CHECK(leads->front().line == 5);
    CHECK(leads->front().end_line == 6);
    REQUIRE(leads->front().related_ranges.size() == 1);
    CHECK(leads->front().related_ranges.front().first == 7);
}

TEST_CASE("qualified process methods are retained alongside lexical display helpers",
          "[repo][leads]") {
    RepoToolFixture fixture{false};
    write_file(fixture.work.path() / "src/process.cpp",
               "namespace example {\n"
               "struct ProcessAttributes {\n"
               "~ProcessAttributes() {}\n"
               "};\n"
               "std::string ProcessRunner::display_command() { return {}; }\n"
               "Result ProcessRunner::run() {\n"
               "  launch();\n"
               "  return {};\n"
               "}\n"
               "}\n");
    fixture.refresh();
    repo::RepoQuery query{fixture.index.store(), fixture.work.path()};
    const auto leads = query.task_sources("Locate managed process launch and cleanup", 1);
    REQUIRE(leads);
    REQUIRE(leads->size() == 1);
    const auto& hit = leads->front();
    CHECK(hit.line != 3);
    CHECK(hit.end_line >= hit.line);
    CHECK(hit.end_line - hit.line <= 127);
    CHECK(hit.related_ranges.size() <= 2);
    CHECK((hit.line == 6 || std::any_of(hit.related_ranges.begin(), hit.related_ranges.end(),
                                        [](const auto& range) { return range.first == 6; })));
}

TEST_CASE("owner action ranges outrank subject names outside class declarations", "[repo][leads]") {
    RepoToolFixture fixture{false};
    write_file(fixture.work.path() / "src/inference/ServerManager.cpp",
               "namespace localcode {\n"
               "ModelServer::ModelServer() = default;\n"
               "ModelServer::~ModelServer() { stop(); }\n"
               "Status ModelServer::start() { return {}; }\n"
               "void ModelServer::stop() {}\n"
               "}\n");
    write_file(fixture.work.path() / "src/gui/widgets/ChatView.cpp",
               "namespace localcode::gui {\n"
               "void ChatView::setShowReasoning(bool show) {}\n"
               "void ChatView::appendReasoning(const QString& text) {}\n"
               "void ChatView::clear() {}\n"
               "}\n");
    fixture.refresh();
    repo::RepoQuery query{fixture.index.store(), fixture.work.path()};
    const auto lifecycle = query.task_sources(
        "Locate the managed model server implementation that starts and stops", 1);
    REQUIRE(lifecycle);
    REQUIRE(lifecycle->size() == 1);
    CHECK(lifecycle->front().file == "src/inference/ServerManager.cpp");
    CHECK(lifecycle->front().line == 4);
    const auto timer = query.task_sources(
        "Find which GUI object owns the reasoning animation timer and determine whether clearing "
        "the chat stops that timer",
        1);
    REQUIRE(timer);
    REQUIRE(timer->size() == 1);
    CHECK(timer->front().file == "src/gui/widgets/ChatView.cpp");
    CHECK(timer->front().line == 4);
}

// ---------------------------------------------------------------------------
// Absence, reported as absence
// ---------------------------------------------------------------------------

TEST_CASE("a tool with no index says so instead of answering", "[repo][tools]") {
    RepoToolFixture fixture;
    fixture.context.repo = nullptr;   // as if the workspace had no index

    for (const char* name :
         {"repo_overview", "repo_search", "repo_symbol", "repo_tests_for", "repo_fact"}) {
        INFO(name);
        auto result = fixture.call(
            name, Json{{"query", "spin"}, {"name", "spin_widget"}, {"file", "src/widget.cpp"}});
        CHECK_FALSE(result.ok);
        // The failure has to send the model somewhere useful, and has to stop
        // it concluding the project is empty.
        CHECK(mentions(result.error, "directly"));
        CHECK_FALSE(mentions(result.error, "no results"));
    }
}

TEST_CASE("an unindexed workspace is not an empty one", "[repo][tools]") {
    RepoToolFixture fixture{/*index_it=*/false};

    auto result = fixture.call("repo_search", Json{{"query", "spin_widget"}});
    REQUIRE_FALSE(result.ok);
    CHECK(mentions(result.error, "not been indexed"));
    CHECK(mentions(result.error, "not evidence"));
}

TEST_CASE("a genuinely absent symbol is reported as absent, with reasons", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result = fixture.call("repo_symbol", Json{{"name", "no_such_function_anywhere"}});
    // This one succeeds: the index was consulted and it really is not there.
    REQUIRE(result.ok);
    CHECK(mentions(result.content, "no definition"));
    // And it says what else could explain the absence, rather than letting the
    // model conclude the name is free to use.
    CHECK(mentions(result.content, "generated"));
    CHECK(mentions(result.content, "repo_search"));
}

// ---------------------------------------------------------------------------
// The answers themselves
// ---------------------------------------------------------------------------

TEST_CASE("repo_overview describes the project without being asked twice", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result = fixture.call("repo_overview");
    REQUIRE(result.ok);
    CHECK(mentions(result.content, "c++"));
    CHECK(mentions(result.content, "cmake"));
    CHECK(mentions(result.content, "src"));
    CHECK(mentions(result.content, "tests"));
    CHECK(mentions(result.content, "symbols"));
    // Build output is not part of the project's shape.
    CHECK_FALSE(mentions(result.content, "generated.cpp"));
}

TEST_CASE("repo_overview marks which commands have actually been run", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result = fixture.call("repo_overview");
    REQUIRE(result.ok);
    if (mentions(result.content, "commands found")) {
        // A command LocalCode has never executed must not read as a fact. The
        // wording is what carries that, so it is worth asserting.
        CHECK(mentions(result.content, "have not been tried"));
    }
}

TEST_CASE("repo_symbol finds a definition and says how it knows", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result = fixture.call("repo_symbol", Json{{"name", "spin_widget"}});
    REQUIRE(result.ok);
    CHECK(mentions(result.content, "widget"));
    // The caveat about conditional compilation is the honest part of a
    // source-read index, and it must not be dropped.
    CHECK(mentions(result.content, "not compiled"));
}

TEST_CASE("repo_symbol in references mode finds the callers", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result =
        fixture.call("repo_symbol", Json{{"name", "spin_widget"}, {"mode", "references"}});
    REQUIRE(result.ok);
    CHECK(mentions(result.content, "main.cpp"));
    // A lexical match must never be presented as a compiler's call graph.
    CHECK(mentions(result.content, "textual matches"));
    CHECK(mentions(result.content, "not a compiler"));
}

TEST_CASE("repo_symbol in references mode finds what includes a file", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result =
        fixture.call("repo_symbol", Json{{"file", "src/widget.h"}, {"mode", "references"}});
    REQUIRE(result.ok);
    // Full paths, not bare names. Checking for "widget.cpp" alone passed while
    // this was broken, because "tests/test_widget.cpp" contains it - and that
    // test file was the only hit, since it wrote the include as "../src/widget.h"
    // while the two files beside the header wrote it as "widget.h" and matched
    // nothing at all.
    CHECK(mentions(result.content, "src/widget.cpp"));
    CHECK(mentions(result.content, "src/main.cpp"));
    CHECK(mentions(result.content, "tests/test_widget.cpp"));
}

TEST_CASE("an include beside its header is found as readily as a distant one", "[repo][tools]") {
    RepoToolFixture fixture;

    // The same header reached three ways: from the same directory, and from
    // another directory by a relative path. All three are the same edge.
    auto references = fixture.index.store().references_to_file("src/widget.h", 30);
    REQUIRE(references);

    std::vector<std::string> from;
    for (const auto& reference : *references)
        from.push_back(reference.from_file);
    std::sort(from.begin(), from.end());

    CHECK(from ==
          std::vector<std::string>{"src/main.cpp", "src/widget.cpp", "tests/test_widget.cpp"});

    // And the stored edge points at a real repository path, not at the text
    // that happened to be typed between the quotes.
    for (const auto& reference : *references) {
        INFO(reference.from_file << " -> " << reference.to_file);
        CHECK(reference.to_file == "src/widget.h");
    }
}

TEST_CASE("an include of something outside the repository is left as written", "[repo][tools]") {
    RepoToolFixture fixture;

    auto references = fixture.index.store().references_from_file("src/widget.cpp");
    REQUIRE(references);
    // <cstdio> and the like must not be turned into src/cstdio. An invented
    // path is worse than an unresolved one.
    for (const auto& reference : *references) {
        INFO(reference.to_file);
        CHECK_FALSE(reference.to_file == "src/cstdio");
    }
}

TEST_CASE("repo_symbol needs something to look for", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result = fixture.call("repo_symbol", Json::object());
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("repo_search covers text and file names", "[repo][tools]") {
    RepoToolFixture fixture;

    SECTION("text") {
        auto result = fixture.call("repo_search", Json{{"query", "spin_widget"}});
        REQUIRE(result.ok);
        CHECK(mentions(result.content, "widget.cpp"));
    }

    SECTION("file names") {
        auto result = fixture.call("repo_search", Json{{"query", "widget"}, {"kind", "file"}});
        REQUIRE(result.ok);
        CHECK(mentions(result.content, "widget.h"));
    }

    SECTION("a miss explains what the index covers") {
        auto result = fixture.call("repo_search", Json{{"query", "zzzz_not_here_zzzz"}});
        REQUIRE(result.ok);
        CHECK(mentions(result.content, "not"));
        CHECK(mentions(result.content, "build output"));
    }

    SECTION("build output is not searched") {
        auto result = fixture.call("repo_search", Json{{"query", "must_not_be_indexed"}});
        REQUIRE(result.ok);
        CHECK_FALSE(mentions(result.content, "generated.cpp"));
    }
}

TEST_CASE("repo_tests_for points at the test that covers a file", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result = fixture.call("repo_tests_for", Json{{"file", "src/widget.cpp"}});
    REQUIRE(result.ok);
    CHECK(mentions(result.content, "test_widget.cpp"));
}

TEST_CASE("test associations do not invent includes for same-named headers", "[repo][tools]") {
    RepoToolFixture fixture;
    write_file(fixture.work.path() / "other" / "widget.h", "#pragma once\n");
    fixture.refresh();
    auto result = fixture.call("repo_tests_for", Json{{"file", "other/widget.h"}});
    REQUIRE(result.ok);
    CHECK_FALSE(mentions(result.content, "it includes"));
}

TEST_CASE("incomplete index results direct the model to current files", "[repo][tools]") {
    RepoToolFixture fixture;
    auto limits = fixture.index.limits();
    limits.max_files = 1;
    fixture.index.set_limits(limits);
    fixture.refresh();
    REQUIRE(fixture.index.state() == repo::IndexState::Stale);
    auto result = fixture.call("repo_symbol", Json{{"name", "spin_widget"}});
    CHECK_FALSE(result.ok);
    CHECK(mentions(result.error, "directly"));
}

// ---------------------------------------------------------------------------
// Facts, and the provenance rule
// ---------------------------------------------------------------------------

TEST_CASE("a fact cannot be recorded without saying where it came from", "[repo][tools]") {
    RepoToolFixture fixture;

    auto result = fixture.call(
        "repo_fact",
        Json{{"action", "set"}, {"key", "build_command"}, {"value", "cmake --build build"}});
    CHECK_FALSE(result.ok);
    CHECK(mentions(result.error, "source"));
}

TEST_CASE("a recorded fact comes back with its provenance", "[repo][tools]") {
    RepoToolFixture fixture;

    auto stored = fixture.call("repo_fact", Json{{"action", "set"},
                                                 {"key", "build_command"},
                                                 {"value", "cmake --build build"},
                                                 {"source", "read from CMakeLists.txt"},
                                                 {"depends_on", Json::array({"CMakeLists.txt"})}});
    REQUIRE(stored.ok);

    auto read = fixture.call("repo_fact", Json{{"action", "get"}, {"key", "build_command"}});
    REQUIRE(read.ok);
    CHECK(mentions(read.content, "cmake --build build"));
    // Without the source the value is just an assertion, so it travels with it.
    CHECK(mentions(read.content, "CMakeLists.txt"));
}

TEST_CASE("a fact is dropped when what it rests on changes", "[repo][tools]") {
    RepoToolFixture fixture;

    auto stored = fixture.call("repo_fact", Json{{"action", "set"},
                                                 {"key", "entry_point"},
                                                 {"value", "src/main.cpp"},
                                                 {"source", "read from CMakeLists.txt"},
                                                 {"depends_on", Json::array({"CMakeLists.txt"})}});
    REQUIRE(stored.ok);

    write_file(fixture.work.path() / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\nproject(widget)\n"
               "add_executable(widget src/other.cpp)\n");
    fixture.refresh();

    auto read = fixture.call("repo_fact", Json{{"action", "get"}, {"key", "entry_point"}});
    // Reading a fact that is gone succeeds - the index was consulted and has
    // nothing. What must not happen is the old value coming back as though it
    // were still established.
    REQUIRE(read.ok);
    CHECK_FALSE(mentions(read.content, "src/main.cpp"));
    CHECK(mentions(read.content, "nothing has been recorded"));

    auto listed = fixture.call("repo_fact", Json{{"action", "list"}});
    REQUIRE(listed.ok);
    CHECK_FALSE(mentions(listed.content, "entry_point"));
}

// ---------------------------------------------------------------------------
// Staying inside the workspace
// ---------------------------------------------------------------------------

TEST_CASE("the tools do not reach outside the workspace", "[repo][tools][security]") {
    RepoToolFixture fixture;

    for (const char* escape : {"../outside.txt", "..\\outside.txt",
                               "../../../../Windows/System32/drivers/etc/hosts", "/etc/passwd"}) {
        INFO(escape);
        auto result = fixture.call("repo_tests_for", Json{{"file", escape}});
        // It may refuse, or it may simply have nothing on record for a path it
        // never indexed. What it must never do is read the file.
        if (result.ok) {
            CHECK_FALSE(mentions(result.content, "root:"));
            CHECK_FALSE(mentions(result.content, "localhost"));
        }
    }
}

TEST_CASE("an index over one workspace does not answer for another", "[repo][tools][security]") {
    RepoToolFixture fixture;

    TempDirectory elsewhere;
    write_file(elsewhere.path() / "secret.cpp", "int confidential_symbol() { return 1; }\n");

    auto result = fixture.call("repo_search", Json{{"query", "confidential_symbol"}});
    REQUIRE(result.ok);
    CHECK_FALSE(mentions(result.content, "secret.cpp"));
}

TEST_CASE("repository excerpts reject a sibling sharing the workspace path prefix",
          "[repo][security][path-prefix]") {
    RepoToolFixture fixture;
    TempDirectory parent;
    const auto workspace = parent.make_directory("project");
    const auto sibling = parent.make_directory("project-external");
    write_file(workspace / "inside.cpp", "int inside() { return 1; }\n");
    write_file(sibling / "secret.cpp", "EXTERNAL_SECRET_MARKER\n");
    repo::RepoQuery query{fixture.index.store(), workspace};
    const auto inside = query.excerpt("inside.cpp", 1, 1);
    REQUIRE(inside);
    CHECK(inside->text.find("int inside") != std::string::npos);
    for (const auto& escape : {std::string("../project-external/secret.cpp"),
                               (sibling / "secret.cpp").generic_string()}) {
        INFO(escape);
        const auto outside = query.excerpt(escape, 1, 1);
        REQUIRE_FALSE(outside);
        CHECK(outside.error().code == ErrorCode::PermissionDenied);
    }
}

// ---------------------------------------------------------------------------
// Freshness
// ---------------------------------------------------------------------------

TEST_CASE("a deleted file stops answering", "[repo][tools]") {
    RepoToolFixture fixture;

    auto before = fixture.call("repo_symbol", Json{{"name", "spin_widget"}});
    REQUIRE(before.ok);
    CHECK(mentions(before.content, "widget.cpp"));

    std::filesystem::remove(fixture.work.path() / "src" / "widget.cpp");
    fixture.refresh();

    auto after = fixture.call("repo_symbol", Json{{"name", "spin_widget"}});
    REQUIRE(after.ok);
    // The declaration in the header survives; the definition in the deleted
    // file must not. Sending the agent to open a file that is gone is the
    // failure this whole layer exists to avoid.
    CHECK_FALSE(mentions(after.content, "widget.cpp"));
}

TEST_CASE("a symbol added after the last refresh appears once it runs", "[repo][tools]") {
    RepoToolFixture fixture;

    write_file(fixture.work.path() / "src" / "extra.cpp", "int freshly_added() { return 2; }\n");

    auto before = fixture.call("repo_symbol", Json{{"name", "freshly_added"}});
    REQUIRE(before.ok);
    CHECK(mentions(before.content, "no definition"));

    fixture.refresh();

    auto after = fixture.call("repo_symbol", Json{{"name", "freshly_added"}});
    REQUIRE(after.ok);
    CHECK(mentions(after.content, "extra.cpp"));
}

// ---------------------------------------------------------------------------
// Saying how much was actually searched
// ---------------------------------------------------------------------------

TEST_CASE("a search that stops early reports that it stopped", "[repo][tools]") {
    RepoToolFixture fixture;
    repo::RepoQuery query{fixture.index.store(), fixture.work.path()};

    SECTION("a complete search says so") {
        repo::SearchStats covered;
        auto hits = query.search_text("spin_widget", repo::SearchOptions{}, &covered);
        REQUIRE(hits);
        CHECK(covered.complete());
        CHECK(covered.files_read > 0);
    }

    SECTION("stopping at the file limit is not a complete search") {
        repo::SearchOptions options;
        options.max_files = 1;
        repo::SearchStats covered;
        auto hits = query.search_text("spin_widget", options, &covered);
        REQUIRE(hits);
        CHECK(covered.hit_file_limit);
        CHECK_FALSE(covered.complete());
        // This is the dangerous shape: a short list, or an empty one, that
        // looks exactly like "there is nothing else".
        CHECK(covered.files_read <= 1);
    }

    SECTION("stopping because there were plenty of matches is not complete either") {
        repo::SearchOptions options;
        options.max_hits = 1;   // the scan stops at four times this
        repo::SearchStats covered;
        auto hits = query.search_text("include", options, &covered);
        REQUIRE(hits);
        CHECK(hits->size() <= 1);
        if (covered.hit_match_limit) CHECK_FALSE(covered.complete());
    }
}

TEST_CASE("an incomplete search is never reported as an absence", "[repo][tools]") {
    RepoToolFixture fixture;
    repo::RepoQuery query{fixture.index.store(), fixture.work.path()};

    repo::SearchOptions options;
    options.max_files = 1;
    repo::SearchStats covered;
    // Something that exists, but not in whichever single file gets read.
    auto hits = query.search_text("check_spin", options, &covered);
    REQUIRE(hits);
    if (hits->empty()) {
        // The query layer's job is to report the shortfall; the tool's job is
        // to turn it into words that stop the model concluding "not present".
        CHECK_FALSE(covered.complete());
    }
}
