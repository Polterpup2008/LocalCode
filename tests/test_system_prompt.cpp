#include <catch2/catch_test_macros.hpp>
#include "agent/SystemPrompt.h"
#include "platform/FileSystem.h"
#include "platform/PathEncoding.h"
#include "support/TempDirectory.h"
#include "workspace/Workspace.h"

using namespace localcode;

TEST_CASE("small-window guidance preserves scope and proof rules without task examples",
          "[prompt][budget]") {
    ModelProfile profile;
    profile.context_size = 4096;
    PromptContext context;
    context.profile = &profile;
    context.compact_instructions = true;
    context.web_enabled = false;
    const auto compact = SystemPrompt::build(context);
    for (const char* invariant :
         {"scope", "approvals", "set_acceptance_criteria", "independent", "STALE", "expect",
          "criterion", "number(s)", "contains_all", "content only", "executable", "arguments",
          "recorded evidence", "NOT VERIFIED", "Web access is disabled", "user's language",
          "command output unchanged"}) {
        INFO(invariant);
        CHECK(compact.find(invariant) != std::string::npos);
    }
    CHECK(compact.find("Fibonacci") == std::string::npos);
    CHECK(compact.find("No project folder") != std::string::npos);
    profile.context_size = 32768;
    const auto full = SystemPrompt::build(context);
    CHECK(full.find(SystemPrompt::evidence_section()) != std::string::npos);
    CHECK(compact.size() * 3 < full.size());
    profile.context_size = 8192;
    CHECK(SystemPrompt::build(context) == compact);
    context.read_only = true;
    context.file_tools_only = true;
    const auto restricted = SystemPrompt::build(context);
    CHECK(restricted.find("READ-ONLY") != std::string::npos);
    CHECK(restricted.find("FILE TOOLS ONLY") != std::string::npos);
    CHECK(restricted.find("Internal feedback cannot expand") != std::string::npos);
    CHECK(restricted.find("Do not invoke commands") != std::string::npos);
    context.compact_instructions = false;
    CHECK(SystemPrompt::build(context).find(SystemPrompt::evidence_section()) != std::string::npos);
}

TEST_CASE("compact guidance retains project facts and complete bounded conventions",
          "[prompt][budget][unicode]") {
    test::TempDirectory temporary{"lc-short-prompt"};
    const auto root = temporary.path() / path_from_utf8("projet é 路径");
    std::filesystem::create_directories(root);
    Workspace workspace;
    REQUIRE(workspace.open(root));
    REQUIRE(fs::write_text_atomic(root / "AGENTS.md",
                                  "Use the existing compiler and retain the public API.\n"));
    RepositoryMap map;
    map.detected_build_system = "owned-build-system";
    map.total_files = 13;
    map.languages.emplace_back("C++", 7);
    map.top_level_entries.push_back("owned-entry");
    map.key_files.push_back("owned-manifest");
    ModelProfile profile;
    profile.context_size = 4096;
    PromptContext context;
    context.profile = &profile;
    context.compact_instructions = true;
    context.workspace = &workspace;
    context.repository_map = &map;
    const auto prompt = SystemPrompt::build(context);
    for (const auto& fact :
         {path_to_utf8(root), std::string("owned-build-system"), std::string("owned-entry"),
          std::string("owned-manifest"), std::string("13 files"), std::string("C++ x7"),
          std::string("Use the existing compiler and retain the public API."),
          std::string("they are not permissions")}) {
        INFO(fact);
        CHECK(prompt.find(fact) != std::string::npos);
    }
    CHECK(prompt.find(SystemPrompt::project_section(context)) != std::string::npos);
    CHECK(prompt.find("write/delete only inside") != std::string::npos);
}

TEST_CASE("verification reporting does not override a read-only literal answer format",
          "[prompt][evidence][recovery][candidate6]") {
    ModelProfile profile;
    profile.context_size = 32768;
    PromptContext context;
    context.profile = &profile;
    context.read_only = true;
    const auto prompt = SystemPrompt::build(context);
    CHECK(prompt.find("When you finish, state explicitly whether the result is VERIFIED") ==
          std::string::npos);
    CHECK(prompt.find("requested response format") != std::string::npos);
    CHECK(prompt.find("literal failing input") != std::string::npos);
    // The correction must preserve actual evidence requirements for coding tasks.
    CHECK(prompt.find("requires a build command that exited successfully") != std::string::npos);
    CHECK(prompt.find("requires a test run that exited successfully") != std::string::npos);
}
