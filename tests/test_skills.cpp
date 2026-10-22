// Skill parsing, validation and selection - including the real bundled set.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "inference/ChatTypes.h"
#include "skills/SkillRegistry.h"
#include "support/TempDirectory.h"

using namespace localcode;
using localcode::test::TempDirectory;

TEST_CASE("a skill file is parsed", "[skills]") {
    const std::string text = R"(---
id: test-skill
name: A Test Skill
description: Describes when this procedure applies.
tags: [cpp, testing]
triggers: [build error, linker]
tools: [read_file, run_command]
priority: 70
---

# The procedure

1. Do the first thing.
2. Do the second thing, which is different.
3. Verify that it worked by checking the output.
)";

    auto skill = parse_skill(text, "test/SKILL.md");
    REQUIRE(skill.ok());
    CHECK(skill->id == "test-skill");
    CHECK(skill->name == "A Test Skill");
    CHECK(skill->priority == 70);
    REQUIRE(skill->tags.size() == 2);
    REQUIRE(skill->triggers.size() == 2);
    CHECK(skill->triggers[0] == "build error");
    REQUIRE(skill->tools.size() == 2);
    CHECK(skill->body.find("# The procedure") != std::string::npos);
    CHECK(skill->validate().empty());
}

TEST_CASE("block-style lists are accepted", "[skills]") {
    const std::string text = R"(---
id: block-style
name: Block style
description: Uses YAML block lists rather than inline ones.
triggers:
  - first trigger
  - second trigger
tags:
  - alpha
---

A procedure body that is long enough to pass validation, describing what to do
and how to check that it worked properly.
)";

    auto skill = parse_skill(text, "block/SKILL.md");
    REQUIRE(skill.ok());
    REQUIRE(skill->triggers.size() == 2);
    CHECK(skill->triggers[1] == "second trigger");
    REQUIRE(skill->tags.size() == 1);
}

TEST_CASE("a file without frontmatter is rejected", "[skills]") {
    auto skill = parse_skill("# Just markdown\n\nNo frontmatter here.", "bad/SKILL.md");
    REQUIRE_FALSE(skill.ok());
    CHECK(skill.error().code == ErrorCode::ParseError);
}

TEST_CASE("validation catches the mistakes that matter", "[skills]") {
    Skill skill;
    CHECK_FALSE(skill.validate().empty());   // empty everything

    skill.id = "has spaces and!";
    skill.name = "Name";
    skill.description = "Description.";
    skill.body = std::string(200, 'x');
    skill.triggers = {"trigger"};
    auto problems = skill.validate();
    CHECK_FALSE(problems.empty());   // bad id characters

    skill.id = "good-id";
    CHECK(skill.validate().empty());

    SECTION("a skill with no way to be selected is invalid") {
        skill.triggers.clear();
        skill.tags.clear();
        CHECK_FALSE(skill.validate().empty());
    }
    SECTION("a trivially short body is invalid") {
        skill.body = "too short";
        CHECK_FALSE(skill.validate().empty());
    }
    SECTION("a one-character trigger is invalid") {
        skill.triggers = {"x"};
        CHECK_FALSE(skill.validate().empty());
    }
}

TEST_CASE("serialisation round-trips", "[skills]") {
    Skill skill;
    skill.id = "round-trip";
    skill.name = "Round Trip";
    skill.description = "A description, with a comma in it.";
    skill.tags = {"a", "b"};
    skill.triggers = {"one trigger", "another"};
    skill.priority = 42;
    skill.validated = false;
    skill.body = std::string("A body long enough to satisfy validation. ") + std::string(120, 'y');

    auto parsed = parse_skill(serialize_skill(skill), "x/SKILL.md");
    REQUIRE(parsed.ok());
    CHECK(parsed->id == skill.id);
    CHECK(parsed->description == skill.description);
    CHECK(parsed->tags == skill.tags);
    CHECK(parsed->triggers == skill.triggers);
    CHECK(parsed->priority == 42);
    CHECK_FALSE(parsed->validated);
}

TEST_CASE("the bundled skills all load and validate", "[skills][bundled]") {
    SkillRegistry registry;
    auto status = registry.load_directory(LOCALCODE_TEST_SKILLS_DIR);
    REQUIRE(status.ok());

    INFO("load errors: " << (registry.load_errors().empty() ? std::string{"none"}
                                                            : registry.load_errors().front()));
    CHECK(registry.load_errors().empty());

    // Exactly as many skills as there are skill directories.
    //
    // This was `>= 15`, which is not a test: it passes while a skill silently
    // fails to parse, and it passed while one was missing from the repository
    // altogether. Counting the directories makes the assertion say what it
    // means - every skill on disk is a skill in the registry.
    std::size_t directories = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(LOCALCODE_TEST_SKILLS_DIR, ec)) {
        if (entry.is_directory(ec) &&
            std::filesystem::is_regular_file(entry.path() / "SKILL.md", ec)) {
            ++directories;
        }
    }
    REQUIRE(directories > 0);
    INFO("skill directories on disk: " << directories);
    CHECK(registry.skills().size() == directories);

    // Skills the agent's behaviour actually depends on. Named explicitly because
    // a missing file changes the directory count too, so the count check above
    // cannot notice that one of these has gone.
    //
    // `cmake-build-fix` is here because it really did disappear: an unanchored
    // `cmake-build-*/` rule in .gitignore matched `skills/cmake-build-fix/`, so
    // the skill existed on one machine and in no clone of the repository.
    for (const char* required : {"verify-before-done", "cmake-build-fix", "minimal-patch",
                                 "compile-test-fix-loop", "dependency-bootstrap"}) {
        INFO("required skill: " << required);
        CHECK(registry.find(required) != nullptr);
    }

    const auto* verify = registry.find("verify-before-done");
    REQUIRE(verify != nullptr);
    CHECK(verify->always);

    for (const auto& skill : registry.skills()) {
        INFO("skill: " << skill.id);
        CHECK(skill.validate().empty());
        CHECK(skill.validated);
    }
}

TEST_CASE("selection returns relevant skills and always-on ones", "[skills][selection]") {
    SkillRegistry registry;
    REQUIRE(registry.load_directory(LOCALCODE_TEST_SKILLS_DIR).ok());

    SECTION("a build failure selects the build skills") {
        auto matches = registry.select("the project has a compile error in render.cpp", "", 3);
        REQUIRE_FALSE(matches.empty());

        bool found_build_skill = false;
        for (const auto& match : matches) {
            if (match.skill->id == "compile-test-fix-loop" ||
                match.skill->id == "cmake-build-fix") {
                found_build_skill = true;
            }
        }
        CHECK(found_build_skill);
    }

    SECTION("a git question selects the git skill") {
        auto matches = registry.select("can you reset my branch and discard changes", "", 4);
        bool found = false;
        for (const auto& match : matches) {
            if (match.skill->id == "git-safe-workflow") found = true;
        }
        CHECK(found);
    }

    SECTION("always-on skills appear regardless of the query") {
        auto matches = registry.select("something completely unrelated to anything", "", 3);
        bool found_always = false;
        for (const auto& match : matches) {
            if (match.skill->always) found_always = true;
        }
        CHECK(found_always);
    }

    SECTION("source navigation does not import unrelated edit or linker procedures") {
        auto matches = registry.select(
            "Locate the implementation that starts and stops the managed model server. "
            "Explain which functions own process launch, readiness and child cleanup. "
            "Cite the relevant source files and line ranges. Keep the answer concise. "
            "Do not edit files or execute commands.",
            "", 3);
        for (const auto& match : matches) {
            CHECK(match.skill->id != "linker-errors");
            CHECK(match.skill->id != "git-safe-workflow");
            CHECK(match.skill->id != "minimal-patch");
        }
    }

    SECTION("a substring does not imply an unrelated trigger") {
        auto matches = registry.select("Explain the trace output", "", 3);
        for (const auto& match : matches)
            CHECK(match.skill->id != "concurrency-bugs");
        auto linker = registry.select("Fix LNK2019: unresolved external symbol", "", 3);
        CHECK(std::any_of(linker.begin(), linker.end(),
                          [](const auto& match) { return match.skill->id == "linker-errors"; }));
    }

    SECTION("the result count is bounded") {
        auto matches =
            registry.select("build cmake error test git crash shader performance", "", 2);
        // always-on skills are additional to the cap, but the total stays small.
        CHECK(matches.size() <= 6);
    }
}

TEST_CASE("disabled and unvalidated skills are not selected", "[skills][selection]") {
    TempDirectory temp{"lc-skills"};
    temp.write("draft/SKILL.md", R"(---
id: draft
name: Draft
description: A skill the agent wrote that nobody has reviewed yet.
triggers: [draft trigger phrase]
validated: false
---

This body is long enough to pass validation but the skill is not trusted yet,
so the selector must leave it alone until a person enables it.
)");

    SkillRegistry registry;
    REQUIRE(registry.load_directory(temp.path()).ok());
    REQUIRE(registry.skills().size() == 1);

    auto matches = registry.select("draft trigger phrase", "", 3);
    CHECK(matches.empty());
}

TEST_CASE("rendering produces one prompt block", "[skills]") {
    SkillRegistry registry;
    REQUIRE(registry.load_directory(LOCALCODE_TEST_SKILLS_DIR).ok());

    auto matches = registry.select("fix the build error", "", 2);
    const auto rendered = SkillRegistry::render(matches);
    CHECK_FALSE(rendered.empty());
    CHECK(rendered.find("Skill:") != std::string::npos);
    CHECK(SkillRegistry::render({}).empty());
}

TEST_CASE("skill rendering respects a token budget", "[skills][budget]") {
    SkillRegistry registry;
    REQUIRE(registry.load_directory(LOCALCODE_TEST_SKILLS_DIR).ok());

    auto matches = registry.select("fix the cmake build error in this c++ project", "", 3);
    REQUIRE_FALSE(matches.empty());

    const auto unbounded = SkillRegistry::render(matches);
    REQUIRE_FALSE(unbounded.empty());

    SECTION("a generous budget changes nothing") {
        const auto rendered = SkillRegistry::render(matches, 100000);
        CHECK(rendered == unbounded);
    }

    SECTION("a tight budget is honoured") {
        const int budget = 400;
        const auto rendered = SkillRegistry::render(matches, budget);
        CHECK(rendered.size() < unbounded.size());
        // Allow generous slack for the estimator, but it must be bounded.
        CHECK(estimate_tokens(rendered) < budget * 2);
    }

    SECTION("a partially included skill says so") {
        const auto rendered = SkillRegistry::render(matches, 500);
        if (rendered.size() < unbounded.size() && !rendered.empty()) {
            const bool bounded =
                rendered.find("abridged") != std::string::npos || estimate_tokens(rendered) <= 500;
            CHECK(bounded);
        }
    }

    SECTION("a budget too small for anything yields nothing rather than a stub") {
        const auto rendered = SkillRegistry::render(matches, 20);
        CHECK(rendered.empty());
    }
}

TEST_CASE("only one skill is unconditionally present", "[skills][budget]") {
    SkillRegistry registry;
    REQUIRE(registry.load_directory(LOCALCODE_TEST_SKILLS_DIR).ok());

    int always_on = 0;
    for (const auto& skill : registry.skills()) {
        if (skill.always) ++always_on;
    }
    // Each always-on skill costs its full body in every single request, so the
    // bar for adding one is high.
    INFO("always-on skills: " << always_on);
    CHECK(always_on <= 1);
}

TEST_CASE("the shipped skill count is pinned", "[skills][inventory]") {
    // Four places state a skill count - the staged directory, the doctor CLI,
    // the GUI panel and docs/SKILLS.md - and nothing made them agree. The test
    // above ties the registry to what is on disk; this ties both to the number
    // written down, so adding or removing a skill has to be deliberate and the
    // documentation has to move with it.
    SkillRegistry registry;
    REQUIRE(registry.load_directory(LOCALCODE_TEST_SKILLS_DIR).ok());
    INFO("skills loaded: " << registry.skills().size());
    CHECK(registry.skills().size() == 26);
}
