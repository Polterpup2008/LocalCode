// ---------------------------------------------------------------------------
// Adversarial criteria: true, cleanly settled, and beside the point.
//
// The dangerous shape is not a false criterion - the reference checker catches
// those. It is a criterion that passes honestly and says nothing about what was
// asked. An agent writing its own definition of done drifts towards the
// definition it can satisfy, and "it compiles" is always the easiest one going.
//
// The opposite case is why this is not simply strict: when the task really is
// "make it compile", a build criterion is the whole of it.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "agent/CriterionStrength.h"
#include "agent/ReferenceCheck.h"

using namespace localcode;

// A ------------------------------------------------------------------------
TEST_CASE("a wrong sequence is caught even when the request gives a range",
          "[criterion][adversarial]") {
    // "Print squares from 1 through 5" states its count as a range rather than
    // a count, which the checker used to read as "no opinion" - and a criterion
    // claiming the answer is 1 2 3 4 5 would have gone unchallenged.
    const auto verdict = check_against_reference("Print the squares from 1 through 5.",
                                                 "output is 1 2 3 4 5");
    INFO(verdict.conflict);
    CHECK(verdict.applicable);
    CHECK_FALSE(verdict.agrees);
    CHECK(verdict.conflict.find("1 4 9 16 25") != std::string::npos);

    // The right answer still agrees.
    const auto right = check_against_reference("Print the squares from 1 through 5.",
                                               "output is 1 4 9 16 25");
    CHECK(right.applicable);
    CHECK(right.agrees);

    // Other ways of writing the same range.
    CHECK(check_against_reference("print squares 1 to 5", "1 2 3 4 5").applicable);
    CHECK(check_against_reference("squares from 1 through 6", "1 4 9 16 25 36").agrees);
}

// B ------------------------------------------------------------------------
TEST_CASE("checking that a field exists is not checking its value",
          "[criterion][adversarial]") {
    // "count = 4" and a criterion of "the JSON contains a count field": true of
    // count = 0, which is the bug being reported.
    const auto finding = assess_plan_strength(
        "Return JSON with count = 4.",
        {"the response is valid JSON", "the JSON output contains a count field"});
    INFO(finding.reason);
    CHECK(finding.insufficient);
    CHECK(finding.reason.find("4") != std::string::npos);

    // Naming the value closes it.
    const auto named = assess_plan_strength(
        "Return JSON with count = 4.",
        {"the JSON output has count = 4"});
    CHECK_FALSE(named.insufficient);
}

// C ------------------------------------------------------------------------
TEST_CASE("a task about a crash is not settled by the project building",
          "[criterion][adversarial]") {
    // It built before the fix too.
    const auto finding = assess_plan_strength(
        "Fix the crash when opening an empty file.",
        {"the project builds", "no compiler warnings"});
    INFO(finding.reason);
    CHECK(finding.insufficient);
    CHECK(finding.suggestion.find("running the program") != std::string::npos);

    // A criterion about what happens on an empty file closes it.
    const auto covered = assess_plan_strength(
        "Fix the crash when opening an empty file.",
        {"the project builds", "opening an empty file returns cleanly instead of crashing"});
    CHECK_FALSE(covered.insufficient);
}

// D ------------------------------------------------------------------------
TEST_CASE("a task about compiling is settled by compiling", "[criterion][adversarial]") {
    // The point of the opposite case: demanding a behavioural check here would
    // be noise, and noise is what buries the warnings that matter.
    const auto finding =
        assess_plan_strength("Make the project compile.", {"a fresh build succeeds"});
    CHECK_FALSE(finding.insufficient);

    CHECK(demand_of("Make the project compile.") == ObjectiveDemand::BuildOnly);
    CHECK(demand_of("Fix the crash when opening an empty file.") ==
          ObjectiveDemand::RuntimeBehaviour);
    // Both mentioned: the behaviour is what is being asked for, the build is on
    // the way there.
    CHECK(demand_of("Build it and check it prints the right answer.") ==
          ObjectiveDemand::RuntimeBehaviour);
}

TEST_CASE("a request this cannot read is left alone", "[criterion][adversarial]") {
    // Silence is the normal answer. Most tasks say nothing this can classify,
    // and guessing at them would block ordinary work.
    CHECK(demand_of("Tidy up the header includes.") == ObjectiveDemand::Unknown);
    CHECK_FALSE(assess_plan_strength("Tidy up the header includes.",
                                     {"the project builds"})
                    .insufficient);
    CHECK_FALSE(assess_plan_strength("Fix the crash on empty input", {}).insufficient);
}

TEST_CASE("the Fibonacci benchmark's own plans are not flagged",
          "[criterion][adversarial][regression]") {
    // This check can block an objective, so it must not fire on ordinary work.
    // These are the criteria real runs actually wrote, copied from the logs.
    const std::string objective =
        "Write a C++ program that prints the first ten Fibonacci numbers starting from 0, "
        "one per line. Build it and verify it actually prints them.";

    CHECK(demand_of(objective) == ObjectiveDemand::RuntimeBehaviour);

    const std::vector<std::vector<std::string>> real_plans = {
        {"C++ program prints the first ten Fibonacci numbers starting from 0, one per line",
         "Program builds successfully with CMake",
         "Program runs and outputs the correct Fibonacci sequence"},
        {"C++ program prints first ten Fibonacci numbers (0, 1, 1, 2, 3, 5, 8, 13, 21, 34) "
         "starting from 0, one per line",
         "Build succeeds without errors",
         "Program output matches expected Fibonacci sequence: 0, 1, 1, 2, 3, 5, 8, 13, 21, 34"},
        {"The program prints exactly ten Fibonacci numbers starting from 0",
         "Program builds successfully",
         "Program runs and produces correct output"},
    };
    for (const auto& plan : real_plans) {
        INFO(plan.front());
        const auto finding = assess_plan_strength(objective, plan);
        INFO(finding.reason);
        CHECK_FALSE(finding.insufficient);
    }
}

TEST_CASE("a value demand is only read from a real assignment",
          "[criterion][adversarial][regression]") {
    // The value rule blocks an objective, so a phrase that merely looks like an
    // assignment must not trigger it. "one per line" and "starting from 0" are
    // not "count = 4".
    const auto finding = assess_plan_strength(
        "Write a program that prints the first ten Fibonacci numbers starting from 0, "
        "one per line.",
        {"the program runs and prints ten numbers"});
    INFO(finding.reason);
    CHECK_FALSE(finding.insufficient);
}

TEST_CASE("ordinary prose does not invent required output values",
          "[criterion][adversarial][regression]") {
    const auto finding = assess_plan_strength(
        "Edit ONLY README.md: the expected demo output is outdated. Preserve Ninja/MSVC. "
        "Check discounted_total=850 and checkout_total=1350.",
        {"README contains discounted_total=850, checkout_total=1350 and Ninja/MSVC",
         "Existing checkout.exe prints discounted_total=850 and checkout_total=1350"});
    INFO(finding.reason);
    CHECK_FALSE(finding.insufficient);
    CHECK_FALSE(assess_plan_strength("The output is wrong; return the correct result.",
                                     {"the program runs and returns the correct result"})
                    .insufficient);
    // Skip the adjective and still inspect the concrete requirement after it.
    CHECK(assess_plan_strength("The output is outdated. Return JSON with count = 4.",
                               {"the JSON output contains a count field"})
              .insufficient);
}

TEST_CASE("named numeric and quoted values still require matching criteria",
          "[criterion][adversarial][regression]") {
    for (const auto* objective : {"Return count is 4.", "Return count: 4.",
                                  "Return count equals 4.", "Return count of 4."}) {
        INFO(objective);
        CHECK(assess_plan_strength(objective, {"the output contains count"}).insufficient);
        CHECK_FALSE(assess_plan_strength(objective, {"the output contains count 4"}).insufficient);
    }
    CHECK(assess_plan_strength("Return status is \"ready\".", {"the output contains status"})
              .insufficient);
    CHECK_FALSE(
        assess_plan_strength("Return status is \"ready\".", {"the output contains status ready"})
            .insufficient);
}
