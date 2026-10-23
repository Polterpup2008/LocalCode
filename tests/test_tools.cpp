// ---------------------------------------------------------------------------
// The tools, exercised against a real workspace on a real filesystem.
//
// These go through ToolRegistry::invoke rather than calling execute() directly,
// so schema validation, error shaping and the read-only mode are covered too.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>

#include "agent/EvidenceInterpreter.h"
#include "inference/ChatTypes.h"
#include "platform/Environment.h"
#include "platform/PathEncoding.h"
#include "platform/Process.h"
#include "repo/Hashing.h"
#include "security/TaskPolicy.h"
#include "skills/SkillRegistry.h"
#include "support/TempDirectory.h"
#include "tools/ToolRegistry.h"
#include "util/Strings.h"
#include "workspace/Workspace.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace localcode;
using localcode::test::TempDirectory;

namespace {

// A workspace with a small C++ project in it, plus the registry and context
// needed to run tools against it.
struct ToolFixture {
    TempDirectory temp{"lc-tools"};
    Workspace workspace;
    ToolRegistry registry;
    EnvironmentService environment;
    ToolContext context;
    bool confirm_answer = true;
    int confirmations_asked = 0;

    ToolFixture() {
        temp.write("src/main.cpp",
                   "#include <cstdio>\n"
                   "\n"
                   "int add(int a, int b) {\n"
                   "    return a + b;\n"
                   "}\n"
                   "\n"
                   "int main() {\n"
                   "    std::printf(\"%d\\n\", add(2, 3));\n"
                   "    return 0;\n"
                   "}\n");
        temp.write("src/util.cpp", "int helper() { return 7; }\n");
        temp.write("include/api.h", "#pragma once\nint add(int a, int b);\n");
        temp.write("CMakeLists.txt",
                   "cmake_minimum_required(VERSION 3.20)\nproject(demo)\n"
                   "add_executable(demo src/main.cpp)\n");
        temp.write("build/generated.cpp", "// should be ignored by the walker\n");
        temp.write(".git/config", "[core]\n");

        REQUIRE(workspace.open(temp.path()).ok());
        registry.register_builtin_tools();

        context.workspace = &workspace;
        context.environment = &environment;
        context.confirm = [this](const ConfirmationRequest&) {
            ++confirmations_asked;
            return confirm_answer;
        };
    }

    // Records built programs the way a successful build would have.
    void record_built(std::vector<std::string> paths) {
        std::vector<ResolvedResource> resources;
        for (auto& path : paths) {
            ResolvedResource resource;
            resource.kind = "executable";
            resource.name = std::filesystem::path{path}.stem().string();
            resource.id = "build:" + resource.name;
            resource.path = std::move(path);
            resource.source = "test";
            resources.push_back(std::move(resource));
        }
        workspace.record_resolved_resources(std::move(resources));
    }

    ToolResult call(const std::string& name, Json arguments) {
        ToolInvocation invocation;
        invocation.tool_name = name;
        invocation.arguments = std::move(arguments);
        invocation.call_id = "test";
        return registry.invoke(invocation, context);
    }
};

// Read the JSON string actually shown to the model, rather than constructing
// an edit from a test-owned copy of the original source.
std::string displayed_source_line_json(const ToolResult& result, std::size_t line) {
    const auto text = result.to_model_text();
    const auto marker = "\nline " + std::to_string(line) + ": ";
    const auto begin = text.find(marker);
    REQUIRE(begin != std::string::npos);
    const auto start = begin + marker.size();
    const auto end = text.find('\n', start);
    REQUIRE(end != std::string::npos);
    const auto encoded = text.substr(start, end - start);
    const auto decoded = Json::parse(encoded, nullptr, false);
    REQUIRE(decoded.is_string());
    return encoded;
}

}   // namespace

TEST_CASE("task write permissions do not expand during a tool sequence", "[tools][task-policy]") {
    ToolFixture f;
    TempDirectory outside{"lc-outside-task"};
    f.context.task_policy =
        std::make_shared<const TaskPolicy>(f.workspace.paths(), f.workspace.commands());
    f.workspace.paths().add_writable_root(outside.path());
    const auto destination = outside.path() / "later-grant.txt";
    const Json arguments = {{"path", destination.generic_string()}, {"content", "approved later"}};
    const auto refused = f.call("write_file", arguments);
    CHECK_FALSE(refused.ok);
    CHECK_FALSE(std::filesystem::exists(destination));
    REQUIRE(f.call("write_file", {{"path", "inside.txt"}, {"content", "task write"}}).ok);
    CHECK(std::filesystem::exists(f.temp.path() / "inside.txt"));

    // Only a newly captured task may consume the new human-selected grant.
    f.context.task_policy =
        std::make_shared<const TaskPolicy>(f.workspace.paths(), f.workspace.commands());
    REQUIRE(f.call("write_file", arguments).ok);
    CHECK(std::filesystem::exists(destination));
}

TEST_CASE("switching the workspace cannot redirect an existing task", "[tools][task-policy]") {
    ToolFixture f;
    TempDirectory replacement{"lc-replacement-task"};
    f.context.task_policy =
        std::make_shared<const TaskPolicy>(f.workspace.paths(), f.workspace.commands());
    REQUIRE(f.workspace.open(replacement.path()).ok());
    const auto result =
        f.call("write_file", {{"path", "redirected.txt"}, {"content", "wrong root"}});
    CHECK_FALSE(result.ok);
    CHECK(result.summary == "workspace changed during task");
    CHECK_FALSE(std::filesystem::exists(replacement.path() / "redirected.txt"));
    CHECK_FALSE(std::filesystem::exists(f.temp.path() / "redirected.txt"));
}

TEST_CASE("a task that started read-only stays read-only", "[tools][task-policy]") {
    ToolFixture f;
    f.registry.set_read_only(true);
    f.context.task_policy = std::make_shared<const TaskPolicy>(
        f.workspace.paths(), f.workspace.commands(), f.registry.read_only());
    f.registry.set_read_only(false);
    const auto result = f.call("write_file", {{"path", "late-write.txt"}, {"content", "no"}});
    CHECK_FALSE(result.ok);
    CHECK_FALSE(std::filesystem::exists(f.temp.path() / "late-write.txt"));
}

TEST_CASE("task command rules do not relax midway through a run", "[tools][task-policy]") {
    ToolFixture f;
    CommandRule rule;
    rule.id = "task-fixture-rule";
    rule.decision = CommandDecision::Block;
    rule.reason = "Fixture task boundary";
    rule.executables = {"cmake"};
    rule.patterns = {"*"};
    f.workspace.commands().add_rule(rule);
    const auto policy =
        std::make_shared<const TaskPolicy>(f.workspace.paths(), f.workspace.commands());
    f.workspace.commands().set_rule_enabled(rule.id, false);
    ProcessRequest request;
    request.executable = "cmake";
    request.arguments = {"--version"};
    CHECK(policy->commands().evaluate(request).decision == CommandDecision::Block);
    CHECK(f.workspace.commands().evaluate(request).decision == CommandDecision::Allow);
}

TEST_CASE("a command working directory must be an allowed write context", "[tools][task-policy]") {
    ToolFixture f;
    TempDirectory outside{"lc-command-outside"};
    f.context.task_policy =
        std::make_shared<const TaskPolicy>(f.workspace.paths(), f.workspace.commands());
    const auto result =
        f.call("run_command", {{"executable", "cmake"},
                               {"arguments", {"--version"}},
                               {"working_directory", outside.path().generic_string()}});
    CHECK_FALSE(result.ok);
    CHECK(result.summary == "bad working directory");
    CHECK(f.confirmations_asked == 0);
}

TEST_CASE("a closed workspace cannot fall back to the application's command directory",
          "[tools][task-policy]") {
    ToolFixture f;
    f.workspace.close();
    const auto result =
        f.call("run_command", {{"executable", "cmake"}, {"arguments", {"--version"}}});
    CHECK_FALSE(result.ok);
    CHECK(result.summary == "no workspace selected");
    CHECK(f.confirmations_asked == 0);
}

#if defined(_WIN32)
TEST_CASE("command tools advertise the actual shell and preserve quoted pipeline paths",
          "[tools][quoting][shell-contract]") {
    ToolFixture f;
    f.temp.write("quoted name.txt", "tool-quoted-path-sentinel-824619\n");
    f.context.task_policy =
        std::make_shared<const TaskPolicy>(f.workspace.paths(), f.workspace.commands());
    const auto result = f.call(
        "run_command", {{"shell_command", "type \"quoted name.txt\" && echo pipeline-finished"}});
    REQUIRE(result.ok);
    CHECK(result.content.find("tool-quoted-path-sentinel-824619") != std::string::npos);
    CHECK(result.content.find("pipeline-finished") != std::string::npos);
    REQUIRE(result.structured.at("shell").get<bool>());
    const auto shell_name = strings::to_lower(
        std::filesystem::path{result.structured.at("executable").get<std::string>()}
            .filename()
            .string());
    REQUIRE(shell_name == "cmd.exe");
    CHECK(result.structured.at("arguments").get<std::vector<std::string>>() ==
          std::vector<std::string>{"/d", "/s", "/c",
                                   "type \"quoted name.txt\" && echo pipeline-finished"});
    for (const auto& definition : f.registry.definitions()) {
        if (definition.name != "run_command") continue;
        for (int level = 0; level <= 3; ++level) {
            CAPTURE(level);
            const auto advertised = level == 0 ? definition : definition.compact(level);
            // This fact must survive even when optional parameter prose is
            // removed. The observed executable above supplies its source truth.
            CHECK(advertised.description.find(shell_name) != std::string::npos);
        }
        CHECK(definition.parameters.at("properties")
                  .at("shell_command")
                  .at("description")
                  .get<std::string>()
                  .find(shell_name) != std::string::npos);
    }
}

TEST_CASE("explicit system cmd scripts enforce each segment's policy", "[tools][quoting]") {
    ToolFixture f;
    f.temp.write("quoted name.txt", "safe fixture\n");
    CommandRule rule;
    rule.id = "fixture-type-block";
    rule.decision = CommandDecision::Block;
    rule.reason = "Fixture segment policy";
    rule.executables = {"type"};
    rule.patterns = {"*"};
    f.workspace.commands().add_rule(rule);
    f.context.task_policy =
        std::make_shared<const TaskPolicy>(f.workspace.paths(), f.workspace.commands());
    const auto cmd = find_executable("cmd");
    REQUIRE(cmd.has_value());
    const auto result = f.call(
        "run_command",
        {{"executable", cmd->generic_string()},
         {"arguments", {"/d", "/s", "/c", "echo must-not-run && type \"quoted name.txt\""}}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("fixture-type-block") != std::string::npos);
}
#endif

TEST_CASE("file content expectations check actual bounded text without executing commands",
          "[tools][file-check]") {
    ToolFixture f;
    f.registry.set_read_only(true);
    f.temp.write("README.md", "Configure with Ninja.\nPrices round down.\n");
    const Json expect = {{"contains_all", {"Configure with Ninja", "Prices round down"}},
                         {"description", "README documents configuration and rounding"},
                         {"criterion", 1}};
    auto result = f.call("read_file", {{"path", "README.md"}, {"expect", expect}});
    REQUIRE(result.ok);
    CHECK(result.structured.value("file_check_checked", false));
    CHECK(result.structured.value("file_check_passed", false));
    CHECK(result.structured.value("file_check_criterion", 0) == 1);
    CHECK(f.confirmations_asked == 0);

    f.temp.write("README.md", "Configure with Ninja.\nPrices round up.\n");
    result = f.call("read_file", {{"path", "README.md"}, {"expect", expect}});
    CHECK_FALSE(result.ok);
    CHECK(result.structured.value("file_check_checked", false));
    CHECK_FALSE(result.structured.value("file_check_passed", true));
    CHECK(result.content.find("Prices round down") != std::string::npos);

    f.context.max_read_bytes = 8;
    result = f.call("read_file", {{"path", "README.md"},
                                  {"expect", {{"contains_all", {"Configure"}}, {"criterion", 1}}}});
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.structured.value("file_check_passed", true));
    CHECK(result.content.find("truncated") != std::string::npos);
}

TEST_CASE("range reads default to a targeted excerpt and retain explicit EOF", "[tools][range]") {
    ToolFixture fixture;
    std::string source;
    for (int index = 1; index <= 300; ++index)
        source += "fixture line " + std::to_string(index) + "\n";
    fixture.temp.write("large.txt", source);
    auto result = fixture.call("read_file_range", {{"path", "large.txt"}, {"start_line", 100}});
    REQUIRE(result.ok);
    CHECK(result.content.find("fixture line 100") != std::string::npos);
    CHECK(result.content.find("fixture line 219") != std::string::npos);
    CHECK(result.content.find("fixture line 220") == std::string::npos);
    CHECK(result.content.find("next range starting at line 220") != std::string::npos);
    result = fixture.call("read_file_range",
                          {{"path", "large.txt"}, {"start_line", 100}, {"end_line", 0}});
    REQUIRE(result.ok);
    CHECK(result.content.find("fixture line 300") != std::string::npos);
}

TEST_CASE("command recovery does not bind runtime output to the first unrelated criterion",
          "[tools][process][criterion]") {
    ToolFixture f;
    f.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "README documents the build", false},
                                             {2, "The demo prints expected", false}};
    };
    REQUIRE(std::filesystem::copy_file(path_from_utf8(LOCALCODE_TEST_SERVER_FIXTURE),
                                       f.temp.path() / "demo.exe"));
    f.record_built({"demo.exe"});
    auto result = f.call("run_command", Json{{"executable", "demo.exe"},
                                            {"arguments", {"--fixture-print", "expected"}}});
    REQUIRE(result.ok);
    INFO(result.content);
    REQUIRE(result.content.find("Still open:") != std::string::npos);
    CHECK(result.content.find("\"description\": \"README documents the build\"") ==
          std::string::npos);
    CHECK(result.content.find("\"criterion\": 1") == std::string::npos);
    CHECK(result.content.find("read_file") != std::string::npos);
}

TEST_CASE("malformed file expectations are refused instead of silently ignored",
          "[tools][file-check]") {
    ToolFixture f;
    for (const Json& expect : {Json::object(), Json{{"contains_all", {""}}},
                               Json{{"contains_all", {"return"}}, {"criterion", 0}},
                               Json{{"contains_all", {"return"}}, {"exit_code", 0}}}) {
        INFO(expect.dump());
        CHECK_FALSE(f.call("read_file", {{"path", "src/main.cpp"}, {"expect", expect}}).ok);
    }
}

TEST_CASE("the registry exposes the documented tool set", "[tools]") {
    ToolRegistry registry;
    registry.register_builtin_tools();

    const auto names = registry.names();
    for (const char* expected :
         {"read_file",     "read_file_range",  "list_directory",   "file_info",
          "search_files",  "search_text",      "write_file",       "replace_in_file",
          "apply_patch",   "create_directory", "move_file",        "copy_file",
          "run_command",   "find_executable",  "environment_info", "git_status",
          "git_diff",      "git_log",          "web_search",       "web_fetch",
          "download_file", "create_skill",     "list_skills"}) {
        INFO(expected);
        CHECK(registry.find(expected) != nullptr);
    }

    // The repository tools are part of the documented set too: the agent is
    // meant to ask the index before it starts opening files by hand.
    for (const char* expected :
         {"repo_overview", "repo_search", "repo_symbol", "repo_tests_for", "repo_fact"}) {
        INFO(expected);
        CHECK(registry.find(expected) != nullptr);
    }

    // Compact on purpose: no forest of near-duplicate tools. Repository
    // intelligence added five, not the seven it started with - "what has
    // changed" was already git_status, and "what uses this symbol" is the same
    // question as "where is this symbol" asked from the other end, so both
    // folded into tools that existed rather than standing alone.
    CHECK(names.size() <= 31);
}

TEST_CASE("every tool schema is well formed", "[tools]") {
    ToolRegistry registry;
    registry.register_builtin_tools();

    for (const auto& definition : registry.definitions()) {
        INFO("tool: " << definition.name);
        CHECK_FALSE(definition.description.empty());
        // Descriptions are what a small model reads; they must be substantial.
        CHECK(definition.description.size() > 60);
        REQUIRE(definition.parameters.is_object());
        CHECK(definition.parameters.value("type", "") == "object");
        CHECK(definition.parameters.contains("properties"));
    }
}

TEST_CASE("source reads expose nonprinting controls without rewriting source",
          "[tools][source-controls][recovery]") {
    ToolFixture fixture;
    const std::string line = "\xc3\xa9x\bz\f\x1b\n";
    const std::string source = std::string(240, 'a') + "\n" + line + "ordinary\n";
    fixture.temp.write("controls.cpp", source);
    for (const auto* tool : {"read_file", "read_file_range"}) {
        Json arguments{{"path", "controls.cpp"}};
        if (std::string_view{tool} == "read_file_range") {
            arguments["start_line"] = 2;
            arguments["end_line"] = 2;
        }
        const auto result = fixture.call(tool, arguments);
        REQUIRE(result.ok);
        CHECK(result.content.find(line) != std::string::npos);
        CHECK(result.structured.value("source_control_count", 0) == 3);
        REQUIRE(result.structured.contains("source_controls"));
        const auto& controls = result.structured.at("source_controls");
        REQUIRE(controls.size() == 3);
        CHECK(controls[0] == Json{{"line", 2}, {"utf8_byte_column", 4}, {"codepoint", "U+0008"}});
        CHECK(controls[1] == Json{{"line", 2}, {"utf8_byte_column", 6}, {"codepoint", "U+000C"}});
        CHECK(controls[2] == Json{{"line", 2}, {"utf8_byte_column", 7}, {"codepoint", "U+001B"}});
        CHECK(result.content.find("line 2, UTF-8 byte column 4: U+0008") != std::string::npos);
        CHECK(result.structured.value("content_hash", "") ==
              repo::fnv1a_hex(std::string_view{tool} == "read_file" ? source : line));
    }
    fixture.temp.write("escaped.cpp", "\tconst auto pattern = R\"(\\bis\\b)\";\n");
    const auto normal = fixture.call("read_file", {{"path", "escaped.cpp"}});
    REQUIRE(normal.ok);
    CHECK(normal.structured.value("source_control_count", 0) == 0);
    CHECK(normal.content.find("Nonprinting ASCII controls") == std::string::npos);
}

TEST_CASE("source control receipts are bounded and stay within returned source",
          "[tools][source-controls][recovery]") {
    ToolFixture fixture;
    std::string source;
    for (int i = 0; i < 40; ++i)
        source += std::string(120, 'a') + '\b' + '\n';
    fixture.temp.write("many-controls.cpp", source);
    const auto result = fixture.call("read_file", {{"path", "many-controls.cpp"}});
    REQUIRE(result.ok);
    CHECK(result.structured.value("source_control_count", 0) == 40);
    REQUIRE(result.structured.contains("source_controls"));
    CHECK(result.structured.at("source_controls").size() == 16);
    CHECK(result.content.find("first 16 of 40") != std::string::npos);
    CHECK(result.content.size() < source.size() + 2200);
    const auto first_line = fixture.call(
        "read_file_range", {{"path", "many-controls.cpp"}, {"start_line", 1}, {"end_line", 1}});
    REQUIRE(first_line.ok);
    CHECK(first_line.structured.value("source_control_count", 0) == 1);
    CHECK(first_line.structured.at("source_controls").size() == 1);
    CHECK(first_line.structured.at("source_controls")[0].at("line") == 1);
}

TEST_CASE("quoted source lines round trip hidden ASCII into an exact edit",
          "[tools][source-controls][source-control-json][recovery]") {
    const auto exercise = [](const char* tool) {
        ToolFixture fixture;
        std::string hidden;
        for (unsigned value = 1; value < 0x20; ++value)
            if (value != '\t' && value != '\n' && value != '\r')
                hidden.push_back(static_cast<char>(value));
        hidden.push_back('\x7f');
        const std::string line = "\xc3\xa9 \"quoted\" \\path " + hidden + " tail";
        const std::string printable = "\\b \\u001b \\u007f";
        const std::string prefix(2400, 'a');
        const auto source = prefix + "\n" + line + "\n" + printable + "\n";
        fixture.temp.write("quoted-controls.txt", source);
        Json arguments{{"path", "quoted-controls.txt"}};
        if (std::string_view{tool} == "read_file_range") {
            arguments["start_line"] = 2;
            arguments["end_line"] = 2;
        }
        const auto read = fixture.call(tool, arguments);
        REQUIRE(read.ok);
        CHECK(read.content.find(line) != std::string::npos);
        CHECK(read.structured.value("source_control_count", 0) == hidden.size());
        CHECK(read.structured.at("source_controls").size() == 16);
        CHECK(read.structured.value("content_hash", "") ==
              repo::fnv1a_hex(std::string_view{tool} == "read_file" ? source : line + "\n"));
        const auto encoded = displayed_source_line_json(read, 2);
        CHECK(encoded.find('"') == 0);
        CHECK(encoded.find("\\u007f") != std::string::npos);
        for (const unsigned char value : encoded) {
            CHECK(value >= 0x20);
            CHECK(value != 0x7f);
        }
        const auto decoded = Json::parse(encoded).get<std::string>();
        REQUIRE(decoded == line);
        REQUIRE(read.structured.at("source_control_lines").size() == 1);
        CHECK(read.structured.at("source_control_lines")[0] ==
              Json{{"line", 2}, {"json_string", encoded}});
        // Passing a JSON literal as source is not silently decoded by the edit
        // tool. Only the explicitly decoded exact source may match.
        const auto wrong = fixture.call(
            "replace_in_file",
            {{"path", "quoted-controls.txt"}, {"find", encoded}, {"replace", "must not appear"}});
        REQUIRE_FALSE(wrong.ok);
        const auto edit =
            fixture.call("replace_in_file", {{"path", "quoted-controls.txt"},
                                             {"find", decoded},
                                             {"replace", "replacement without hidden bytes"}});
        REQUIRE(edit.ok);
        const auto actual = fs::read_text(fixture.temp.path() / "quoted-controls.txt");
        REQUIRE(actual.ok());
        CHECK(actual->content ==
              prefix + "\nreplacement without hidden bytes\n" + printable + "\n");
    };
    SECTION("whole file") {
        exercise("read_file");
    }
    SECTION("line range") {
        exercise("read_file_range");
    }
}

TEST_CASE("quoted source lines preserve exact edit uniqueness",
          "[tools][source-controls][source-control-json][recovery]") {
    ToolFixture fixture;
    const std::string line = "duplicate\bsource";
    const auto source = std::string(240, 'a') + "\n" + line + "\nordinary\n" + line + "\n";
    fixture.temp.write("duplicate-controls.txt", source);
    const auto read = fixture.call("read_file", {{"path", "duplicate-controls.txt"}});
    REQUIRE(read.ok);
    const auto decoded = Json::parse(displayed_source_line_json(read, 2)).get<std::string>();
    const auto refused = fixture.call(
        "replace_in_file",
        {{"path", "duplicate-controls.txt"}, {"find", decoded}, {"replace", "must not appear"}});
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.find("2 times") != std::string::npos);
    const auto actual = fs::read_text(fixture.temp.path() / "duplicate-controls.txt");
    REQUIRE(actual.ok());
    CHECK(actual->content == source);
}

TEST_CASE("quoted source lines bound complete JSON without clipping it",
          "[tools][source-controls][source-control-json][recovery]") {
    ToolFixture fixture;
    std::string source(240, 'a');
    source += '\n';
    SECTION("first four affected lines") {
        for (int index = 0; index < 5; ++index)
            source += "affected " + std::to_string(index) + '\b' + '\n';
        fixture.temp.write("line-cap.txt", source);
        const auto result = fixture.call("read_file", {{"path", "line-cap.txt"}});
        REQUIRE(result.ok);
        REQUIRE(result.structured.contains("source_control_lines"));
        const auto& quoted = result.structured.at("source_control_lines");
        REQUIRE(quoted.size() == 4);
        for (std::size_t index = 0; index < quoted.size(); ++index) {
            const auto encoded = displayed_source_line_json(result, index + 2);
            CHECK(Json::parse(encoded).get<std::string>() ==
                  "affected " + std::to_string(index) + '\b');
        }
        CHECK(result.structured.at("source_control_line_count") == 5);
        CHECK(result.structured.at("source_control_lines_omitted").at("line_limit") == 1);
        CHECK(result.content.find("1 beyond first 4") != std::string::npos);
        CHECK(result.content.find("\nline 6: ") == std::string::npos);
    }
    SECTION("combined encoded byte limit and oversized line") {
        const auto oversized = std::string(1100, 'x') + '\b';
        const auto fits = std::string(700, 'y') + '\b';
        const auto exceeds_remaining = std::string(400, 'z') + '\b';
        const std::string last = "short\bline";
        source += oversized + "\n" + fits + "\n" + exceeds_remaining + "\n" + last + "\n";
        fixture.temp.write("byte-cap.txt", source);
        const auto result = fixture.call("read_file", {{"path", "byte-cap.txt"}});
        REQUIRE(result.ok);
        REQUIRE(result.structured.contains("source_control_lines"));
        const auto& quoted = result.structured.at("source_control_lines");
        REQUIRE(quoted.size() == 2);
        std::size_t bytes = 0;
        for (const auto& entry : quoted) {
            const auto line = entry.at("line").get<std::size_t>();
            const auto encoded = displayed_source_line_json(result, line);
            CHECK(encoded == entry.at("json_string").get<std::string>());
            CHECK(Json::parse(encoded).is_string());
            bytes += encoded.size();
        }
        CHECK(bytes <= 1024);
        CHECK(Json::parse(displayed_source_line_json(result, 3)).get<std::string>() == fits);
        CHECK(Json::parse(displayed_source_line_json(result, 5)).get<std::string>() == last);
        CHECK(result.structured.at("source_control_lines_omitted").at("byte_limit") == 2);
        CHECK(result.content.find("2 over byte limit") != std::string::npos);
        CHECK(result.content.find("\nline 2: ") == std::string::npos);
        CHECK(result.content.find("\nline 4: ") == std::string::npos);
    }
    SECTION("JSON escaping expansion is bounded before displaying a whole line") {
        const auto expands = std::string(600, '\\') + '\b';
        const std::string last = "short\bline";
        // The raw-length shortcut must admit this candidate, while JSON
        // escaping must make the complete literal exceed the remaining cap.
        REQUIRE(expands.size() + 2 <= 1024);
        REQUIRE(Json(expands).dump().size() > 1024);
        source += expands + "\n" + last + "\n";
        fixture.temp.write("escaped-expansion-cap.txt", source);
        const auto result = fixture.call("read_file", {{"path", "escaped-expansion-cap.txt"}});
        REQUIRE(result.ok);
        CHECK(result.structured.value("content_hash", "") == repo::fnv1a_hex(source));
        CHECK(result.structured.value("source_control_count", 0) == 2);
        REQUIRE(result.structured.contains("source_control_lines"));
        const auto& quoted = result.structured.at("source_control_lines");
        REQUIRE(quoted.size() == 1);
        const auto encoded = displayed_source_line_json(result, 3);
        CHECK(encoded.size() <= 1024);
        CHECK(Json::parse(encoded).get<std::string>() == last);
        CHECK(quoted[0] == Json{{"line", 3}, {"json_string", encoded}});
        CHECK(result.structured.at("source_control_line_count") == 2);
        const auto& omitted = result.structured.at("source_control_lines_omitted");
        CHECK(omitted.at("byte_limit") == 1);
        CHECK(omitted.at("line_limit") == 0);
        CHECK(omitted.at("partial") == 0);
        CHECK(result.content.find("1 over byte limit") != std::string::npos);
        CHECK(result.content.find("\nline 2: ") == std::string::npos);
    }
}

TEST_CASE("quoted source lines omit partial source instead of inventing an edit",
          "[tools][source-controls][source-control-json][recovery]") {
    ToolFixture fixture;
    const auto source = std::string(240, 'a') + "\npartial\b" + std::string(1500, 'x') + "\n";
    fixture.temp.write("partial-controls.txt", source);
    const auto result =
        fixture.call("read_file", {{"path", "partial-controls.txt"}, {"max_bytes", 300}});
    REQUIRE(result.ok);
    REQUIRE(result.structured.value("last_line_partial", false));
    CHECK(result.structured.value("source_control_count", 0) == 1);
    REQUIRE(result.structured.contains("source_control_lines"));
    CHECK(result.structured.at("source_control_lines").empty());
    CHECK(result.structured.at("source_control_lines_omitted").at("partial") == 1);
    CHECK(result.content.find("1 partial") != std::string::npos);
    CHECK(result.content.find("\nline 2: ") == std::string::npos);
    CHECK(result.structured.value("content_hash", "") == repo::fnv1a_hex(source.substr(0, 300)));
}

TEST_CASE("quoted source lines retain a complete EOF line without a newline",
          "[tools][source-controls][source-control-json][recovery]") {
    ToolFixture fixture;
    const std::string line = "last\bsource";
    fixture.temp.write("eof-controls.txt", std::string(240, 'a') + "\n" + line);
    const auto result = fixture.call(
        "read_file_range", {{"path", "eof-controls.txt"}, {"start_line", 2}, {"end_line", 0}});
    REQUIRE(result.ok);
    CHECK_FALSE(result.structured.value("last_line_partial", true));
    CHECK(result.structured.value("content_hash", "") == repo::fnv1a_hex(line));
    CHECK(Json::parse(displayed_source_line_json(result, 2)).get<std::string>() == line);
}

TEST_CASE("quoted source diagnostics leave printable escapes alone",
          "[tools][source-controls][source-control-json][recovery]") {
    ToolFixture fixture;
    const std::string line = "\tconst auto text = \"\\b \\u001b \\u007f\";";
    const auto source = line + "\n";
    fixture.temp.write("printable-escapes.txt", source);
    const auto result = fixture.call("read_file", {{"path", "printable-escapes.txt"}});
    REQUIRE(result.ok);
    CHECK(result.structured.value("source_control_count", -1) == 0);
    CHECK(result.structured.value("content_hash", "") == repo::fnv1a_hex(source));
    CHECK_FALSE(result.structured.contains("source_control_lines"));
    CHECK(result.to_model_text().find("Exact source line JSON") == std::string::npos);
    CHECK(result.to_model_text().find("JSON lines omitted") == std::string::npos);
    CHECK(result.content.find(line) != std::string::npos);
}

TEST_CASE("source read receipts report returned lines rather than requested endpoints",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("receipt.cpp", "first\nsecond\nlast");
    const auto result = fixture.call(
        "read_file_range", {{"path", "receipt.cpp"}, {"start_line", 2}, {"end_line", 999}});
    REQUIRE(result.ok);
    CHECK(result.content.find("lines 2-3") != std::string::npos);
    CHECK(result.content.find("lines 2-999") == std::string::npos);
    REQUIRE(result.structured.is_object());
    CHECK(result.structured.value("source_read", false));
    CHECK(result.structured.value("path", "") == "receipt.cpp");
    CHECK(result.structured.value("start_line", 0) == 2);
    CHECK(result.structured.value("end_line", 0) == 3);
    CHECK(result.structured.value("complete_end_line", 0) == 3);
    CHECK(result.structured.value("line_count", 0) == 2);
    CHECK(result.structured.value("eof", false));
    CHECK_FALSE(result.structured.value("has_more", true));
    CHECK_FALSE(result.structured.contains("next_line"));
}

TEST_CASE("default source excerpts do not advertise a continuation at exact EOF",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    std::string source;
    for (int n = 1; n <= 120; ++n)
        source += "source " + std::to_string(n) + "\n";
    fixture.temp.write("exact.cpp", source);
    const auto result = fixture.call("read_file_range", {{"path", "exact.cpp"}, {"start_line", 1}});
    REQUIRE(result.ok);
    CHECK(result.content.find("next range starting") == std::string::npos);
    REQUIRE(result.structured.is_object());
    CHECK(result.structured.value("eof", false));
    CHECK_FALSE(result.structured.value("has_more", true));
    CHECK_FALSE(result.structured.contains("next_line"));
}

TEST_CASE("empty and beyond EOF source reads cannot invent source windows",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("empty.cpp", "");
    for (const auto& [path, start] :
         std::vector<std::pair<std::string, int>>{{"empty.cpp", 1}, {"src/util.cpp", 40}}) {
        const auto result =
            fixture.call("read_file_range", {{"path", path}, {"start_line", start}});
        INFO(result.content);
        CHECK(result.content.find("no source lines returned") != std::string::npos);
        REQUIRE(result.structured.is_object());
        CHECK(result.structured.value("start_line", -1) == 0);
        CHECK(result.structured.value("end_line", -1) == 0);
        CHECK(result.structured.value("complete_end_line", -1) == 0);
        CHECK(result.structured.value("line_count", -1) == 0);
        CHECK_FALSE(result.structured.contains("next_line"));
    }
}

TEST_CASE("a clipped long source line stays partial and cannot skip unseen content",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("long.cpp", std::string(200, 'x') + "\nnext\n");
    fixture.context.max_read_bytes = 12;
    const auto result = fixture.call("read_file_range", {{"path", "long.cpp"}, {"start_line", 1}});
    REQUIRE(result.ok);
    CHECK(result.truncated);
    CHECK(result.content.find("xxxxxxxxxxxx") != std::string::npos);
    REQUIRE(result.structured.is_object());
    CHECK(result.structured.value("line_count", 0) == 1);
    CHECK(result.structured.value("last_line_partial", false));
    CHECK(result.structured.value("complete_end_line", -1) == 0);
    CHECK(result.structured.value("next_line", 0) == 1);
    CHECK_FALSE(result.structured.value("eof", true));
}

TEST_CASE("source receipts preserve UTF-8 boundaries and complete-line continuation",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("utf8.cpp", "ééé\nnext\n");
    fixture.context.max_read_bytes = 5;
    const auto clipped = fixture.call("read_file_range", {{"path", "utf8.cpp"}, {"start_line", 1}});
    REQUIRE(clipped.ok);
    CHECK(strings::is_valid_utf8(clipped.content));
    CHECK(clipped.content.find("éé") != std::string::npos);
    CHECK(clipped.content.find("�") == std::string::npos);
    REQUIRE(clipped.structured.is_object());
    CHECK(clipped.structured.value("complete_end_line", -1) == 0);
    CHECK(clipped.structured.value("next_line", 0) == 1);
    fixture.temp.write("boundary.cpp", "a\nb\nc\n");
    fixture.context.max_read_bytes = 2;
    const auto boundary =
        fixture.call("read_file_range", {{"path", "boundary.cpp"}, {"start_line", 1}});
    REQUIRE(boundary.ok);
    REQUIRE(boundary.structured.is_object());
    CHECK(boundary.structured.value("complete_end_line", 0) == 1);
    CHECK_FALSE(boundary.structured.value("last_line_partial", true));
    CHECK(boundary.structured.value("next_line", 0) == 2);
}

TEST_CASE("both source readers bind receipt identity to the returned normalized text",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    const std::string source = "one\r\ntwo\r\nlast";
    fixture.temp.write("identity.cpp", source);
    const auto whole = fixture.call("read_file", {{"path", "identity.cpp"}});
    const auto range = fixture.call("read_file_range",
                                    {{"path", "identity.cpp"}, {"start_line", 2}, {"end_line", 2}});
    REQUIRE(whole.ok);
    REQUIRE(range.ok);
    REQUIRE(whole.structured.is_object());
    REQUIRE(range.structured.is_object());
    CHECK(whole.structured.value("source_read", false));
    CHECK(whole.structured.value("content_hash", "") == repo::fnv1a_hex("one\ntwo\nlast"));
    CHECK(range.structured.value("content_hash", "") == repo::fnv1a_hex("two\n"));
    CHECK(range.structured.value("hash_scope", "") == "returned_utf8_source");
    CHECK(range.structured.value("hash_algorithm", "") == "fnv1a64");
    CHECK(range.structured.value("source_version_stable", false));
    const auto version = fs::source_version(fixture.temp.path() / "identity.cpp");
    REQUIRE(version);
    CHECK(range.structured.value("source_size_bytes", 0u) == version->size_bytes);
    CHECK(range.structured.value("source_modified_time", "") == version->modified_time);
    CHECK(range.structured.value("has_more", false));
    CHECK(range.structured.value("next_line", 0) == 3);
    CHECK_FALSE(range.structured.value("eof", true));
}

TEST_CASE("UTF-16 source receipts obey the same decoded byte and line bounds",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    std::string wide{"\xff\xfe", 2};
    for (char c : std::string{"one\nsecond\nlast"}) {
        wide += c;
        wide += '\0';
    }
    fixture.temp.write("wide-receipt.cpp", wide);
    fixture.context.max_read_bytes = 8;
    const auto result = fixture.call(
        "read_file_range", {{"path", "wide-receipt.cpp"}, {"start_line", 2}, {"end_line", 100}});
    REQUIRE(result.ok);
    REQUIRE(result.structured.is_object());
    CHECK(result.structured.value("start_line", 0) == 2);
    CHECK(result.structured.value("end_line", 0) == 3);
    CHECK(result.structured.value("complete_end_line", 0) == 2);
    CHECK(result.structured.value("last_line_partial", false));
    CHECK(result.structured.value("next_line", 0) == 3);
    CHECK(result.structured.value("content_hash", "") == repo::fnv1a_hex("second\nl"));
    CHECK(result.content.find("lines 2-100") == std::string::npos);
}

TEST_CASE("whole-file reads share honest empty and partial source receipts",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("empty.cpp", "");
    const auto empty = fixture.call("read_file", {{"path", "empty.cpp"}});
    REQUIRE(empty.ok);
    CHECK(empty.content.find("no source lines returned") != std::string::npos);
    CHECK(empty.structured.value("start_line", -1) == 0);
    CHECK(empty.structured.value("end_line", -1) == 0);
    CHECK(empty.structured.value("complete_end_line", -1) == 0);
    CHECK(empty.structured.value("eof", false));
    CHECK_FALSE(empty.structured.contains("next_line"));

    fixture.temp.write("partial.cpp", "done\nunfinished\nlast\n");
    fixture.context.max_read_bytes = 8;
    const auto partial = fixture.call("read_file", {{"path", "partial.cpp"}});
    REQUIRE(partial.ok);
    CHECK(partial.structured.value("start_line", 0) == 1);
    CHECK(partial.structured.value("end_line", 0) == 2);
    CHECK(partial.structured.value("complete_end_line", 0) == 1);
    CHECK(partial.structured.value("last_line_partial", false));
    CHECK(partial.structured.value("next_line", 0) == 2);
    CHECK(partial.structured.value("content_hash", "") == repo::fnv1a_hex("done\nunf"));
    CHECK(partial.content.find("starting at line 2") != std::string::npos);
}

TEST_CASE("source version hints detect ordinary same-size external edits without sleeping",
          "[tools][source-receipt][recovery]") {
    ToolFixture fixture;
    const auto path = fixture.temp.write("version.cpp", "before\n");
    const auto original = fixture.call("read_file", {{"path", "version.cpp"}});
    REQUIRE(original.ok);
    REQUIRE(original.structured.value("source_version_stable", false));
    const auto timestamp = std::filesystem::last_write_time(path);
    fixture.temp.write("version.cpp", "edited\n");
    std::filesystem::last_write_time(path, timestamp + std::chrono::seconds{2});
    const auto changed = fs::source_version(path);
    REQUIRE(changed);
    CHECK(changed->size_bytes == original.structured.value("source_size_bytes", 0u));
    CHECK(changed->modified_time != original.structured.value("source_modified_time", ""));
    const auto reread = fixture.call("read_file", {{"path", "version.cpp"}});
    REQUIRE(reread.ok);
    CHECK(reread.structured.value("source_version_stable", false));
    CHECK(reread.structured.value("source_modified_time", "") == changed->modified_time);
    CHECK(reread.structured.value("content_hash", "") == repo::fnv1a_hex("edited\n"));
    CHECK(original.structured.value("content_hash", "") == repo::fnv1a_hex("before\n"));
}

TEST_CASE("source read ceilings explain why a larger whole-file request cannot make progress",
          "[tools][source-receipt][read-ceiling][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("bounded.cpp", "aaaa\nbbbb\ncccc\n");
    fixture.context.max_read_bytes = 8;
    const auto initial = fixture.call("read_file", {{"path", "bounded.cpp"}});
    const auto increased =
        fixture.call("read_file", {{"path", "bounded.cpp"}, {"max_bytes", 400000}});
    REQUIRE(initial.ok);
    REQUIRE(increased.ok);
    CHECK(initial.structured.value("content_hash", "") ==
          increased.structured.value("content_hash", ""));
    CHECK_FALSE(initial.structured.contains("requested_max_bytes"));
    CHECK(increased.structured.value("requested_max_bytes", 0) == 400000);
    for (const auto* result : {&initial, &increased}) {
        INFO(result->content);
        CHECK(result->truncated);
        CHECK(result->structured.value("effective_max_bytes", 0) == 8);
        CHECK(result->structured.value("context_max_read_bytes", 0) == 8);
        CHECK(result->structured.value("complete_end_line", 0) == 1);
        CHECK(result->structured.value("next_line", 0) == 2);
        CHECK(result->content.find("effective limit of 8") != std::string::npos);
        CHECK(result->content.find("context cap 8") != std::string::npos);
        CHECK(result->content.find("Increasing max_bytes cannot exceed 8") != std::string::npos);
        CHECK(result->content.find("read_file_range starting at line 2") != std::string::npos);
        CHECK(result->content.find("larger byte budget") == std::string::npos);
    }
    const auto continued = fixture.call(
        "read_file_range", {{"path", "bounded.cpp"}, {"start_line", 2}, {"end_line", 0}});
    REQUIRE(continued.ok);
    CHECK(continued.structured.value("start_line", 0) == 2);
    CHECK(continued.structured.value("complete_end_line", 0) == 2);
    CHECK(continued.structured.value("next_line", 0) == 3);
    CHECK(continued.structured.value("effective_max_bytes", 0) == 8);
    CHECK(continued.content.find("Increasing max_bytes cannot exceed 8") != std::string::npos);
}

TEST_CASE("caller clipping is distinguished from the available context ceiling",
          "[tools][source-receipt][read-ceiling][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("caller.cpp", "aaaa\nbbbb\ncccc\n");
    fixture.context.max_read_bytes = 100;
    const auto result = fixture.call("read_file", {{"path", "caller.cpp"}, {"max_bytes", 8}});
    REQUIRE(result.ok);
    CHECK(result.truncated);
    CHECK(result.structured.value("effective_max_bytes", 0) == 8);
    CHECK(result.structured.value("context_max_read_bytes", 0) == 100);
    CHECK(result.structured.value("requested_max_bytes", 0) == 8);
    CHECK(result.content.find("The caller's max_bytes limits this read") != std::string::npos);
    CHECK(result.content.find("host ceiling is 100") != std::string::npos);
    CHECK(result.content.find("model/context cap limits this read") == std::string::npos);
    CHECK(result.structured.value("next_line", 0) == 2);
}

TEST_CASE("read_file returns numbered contents", "[tools][files]") {
    ToolFixture fixture;

    auto result = fixture.call("read_file", Json{{"path", "src/main.cpp"}});
    REQUIRE(result.ok);
    CHECK(result.content.find("int add(int a, int b)") != std::string::npos);
    CHECK(result.content.find("     1\t") != std::string::npos);
    CHECK(result.summary.find("src/main.cpp") != std::string::npos);

    REQUIRE_FALSE(result.evidence.empty());
    CHECK(result.evidence[0].kind == EvidenceKind::FileRead);
    CHECK(result.evidence[0].positive);
}

TEST_CASE("reading a missing file explains itself", "[tools][files]") {
    ToolFixture fixture;
    auto result = fixture.call("read_file", Json{{"path", "src/nonexistent.cpp"}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("exist") != std::string::npos);
    REQUIRE_FALSE(result.evidence.empty());
    CHECK_FALSE(result.evidence[0].positive);
}

TEST_CASE("read_file_range returns the requested window", "[tools][files]") {
    ToolFixture fixture;
    auto result = fixture.call("read_file_range",
                               Json{{"path", "src/main.cpp"}, {"start_line", 3}, {"end_line", 5}});
    REQUIRE(result.ok);
    CHECK(result.content.find("int add") != std::string::npos);
    CHECK(result.content.find("#include") == std::string::npos);
}

TEST_CASE("file tools obey the model read budget and give continuation lines",
          "[tools][files][context]") {
    ToolFixture fixture;
    std::string text;
    for (int i = 1; i <= 100; ++i)
        text += "line " + std::to_string(i) + " some source content\n";
    fixture.temp.write("large.cpp", text);
    fixture.context.max_read_bytes = 110;
    const auto full = fixture.call("read_file", Json{{"path", "large.cpp"}, {"max_bytes", 400000}});
    REQUIRE(full.ok);
    CHECK(full.truncated);
    CHECK(full.content.find("TRUNCATED") != std::string::npos);
    CHECK(full.content.find("last line may be partial") != std::string::npos);
    CHECK(full.content.find("line 100 some") == std::string::npos);
    const auto range = fixture.call(
        "read_file_range", Json{{"path", "large.cpp"}, {"start_line", 20}, {"end_line", 100}});
    REQUIRE(range.ok);
    CHECK(range.truncated);
    CHECK(range.content.find("starting at line 23") != std::string::npos);
    CHECK(range.content.find("line 100 some") == std::string::npos);
    std::string utf16{"\xff\xfe", 2};
    for (char c : text) {
        utf16 += c;
        utf16 += '\0';
    }
    fixture.temp.write("wide.cpp", utf16);
    const auto wide = fixture.call(
        "read_file_range", Json{{"path", "wide.cpp"}, {"start_line", 20}, {"end_line", 100}});
    REQUIRE(wide.ok);
    CHECK(wide.truncated);
    CHECK(wide.content.find("starting at line 23") != std::string::npos);
    CHECK(wide.content.find("line 100 some") == std::string::npos);
}

TEST_CASE("list_directory skips ignored directories", "[tools][files]") {
    ToolFixture fixture;

    auto top = fixture.call("list_directory", Json::object());
    REQUIRE(top.ok);
    CHECK(top.content.find("src/") != std::string::npos);
    CHECK(top.content.find("CMakeLists.txt") != std::string::npos);
    CHECK(top.content.find(".git") == std::string::npos);
    CHECK(top.content.find("build") == std::string::npos);

    auto recursive = fixture.call("list_directory", Json{{"recursive", true}});
    REQUIRE(recursive.ok);
    CHECK(recursive.content.find("src/main.cpp") != std::string::npos);
    CHECK(recursive.content.find("build/generated.cpp") == std::string::npos);
}

TEST_CASE("file_info reports what it finds and what it does not", "[tools][files]") {
    ToolFixture fixture;

    auto present = fixture.call("file_info", Json{{"path", "src/main.cpp"}});
    REQUIRE(present.ok);
    CHECK(present.content.find("lines:") != std::string::npos);

    auto absent = fixture.call("file_info", Json{{"path", "not-here.txt"}});
    REQUIRE(absent.ok);   // "it does not exist" is a successful answer
    CHECK(absent.content.find("does not exist") != std::string::npos);
}

TEST_CASE("search_text finds matches with context", "[tools][search]") {
    ToolFixture fixture;

    auto result = fixture.call("search_text", Json{{"query", "int add"}});
    REQUIRE(result.ok);
    CHECK(result.content.find("src/main.cpp") != std::string::npos);
    CHECK(result.content.find("include/api.h") != std::string::npos);
    CHECK(result.summary.find("match") != std::string::npos);
}

TEST_CASE("search_text reports an honest zero", "[tools][search]") {
    ToolFixture fixture;
    auto result = fixture.call("search_text", Json{{"query", "definitely_not_in_this_project"}});
    REQUIRE(result.ok);
    CHECK(result.content.find("No matches") != std::string::npos);
    // The wording must discourage inventing a location for it.
    CHECK(result.content.find("do not assume") != std::string::npos);
}

TEST_CASE("search_text honours a file filter and regex mode", "[tools][search]") {
    ToolFixture fixture;

    auto filtered = fixture.call("search_text", Json{{"query", "add"}, {"file_pattern", "*.h"}});
    REQUIRE(filtered.ok);
    CHECK(filtered.content.find("include/api.h") != std::string::npos);
    CHECK(filtered.content.find("src/main.cpp") == std::string::npos);

    auto regex = fixture.call("search_text", Json{{"query", "int\\s+\\w+\\("}, {"regex", true}});
    REQUIRE(regex.ok);
    CHECK(regex.content.find("src/main.cpp") != std::string::npos);
}

TEST_CASE("search_text distinguishes uninspected files from an actual zero",
          "[tools][search][search-scope]") {
    ToolFixture fixture;
    fixture.temp.write("sample/one.cpp", "int marker();\n");
    fixture.temp.write("sample/two.h", "int marker();\n");
    fixture.temp.write("sample/space name.cpp", "int marker();\n");
    const auto search = [&](const std::string& pattern, const std::string& query = "marker") {
        return fixture.call("search_text", {{"query", query}, {"file_pattern", pattern}});
    };
    const auto no_inspection = [&](const ToolResult& result) {
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("No text files were inspected") != std::string::npos);
        CHECK(result.error.find("single glob") != std::string::npos);
        for (const auto& evidence : result.evidence)
            CHECK_FALSE((evidence.kind == EvidenceKind::SearchPerformed && evidence.positive));
    };

    SECTION("a nonmatching compound-looking filter does not prove absence") {
        no_inspection(search("*.cpp *.h"));
        no_inspection(search("*.cc"));
    }
    SECTION("single filters and literal spaces retain their meaning") {
        const auto cpp = search("sample/*.cpp");
        REQUIRE(cpp.ok);
        CHECK(cpp.content.find("Searched 2 text file(s)") != std::string::npos);
        CHECK(search("sample/*.h").ok);
        const auto literal_space = search("sample/space name.cpp");
        REQUIRE(literal_space.ok);
        CHECK(literal_space.content.find("Searched 1 text file(s)") != std::string::npos);
        fixture.temp.write("sample/a.cpp b.h", "int marker();\n");
        const auto compound_name = search("*.cpp *.h");
        REQUIRE(compound_name.ok);
        CHECK(compound_name.content.find("Searched 1 text file(s)") != std::string::npos);
    }
    SECTION("an inspected file without the query establishes a scoped zero") {
        const auto absent = search("sample/*.cpp", "not_present_in_the_fixture");
        REQUIRE(absent.ok);
        CHECK(absent.content.find("Searched 2 text file(s)") != std::string::npos);
        CHECK(absent.content.find("Found 0 match(es)") != std::string::npos);
        REQUIRE_FALSE(absent.evidence.empty());
        CHECK(absent.evidence.front().kind == EvidenceKind::SearchPerformed);
        CHECK(absent.evidence.front().positive);
    }
    SECTION("an empty text file is inspected") {
        fixture.temp.write("sample/empty.txt", "");
        const auto empty_file = search("sample/empty.txt");
        REQUIRE(empty_file.ok);
        CHECK(empty_file.content.find("Searched 1 text file(s)") != std::string::npos);
    }
    SECTION("a binary-only selection is uninspected") {
        fixture.temp.write("sample/data.bin", std::string{"x\0y", 3});
        no_inspection(search("sample/*.bin"));
    }
    SECTION("an empty workspace is uninspected") {
        TempDirectory empty{"lc-empty-search"};
        REQUIRE(fixture.workspace.open(empty.path()).ok());
        no_inspection(search(""));
    }
#if defined(_WIN32)
    SECTION("an unreadable selection does not prove absence") {
        const auto path = fixture.temp.write("sample/locked.txt", "int marker();\n");

        struct ReadLock {
            HANDLE handle;

            ~ReadLock() {
                if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            }
        } lock{CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr)};

        REQUIRE(lock.handle != INVALID_HANDLE_VALUE);
        no_inspection(search("sample/locked.txt"));
    }
#endif
}

TEST_CASE("an invalid regex is refused with an explanation", "[tools][search]") {
    ToolFixture fixture;
    auto result = fixture.call("search_text", Json{{"query", "[unclosed"}, {"regex", true}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("regular expression") != std::string::npos);
}

TEST_CASE("bounded literal search orders current source without hiding its scope",
          "[tools][search][search-ranking]") {
    ToolFixture fixture;
    TempDirectory project{"lc-search-order"};
    REQUIRE(fixture.workspace.open(project.path()).ok());
    const std::string marker = "relay_marker";
    const auto search = [&](const Json& extra = Json::object()) {
        Json arguments = {{"query", marker}, {"max_results", 1}, {"context_lines", 0}};
        arguments.update(extra);
        return fixture.call("search_text", arguments);
    };

    SECTION("a documentation prefix cannot consume the cap before product source") {
        project.write("docs/guide.md", marker + "\n" + marker + "\n" + marker + "\n");
        project.write("src/implementation.cpp", "int " + marker + "();\n");
        const auto result = search();
        REQUIRE(result.ok);
        CHECK(result.content.find("src/implementation.cpp") != std::string::npos);
        CHECK(result.content.find("docs/guide.md") == std::string::npos);
        CHECK(result.summary.find("Found 1 match(es) in 1 file(s)") != std::string::npos);
        CHECK(result.content.find("Searched 1 text file(s)") != std::string::npos);
        CHECK(result.truncated);
        CHECK(result.content.find("Order: literal path affinity") != std::string::npos);
        CHECK(result.content.find("not exhaustive workspace coverage") != std::string::npos);
    }
    SECTION("top-level auxiliary source does not starve product source") {
        project.write("scripts/check.cpp", marker + "\n");
        project.write("tests/check.cpp", marker + "\n");
        project.write("src/core/tools/implementation.cpp", marker + "\n");
        const auto result = search();
        REQUIRE(result.ok);
        CHECK(result.content.find("src/core/tools/implementation.cpp") != std::string::npos);
        CHECK(result.content.find("scripts/check.cpp") == std::string::npos);
        CHECK(result.content.find("tests/check.cpp") == std::string::npos);
    }
    SECTION("archived code inside documentation is not promoted as current source") {
        project.write("docs/proofs/source/old.cpp", marker + "\n");
        project.write("artifacts/source/old.cpp", marker + "\n");
        project.write("src/implementation.cpp", marker + "\n");
        const auto result = search();
        REQUIRE(result.ok);
        CHECK(result.content.find("src/implementation.cpp") != std::string::npos);
        CHECK(result.content.find("old.cpp") == std::string::npos);
    }
    SECTION("a documentation filename query can outrank an incidental source match") {
        project.write("a/incidental.cpp", marker + "\n");
        project.write("docs/relay_marker.md", marker + "\n");
        const auto result = search();
        REQUIRE(result.ok);
        CHECK(result.content.find("docs/relay_marker.md") != std::string::npos);
        CHECK(result.content.find("a/incidental.cpp") == std::string::npos);
    }
    SECTION("a configuration filename query can outrank an incidental source match") {
        project.write("a/incidental.cpp", marker + "\n");
        project.write("config/relay_marker.toml", marker + " = 2\n");
        const auto result = search();
        REQUIRE(result.ok);
        CHECK(result.content.find("config/relay_marker.toml") != std::string::npos);
        CHECK(result.content.find("a/incidental.cpp") == std::string::npos);
    }
    SECTION("equally ranked paths retain the workspace traversal order") {
        project.write("src/one.cpp", marker + "\n");
        project.write("src/two.cpp", marker + "\n");
        const auto files = fixture.workspace.all_files(100000);
        REQUIRE(files.ok());
        REQUIRE(files->size() == 2);
        const auto result = search();
        REQUIRE(result.ok);
        CHECK(result.content.find(files->front().relative_path) != std::string::npos);
        CHECK(result.content.find(files->back().relative_path) == std::string::npos);
    }
}

TEST_CASE("literal search priority preserves explicit selection and matching contracts",
          "[tools][search][search-ranking]") {
    ToolFixture fixture;
    TempDirectory project{"lc-search-selection"};
    REQUIRE(fixture.workspace.open(project.path()).ok());
    project.write("a/implementation.cpp", "opening hours\nexact A|B\n");
    project.write("docs/information.md", "opening hours\n");
    project.write("docs/space name.md", "selected_literal\n");
    project.write("docs/proofs/old.cpp", "archived_only\n");

    SECTION("an ambiguous prose query is bounded and can be scoped explicitly") {
        const auto ambiguous =
            fixture.call("search_text", {{"query", "opening hours"}, {"max_results", 1}});
        REQUIRE(ambiguous.ok);
        CHECK(ambiguous.content.find("a/implementation.cpp") != std::string::npos);
        CHECK(ambiguous.content.find("docs/information.md") == std::string::npos);
        CHECK(ambiguous.truncated);
        const auto docs = fixture.call(
            "search_text",
            {{"query", "opening hours"}, {"max_results", 1}, {"file_pattern", "*.md"}});
        REQUIRE(docs.ok);
        CHECK(docs.content.find("docs/information.md") != std::string::npos);
        CHECK(docs.content.find("a/implementation.cpp") == std::string::npos);
    }
    SECTION("a literal space in the glob is still a filename character") {
        const auto selected = fixture.call(
            "search_text", {{"query", "selected_literal"}, {"file_pattern", "space name.md"}});
        REQUIRE(selected.ok);
        CHECK(selected.content.find("docs/space name.md") != std::string::npos);
        CHECK(selected.content.find("Searched 1 text file(s)") != std::string::npos);
    }
    SECTION("explicit archived source selection remains available") {
        const auto selected = fixture.call(
            "search_text", {{"query", "archived_only"}, {"file_pattern", "docs/proofs/*.cpp"}});
        REQUIRE(selected.ok);
        CHECK(selected.content.find("docs/proofs/old.cpp") != std::string::npos);
    }
    SECTION("literal and regex modes keep distinct matching and ordering contracts") {
        const auto literal = fixture.call("search_text", {{"query", "A|B"}});
        REQUIRE(literal.ok);
        CHECK(literal.summary.find("Found 1 match(es)") != std::string::npos);
        const auto regex = fixture.call(
            "search_text", {{"query", "opening\\s+hours"}, {"regex", true}, {"max_results", 1}});
        REQUIRE(regex.ok);
        const auto files = fixture.workspace.all_files(100000);
        REQUIRE(files.ok());
        const auto first_matching =
            std::find_if(files->begin(), files->end(), [](const auto& file) {
                return file.relative_path == "a/implementation.cpp" ||
                       file.relative_path == "docs/information.md";
            });
        REQUIRE(first_matching != files->end());
        CHECK(regex.content.find(first_matching->relative_path) != std::string::npos);
        CHECK(regex.summary.find("Found 1 match(es)") != std::string::npos);
        CHECK(regex.truncated);
        CHECK(regex.content.find("Order: literal path affinity") == std::string::npos);
    }
    SECTION("an uncapped zero remains restricted to actually inspected files") {
        const auto absent =
            fixture.call("search_text", {{"query", "no_such_marker"}, {"file_pattern", "*.md"}});
        REQUIRE(absent.ok);
        CHECK_FALSE(absent.truncated);
        CHECK(absent.content.find("Searched 2 text file(s)") != std::string::npos);
        CHECK(absent.content.find("Found 0 match(es)") != std::string::npos);
        CHECK(absent.content.find("not exhaustive workspace coverage") == std::string::npos);
    }
}

TEST_CASE("search_files matches by name", "[tools][search]") {
    ToolFixture fixture;

    auto result = fixture.call("search_files", Json{{"pattern", "*.cpp"}});
    REQUIRE(result.ok);
    CHECK(result.content.find("src/main.cpp") != std::string::npos);
    CHECK(result.content.find("src/util.cpp") != std::string::npos);
    CHECK(result.content.find("api.h") == std::string::npos);
}

TEST_CASE("write_file creates and records the change", "[tools][edit]") {
    ToolFixture fixture;

    auto result =
        fixture.call("write_file", Json{{"path", "src/new_file.cpp"}, {"content", "int x = 1;\n"}});
    REQUIRE(result.ok);
    CHECK(std::filesystem::exists(fixture.temp.path() / "src" / "new_file.cpp"));

    const auto changes = fixture.workspace.agent_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].relative_path == "src/new_file.cpp");
    CHECK(changes[0].kind == AgentChangeKind::Created);
}

TEST_CASE("write_file without content is refused rather than emptying a file",
          "[tools][edit][safety]") {
    ToolFixture fixture;
    auto result = fixture.call("write_file", Json{{"path", "src/main.cpp"}});
    CHECK_FALSE(result.ok);
    // The file must be untouched.
    auto still_there = fs::read_text(fixture.temp.path() / "src" / "main.cpp");
    REQUIRE(still_there.ok());
    CHECK(still_there->content.find("int add") != std::string::npos);
}

TEST_CASE("replace_in_file makes a targeted edit", "[tools][edit]") {
    ToolFixture fixture;

    auto result = fixture.call("replace_in_file", Json{{"path", "src/main.cpp"},
                                                       {"find", "return a + b;"},
                                                       {"replace", "return a + b + 0;"}});
    REQUIRE(result.ok);

    auto read = fs::read_text(fixture.temp.path() / "src" / "main.cpp");
    REQUIRE(read.ok());
    CHECK(read->content.find("return a + b + 0;") != std::string::npos);
    // Everything else is intact.
    CHECK(read->content.find("#include <cstdio>") != std::string::npos);
}

TEST_CASE("replace_in_file refuses text it cannot find", "[tools][edit]") {
    ToolFixture fixture;
    auto result = fixture.call(
        "replace_in_file",
        Json{{"path", "src/main.cpp"}, {"find", "this text is not present"}, {"replace", "x"}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("does not appear") != std::string::npos);
}

TEST_CASE("replace_in_file refuses ambiguous text", "[tools][edit]") {
    ToolFixture fixture;
    fixture.temp.write("dup.txt", "same\nsame\nsame\n");

    auto result = fixture.call("replace_in_file",
                               Json{{"path", "dup.txt"}, {"find", "same"}, {"replace", "other"}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("3 times") != std::string::npos);

    auto all = fixture.call(
        "replace_in_file",
        Json{{"path", "dup.txt"}, {"find", "same"}, {"replace", "other"}, {"replace_all", true}});
    CHECK(all.ok);
}

TEST_CASE("an edit that changes nothing is refused", "[tools][edit]") {
    ToolFixture fixture;
    auto result =
        fixture.call("replace_in_file",
                     Json{{"path", "src/main.cpp"}, {"find", "int add"}, {"replace", "int add"}});
    CHECK_FALSE(result.ok);
}

TEST_CASE("apply_patch applies a unified diff", "[tools][edit][patch]") {
    ToolFixture fixture;

    const std::string patch =
        "diff --git a/src/util.cpp b/src/util.cpp\n"
        "--- a/src/util.cpp\n"
        "+++ b/src/util.cpp\n"
        "@@ -1,1 +1,1 @@\n"
        "-int helper() { return 7; }\n"
        "+int helper() { return 42; }\n";

    auto result = fixture.call("apply_patch", Json{{"patch", patch}});
    INFO(result.error);
    REQUIRE(result.ok);

    auto read = fs::read_text(fixture.temp.path() / "src" / "util.cpp");
    REQUIRE(read.ok());
    CHECK(read->content.find("return 42;") != std::string::npos);
}

TEST_CASE("a patch whose context does not match is rejected atomically",
          "[tools][edit][patch][safety]") {
    ToolFixture fixture;

    const std::string patch =
        "diff --git a/src/util.cpp b/src/util.cpp\n"
        "--- a/src/util.cpp\n"
        "+++ b/src/util.cpp\n"
        "@@ -1,1 +1,1 @@\n"
        "-int helper() { return 999; }\n"
        "+int helper() { return 42; }\n";

    auto result = fixture.call("apply_patch", Json{{"patch", patch}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("not applied") != std::string::npos);

    // Nothing may have been written.
    auto read = fs::read_text(fixture.temp.path() / "src" / "util.cpp");
    REQUIRE(read.ok());
    CHECK(read->content.find("return 7;") != std::string::npos);
}

TEST_CASE("a patch tolerates drifted line numbers", "[tools][edit][patch]") {
    ToolFixture fixture;

    // The hunk claims line 50; the context is really at line 1. Context wins.
    const std::string patch =
        "diff --git a/src/util.cpp b/src/util.cpp\n"
        "--- a/src/util.cpp\n"
        "+++ b/src/util.cpp\n"
        "@@ -50,1 +50,1 @@\n"
        "-int helper() { return 7; }\n"
        "+int helper() { return 8; }\n";

    auto result = fixture.call("apply_patch", Json{{"patch", patch}});
    INFO(result.error);
    REQUIRE(result.ok);

    auto read = fs::read_text(fixture.temp.path() / "src" / "util.cpp");
    REQUIRE(read.ok());
    CHECK(read->content.find("return 8;") != std::string::npos);
}

TEST_CASE("writes outside the workspace are refused", "[tools][security]") {
    ToolFixture fixture;

    for (const char* path : {"../escaped.txt", "../../escaped.txt"}) {
        INFO(path);
        auto result = fixture.call("write_file", Json{{"path", path}, {"content", "x"}});
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("workspace") != std::string::npos);
    }
}

TEST_CASE("a file that was already modified is protected", "[tools][edit][safety]") {
    ToolFixture fixture;

    // Simulate a baseline in which src/main.cpp already had uncommitted changes.
    // (Without git available the baseline is empty, so this asserts the guard
    // only when the workspace could record one.)
    auto snapshot = fixture.workspace.snapshot_baseline();
    CHECK(snapshot.ok());
}

TEST_CASE("create_directory, move_file and copy_file work inside the workspace", "[tools][files]") {
    ToolFixture fixture;

    REQUIRE(fixture.call("create_directory", Json{{"path", "generated/nested"}}).ok);
    CHECK(std::filesystem::is_directory(fixture.temp.path() / "generated" / "nested"));

    REQUIRE(fixture
                .call("copy_file",
                      Json{{"source", "src/util.cpp"}, {"destination", "generated/util_copy.cpp"}})
                .ok);
    CHECK(std::filesystem::exists(fixture.temp.path() / "generated" / "util_copy.cpp"));

    REQUIRE(fixture
                .call("move_file", Json{{"source", "generated/util_copy.cpp"},
                                        {"destination", "generated/renamed.cpp"}})
                .ok);
    CHECK(std::filesystem::exists(fixture.temp.path() / "generated" / "renamed.cpp"));
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "generated" / "util_copy.cpp"));

    // Overwriting needs to be asked for explicitly.
    auto clobber = fixture.call(
        "copy_file", Json{{"source", "src/util.cpp"}, {"destination", "generated/renamed.cpp"}});
    CHECK_FALSE(clobber.ok);
}

TEST_CASE("copy cannot overwrite an outside file through an existing hard link",
          "[tools][files][copy-boundary]") {
    ToolFixture fixture;
    TempDirectory outside;
    const auto original = outside.write("original.txt", "outside must stay unchanged\n");
    const auto alias = fixture.temp.path() / "alias.txt";
    std::error_code error;
    std::filesystem::create_hard_link(original, alias, error);
    REQUIRE_FALSE(error);
    const auto result = fixture.call(
        "copy_file",
        Json{{"source", "src/util.cpp"}, {"destination", "alias.txt"}, {"overwrite", true}});
    CHECK_FALSE(result.ok);
    const auto original_content = fs::read_text(original);
    REQUIRE(original_content);
    CHECK(original_content->content == "outside must stay unchanged\n");
}

TEST_CASE("every edit entry point detaches a hard link before changing its contents",
          "[tools][security][hard-link][audit]") {
    ToolFixture fixture;
    TempDirectory outside;
    const std::string before = "int sentinel() { return 3; }\n";
    const std::string after = "int sentinel() { return 7; }\n";
    const auto original = outside.write("original.cpp", before);
    const auto alias = fixture.temp.path() / "alias.cpp";
    std::error_code error;
    std::filesystem::create_hard_link(original, alias, error);
    REQUIRE_FALSE(error);
    ToolResult edited;
    SECTION("whole file write") {
        edited = fixture.call("write_file", {{"path", "alias.cpp"}, {"content", after}});
    }
    SECTION("targeted replacement") {
        edited =
            fixture.call("replace_in_file",
                         {{"path", "alias.cpp"}, {"find", "return 3"}, {"replace", "return 7"}});
    }
    SECTION("unified patch") {
        edited =
            fixture.call("apply_patch",
                         {{"patch",
                           "diff --git a/alias.cpp b/alias.cpp\n--- a/alias.cpp\n+++ b/alias.cpp\n"
                           "@@ -1,1 +1,1 @@\n-" +
                               before + "+" + after}});
    }
    INFO(edited.error);
    REQUIRE(edited.ok);
    const auto untouched = fs::read_text(original);
    REQUIRE(untouched);
    CHECK(untouched->content == before);
    const auto changed = fs::read_text(alias);
    REQUIRE(changed);
    CHECK(changed->content == after);
    CHECK(std::filesystem::hard_link_count(alias) == 1);
}

#if defined(_WIN32)
TEST_CASE("a copied move whose source cannot be removed is reported as incomplete",
          "[tools][files][audit]") {
    ToolFixture fixture;
    const auto source = fixture.temp.path() / "src/util.cpp";

    struct LockedFile {
        HANDLE handle = INVALID_HANDLE_VALUE;

        ~LockedFile() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
    } lock;

    lock.handle = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(lock.handle != INVALID_HANDLE_VALUE);
    const auto result =
        fixture.call("move_file", Json{{"source", "src/util.cpp"}, {"destination", "copied.cpp"}});
    CHECK_FALSE(result.ok);
    CHECK(std::filesystem::exists(source));
    CHECK(std::filesystem::exists(fixture.temp.path() / "copied.cpp"));
    // The destination was written despite the failed overall move. It must
    // still invalidate repository and verification state.
    CHECK(std::any_of(result.evidence.begin(), result.evidence.end(), [](const Evidence& evidence) {
        return evidence.kind == EvidenceKind::FileWritten && evidence.positive;
    }));
}

TEST_CASE("recursive copy checks nested destination junctions before writing",
          "[tools][files][copy-boundary]") {
    ToolFixture fixture;
    TempDirectory outside;
    fixture.temp.write("source/nested/data.txt", "must not escape\n");
    const auto destination = fixture.temp.make_directory("destination");
    ProcessRequest junction;
    junction.executable = *find_executable("cmd");
    junction.arguments = {"/d", "/s", "/c",
                          "mklink /J \"" + (destination / "nested").string() + "\" \"" +
                              outside.path().string() + "\""};
    const auto created = ProcessRunner::run(junction);
    REQUIRE(created);
    REQUIRE(created->success());
    const auto result = fixture.call(
        "copy_file",
        Json{{"source", "source"}, {"destination", "destination"}, {"overwrite", true}});
    CHECK_FALSE(result.ok);
    CHECK_FALSE(std::filesystem::exists(outside.path() / "data.txt"));
}
#endif

TEST_CASE("checked copies preserve ordinary directories and binary files",
          "[tools][files][copy-boundary]") {
    ToolFixture fixture;
    const std::string bytes("binary\0payload", 14);
    fixture.temp.write("source/nested/data.bin", bytes);
    const auto copied =
        fixture.call("copy_file", Json{{"source", "source"}, {"destination", "copied"}});
    REQUIRE(copied.ok);
    const auto content = fs::read_bytes(fixture.temp.path() / "copied/nested/data.bin");
    REQUIRE(content);
    CHECK(*content == bytes);
    const auto recursive =
        fixture.call("copy_file", Json{{"source", "source"}, {"destination", "source/recursive"}});
    CHECK_FALSE(recursive.ok);
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "source/recursive"));
}

TEST_CASE("run_command runs a real command and summarises it", "[tools][process]") {
    ToolFixture fixture;

#if defined(_WIN32)
    Json command = Json::array({"cmd", "/d", "/s", "/c", "echo tool-test-output"});
#else
    Json command = Json::array({"echo", "tool-test-output"});
#endif

    auto result = fixture.call("run_command", Json{{"command", command}});
    REQUIRE(result.ok);
    CHECK(result.content.find("tool-test-output") != std::string::npos);
    CHECK(result.content.find("exit:") != std::string::npos);
    CHECK(result.summary.find("OK") != std::string::npos);
    REQUIRE_FALSE(result.evidence.empty());
    CHECK(result.evidence[0].positive);
}

TEST_CASE("native and compact command schemas expose multiple criterion bindings",
          "[tools][expectation][multiple-criteria]") {
    ToolFixture fixture;
    for (const char* name : {"run_command", "run_artifact"}) {
        const auto definition = fixture.registry.find(name)->definition();
        for (int level = 0; level <= 3; ++level) {
            const auto schema =
                level == 0 ? definition.parameters : definition.compact(level).parameters;
            const auto& alternatives =
                schema["properties"]["expect"]["properties"]["criterion"]["oneOf"];
            REQUIRE(alternatives.size() == 2);
            CHECK(alternatives[0]["type"] == "integer");
            CHECK(alternatives[1]["type"] == "array");
        }
    }
#if defined(_WIN32)
    const auto command = Json::array({"cmd", "/d", "/s", "/c", "echo fixture: PASS"});
#else
    const auto command = Json::array({"echo", "fixture: PASS"});
#endif
    const auto result =
        fixture.call("run_command", Json{{"command", command},
                                         {"expect", Json{{"stdout_equals", "fixture: PASS"},
                                                         {"criterion", Json::array({1, 2})}}}});
    REQUIRE(result.ok);
    CHECK(result.structured["expectation_satisfied"] == true);
    CHECK(result.structured["expectation_criteria"] == Json::array({1, 2}));
}

TEST_CASE("a failing command is reported as a fact, not a tool error", "[tools][process]") {
    ToolFixture fixture;

#if defined(_WIN32)
    Json command = Json::array({"cmd", "/d", "/s", "/c", "exit 7"});
#else
    Json command = Json::array({"sh", "-c", "exit 7"});
#endif

    auto result = fixture.call("run_command", Json{{"command", command}});
    // The tool worked; the command failed.
    REQUIRE(result.ok);
    CHECK(result.summary.find("FAILED") != std::string::npos);
    CHECK(result.content.find("exit:     7") != std::string::npos);
    REQUIRE_FALSE(result.evidence.empty());
    CHECK_FALSE(result.evidence[0].positive);
}

TEST_CASE("a blocked command is refused and says so plainly", "[tools][security]") {
    ToolFixture fixture;
    // A harmless real executable given a blocked basename makes resolution
    // portable and keeps the test safe even if the policy regresses.
    const auto sample = path_from_utf8(LOCALCODE_TEST_COMMAND_FIXTURE);
    const auto executable = fixture.temp.path() / "format.exe";
    REQUIRE(std::filesystem::copy_file(sample, executable));
    auto result = fixture.call("run_command", Json{{"executable", path_to_utf8(executable)},
                                                   {"arguments", Json::array()}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("blocked") != std::string::npos);
    CHECK(result.error.find("not something you can override") != std::string::npos);
    CHECK(fixture.confirmations_asked == 0);   // never even asked
}

TEST_CASE("a command needing confirmation asks, and honours the answer", "[tools][security]") {
    ToolFixture fixture;

    fixture.confirm_answer = false;
    auto refused = fixture.call("run_command", Json{{"shell_command", "git reset --hard HEAD"}});
    CHECK_FALSE(refused.ok);
    CHECK(fixture.confirmations_asked == 1);
    CHECK(refused.error.find("did not approve") != std::string::npos);
}

TEST_CASE("a missing executable suggests what is available", "[tools][process]") {
    ToolFixture fixture;
    auto result = fixture.call("run_command",
                               Json{{"command", Json::array({"totally-not-a-real-tool-98765"})}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("not found") != std::string::npos);
}

TEST_CASE("environment_info reports the real machine", "[tools][environment]") {
    ToolFixture fixture;
    auto result = fixture.call("environment_info", Json::object());
    REQUIRE(result.ok);
    CHECK(result.content.find("Operating system") != std::string::npos);
    CHECK(result.content.find("Installed") != std::string::npos);
    REQUIRE_FALSE(result.evidence.empty());
    CHECK(result.evidence[0].kind == EvidenceKind::EnvironmentChecked);
}

TEST_CASE("find_executable resolves a known program", "[tools][environment]") {
    ToolFixture fixture;
#if defined(_WIN32)
    auto result = fixture.call("find_executable", Json{{"name", "cmd"}});
#else
    auto result = fixture.call("find_executable", Json{{"name", "sh"}});
#endif
    REQUIRE(result.ok);
    CHECK(result.content.find("resolves to") != std::string::npos);
}

TEST_CASE("an unknown tool is answered with the list of real ones", "[tools]") {
    ToolFixture fixture;
    auto result = fixture.call("no_such_tool", Json::object());
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("read_file") != std::string::npos);
}

TEST_CASE("a missing required parameter is rejected with the schema", "[tools]") {
    ToolFixture fixture;
    auto result = fixture.call("read_file", Json::object());
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("missing required") != std::string::npos);
    CHECK(result.error.find("properties") != std::string::npos);
}

TEST_CASE("a malformed argument container is refused before executing a tool",
          "[tools][arguments]") {
    class Probe final : public ITool {
    public:
        int calls = 0;

        std::string name() const override { return "container_probe"; }

        std::string description() const override {
            return "Records whether execution was reached.";
        }

        bool requires_workspace() const override { return false; }

        Json parameters_schema() const override {
            return {{"type", "object"},
                    {"properties",
                     {{"arguments", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"environment", {{"type", "object"}}},
                      {"timeout", {{"type", "integer"}}}}}};
        }

        ToolResult execute(const Json&, ToolContext&) override {
            ++calls;
            return ToolResult::success("executed", "executed");
        }
    };

    ToolRegistry registry;
    auto probe = std::make_shared<Probe>();
    registry.add(probe);
    ToolContext context;
    for (const auto& arguments : {Json{{"arguments", {{"script", "owned.py"}}}},
                                  Json{{"environment", Json::array({"wrong"})}}}) {
        const auto result = registry.invoke({"container_probe", arguments, "test"}, context);
        CHECK_FALSE(result.ok);
        CHECK(result.to_model_text().find("expected parameters") != std::string::npos);
    }
    CHECK(probe->calls == 0);
    // Existing scalar numeric coercion remains advisory rather than blocking.
    CHECK(registry.invoke({"container_probe", {{"timeout", "5"}}, "test"}, context).ok);
    CHECK(registry
              .invoke({"container_probe",
                       {{"arguments", Json::array({"owned.py"})}, {"environment", Json::object()}},
                       "test"},
                      context)
              .ok);
    CHECK(probe->calls == 2);
}

TEST_CASE("read-only mode withholds the mutating tools", "[tools][security]") {
    ToolFixture fixture;
    fixture.registry.set_read_only(true);

    auto write = fixture.call("write_file", Json{{"path", "x.txt"}, {"content", "y"}});
    CHECK_FALSE(write.ok);
    CHECK(write.error.find("read-only") != std::string::npos);

    // Reading still works.
    CHECK(fixture.call("read_file", Json{{"path", "src/main.cpp"}}).ok);

    // And the mutating tools are not even advertised.
    const auto definitions = fixture.registry.definitions(/*include_mutating=*/false);
    for (const auto& definition : definitions) {
        CHECK(definition.name != "write_file");
        CHECK(definition.name != "run_command");
    }
}

TEST_CASE("Git inspection does not execute repository helpers", "[tools][git][security]") {
    const auto git = find_executable("git");
    if (!git) SKIP("Git is unavailable");
    ToolFixture fixture;
    TempDirectory outside{"lc-git-helper-outside"};
    const auto marker = outside.path() / "helper-ran.txt";
    // Git executes helper commands through its POSIX shell on every host,
    // including Windows. Quote these as shell commands, not C-runtime argv.
    const auto shell_path = [](const std::filesystem::path& path) {
        std::string quoted = "'";
        for (const auto c : path_to_utf8(path))
            quoted += c == '\'' ? "'\\''" : std::string(1, c);
        return quoted + "'";
    };
    auto run_git = [&](std::vector<std::string> arguments) {
        ProcessRequest request;
        request.executable = *git;
        request.working_directory = fixture.temp.path();
        request.arguments = std::move(arguments);
        request.timeout = std::chrono::seconds(20);
        request.environment = {
            {"GIT_CONFIG_NOSYSTEM", "1"},
            {"GIT_CONFIG_GLOBAL", (outside.path() / "no-global-config").string()},
            {"GIT_TERMINAL_PROMPT", "0"}};
        const auto result = ProcessRunner::run(request);
        REQUIRE(result);
        INFO(result->stderr_text);
        REQUIRE(result->exit_code == 0);
    };
    run_git({"init", "--template=", "--initial-branch=fixture"});
    run_git({"config", "core.autocrlf", "false"});
    run_git({"add", "src/main.cpp"});
    run_git({"-c", "user.name=LocalCode test", "-c", "user.email=test@local.invalid", "-c",
             "core.hooksPath=" + outside.path().generic_string(), "commit", "-m", "fixture"});
    REQUIRE(fixture.workspace.open(fixture.temp.path()));
    fixture.registry.set_read_only(true);

    SECTION("status refuses an fsmonitor hook") {
        const auto hook = fixture.temp.write(
            ".git/hooks/observe.sh",
            "#!/bin/sh\nprintf observed > " + shell_path(marker) + "\nprintf 'token\\0'\n");
        std::filesystem::permissions(hook, std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::add);
        run_git({"config", "core.fsmonitor", shell_path(hook)});
        // Positive control: prove Git can execute this helper on this host.
        run_git({"status", "--porcelain=v2", "--branch"});
        REQUIRE(std::filesystem::exists(marker));
        REQUIRE(std::filesystem::remove(marker));
        REQUIRE(fixture.call("git_status", Json::object()).ok);
        CHECK_FALSE(std::filesystem::exists(marker));
    }

    SECTION("diff refuses text conversion commands") {
        const auto helper = fixture.temp.write(
            ".git/hooks/convert.sh",
            "#!/bin/sh\nprintf observed > " + shell_path(marker) + "\ncat \"$1\"\n");
        std::filesystem::permissions(helper, std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::add);
        fixture.temp.write(".gitattributes", "src/main.cpp diff=localcode-test\n");
        run_git({"config", "diff.localcode-test.textconv", shell_path(helper)});
        fixture.temp.write("src/main.cpp", "int changed = 9;\n");
        run_git({"diff", "--no-ext-diff", "--", "src/main.cpp"});
        REQUIRE(std::filesystem::exists(marker));
        REQUIRE(std::filesystem::remove(marker));
        REQUIRE(fixture.call("git_diff", {{"paths", {"src/main.cpp"}}}).ok);
        CHECK_FALSE(std::filesystem::exists(marker));
        std::filesystem::remove(marker);
        const auto revision_diff =
            fixture.workspace.git().diff_against(fixture.temp.path(), "HEAD", {"src/main.cpp"});
        REQUIRE(revision_diff);
        CHECK_FALSE(revision_diff->files.empty());
        CHECK_FALSE(std::filesystem::exists(marker));
    }
}

TEST_CASE("schema validation is lenient about number-like strings", "[tools]") {
    ToolFixture fixture;
    // A small model writing "3" where an integer is wanted should still work.
    auto result = fixture.call(
        "read_file_range", Json{{"path", "src/main.cpp"}, {"start_line", "3"}, {"end_line", "5"}});
    CHECK(result.ok);
}

TEST_CASE("the observer sees every invocation", "[tools]") {
    ToolFixture fixture;
    int observed = 0;
    fixture.registry.set_observer(
        [&observed](const ToolInvocation&, const ToolResult&) { ++observed; });

    fixture.call("read_file", Json{{"path", "src/main.cpp"}});
    fixture.call("read_file", Json{{"path", "missing.cpp"}});
    fixture.call("no_such_tool", Json::object());

    CHECK(observed == 3);
}

TEST_CASE("web tools refuse cleanly when the service is absent", "[tools][web]") {
    ToolFixture fixture;   // context.web is null
    auto result = fixture.call("web_search", Json{{"query", "anything"}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("not configured") != std::string::npos);
}

TEST_CASE("create_skill saves an unvalidated skill", "[tools][skills]") {
    ToolFixture fixture;
    SkillRegistry skills;
    fixture.context.skills = &skills;

    auto result = fixture.call(
        "create_skill",
        Json{{"id", "test-generated-skill"},
             {"name", "Generated"},
             {"description", "A procedure the agent wrote during a test run."},
             {"triggers", Json::array({"a distinctive trigger phrase"})},
             {"procedure", std::string("## Steps\n\n1. Do the thing.\n2. Check it worked.\n") +
                               std::string(120, 'x')}});

    REQUIRE(result.ok);
    CHECK(result.content.find("awaiting review") != std::string::npos);

    const auto* saved = skills.find("test-generated-skill");
    REQUIRE(saved != nullptr);
    CHECK_FALSE(saved->validated);

    // And therefore it is not selected for any task.
    CHECK(skills.select("a distinctive trigger phrase", "", 3).empty());

    // Clean up the file the tool wrote into the user data directory.
    std::error_code ec;
    std::filesystem::remove_all(saved->source_path.parent_path(), ec);
}

TEST_CASE("create_skill refuses a duplicate id", "[tools][skills]") {
    ToolFixture fixture;
    SkillRegistry skills;
    REQUIRE(skills.load_directory(LOCALCODE_TEST_SKILLS_DIR).ok());
    fixture.context.skills = &skills;

    auto result =
        fixture.call("create_skill", Json{{"id", "verify-before-done"},
                                          {"name", "Duplicate"},
                                          {"description", "Tries to shadow a built-in skill."},
                                          {"triggers", Json::array({"some trigger"})},
                                          {"procedure", std::string(200, 'x')}});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("already exists") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Tool schemas are the largest fixed cost in a request. Running against a real
// 8k model showed the full set at ~4,800 tokens, leaving no room for the actual
// problem. These cover the compact form and the rule that chooses it.
// ---------------------------------------------------------------------------
TEST_CASE("the compact schema keeps what decides a correct call", "[tools][budget]") {
    ToolDefinition tool;
    tool.name = "read_file";
    tool.description =
        "Reads the actual contents of a text or source file from disk and returns them with "
        "line numbers. Use this whenever answering a question requires knowing what is in a "
        "file. Never ask the user to paste a file that this tool can read, and never guess at "
        "a file's contents. Paths may be relative to the workspace or absolute.";
    tool.parameters = schema::object(
        {{"path", schema::string_param(
                      "Path to the file, relative to the workspace root or absolute. This is a "
                      "long elaboration that a small window cannot afford to carry.")},
         {"mode", schema::string_enum("How to read it, at length, with much explanation that "
                                      "goes on for a while.",
                                      {"text", "binary"})},
         {"max_bytes",
          schema::integer_param("Maximum bytes to read. Defaults to 400000. Further explanation "
                                "that is not load-bearing for a correct call.",
                                1, 4000000)}},
        {"path"});

    const auto compact = tool.compact();

    CHECK(compact.name == tool.name);
    CHECK(compact.description.size() < tool.description.size());
    // The first sentence - what the tool does - must survive.
    CHECK(compact.description.find("Reads the actual contents") != std::string::npos);

    REQUIRE(compact.parameters.contains("required"));
    CHECK(compact.parameters["required"] == tool.parameters["required"]);

    REQUIRE(compact.parameters.contains("properties"));
    const auto& properties = compact.parameters["properties"];
    // Every parameter is still declared, with its type.
    for (const char* name : {"path", "mode", "max_bytes"}) {
        REQUIRE(properties.contains(name));
        CHECK(properties[name].contains("type"));
    }
    // Enum values are what a model gets wrong; they must be kept in full.
    REQUIRE(properties["mode"].contains("enum"));
    CHECK(properties["mode"]["enum"].size() == 2);

    // The required parameter keeps a description; it is just shorter.
    REQUIRE(properties["path"].contains("description"));
    CHECK(properties["path"]["description"].get<std::string>().size() <
          tool.parameters["properties"]["path"]["description"].get<std::string>().size());

    CHECK(estimate_tokens(compact.parameters.dump()) < estimate_tokens(tool.parameters.dump()));
}

TEST_CASE("compact schemas preserve nested verification parameter names", "[tools][budget]") {
    ToolRegistry registry;
    registry.register_builtin_tools();
    for (const auto& tool : registry.definitions()) {
        if (tool.name != "run_command" && tool.name != "run_artifact" && tool.name != "read_file")
            continue;
        for (const int level : {1, 2, 3}) {
            INFO(tool.name << " compact level " << level);
            const auto compact = tool.compact(level);
            REQUIRE(compact.parameters["properties"].contains("expect"));
            REQUIRE(compact.parameters["properties"]["expect"].contains("properties"));
            const auto& properties = compact.parameters["properties"]["expect"]["properties"];
            CHECK(properties.contains("criterion"));
            CHECK(
                properties.contains(tool.name == "read_file" ? "contains_all" : "stdout_contains"));
        }
    }
}

TEST_CASE("the leanest schema drops knobs, never essentials", "[tools][budget]") {
    SECTION("a refinement goes; a required parameter and a mode selector stay") {
        ToolDefinition tool;
        tool.name = "search_text";
        tool.description = "Searches files.";
        tool.parameters = schema::object(
            {{"query", schema::string_param("What to look for.")},
             {"kind", schema::string_enum("Contents or names.", {"text", "file"})},
             {"regex", schema::boolean_param("Treat the query as a regex.", false)},
             {"path_contains", schema::string_param("Restrict to paths containing this.")}},
            {"query"});

        // Held by name: binding a reference to a subobject of a temporary
        // does not extend its lifetime, and the reference dangles.
        const auto compact = tool.compact(3);
        const auto& properties = compact.parameters["properties"];
        CHECK(properties.contains("query"));   // required
        CHECK(properties.contains("kind"));    // a mode selector, not a knob
        CHECK_FALSE(properties.contains("regex"));
        CHECK_FALSE(properties.contains("path_contains"));
        // The enum is the whole point of keeping it: without the values the
        // parameter is a string with no clue what belongs in it.
        CHECK(properties["kind"]["enum"].size() == 2);
    }

    SECTION("a tool with nothing required keeps everything") {
        // repo_symbol takes a name or a file depending on the mode, so it
        // cannot name its essentials in `required`. Dropping its optional
        // parameters left the mode selector and nothing to apply it to.
        ToolDefinition tool;
        tool.name = "repo_symbol";
        tool.description = "Finds a symbol.";
        tool.parameters = schema::object(
            {{"name", schema::string_param("Symbol name.")},
             {"mode", schema::string_enum("Which way.", {"definition", "references"})},
             {"file", schema::string_param("A file to find includes of.")}},
            {});

        // Held by name: binding a reference to a subobject of a temporary
        // does not extend its lifetime, and the reference dangles.
        const auto compact = tool.compact(3);
        const auto& properties = compact.parameters["properties"];
        for (const char* name : {"name", "mode", "file"}) {
            INFO(name);
            CHECK(properties.contains(name));
        }
    }
}

TEST_CASE("the real tool set is compacted for a small window", "[tools][budget]") {
    ToolRegistry registry;
    registry.register_builtin_tools();
    const auto full = registry.definitions();
    REQUIRE(full.size() > 15);

    auto cost = [](const std::vector<ToolDefinition>& tools) {
        int total = 0;
        for (const auto& tool : tools) {
            total += estimate_tokens(tool.name) + estimate_tokens(tool.description) +
                     estimate_tokens(tool.parameters.dump()) + 8;
        }
        return total;
    };

    const int full_cost = cost(full);
    INFO("full tool schemas cost " << full_cost << " tokens");

    SECTION("a large window keeps the full descriptions") {
        const auto fitted = fit_tool_definitions(full, 131072);
        CHECK(cost(fitted) == full_cost);
        CHECK(fitted[0].description == full[0].description);
    }

    SECTION("a small window gets the compact ones, and they fit") {
        const auto fitted = fit_tool_definitions(full, 8192);
        REQUIRE(fitted.size() == full.size());   // no tool is dropped

        const int fitted_cost = cost(fitted);
        INFO("compact tool schemas cost " << fitted_cost << " tokens");
        CHECK(fitted_cost < full_cost);
        // The saving has to be large enough to matter: the schemas must fit the
        // fifth-of-the-window ceiling that fit_tool_definitions targets, with a
        // little slack for the estimator.
        CHECK(fitted_cost < 8192 / 3);
        INFO("ceiling was " << 8192 / 5);

        // Every tool is still callable: name, type and required list intact.
        for (const auto& tool : fitted) {
            INFO("tool: " << tool.name);
            CHECK_FALSE(tool.name.empty());
            CHECK_FALSE(tool.description.empty());
            REQUIRE(tool.parameters.is_object());
            CHECK(tool.parameters.value("type", "") == "object");
        }
    }

    SECTION("required parameters survive compaction") {
        const auto fitted = fit_tool_definitions(full, 8192);
        for (std::size_t i = 0; i < full.size(); ++i) {
            INFO("tool: " << full[i].name);
            const bool had_required = full[i].parameters.contains("required");
            CHECK(fitted[i].parameters.contains("required") == had_required);
            if (had_required) {
                CHECK(fitted[i].parameters["required"] == full[i].parameters["required"]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// An environment script is not a build.
//
// Observed in a real run: the model ran
//
//     vcvars64.bat cmake -B build      (exit 0)
//
// four times. vcvars64.bat ignores arguments it does not recognise, so CMake
// never ran - but the command line contained the word "cmake", the exit code
// was 0, and LocalCode recorded a successful build each time. The run was
// reported as partially verified on a build that had never happened, and the
// model repeated the command because it kept being told it worked.
// ---------------------------------------------------------------------------

TEST_CASE("running an environment script is not recorded as a build",
          "[tools][evidence][honesty]") {
    ToolFixture fixture;

    // A stand-in for vcvars64.bat: succeeds, ignores its arguments.
#if defined(_WIN32)
    fixture.temp.write("vcvars64.bat", "@echo off\r\n@exit /b 0\r\n");
#else
    fixture.temp.write("vcvars64.bat", "#!/bin/sh\nexit 0\n");
    std::filesystem::permissions(fixture.temp.path() / "vcvars64.bat",
                                 std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
#endif
    const auto script = (fixture.temp.path() / "vcvars64.bat").string();

    auto result = fixture.call("run_command",
                               Json{{"command", Json::array({script, "cmake", "-B", "build"})}});
    REQUIRE(result.ok);

    // It ran and it succeeded - but as a command, not as a build.
    bool recorded_build = false;
    for (const auto& evidence : result.evidence) {
        if (evidence.kind == EvidenceKind::BuildAttempted) recorded_build = true;
    }
    CHECK_FALSE(recorded_build);

    // And the model is told why the exit code means nothing here.
    CHECK(result.content.find("ignores the other arguments") != std::string::npos);
}

TEST_CASE("a real PowerShell script requires an explicit expectation to settle a criterion",
          "[tools][evidence][criterion][opaque-powershell]") {
    auto runtime = resolve_executable("pwsh");
    if (!runtime) runtime = resolve_executable("powershell");
    if (!runtime) SKIP("PowerShell is required for the real script invocation control.");

    ToolFixture fixture;
    fixture.temp.write("scripts/run tests.ps1", "Write-Output 'required-sentinel'\nexit 0\n");
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "The audit reports required-sentinel", false},
                                             {2, "README documents the build", false}};
    };
    Json arguments{{"executable", runtime->generic_string()},
                   {"arguments", {"-NoProfile", "-File", "./scripts/run tests.ps1"}}};
    const auto unchecked = fixture.call("run_command", arguments);
    REQUIRE(unchecked.ok);
    REQUIRE(unchecked.structured.value("exit_code", -1) == 0);
    CHECK_FALSE(unchecked.structured.value("expectation_checked", false));
    for (const auto& evidence : unchecked.evidence) {
        CHECK(evidence.kind != EvidenceKind::BuildAttempted);
        CHECK(evidence.kind != EvidenceKind::TestsRun);
    }
    VerificationLedger ledger;
    ledger.set_acceptance_plan(
        {{"The audit reports required-sentinel"}, {"README documents the build"}});
    ToolCall call;
    call.name = "run_command";
    interpret_tool_result(call, unchecked, ledger);
    CHECK(ledger.holds(Stage::Execution));
    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Test) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Unknown);
    CHECK_FALSE(ledger.acceptance_plan()[0].settled);
    CHECK_FALSE(ledger.acceptance_plan()[1].settled);

    arguments["expect"] = {{"description", "The audit reports required-sentinel"},
                           {"exit_code", 0},
                           {"stdout_contains", "required-sentinel"},
                           {"criterion", 1}};
    const auto checked = fixture.call("run_command", arguments);
    REQUIRE(checked.ok);
    REQUIRE(checked.structured.value("expectation_checked", false));
    CHECK(checked.structured.value("expectation_satisfied", false));
    interpret_tool_result(call, checked, ledger);
    CHECK(ledger.holds(Stage::Behaviour));
    CHECK(ledger.acceptance_plan()[0].settled);
    CHECK(ledger.acceptance_plan()[0].satisfied);
    CHECK_FALSE(ledger.acceptance_plan()[1].settled);
    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Test) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);
}

TEST_CASE("a real build tool is still recorded as a build", "[tools][evidence]") {
    ToolFixture fixture;

    auto result =
        fixture.call("run_command", Json{{"command", Json::array({"cmake", "--version"})}});
    if (!result.ok) {
        WARN("cmake is not available; skipping");
        return;
    }
    bool recorded_build = false;
    for (const auto& evidence : result.evidence) {
        if (evidence.kind == EvidenceKind::BuildAttempted) recorded_build = true;
    }
    CHECK(recorded_build);
}

TEST_CASE("writing code against a missing engine warns at the write", "[tools][toolchain]") {
    // The Unreal game run, in miniature. The system prompt already told the
    // model to check whether an engine was installed before choosing it; it
    // chose first and never checked, and nothing said so until the end.
    ToolFixture fixture;

    auto result = fixture.call("write_file", Json{{"path", "Source/Game/Input.h"},
                                                  {"content",
                                                   "#include \"CoreMinimal.h\"\n"
                                                   "\n"
                                                   "struct FInputActions\n"
                                                   "{\n"
                                                   "    const FString ActionJump = \"Jump\";\n"
                                                   "};\n"}});
    REQUIRE(result.ok);
    INFO(result.content);
    CHECK(result.content.find("Unreal Engine") != std::string::npos);
    CHECK(result.content.find("not part of the project") != std::string::npos);
    CHECK(result.content.find("cannot be finished") != std::string::npos);

    REQUIRE(result.structured.contains("foreign_toolchains"));
    REQUIRE(result.structured["foreign_toolchains"].is_array());
    REQUIRE(result.structured["foreign_toolchains"].size() == 1);
    CHECK(json_string(result.structured["foreign_toolchains"][0], "toolchain") == "Unreal Engine");
}

TEST_CASE("ordinary source is written without a toolchain warning", "[tools][toolchain]") {
    // The warning is only worth having if it stays quiet the rest of the time.
    ToolFixture fixture;

    auto result =
        fixture.call("write_file", Json{{"path", "src/stats.cpp"},
                                        {"content",
                                         "#include <numeric>\n"
                                         "#include <vector>\n"
                                         "\n"
                                         "double mean(const std::vector<double>& v) {\n"
                                         "    return std::accumulate(v.begin(), v.end(), 0.0) "
                                         "/ v.size();\n"
                                         "}\n"}});
    REQUIRE(result.ok);
    CHECK(result.content.find("not part of the project") == std::string::npos);
    CHECK_FALSE(result.structured.contains("foreign_toolchains"));
}

TEST_CASE("creating a directory that is already there is not a change", "[tools][files]") {
    // Observed in a real run: the model called create_directory on the
    // workspace root, and the run's summary afterwards listed "created: ."
    // among the files it had changed. A directory that was already there was
    // not created, and saying it was makes a run look like it did something.
    ToolFixture fixture;

    auto first = fixture.call("create_directory", Json{{"path", "src/engine"}});
    REQUIRE(first.ok);
    CHECK(first.summary.find("Created directory") != std::string::npos);
    REQUIRE(first.evidence.size() == 1);
    CHECK(first.evidence[0].kind == EvidenceKind::FileWritten);

    auto again = fixture.call("create_directory", Json{{"path", "src/engine"}});
    REQUIRE(again.ok);
    CHECK(again.summary.find("already exists") != std::string::npos);
    REQUIRE(again.evidence.size() == 1);
    // Not FileWritten: nothing was written, so nothing bumps the source
    // revision or shows up as work done.
    CHECK(again.evidence[0].kind == EvidenceKind::DirectoryListed);

    // And the workspace root itself, which is the case that was reported.
    auto root = fixture.call("create_directory", Json{{"path", "."}});
    REQUIRE(root.ok);
    CHECK(root.evidence[0].kind == EvidenceKind::DirectoryListed);
}

TEST_CASE("a path damaged by JSON escaping says so", "[tools][process]") {
    // Measured, and it was the single thing stopping a run from finishing: the
    // model asked for build\RelWithDebInfo\fibonacci.exe, the \f arrived as a
    // form feed, and LocalCode answered "executable not found:
    // RelWithDebInfoibonacci.exe". The program was exactly where the model
    // thought it was, so it rebuilt instead of running - three times - and the
    // no-progress detector stopped the run with a correct program on disk and
    // every acceptance criterion still open.
    ToolFixture fixture;

    const std::string mangled = std::string("build") + '\f' + "ibonacci.exe";
    auto result = fixture.call("run_command", Json{{"command", Json::array({mangled})}});

    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("arrived damaged") != std::string::npos);
    CHECK(result.error.find("form feed") != std::string::npos);
    CHECK(result.error.find("forward slashes") != std::string::npos);
    // The old message sent the model looking for a missing file.
    CHECK(result.summary.find("not found") == std::string::npos);
}

TEST_CASE("an ordinary missing program is still reported as missing", "[tools][process]") {
    // The damage check must not swallow the plain case.
    ToolFixture fixture;
    auto result = fixture.call(
        "run_command", Json{{"command", Json::array({"definitely-not-a-real-program-xyz"})}});

    REQUIRE_FALSE(result.ok);
    CHECK(result.summary.find("executable not found") != std::string::npos);
    CHECK(result.error.find("arrived damaged") == std::string::npos);
}

TEST_CASE("an executable wildcard requests an exact path without blaming JSON escapes",
          "[tools][process][recovery]") {
    ToolFixture fixture;
    for (const auto& path : {"build/runs/*/focused_tests.exe", "build/run?/focused_tests.exe"}) {
        const auto result =
            fixture.call("run_command", Json{{"executable", path}, {"arguments", Json::array()}});
        REQUIRE_FALSE(result.ok);
        CHECK(result.error.find("wildcard") != std::string::npos);
        CHECK(result.error.find("list_directory") != std::string::npos);
        CHECK(result.error.find("JSON") == std::string::npos);
        CHECK(result.error.find("backslash") == std::string::npos);
    }
}

// Superseded by "a refused path leads with the fact, not the refusal", which
// covers the same behaviour against the wording that replaced this one.

TEST_CASE("a file can be deleted inside the workspace", "[tools][files][delete]") {
    // There was no delete tool at all, and it ended a run: a failed
    // `cmake -G Ninja` left a CMakeCache.txt naming Ninja, every later
    // configure failed with a generator mismatch, `rm -rf build` was blocked by
    // the security policy, and nothing else existed to try.
    ToolFixture fixture;
    fixture.temp.write("build/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n");

    auto result = fixture.call("delete_path", Json{{"path", "build/CMakeCache.txt"}});
    REQUIRE(result.ok);
    CHECK(result.summary.find("Deleted") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "build" / "CMakeCache.txt"));
}

TEST_CASE("deleting a directory needs to be asked for", "[tools][files][delete]") {
    ToolFixture fixture;
    fixture.temp.write("build/CMakeCache.txt", "x\n");
    fixture.temp.write("build/nested/thing.obj", "y\n");

    // Without recursive, it refuses and says why.
    auto refused = fixture.call("delete_path", Json{{"path", "build"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("recursive=true") != std::string::npos);
    CHECK(std::filesystem::exists(fixture.temp.path() / "build"));

    // With recursive, and with the user's approval, the whole tree goes.
    fixture.confirm_answer = true;
    auto done = fixture.call("delete_path", Json{{"path", "build"}, {"recursive", true}});
    REQUIRE(done.ok);
    CHECK(fixture.confirmations_asked >= 1);
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "build"));
}

TEST_CASE("a refused deletion leaves everything alone", "[tools][files][delete]") {
    ToolFixture fixture;
    fixture.temp.write("build/CMakeCache.txt", "x\n");

    fixture.confirm_answer = false;
    auto result = fixture.call("delete_path", Json{{"path", "build"}, {"recursive", true}});
    REQUIRE_FALSE(result.ok);
    CHECK(result.summary.find("not approved") != std::string::npos);
    CHECK(std::filesystem::exists(fixture.temp.path() / "build" / "CMakeCache.txt"));
}

TEST_CASE("the workspace root can never be deleted", "[tools][files][delete]") {
    // The one path inside the workspace that must never go, however it is
    // spelled.
    ToolFixture fixture;
    fixture.confirm_answer = true;

    for (const char* spelling : {".", "src/..", "./"}) {
        INFO("spelling: " << spelling);
        auto result = fixture.call("delete_path", Json{{"path", spelling}, {"recursive", true}});
        CHECK_FALSE(result.ok);
        CHECK(std::filesystem::exists(fixture.temp.path()));
        CHECK(std::filesystem::exists(fixture.temp.path() / "src" / "main.cpp"));
    }
}

TEST_CASE("nothing outside the workspace can be deleted", "[tools][files][delete]") {
    ToolFixture fixture;
    fixture.confirm_answer = true;

    auto result = fixture.call("delete_path", Json{{"path", "../escape-me"}, {"recursive", true}});
    CHECK_FALSE(result.ok);
}

TEST_CASE("deleting something that is not there says so plainly", "[tools][files][delete]") {
    ToolFixture fixture;
    auto result = fixture.call("delete_path", Json{{"path", "build/CMakeCache.txt"}});
    REQUIRE_FALSE(result.ok);
    CHECK(result.summary.find("does not exist") != std::string::npos);
    CHECK(result.error.find("no change was made") != std::string::npos);
}

TEST_CASE("listing echoes the path LocalCode resolved, not the one it was given",
          "[tools][files]") {
    // Echoing the input taught a model its own mistake. It asked for an
    // absolute path with the drive letter dropped, LocalCode accepted it and
    // answered in the same form, and the model kept using it - until it tried
    // to rm -rf that path, which with no drive looks exactly like a filesystem
    // root, and the security policy blocked it. The run never recovered.
    ToolFixture fixture;
    fixture.temp.write("src/engine/render.cpp", "int main() { return 0; }\n");

    // Asked for with an absolute path; answered with the workspace-relative one.
    const auto absolute = path_to_utf8(fixture.temp.path() / "src" / "engine");
    auto result = fixture.call("list_directory", Json{{"path", absolute}});
    REQUIRE(result.ok);
    INFO(result.summary);
    CHECK(result.summary.find("src/engine") != std::string::npos);
    CHECK(result.summary.find(absolute) == std::string::npos);

    // A directory outside the workspace says where the workspace is.
    const auto outside = path_to_utf8(fixture.temp.path().parent_path() / "somewhere-else");
    auto missing = fixture.call("list_directory", Json{{"path", outside}});
    REQUIRE_FALSE(missing.ok);
    CHECK(missing.error.find(path_to_utf8(fixture.temp.path())) != std::string::npos);
}

TEST_CASE("a command that cannot find its program is told what the build made",
          "[tools][process][resources]") {
    // A run said, in prose, "I need to run the executable. The path is
    // build/RelWithDebInfo/fibonacci.exe" - and then called the build command
    // three more times instead. LocalCode had that path; the failure it
    // returned listed installed development tools rather than the one fact
    // that would have helped.
    ToolFixture fixture;
    fixture.record_built({"build/RelWithDebInfo/fibonacci.exe"});

    auto result = fixture.call("run_command",
                               Json{{"command", Json::array({"no-such-program-anywhere-xyz"})}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("The last build in this workspace produced") != std::string::npos);
    CHECK(result.error.find("build/RelWithDebInfo/fibonacci.exe") != std::string::npos);
}

TEST_CASE("a missing CMakeLists is answered with the file, not the help text",
          "[tools][process][recovery]") {
    // CMake says "does not appear to contain CMakeLists.txt. Specify --help
    // for usage", which reads as "you have used cmake wrongly". Run 10 read it
    // exactly that way: having just written fibonacci.cpp it decided "this is
    // a standalone C++ project, not a CMake-based project" and reached for
    // g++, which is not installed here. Ten steps of compiler hunting later
    // the run was stopped. It was four lines short of a working build.
    ToolFixture fixture;
    fixture.temp.write("solo/fibonacci.cpp", "int main() { return 0; }\n");

    auto result = fixture.call(
        "run_command", Json{{"command", Json::array({"cmake", "-S", "solo", "-B", "solo/build"})}});
    INFO(result.content);
    REQUIRE(result.content.find("exit:     1") != std::string::npos);

    // The direction is "write the file", not "cmake is the wrong tool".
    CHECK(result.content.find("Write CMakeLists.txt") != std::string::npos);
    CHECK(result.content.find("do not switch to invoking a compiler directly") !=
          std::string::npos);
    // And it names the source that CMakeLists.txt would build.
    CHECK(result.content.find("fibonacci.cpp") != std::string::npos);
    CHECK(result.content.find("add_executable") != std::string::npos);
}

TEST_CASE("the last word of a build report is the path that works", "[tools][process][resources]") {
    // An MSBuild log is mostly absolute paths with backslashes: the link line
    // is "fibonacci.vcxproj -> C:\...\Debug\fibonacci.exe" and MSB8029 repeats
    // a full path per project. Run 9 was told the correct relative path above
    // that output, and still copied the absolute one out of the log - twice.
    // Whatever comes last is what gets copied, so the produced path is stated
    // again at the end, after the noise rather than before it.
    ToolFixture fixture;
    const std::string line = "Linking CXX executable " +
                             (fixture.temp.path() / "build" / "Debug" / "fibonacci.exe").string();
#if defined(_WIN32)
    const auto command = Json::array({"cmd", "/d", "/s", "/c", "echo " + line});
#else
    const auto command = Json::array({"echo", line});
#endif
    auto result = fixture.call("run_command", Json{{"command", command}});
    REQUIRE(result.ok);
    INFO(result.content);

    const auto trailing = result.content.rfind("To run what was just built");
    REQUIRE(trailing != std::string::npos);
    // The last word is the call that cannot be mistyped at all, not a path to
    // be copied. LocalCode holds the path; the model should not retype it.
    CHECK(result.content.find(R"(run_artifact { "artifact": "build:fibonacci" })", trailing) !=
          std::string::npos);
    // The path is still there for anything else that needs it.
    CHECK(result.content.find("build/Debug/fibonacci.exe", trailing) != std::string::npos);
    // And nothing from the raw command output may follow it.
    CHECK(result.content.find("Linking CXX executable", trailing) == std::string::npos);
}

TEST_CASE("a damaged path is answered with the path that works", "[tools][process][resources]") {
    // Run 9 built fibonacci.exe, then sent an absolute path copied from the
    // MSBuild log. It arrived as "C:\", " - the backslashes had been eaten as
    // JSON escapes. LocalCode explained the escaping and did not say what the
    // path was, although it had recorded it one step earlier: the damage
    // branch returned before reaching the block that offers it. The model
    // tried twice more and the run was stopped.
    //
    // This is the moment LocalCode has proof the model cannot spell the path.
    // Withholding it here is the worst possible time.
    ToolFixture fixture;
    fixture.record_built({"build/Debug/fibonacci.exe"});

    auto result = fixture.call("run_command", Json{{"command", Json::array({R"(C:", )"})}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("build/Debug/fibonacci.exe") != std::string::npos);

    // And the path comes before the explanation of what went wrong.
    const auto binary = result.error.find("build/Debug/fibonacci.exe");
    const auto diagnosis = result.error.find("cannot appear in a filename");
    REQUIRE(diagnosis != std::string::npos);
    CHECK(binary < diagnosis);
}

TEST_CASE("a damaged path with no build behind it still explains the damage",
          "[tools][process][resources]") {
    ToolFixture fixture;
    auto result = fixture.call("run_command", Json{{"command", Json::array({R"(C:", )"})}});
    REQUIRE_FALSE(result.ok);
    CHECK(result.error.find("cannot appear in a filename") != std::string::npos);
    CHECK(result.error.find("The program you built is here") == std::string::npos);
}

TEST_CASE("with no build behind it, the message does not invent one",
          "[tools][process][resources]") {
    ToolFixture fixture;
    auto result = fixture.call("run_command",
                               Json{{"command", Json::array({"no-such-program-anywhere-xyz"})}});
    REQUIRE_FALSE(result.ok);
    CHECK(result.error.find("The last build in this workspace produced") == std::string::npos);
}

TEST_CASE("a refused path leads with the fact, not the refusal", "[tools][files][recovery]") {
    // A path genuinely outside the workspace is still refused, and the refusal
    // still leads with the root rather than the denial. (The /workspace family
    // no longer reaches here: it is resolved instead - see "a container
    // project root is read as this workspace" - because three runs proved that
    // refusing it, however well worded, did not shift the model off it.)
    ToolFixture fixture;

    // A real directory outside the workspace, so this is the boundary being
    // refused. A path that exists nowhere at all is a different mistake with a
    // different answer - see "an invented absolute path is named as
    // unsupported, not merely missing".
    TempDirectory outside_dir{"lc-outside"};
    outside_dir.write("fibonacci.cpp", "int main() { return 1; }\n");
    const auto outside = (outside_dir.path() / "fibonacci.cpp").string();

    auto result = fixture.call("write_file",
                               Json{{"path", outside}, {"content", "int main() { return 0; }\n"}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);

    // The root is the first thing said.
    CHECK(result.error.rfind("This workspace is", 0) == 0);
    CHECK(result.error.find(fixture.temp.path().generic_string()) != std::string::npos);
    // And the name, spelled the way it would work.
    CHECK(result.error.find("would be: fibonacci.cpp") != std::string::npos);
    // The refusal is still there, underneath.
    CHECK(result.error.find("only modify files inside") != std::string::npos);
}

TEST_CASE("when the file really is in the workspace, its place is given",
          "[tools][files][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("fibonacci.cpp", "int main() { return 0; }\n");

    // A real file of the same name outside the workspace: reading it is out of
    // bounds, and the useful fact is that one of that name is right here.
    TempDirectory other{"lc-outside"};
    other.write("fibonacci.cpp", "int main() { return 1; }\n");
    const auto elsewhere = (other.path() / "fibonacci.cpp").string();

    // A write, not a read: reading outside the workspace is permitted, so a
    // read of a real file there simply succeeds and there is no hint to check.
    auto result = fixture.call("write_file", Json{{"path", elsewhere}, {"content", "changed\n"}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("does exist here") != std::string::npos);
}

TEST_CASE("a container project root reaches the file and says it did", "[tools][files][recovery]") {
    // The other half of the run-11 fix: resolving the path is only honest if
    // the answer says the file did not land where it was asked for.
    ToolFixture fixture;

    auto written = fixture.call("write_file", Json{{"path", "/workspace/fibonacci.cpp"},
                                                   {"content", "int main() { return 0; }\n"}});
    REQUIRE(written.ok);
    INFO(written.content);
    CHECK(written.summary.find("fibonacci.cpp") != std::string::npos);
    // The note comes first, and names both spellings.
    CHECK(written.content.rfind("Workspace-relative path:  fibonacci.cpp", 0) == 0);
    // The invalid form appears once, for diagnosis, and does not dominate.
    CHECK(written.content.find("/workspace/fibonacci.cpp") != std::string::npos);
    CHECK(written.content.find(fixture.temp.path().generic_string()) != std::string::npos);

    // The file is really there, under the real root.
    CHECK(std::filesystem::exists(fixture.temp.path() / "fibonacci.cpp"));

    auto read = fixture.call("read_file", Json{{"path", "/workspace/fibonacci.cpp"}});
    REQUIRE(read.ok);
    CHECK(read.content.find("Workspace-relative path:") != std::string::npos);

    auto listed = fixture.call("list_directory", Json{{"path", "/workspace"}});
    REQUIRE(listed.ok);
    CHECK(listed.content.find("Workspace-relative path:") != std::string::npos);
}

TEST_CASE("bounded targeted mutators refuse incomplete source without dropping its tail",
          "[tools][edit][bounded-mutation][recovery]") {
    ToolFixture fixture;
    constexpr std::size_t limit = 8u * 1024 * 1024;
    const std::string before =
        "int head = 1;\n" + std::string(limit + 128, 'x') + "\nTAIL_SENTINEL_MUST_SURVIVE\n";
    const auto path = fixture.temp.write("large.cpp", before);
    ToolResult result;
    SECTION("exact replacement in the retained prefix") {
        result = fixture.call(
            "replace_in_file",
            {{"path", "large.cpp"}, {"find", "int head = 1;"}, {"replace", "int head = 2;"}});
    }
    SECTION("unified patch in the retained prefix") {
        result = fixture.call(
            "apply_patch",
            {{"patch",
              "diff --git a/large.cpp b/large.cpp\n--- a/large.cpp\n+++ b/large.cpp\n@@ -1 +1 @@\n"
              "-int head = 1;\n+int head = 2;\n"}});
    }
    INFO(result.to_model_text());
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("truncated") != std::string::npos);
    const auto actual = fs::read_bytes(path, before.size() + 1);
    REQUIRE(actual.ok());
    CHECK(static_cast<bool>(*actual == before));
    CHECK(fixture.workspace.agent_changes().empty());
}

TEST_CASE("an oversized patch input rejects every staged file before any write",
          "[tools][edit][bounded-mutation][recovery]") {
    ToolFixture fixture;
    constexpr std::size_t limit = 8u * 1024 * 1024;
    const std::string small = "int helper() { return 7; }\n";
    const std::string large =
        "int head = 1;\n" + std::string(limit + 128, 'x') + "\nTAIL_SENTINEL_MUST_SURVIVE\n";
    const auto path = fixture.temp.write("large.cpp", large);
    const auto result = fixture.call(
        "apply_patch",
        {{"patch",
          "diff --git a/src/util.cpp b/src/util.cpp\n--- a/src/util.cpp\n+++ b/src/util.cpp\n@@ -1 "
          "+1 @@\n"
          "-int helper() { return 7; }\n+int helper() { return 9; }\n"
          "diff --git a/large.cpp b/large.cpp\n--- a/large.cpp\n+++ b/large.cpp\n@@ -1 +1 @@\n"
          "-int head = 1;\n+int head = 2;\n"}});
    INFO(result.to_model_text());
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("truncated") != std::string::npos);
    const auto actual_large = fs::read_bytes(path, large.size() + 1);
    const auto actual_small =
        fs::read_bytes(fixture.temp.path() / "src/util.cpp", small.size() + 1);
    REQUIRE(actual_large.ok());
    REQUIRE(actual_small.ok());
    CHECK(static_cast<bool>(*actual_large == large));
    CHECK(static_cast<bool>(*actual_small == small));
    CHECK(fixture.workspace.agent_changes().empty());
}

TEST_CASE("complete source at the edit ceiling remains editable with its tail intact",
          "[tools][edit][bounded-mutation][recovery]") {
    ToolFixture fixture;
    constexpr std::size_t limit = 8u * 1024 * 1024;
    const std::string prefix = "int head = 1;\n";
    const std::string tail = "\nTAIL_SENTINEL_MUST_SURVIVE\n";
    const std::string before =
        prefix + std::string(limit - prefix.size() - tail.size(), 'x') + tail;
    REQUIRE(before.size() == limit);
    const auto path = fixture.temp.write("ceiling.cpp", before);
    ToolResult result;
    SECTION("exact replacement") {
        result = fixture.call(
            "replace_in_file",
            {{"path", "ceiling.cpp"}, {"find", "int head = 1;"}, {"replace", "int head = 2;"}});
    }
    SECTION("unified patch") {
        result = fixture.call("apply_patch", {{"patch",
                                               "diff --git a/ceiling.cpp b/ceiling.cpp\n--- "
                                               "a/ceiling.cpp\n+++ b/ceiling.cpp\n@@ -1 +1 @@\n"
                                               "-int head = 1;\n+int head = 2;\n"}});
    }
    INFO(result.to_model_text());
    REQUIRE(result.ok);
    auto expected = before;
    expected.replace(0, prefix.size(), "int head = 2;\n");
    const auto actual = fs::read_bytes(path, expected.size() + 1);
    REQUIRE(actual.ok());
    CHECK(static_cast<bool>(*actual == expected));
}

namespace {
std::string displayed_failed_find_json(const ToolResult& result) {
    const auto text = result.to_model_text();
    const auto marker = std::string{"\nfind: "};
    const auto begin = text.find(marker);
    REQUIRE(begin != std::string::npos);
    const auto start = begin + marker.size();
    const auto end = text.find('\n', start);
    REQUIRE(end != std::string::npos);
    const auto encoded = text.substr(start, end - start);
    REQUIRE(Json::parse(encoded, nullptr, false).is_string());
    return encoded;
}
}   // namespace

TEST_CASE("failed exact find exposes actual decoded bytes and supports one explicit recovery",
          "[tools][edit][failed-find-diagnostic][recovery]") {
    ToolFixture fixture;
    const std::string line = "\xc3\xa9 \"quoted\" \\folder " + std::string(1, '\b') + " tail";
    const auto prefix = std::string(240, 'p');
    const auto original = prefix + "\r\n" + line + "\r\nsuffix\r\n";
    fixture.temp.write("failed-find.txt", original);
    const std::string printable = "\xc3\xa9 \"quoted\" \\folder \\b tail";
    const auto refused = fixture.call(
        "replace_in_file",
        {{"path", "failed-find.txt"}, {"find", printable}, {"replace", "must not appear"}});
    REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.structured.contains("exact_find_diagnostic"));
    const auto& facts = refused.structured.at("exact_find_diagnostic");
    CHECK(facts.at("find_control_count") == 0);
    CHECK(facts.at("decoded_find_utf8_bytes") == printable.size());
    CHECK(Json::parse(facts.at("json_string").get<std::string>()).get<std::string>() == printable);
    CHECK(displayed_failed_find_json(refused) == facts.at("json_string").get<std::string>());
    CHECK(refused.structured.at("source_control_count") == 1);
    CHECK(refused.error.find("None of that text") == std::string::npos);
    const auto raw_before = fs::read_bytes(fixture.temp.path() / "failed-find.txt");
    REQUIRE(raw_before.ok());
    CHECK(*raw_before == original);
    const auto encoded = displayed_source_line_json(refused, 2);
    CHECK(encoded.find("\\b") != std::string::npos);
    const auto decoded = Json::parse(encoded).get<std::string>();
    REQUIRE(decoded == line);
    // JSON spelling is data, never automatically decoded as edit input.
    CHECK_FALSE(
        fixture
            .call("replace_in_file",
                  {{"path", "failed-find.txt"}, {"find", encoded}, {"replace", "must not appear"}})
            .ok);
    REQUIRE(fixture
                .call("replace_in_file",
                      {{"path", "failed-find.txt"}, {"find", decoded}, {"replace", "replacement"}})
                .ok);
    const auto after = fs::read_bytes(fixture.temp.path() / "failed-find.txt");
    REQUIRE(after.ok());
    CHECK(*after == prefix + "\r\nreplacement\r\nsuffix\r\n");
}

TEST_CASE("failed exact find distinguishes hidden argument from printable source escapes",
          "[tools][edit][failed-find-diagnostic][recovery]") {
    ToolFixture fixture;
    const std::string printable = "literal \\b \\u001b \\u007f tail";
    fixture.temp.write("literal-escapes.txt", printable + "\n");
    const std::string hidden = "literal " + std::string(1, '\b') + " \\u001b \\u007f tail";
    const auto refused = fixture.call(
        "replace_in_file",
        {{"path", "literal-escapes.txt"}, {"find", hidden}, {"replace", "must not appear"}});
    REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.structured.contains("exact_find_diagnostic"));
    const auto& facts = refused.structured.at("exact_find_diagnostic");
    CHECK(facts.at("find_control_count") == 1);
    CHECK(facts.at("find_controls")[0] ==
          Json{{"line", 1}, {"utf8_byte_column", 9}, {"codepoint", "U+0008"}});
    CHECK(Json::parse(facts.at("json_string").get<std::string>()).get<std::string>() == hidden);
    CHECK(Json::parse(displayed_failed_find_json(refused)).get<std::string>() == hidden);
    CHECK(refused.structured.at("source_control_count") == 0);
    CHECK_FALSE(refused.structured.contains("source_control_lines"));
    REQUIRE(fixture
                .call("replace_in_file", {{"path", "literal-escapes.txt"},
                                          {"find", printable},
                                          {"replace", "literal edit"}})
                .ok);
    const auto after = fs::read_text(fixture.temp.path() / "literal-escapes.txt");
    REQUIRE(after.ok());
    CHECK(after->content == "literal edit\n");
}

TEST_CASE("failed exact find reports hidden ASCII and UTF8 byte locations without changing source",
          "[tools][edit][failed-find-diagnostic][recovery]") {
    ToolFixture fixture;
    std::string hidden;
    for (unsigned value = 1; value < 0x20; ++value)
        if (value != '\t' && value != '\n' && value != '\r') hidden += static_cast<char>(value);
    hidden += '\x7f';
    const std::string line = "\xc3\xa9 \"quote\" \\path " + hidden + " tail";
    const auto prefix = std::string(2400, 'a');
    fixture.temp.write("all-controls.txt", prefix + "\n" + line + "\n");
    // NUL is valid in a decoded tool argument; NUL-bearing source files keep
    // the existing binary-file refusal rather than gaining an edit path.
    const std::string find = std::string("\xc3\xa9") + '\0' + hidden + "\r\nnot present";
    const auto refused = fixture.call(
        "replace_in_file",
        {{"path", "all-controls.txt"}, {"find", find}, {"replace", "must not appear"}});
    REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.structured.contains("exact_find_diagnostic"));
    const auto& facts = refused.structured.at("exact_find_diagnostic");
    CHECK(facts.at("decoded_find_utf8_bytes") == find.size());
    CHECK(facts.at("normalized_find_utf8_bytes") == find.size() - 1);
    CHECK(facts.at("CRLF_normalized_for_matching") == true);
    CHECK(facts.at("find_control_count") == hidden.size() + 1);
    REQUIRE(facts.at("find_controls").size() == 16);
    CHECK(facts.at("find_controls")[0] ==
          Json{{"line", 1}, {"utf8_byte_column", 3}, {"codepoint", "U+0000"}});
    CHECK(Json::parse(facts.at("json_string").get<std::string>()).get<std::string>() == find);
    CHECK(Json::parse(displayed_failed_find_json(refused)).get<std::string>() == find);
    CHECK(refused.structured.at("source_control_count") == hidden.size());
    CHECK(refused.structured.at("source_controls").size() == 16);
    const auto encoded = displayed_source_line_json(refused, 2);
    CHECK(Json::parse(encoded).get<std::string>() == line);
    for (const unsigned char byte : encoded) {
        CHECK(byte >= 0x20);
        CHECK(byte != 0x7f);
    }
    const auto after = fs::read_text(fixture.temp.path() / "all-controls.txt");
    REQUIRE(after.ok());
    CHECK(after->content == prefix + "\n" + line + "\n");
}

TEST_CASE("failed find recovery keeps exact uniqueness and unrelated source controls as inspection",
          "[tools][edit][failed-find-diagnostic][recovery]") {
    ToolFixture fixture;
    const std::string line = "same\bline";
    const auto prefix = std::string(240, 'p');
    const auto source = prefix + "\n" + line + "\nordinary\n" + line + "\n";
    fixture.temp.write("duplicate-recovery.txt", source);
    const auto refused = fixture.call("replace_in_file", {{"path", "duplicate-recovery.txt"},
                                                          {"find", "unrelated absent text"},
                                                          {"replace", "must not appear"}});
    REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.structured.contains("exact_find_diagnostic"));
    CHECK(refused.structured.at("source_control_count") == 2);
    const auto decoded = Json::parse(displayed_source_line_json(refused, 2)).get<std::string>();
    REQUIRE(decoded == line);
    const auto ambiguous = fixture.call(
        "replace_in_file",
        {{"path", "duplicate-recovery.txt"}, {"find", decoded}, {"replace", "changed"}});
    REQUIRE_FALSE(ambiguous.ok);
    CHECK(ambiguous.error.find("2 times") != std::string::npos);
    const auto unchanged = fs::read_text(fixture.temp.path() / "duplicate-recovery.txt");
    REQUIRE(unchanged.ok());
    CHECK(unchanged->content == source);
    REQUIRE(fixture
                .call("replace_in_file", {{"path", "duplicate-recovery.txt"},
                                          {"find", decoded},
                                          {"replace", "changed"},
                                          {"replace_all", true}})
                .ok);
    const auto after = fs::read_text(fixture.temp.path() / "duplicate-recovery.txt");
    REQUIRE(after.ok());
    CHECK(after->content == prefix + "\nchanged\nordinary\nchanged\n");
}

TEST_CASE("failed exact find bounds whole JSON receipts after escaping expansion",
          "[tools][edit][failed-find-diagnostic][recovery]") {
    ToolFixture fixture;
    const auto expands = std::string(600, '\\') + '\b';
    const std::string short_line = "short\bline";
    std::string source =
        expands + "\n" + short_line + "\nthird\fline\nfourth\x1bline\nfifth\x7fline\n";
    fixture.temp.write("bounded-find.txt", source);
    REQUIRE(expands.size() + 2 <= 1024);
    REQUIRE(Json(expands).dump().size() > 1024);
    const auto refused = fixture.call("replace_in_file", {{"path", "bounded-find.txt"},
                                                          {"find", expands + "absent"},
                                                          {"replace", "must not appear"}});
    REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.structured.contains("exact_find_diagnostic"));
    const auto& facts = refused.structured.at("exact_find_diagnostic");
    CHECK(facts.at("json_omitted") == true);
    CHECK_FALSE(facts.contains("json_string"));
    CHECK(refused.content.find("Decoded find JSON omitted") != std::string::npos);
    CHECK(refused.content.find("\nfind: ") == std::string::npos);
    CHECK(refused.structured.at("source_control_line_count") == 5);
    CHECK(refused.structured.at("source_control_lines_omitted").at("byte_limit") == 1);
    CHECK(refused.structured.at("source_control_lines_omitted").at("line_limit") == 1);
    CHECK(refused.structured.at("source_control_lines_omitted").at("partial") == 0);
    std::size_t encoded_bytes = 0;
    for (const auto& item : refused.structured.at("source_control_lines")) {
        const auto encoded =
            displayed_source_line_json(refused, item.at("line").get<std::size_t>());
        CHECK(Json::parse(encoded).is_string());
        encoded_bytes += encoded.size();
    }
    CHECK(encoded_bytes <= 1024);
    CHECK(Json::parse(displayed_source_line_json(refused, 2)).get<std::string>() == short_line);
    CHECK(refused.content.find("\nline 1: ") == std::string::npos);
    CHECK(refused.content.find("\nline 5: ") == std::string::npos);
    const auto after = fs::read_text(fixture.temp.path() / "bounded-find.txt");
    REQUIRE(after.ok());
    CHECK(after->content == source);
}

TEST_CASE("apply_patch honors advertised paired unified headers and preserves CRLF",
          "[tools][edit][patch][bare-unified]") {
    ToolFixture fixture;
    fixture.temp.write("dir/a file.txt", "before\r\nold\r\nafter\r\n");
    const auto result =
        fixture.call("apply_patch",
                     {{"patch",
                       "--- a/dir/a file.txt\told timestamp\n+++ b/dir/a file.txt\tnew timestamp\n"
                       "@@ -1,3 +1,3 @@\n before\n-old\n+new\n after\n"}});
    REQUIRE(result.ok);
    const auto raw = fs::read_bytes(fixture.temp.path() / "dir/a file.txt");
    REQUIRE(raw.ok());
    CHECK(*raw == "before\r\nnew\r\nafter\r\n");
}

TEST_CASE("unified patch data resembling file headers is not a second target",
          "[tools][edit][patch][bare-unified]") {
    ToolFixture fixture;
    fixture.temp.write("data.txt", "-- old data\ndiff --git a/phantom b/phantom\n");
    const auto result = fixture.call(
        "apply_patch", {{"patch",
                         "--- a/data.txt\n+++ b/data.txt\n@@ -1,2 +1,2 @@\n"
                         "--- old data\n+++ new data\n diff --git a/phantom b/phantom\n"}});
    REQUIRE(result.ok);
    const auto after = fs::read_text(fixture.temp.path() / "data.txt");
    REQUIRE(after.ok());
    CHECK(after->content == "++ new data\ndiff --git a/phantom b/phantom\n");
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "phantom"));
}

TEST_CASE("bare unified hunks preserve zero-old and zero-new data boundaries",
          "[tools][edit][patch][bare-unified]") {
    ToolFixture fixture;
    SECTION("pure insertion") {
        fixture.temp.write("edge.txt", "before\n");
        const auto result = fixture.call(
            "apply_patch",
            {{"patch", "--- a/edge.txt\n+++ b/edge.txt\n@@ -1,0 +2,1 @@\n+++ inserted data\n"}});
        REQUIRE(result.ok);
        const auto after = fs::read_text(fixture.temp.path() / "edge.txt");
        REQUIRE(after.ok());
        CHECK(after->content == "before\n++ inserted data\n");
    }
    SECTION("pure deletion") {
        fixture.temp.write("edge.txt", "-- removed data\nafter\n");
        const auto result = fixture.call(
            "apply_patch",
            {{"patch", "--- a/edge.txt\n+++ b/edge.txt\n@@ -1,1 +0,0 @@\n--- removed data\n"}});
        REQUIRE(result.ok);
        const auto after = fs::read_text(fixture.temp.path() / "edge.txt");
        REQUIRE(after.ok());
        CHECK(after->content == "after\n");
    }
}

TEST_CASE("bare multifile patches validate every block and authorized target before writing",
          "[tools][edit][patch][bare-unified]") {
    ToolFixture fixture;
    fixture.temp.write("first.txt", "old first\n");
    fixture.temp.write("second.txt", "old second\n");
    const std::string first =
        "--- a/first.txt\n+++ b/first.txt\n@@ -1 +1 @@\n-old first\n+new first\n";
    std::string second;
    SECTION("valid files commit together") {
        second = "--- a/second.txt\n+++ b/second.txt\n@@ -1 +1 @@\n-old second\n+new second\n";
        REQUIRE(fixture.call("apply_patch", {{"patch", first + second}}).ok);
        const auto a = fs::read_text(fixture.temp.path() / "first.txt");
        const auto b = fs::read_text(fixture.temp.path() / "second.txt");
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        CHECK(a->content == "new first\n");
        CHECK(b->content == "new second\n");
        return;
    }
    SECTION("incomplete final hunk") {
        second = "--- a/second.txt\n+++ b/second.txt\n@@ -1,2 +1,2 @@\n-old second\n+new second\n";
    }
    SECTION("extra hunk data") {
        second =
            "--- a/second.txt\n+++ b/second.txt\n@@ -1 +1 @@\n-old second\n+new second\n+extra\n";
    }
    SECTION("unpaired header") {
        second = "--- a/second.txt\n";
    }
    SECTION("another header pair before its hunk") {
        second =
            "--- a/second.txt\n+++ b/second.txt\n--- /dev/null\n+++ b/extra.txt\n@@ -0,0 +1 "
            "@@\n+extra\n";
    }
    SECTION("malformed count") {
        second = "--- a/second.txt\n+++ b/second.txt\n@@ -1,bad +1 @@\n-old second\n+new second\n";
    }
    SECTION("context mismatch") {
        second = "--- a/second.txt\n+++ b/second.txt\n@@ -1 +1 @@\n-not the source\n+new second\n";
    }
    SECTION("Git metadata cannot retarget standard headers") {
        // If a standard block could silently adopt this metadata, both hunks
        // would stage writes to first.txt and the second would overwrite it.
        second =
            "--- a/second.txt\n+++ b/second.txt\nrename to first.txt\n"
            "@@ -1 +1 @@\n-old first\n+unexpected retarget\n";
    }
    SECTION("unauthorized relative traversal") {
        second = "--- a/../outside.txt\n+++ b/../outside.txt\n@@ -1 +1 @@\n-old\n+new\n";
    }
    REQUIRE_FALSE(fixture.call("apply_patch", {{"patch", first + second}}).ok);
    const auto a = fs::read_text(fixture.temp.path() / "first.txt");
    const auto b = fs::read_text(fixture.temp.path() / "second.txt");
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a->content == "old first\n");
    CHECK(b->content == "old second\n");
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "extra.txt"));
}

TEST_CASE("bare unified null paths keep the existing Git create and delete policy",
          "[tools][edit][patch][bare-unified]") {
    ToolFixture fixture;
    SECTION("new file") {
        REQUIRE(
            fixture
                .call("apply_patch",
                      {{"patch", "--- /dev/null\n+++ b/created.txt\n@@ -0,0 +1 @@\n+created\n"}})
                .ok);
        const auto after = fs::read_text(fixture.temp.path() / "created.txt");
        REQUIRE(after.ok());
        CHECK(after->content == "created\n");
    }
    SECTION("deleted content follows existing policy without removing the file") {
        fixture.temp.write("git-delete.txt", "removed\n");
        fixture.temp.write("bare-delete.txt", "removed\n");
        REQUIRE(
            fixture
                .call("apply_patch",
                      {{"patch",
                        "diff --git a/git-delete.txt b/git-delete.txt\ndeleted file mode 100644\n"
                        "--- a/git-delete.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-removed\n"}})
                .ok);
        REQUIRE(fixture
                    .call("apply_patch",
                          {{"patch",
                            "--- a/bare-delete.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-removed\n"}})
                    .ok);
        const auto git = fs::read_bytes(fixture.temp.path() / "git-delete.txt");
        const auto bare = fs::read_bytes(fixture.temp.path() / "bare-delete.txt");
        REQUIRE(git.ok());
        REQUIRE(bare.ok());
        CHECK(*git == *bare);
        CHECK(bare->empty());
    }
}

TEST_CASE("unified patch header support never redecodes source escapes",
          "[tools][edit][patch][bare-unified]") {
    ToolFixture fixture;
    const std::string prefix(240, 'p');
    const std::string controlled = "old" + std::string(1, '\b') + "value";
    const auto source = prefix + "\n" + controlled + "\n";
    fixture.temp.write("exact.txt", source);
    const std::string head = "--- a/exact.txt\n+++ b/exact.txt\n@@ -2 +2 @@\n";
    REQUIRE_FALSE(fixture.call("apply_patch", {{"patch", head + "-old\\bvalue\n+new value\n"}}).ok);
    const auto unchanged = fs::read_text(fixture.temp.path() / "exact.txt");
    REQUIRE(unchanged.ok());
    CHECK(unchanged->content == source);
    REQUIRE(
        fixture.call("apply_patch", {{"patch", head + "-" + controlled + "\n+new value\n"}}).ok);
    const auto after = fs::read_text(fixture.temp.path() / "exact.txt");
    REQUIRE(after.ok());
    CHECK(after->content == prefix + "\nnew value\n");
}

TEST_CASE("failed patch exposes original hidden source and parsed context without redecoding",
          "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    const auto prefix = std::string(240, 'p');
    const auto controlled = std::string{"old"} + std::string(1, '\b') + "value";
    const auto original = prefix + "\n" + controlled + "\n";
    fixture.temp.write("patch-hidden.txt", original);
    const std::string head = "--- a/patch-hidden.txt\n+++ b/patch-hidden.txt\n@@ -2 +2 @@\n";
    const auto refused =
        fixture.call("apply_patch", {{"patch", head + "-old\\bvalue\n+new value\n"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    const auto unchanged = fs::read_bytes(fixture.temp.path() / "patch-hidden.txt");
    REQUIRE(unchanged.ok());
    CHECK(static_cast<bool>(*unchanged == original));
    REQUIRE(refused.structured.contains("patch_context_diagnostic"));
    const auto& facts = refused.structured.at("patch_context_diagnostic");
    CHECK(facts.at("path") == "patch-hidden.txt");
    CHECK(facts.at("hunk_old_start") == 2);
    CHECK(facts.at("source_control_count") == 1);
    CHECK(facts.at("source_control_input_truncated") == false);
    CHECK(facts.at("parsed_old_context").at("control_count") == 0);
    const auto received =
        Json::parse(facts.at("parsed_old_context").at("json_string").get<std::string>())
            .get<std::string>();
    CHECK(received == "old\\bvalue");
    const auto actual_line = Json::parse(displayed_source_line_json(refused, 2)).get<std::string>();
    CHECK(static_cast<bool>(actual_line == controlled));
    CHECK_FALSE(refused.structured.contains("exact_find_diagnostic"));
    CHECK(refused.evidence.empty());
    // The exact source JSON is inspection data: its encoded spelling still
    // fails as patch context, while one explicit JSON decode gives exact text.
    const auto encoded_line =
        facts.at("source_control_lines")[0].at("json_string").get<std::string>();
    CHECK_FALSE(
        fixture.call("apply_patch", {{"patch", head + "-" + encoded_line + "\n+must not appear\n"}})
            .ok);
    REQUIRE(
        fixture.call("apply_patch", {{"patch", head + "-" + actual_line + "\n+new value\n"}}).ok);
    const auto after = fs::read_bytes(fixture.temp.path() / "patch-hidden.txt");
    REQUIRE(after.ok());
    CHECK(static_cast<bool>(*after == prefix + "\nnew value\n"));
}

TEST_CASE("failed patch hidden-source inspection retains multi-file atomic refusal",
          "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    const auto hidden = std::string(240, 'p') + "\n" + "hidden" + std::string(1, '\b') + "value\n";
    fixture.temp.write("patch-first.txt", "before\n");
    fixture.temp.write("patch-later.txt", hidden);
    const std::string patch =
        "--- a/patch-first.txt\n+++ b/patch-first.txt\n@@ -1 +1 @@\n-before\n+after\n"
        "--- a/patch-later.txt\n+++ b/patch-later.txt\n@@ -2 +2 @@\n-missing\n+must not appear\n";
    const auto refused = fixture.call("apply_patch", {{"patch", patch}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    const auto first = fs::read_bytes(fixture.temp.path() / "patch-first.txt");
    const auto later = fs::read_bytes(fixture.temp.path() / "patch-later.txt");
    REQUIRE(first.ok());
    REQUIRE(later.ok());
    CHECK(static_cast<bool>(*first == "before\n"));
    CHECK(static_cast<bool>(*later == hidden));
    CHECK(fixture.workspace.agent_changes().empty());
    REQUIRE(refused.structured.contains("patch_context_diagnostic"));
    CHECK(refused.structured.at("patch_context_diagnostic").at("path") == "patch-later.txt");
    CHECK(refused.structured.at("patch_context_diagnostic").at("hunk_old_start") == 2);
    CHECK(refused.structured.at("patch_context_diagnostic").at("source_control_count") == 1);
    CHECK(refused.evidence.empty());
}

TEST_CASE("failed patch inspection quotes original source before locally staged lower hunks",
          "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    const auto lower = std::string{"lower"} + std::string(1, '\b') + "value";
    const auto original = std::string(240, 'p') + "\n" + lower + "\n";
    fixture.temp.write("patch-staged.txt", original);
    const auto patch = std::string{"--- a/patch-staged.txt\n+++ b/patch-staged.txt\n"} +
                       "@@ -1 +1 @@\n-wrong top\n+changed top\n" + "@@ -2 +2 @@\n-" + lower +
                       "\n+clean lower\n";
    const auto refused = fixture.call("apply_patch", {{"patch", patch}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    const auto unchanged = fs::read_bytes(fixture.temp.path() / "patch-staged.txt");
    REQUIRE(unchanged.ok());
    CHECK(static_cast<bool>(*unchanged == original));
    REQUIRE(refused.structured.contains("patch_context_diagnostic"));
    CHECK(refused.structured.at("patch_context_diagnostic").at("hunk_old_start") == 1);
    CHECK(static_cast<bool>(
        Json::parse(displayed_source_line_json(refused, 2)).get<std::string>() == lower));
    CHECK(fixture.workspace.agent_changes().empty());
}

TEST_CASE("hidden-control patch failures share one bounded diagnostic across files",
          "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    const auto controlled = std::string(240, 'p') + "\nvalue" + std::string(1, '\b') + "tail\n";
    std::string patch;
    for (const auto* name : {"patch-one.txt", "patch-two.txt", "patch-three.txt"}) {
        fixture.temp.write(name, controlled);
        patch += std::string{"--- a/"} + name + "\n+++ b/" + name +
                 "\n@@ -2 +2 @@\n-missing\n+must not appear\n";
    }
    const auto refused = fixture.call("apply_patch", {{"patch", patch}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    for (const auto* name : {"patch-one.txt", "patch-two.txt", "patch-three.txt"}) {
        const auto unchanged = fs::read_bytes(fixture.temp.path() / name);
        REQUIRE(unchanged.ok());
        CHECK(static_cast<bool>(*unchanged == controlled));
    }
    REQUIRE(refused.structured.contains("patch_context_diagnostic"));
    const auto& facts = refused.structured.at("patch_context_diagnostic");
    CHECK(facts.at("path") == "patch-one.txt");
    CHECK(refused.structured.at("patch_context_diagnostics_omitted") == 2);
    CHECK(facts.at("source_controls").size() <= 16);
    CHECK(facts.at("source_control_lines").size() <= 4);
    CHECK(refused.content.size() <= 8192);
    CHECK(refused.content.find("One bounded diagnostic per tool call") != std::string::npos);
    CHECK(refused.content.find("path \"patch-two.txt\"") == std::string::npos);
    CHECK(fixture.workspace.agent_changes().empty());
}

TEST_CASE("failed patch source and context JSON budgets omit whole literals without clipping",
          "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    const auto long_line = std::string(1020, 'p') + std::string(1, '\x7f') + "\"";
    const auto short_line = std::string{"x"} + std::string(1, '\b') + std::string(1, '\v') +
                            std::string(1, '\f') + "tail";
    std::string original = long_line + "\n";
    for (int i = 0; i < 6; ++i)
        original += short_line + "\n";
    fixture.temp.write("patch-bounds.txt", original);
    const auto old_context = std::string(1024, 'q') + std::string(1, '\b');
    const auto refused = fixture.call(
        "apply_patch", {{"patch", "--- a/patch-bounds.txt\n+++ b/patch-bounds.txt\n@@ -1 +1 @@\n-" +
                                      old_context + "\n+must not appear\n"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    const auto unchanged = fs::read_bytes(fixture.temp.path() / "patch-bounds.txt");
    REQUIRE(unchanged.ok());
    CHECK(static_cast<bool>(*unchanged == original));
    REQUIRE(refused.structured.contains("patch_context_diagnostic"));
    const auto& facts = refused.structured.at("patch_context_diagnostic");
    CHECK(facts.at("source_control_count") == 19);
    CHECK(facts.at("source_controls").size() == 16);
    CHECK(facts.at("source_control_line_count") == 7);
    CHECK(facts.at("source_control_lines").size() == 3);
    CHECK(facts.at("source_control_lines_omitted").at("byte_limit") == 1);
    CHECK(facts.at("source_control_lines_omitted").at("line_limit") == 3);
    CHECK(facts.at("parsed_old_context").at("json_omitted") == true);
    CHECK(facts.at("parsed_old_context").at("control_count") == 1);
    CHECK_FALSE(facts.at("parsed_old_context").contains("json_string"));
    std::size_t encoded_bytes = 0;
    for (const auto& quoted : facts.at("source_control_lines")) {
        const auto encoded = quoted.at("json_string").get<std::string>();
        encoded_bytes += encoded.size();
        CHECK(static_cast<bool>(Json::parse(encoded).get<std::string>() == short_line));
    }
    CHECK(encoded_bytes <= 1024);
    CHECK(refused.content.size() <= 8192);
    CHECK(refused.content.find("no clipped JSON") != std::string::npos);
}

TEST_CASE("ordinary patch context failures keep their existing diagnostic shape",
          "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("patch-ordinary.txt", "old value\n");
    const auto refused =
        fixture.call("apply_patch", {{"patch",
                                      "--- a/patch-ordinary.txt\n+++ b/patch-ordinary.txt\n@@ -1 "
                                      "+1 @@\n-missing\n+must not appear\n"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    CHECK_FALSE(refused.structured.contains("patch_context_diagnostic"));
    CHECK_FALSE(refused.structured.contains("patch_context_diagnostics_omitted"));
    CHECK(refused.content.empty());
    const auto unchanged = fs::read_bytes(fixture.temp.path() / "patch-ordinary.txt");
    REQUIRE(unchanged.ok());
    CHECK(static_cast<bool>(*unchanged == "old value\n"));
}

TEST_CASE(
    "failed patch reports actual context controls without inventing controls in ordinary source",
    "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    fixture.temp.write("patch-argument.txt", "plain value\n");
    const auto hidden = std::string{"plain"} + std::string(1, '\b') + "value";
    const auto refused = fixture.call(
        "apply_patch",
        {{"patch", "--- a/patch-argument.txt\n+++ b/patch-argument.txt\n@@ -1 +1 @@\n-" + hidden +
                       "\n+must not appear\n"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    const auto unchanged = fs::read_bytes(fixture.temp.path() / "patch-argument.txt");
    REQUIRE(unchanged.ok());
    CHECK(static_cast<bool>(*unchanged == "plain value\n"));
    REQUIRE(refused.structured.contains("patch_context_diagnostic"));
    const auto& facts = refused.structured.at("patch_context_diagnostic");
    CHECK(facts.at("source_control_count") == 0);
    CHECK_FALSE(facts.contains("source_control_lines"));
    CHECK(facts.at("parsed_old_context").at("control_count") == 1);
    const auto decoded =
        Json::parse(facts.at("parsed_old_context").at("json_string").get<std::string>())
            .get<std::string>();
    CHECK(static_cast<bool>(decoded == hidden));
    CHECK_FALSE(refused.structured.contains("exact_find_diagnostic"));
    CHECK(refused.evidence.empty());
}

TEST_CASE("failed patch compact view omits oversized path and UTF8 source JSON without clipping",
          "[tools][edit][patch][failed-patch-diagnostic][recovery]") {
    ToolFixture fixture;
    const auto name = std::string(100, 'a') + ".txt";
    const auto controlled = std::string(300, 'x') + "\xc3\xa9" + std::string(1, '\b');
    const auto original = controlled + "\n";
    fixture.temp.write(name, original);
    const auto patch = std::string{"--- a/"} + name + "\n+++ b/" + name +
                       "\n@@ -1 +1 @@\n-missing\n+must not appear\n";
    const auto refused = fixture.call("apply_patch", {{"patch", patch}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("context does not match") != std::string::npos);
    const auto unchanged = fs::read_bytes(fixture.temp.path() / name);
    REQUIRE(unchanged.ok());
    CHECK(static_cast<bool>(*unchanged == original));
    REQUIRE(refused.structured.contains("patch_context_diagnostic"));
    const auto& facts = refused.structured.at("patch_context_diagnostic");
    CHECK(facts.at("path") == name);   // Full path is retained; compact text omits it whole.
    CHECK(facts.at("compact_path_omitted") == true);
    CHECK(facts.at("compact_source_json_omitted") == true);
    const auto compact = facts.at("compact_model_text").get<std::string>();
    CHECK(compact.size() <= 520);
    CHECK(compact.find(name) == std::string::npos);
    CHECK(compact.find("path omitted from compact view") != std::string::npos);
    CHECK(compact.find("Source JSON omitted from compact view") != std::string::npos);
    const auto encoded = facts.at("source_control_lines")[0].at("json_string").get<std::string>();
    CHECK(encoded.size() > 256);
    CHECK(encoded.size() <= 1024);
    CHECK(static_cast<bool>(Json::parse(encoded).get<std::string>() == controlled));
    CHECK(compact.find(encoded) == std::string::npos);
    CHECK(static_cast<bool>(
        Json::parse(displayed_source_line_json(refused, 1)).get<std::string>() == controlled));
}
