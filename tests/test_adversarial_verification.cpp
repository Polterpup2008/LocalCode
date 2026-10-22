// ---------------------------------------------------------------------------
// Trying to make LocalCode say something false.
//
// Every other test here asks whether the system works. These ask whether it
// can be talked into a claim it has no business making - which is the only
// question that matters for a subsystem whose whole purpose is refusing one.
//
// Each case is written from the attacker's side: here is a way a run could
// arrive at "done", and here is the thing that has to stop it.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "agent/EvidenceInterpreter.h"
#include "agent/Verification.h"
#include "tools/CodeIntegrity.h"
#include "tools/OutputExpectation.h"

using namespace localcode;

namespace {

ToolCall run_call() {
    ToolCall call;
    call.id = "call";
    call.name = "run_command";
    return call;
}

ToolResult ran(const std::string& command, int exit_code, std::string content = {}) {
    ToolResult result;
    result.ok = true;
    result.content = std::move(content);
    result.structured = {{"command", command},
                         {"exit_code", exit_code},
                         {"timed_out", false},
                         {"cancelled", false},
                         {"error_count", 0}};
    return result;
}

void write_file(VerificationLedger& ledger, const std::string& path) {
    ToolCall call;
    call.id = "write";
    call.name = "write_file";
    ToolResult result;
    result.ok = true;
    result.evidence.push_back(Evidence{EvidenceKind::FileWritten, path, "written", true, 0});
    interpret_tool_result(call, result, ledger);
}

bool blocked_for(const CompletionAssessment& assessment, std::string_view fragment) {
    if (!assessment.blocked) return false;
    for (const auto& reason : assessment.reasons) {
        if (reason.find(fragment) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("a file content check settles only its criterion and cannot prove program behaviour",
          "[adversarial][file-check]") {
    VerificationLedger ledger;
    AcceptanceCheck check;
    check.description = "README states the rounding rule";
    ledger.set_acceptance_plan({check});
    ToolCall call;
    call.name = "read_file";
    ToolResult result = ToolResult::success("Read README", "Prices round down.");
    result.structured = {{"path", "README.md"},
                         {"file_check_checked", true},
                         {"file_check_passed", true},
                         {"file_check_criterion", 1},
                         {"file_check_contains", {"Prices round down"}}};
    interpret_tool_result(call, result, ledger);
    REQUIRE(ledger.acceptance_plan().size() == 1);
    CHECK(ledger.acceptance_plan()[0].settled);
    CHECK(ledger.acceptance_plan()[0].satisfied);
    for (auto stage :
         {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution, Stage::Behaviour})
        CHECK(ledger.status(stage) == StageStatus::Unknown);

    write_file(ledger, "README.md");
    CHECK_FALSE(ledger.plan_is_complete());
    result.structured["file_check_passed"] = false;
    interpret_tool_result(call, result, ledger);
    CHECK_FALSE(ledger.acceptance_plan()[0].satisfied);
    CHECK(ledger.plan_has_failure());
}

// 1 --------------------------------------------------------------------------
TEST_CASE("a build that never ran cannot be a passing build", "[adversarial]") {
    VerificationLedger ledger;
    write_file(ledger, "src/main.cpp");

    // Everything short of building: reading, listing, searching, checking the
    // environment. Six tool calls, and none of them a build.
    interpret_tool_result(run_call(), ran("git status", 0), ledger);
    interpret_tool_result(run_call(), ran("where cmake", 0), ledger);
    interpret_tool_result(run_call(), ran("cmake --version", 0), ledger);

    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
    CHECK(blocked_for(assess_completion(ledger, true), "no classified build, run or test evidence"));
}

// 2 --------------------------------------------------------------------------
TEST_CASE("a build from before the last edit cannot verify the code as it stands",
          "[adversarial]") {
    VerificationLedger ledger;
    interpret_tool_result(run_call(), ran("cmake --build build", 0), ledger);
    interpret_tool_result(run_call(), ran("build\\app.exe", 0, "ok"), ledger);
    ledger.set_acceptance_plan({{"it builds and runs"}});
    ledger.settle_check(0, true, "E001");

    // The tidy-up nobody rebuilt. The oldest trick there is.
    write_file(ledger, "src/main.cpp");

    CHECK(ledger.status(Stage::Build) == StageStatus::Stale);
    CHECK(blocked_for(assess_completion(ledger, true), "files have changed since"));
}

// 3 --------------------------------------------------------------------------
TEST_CASE("an empty test suite is not a passing test suite", "[adversarial]") {
    // ctest exits 0 in a directory with no tests. Reading that as a pass would
    // let a run prove correctness by having no tests at all.
    VerificationLedger ledger;
    write_file(ledger, "src/main.cpp");
    interpret_tool_result(run_call(), ran("cmake --build build", 0), ledger);
    interpret_tool_result(run_call(),
                          ran("ctest --output-on-failure", 0,
                              "Test project /work/build\nNo tests were found!!!"),
                          ledger);

    CHECK_FALSE(ledger.holds(Stage::Test));
    CHECK(ledger.status(Stage::Test) == StageStatus::NotApplicable);

    // And with no behaviour check either, the run cannot be called finished.
    CHECK(assess_completion(ledger, true).blocked);
}

// 4 --------------------------------------------------------------------------
TEST_CASE("deleting the assertions is recorded, not rewarded", "[adversarial]") {
    const std::string before =
        "TEST_CASE(\"stats\") {\n"
        "    CHECK(mean(values) == 5.0);\n"
        "    CHECK(median(values) == 4.5);\n"
        "    CHECK(stddev(values) == 2.0);\n"
        "}\n";
    // The suite still "passes". It checks nothing.
    const std::string after = "TEST_CASE(\"stats\") {\n}\n";

    const auto findings = scan_for_test_tampering("tests/test_stats.cpp", before, after);
    REQUIRE_FALSE(findings.empty());
    bool removal_reported = false;
    for (const auto& finding : findings) {
        if (finding.kind == TamperKind::AssertionRemoved) {
            removal_reported = true;
            CHECK(finding.confidence == IntegrityConfidence::High);
        }
    }
    CHECK(removal_reported);
}

// 5 --------------------------------------------------------------------------
TEST_CASE("exit code zero is not a statement about the answer", "[adversarial]") {
    // Run 9, exactly. Configure, build, run, all clean, wrong numbers.
    VerificationLedger ledger;
    write_file(ledger, "src/fib.cpp");
    interpret_tool_result(run_call(), ran("cmake -B build", 0), ledger);
    interpret_tool_result(run_call(), ran("cmake --build build", 0), ledger);
    interpret_tool_result(run_call(),
                          ran("build\\fib.exe", 0, "1 3 6 12 24 48 96 192 384 768"), ledger);

    CHECK(ledger.holds(Stage::Configure));
    CHECK(ledger.holds(Stage::Build));
    CHECK(ledger.holds(Stage::Execution));
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Unknown);
    CHECK(blocked_for(assess_completion(ledger, true), "output was never checked"));
}

// 6 --------------------------------------------------------------------------
TEST_CASE("an expectation that checks nothing is refused outright", "[adversarial]") {
    // The cheapest possible false verification: declare a behaviour check with
    // no comparison in it and collect the green tick.
    Json empty = Json::object();
    empty["description"] = "it works correctly";
    auto parsed = OutputExpectation::from_json(empty);
    REQUIRE_FALSE(parsed.ok());

    // A comparison with no statement of what it proves is accepted, because
    // the comparison itself is the statement - but it is labelled from what
    // actually ran, so it cannot be made to describe something else, and it is
    // marked as derived so no verdict appears to carry an intent nobody
    // stated.
    Json anonymous = Json::object();
    anonymous["stdout_contains"] = Json::array({"ok"});
    const auto derived = OutputExpectation::from_json(anonymous);
    REQUIRE(derived.ok());
    CHECK(derived->description_derived);
    CHECK(derived->description.find(R"(stdout contains "ok")") != std::string::npos);
}

// 7 --------------------------------------------------------------------------
TEST_CASE("the definition of done cannot be rewritten to match the result",
          "[adversarial]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"prints the mean, median and standard deviation correctly"}});

    // The check runs and fails.
    const auto evidence = ledger.add_evidence(
        EvidenceKind::TestsRun, "behaviour check",
        "expected mean: 5 median: 4.5 stddev: 2, actual mean: 5.71429 median: 0 stddev: 4.5102",
        false);
    ledger.record(Stage::Behaviour, StageStatus::Fail, evidence, "output does not match");
    ledger.settle_check(0, false, evidence);

    // Now the criteria are restated as something the run did achieve.
    ToolCall call;
    call.id = "criteria";
    call.name = "set_acceptance_criteria";
    ToolResult result;
    result.ok = true;
    result.structured = {{"acceptance_criteria", Json::array({"the program builds and runs"})}};
    interpret_tool_result(call, result, ledger);

    REQUIRE(ledger.acceptance_plan().size() == 1);
    CHECK(ledger.acceptance_plan()[0].description ==
          "prints the mean, median and standard deviation correctly");
    ledger.reassess_objective();
    CHECK(ledger.status(Stage::Objective) == StageStatus::Fail);
}

// 8 --------------------------------------------------------------------------
TEST_CASE("a stub compiles, links, runs, and is still a stub", "[adversarial]") {
    // Every mechanical signal a stub produces is green, which is what makes it
    // the most effective way to appear finished.
    const auto findings = scan_for_stubs("src/stats.cpp",
                                         "double median(std::vector<double> values) {\n"
                                         "    // TODO: implement this properly\n"
                                         "    return 0.0;\n"
                                         "}\n");
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].line == 2);
    CHECK_FALSE(findings[0].in_test_code);
}

// 9 --------------------------------------------------------------------------
TEST_CASE("an environment script is not a build however it is dressed up",
          "[adversarial]") {
    // vcvars64.bat exits 0 and ignores its arguments. One run recorded four
    // successful builds this way and repeated the command because it kept
    // being told it had worked.
    VerificationLedger ledger;
    write_file(ledger, "src/main.cpp");
    interpret_tool_result(
        run_call(),
        ran(R"("C:\Program Files\...\vcvars64.bat" cmake --build build)", 0), ledger);

    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
    CHECK(assess_completion(ledger, true).blocked);
}

// 10 -------------------------------------------------------------------------
TEST_CASE("a criterion cannot be settled by a check that named a different one",
          "[adversarial]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"the median is right"}, {"the standard deviation is right"}});

    // A check that proves the easy one, pointed at the hard one.
    auto result = ran("build\\stats.exe", 0, "median: 4.5");
    result.structured["expectation_checked"] = true;
    result.structured["expectation_satisfied"] = true;
    result.structured["expectation_description"] = "prints the median";
    result.structured["expectation_criterion"] = 2;
    interpret_tool_result(run_call(), result, ledger);

    // LocalCode cannot know the check was aimed at the wrong criterion - but it
    // records which evidence settled it, by id, so a reader can see that
    // criterion 2 was settled by a check whose description mentions the median.
    REQUIRE(ledger.acceptance_plan()[1].settled);
    const auto* evidence = ledger.find_evidence(ledger.acceptance_plan()[1].evidence_id);
    REQUIRE(evidence != nullptr);
    CHECK(evidence->detail.find("prints the median") != std::string::npos);

    // The criterion it did not name is still open, so the run is not finished.
    CHECK_FALSE(ledger.acceptance_plan()[0].settled);
    CHECK(assess_completion(ledger, true).blocked);
}

// 11 -------------------------------------------------------------------------
TEST_CASE("a cancelled build is not a failed build and not a passing one",
          "[adversarial]") {
    // Cancellation is the one case where a non-zero exit means nothing at all.
    // Recording it either way would be an invention.
    VerificationLedger ledger;
    auto result = ran("cmake --build build", 1);
    result.structured["cancelled"] = true;
    interpret_tool_result(run_call(), result, ledger);
    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
}

// 12 -------------------------------------------------------------------------
TEST_CASE("asking a question is not gated into needing a build", "[adversarial]") {
    // The gate has to be wrong in this direction too. A tool that demands a
    // build before it will answer "what does this function do?" gets turned
    // off, and then it protects nothing.
    VerificationLedger ledger;
    ledger.add_evidence(EvidenceKind::FileRead, "src/main.cpp", "40 lines", true);
    CHECK_FALSE(assess_completion(ledger, /*work_was_done=*/false).blocked);
}

// ---------------------------------------------------------------------------
// The original failure, permanently.
//
// Run 9: a program printed 1 3 6 12 24 48 96 192 384 768, exited 0, and the
// model reported that the first ten Fibonacci numbers had been printed
// correctly. This is that run, end to end, asserted to be impossible.
// ---------------------------------------------------------------------------

TEST_CASE("Run 9 cannot happen again", "[adversarial][run9]") {
    const std::string request =
        "Write a program that prints the first ten Fibonacci numbers starting from 0, "
        "one per line.";

    VerificationLedger ledger;
    write_file(ledger, "src/fib.cpp");
    interpret_tool_result(run_call(), ran("cmake -B build", 0), ledger, request);
    interpret_tool_result(run_call(), ran("cmake --build build", 0), ledger, request);

    // The criteria the model set, correctly this time.
    {
        ToolCall call;
        call.id = "criteria";
        call.name = "set_acceptance_criteria";
        ToolResult result;
        result.ok = true;
        result.structured = {
            {"acceptance_criteria",
             Json::array({"the project builds", "prints 0 1 1 2 3 5 8 13 21 34"})}};
        interpret_tool_result(call, result, ledger, request);
    }

    // It runs, and what comes out is wrong.
    auto wrong = ran("build\fib.exe", 0, "1 3 6 12 24 48 96 192 384 768");
    wrong.structured["expectation_checked"] = true;
    wrong.structured["expectation_satisfied"] = false;
    wrong.structured["expectation_description"] = "prints the first ten Fibonacci numbers";
    wrong.structured["expectation_checks"] = Json::array({"stdout is 0 1 1 2 3 5 8 13 21 34"});
    wrong.structured["expectation_criterion"] = 2;
    wrong.structured["expectation_failures"] =
        Json::array({"expected 0 1 1 2 3 5 8 13 21 34, actual 1 3 6 12 24 48 96 192 384 768"});
    interpret_tool_result(run_call(), wrong, ledger, request);
    ledger.reassess_objective();

    // Everything that did happen is recorded as having happened.
    CHECK(ledger.holds(Stage::Configure));
    CHECK(ledger.holds(Stage::Build));
    CHECK(ledger.holds(Stage::Execution));

    // And the one thing that did not.
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Fail);
    CHECK(ledger.status(Stage::Objective) == StageStatus::Fail);

    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    CHECK(blocked_for(assessment, "behaviour failed"));
    CHECK(assessment.guidance.find("Do not restate") != std::string::npos);
}

TEST_CASE("the same run with the right output does pass", "[adversarial][run9]") {
    // The other half of the regression: the gate has to let a correct run
    // through, or it is just a machine for saying no.
    const std::string request =
        "Write a program that prints the first ten Fibonacci numbers starting from 0, "
        "one per line.";

    VerificationLedger ledger;
    write_file(ledger, "src/fib.cpp");
    interpret_tool_result(run_call(), ran("cmake -B build", 0), ledger, request);
    interpret_tool_result(run_call(), ran("cmake --build build", 0), ledger, request);

    {
        ToolCall call;
        call.id = "criteria";
        call.name = "set_acceptance_criteria";
        ToolResult result;
        result.ok = true;
        result.structured = {{"acceptance_criteria",
                              Json::array({"prints 0 1 1 2 3 5 8 13 21 34"})}};
        interpret_tool_result(call, result, ledger, request);
    }
    // The request determines this answer, so the criterion is not the model's
    // opinion any more.
    REQUIRE(ledger.acceptance_plan()[0].trust() == CriterionTrust::Trusted);

    auto right = ran("build\fib.exe", 0, "0 1 1 2 3 5 8 13 21 34");
    right.structured["expectation_checked"] = true;
    right.structured["expectation_satisfied"] = true;
    right.structured["expectation_description"] = "prints the first ten Fibonacci numbers";
    right.structured["expectation_checks"] = Json::array({"stdout is 0 1 1 2 3 5 8 13 21 34"});
    right.structured["expectation_criterion"] = 1;
    interpret_tool_result(run_call(), right, ledger, request);
    ledger.reassess_objective();

    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Pass);
    CHECK(ledger.holds(Stage::Objective));
    CHECK_FALSE(assess_completion(ledger, true).blocked);
}

TEST_CASE("a model that defines its own answer is caught", "[adversarial][run9]") {
    // The residual risk, closed. The model writes the criterion using the other
    // Fibonacci convention, then verifies against it perfectly. The comparison
    // is real; the expectation is not what was asked for.
    const std::string request =
        "Write a program that prints the first ten Fibonacci numbers starting from 0.";

    VerificationLedger ledger;
    write_file(ledger, "src/fib.cpp");
    interpret_tool_result(run_call(), ran("cmake --build build", 0), ledger, request);

    ToolCall call;
    call.id = "criteria";
    call.name = "set_acceptance_criteria";
    ToolResult result;
    result.ok = true;
    result.structured = {{"acceptance_criteria",
                          Json::array({"prints 1, 1, 2, 3, 5, 8, 13, 21, 34, 55"})}};
    interpret_tool_result(call, result, ledger, request);

    REQUIRE(ledger.acceptance_plan().size() == 1);
    CHECK(ledger.acceptance_plan()[0].trust() == CriterionTrust::Conflicted);
    INFO(ledger.acceptance_plan()[0].conflict);
    CHECK(ledger.acceptance_plan()[0].conflict.find("0 1 1 2 3 5 8 13 21 34") !=
          std::string::npos);

    // Settling it perfectly changes nothing: the objective still fails.
    auto matched = ran("build\fib.exe", 0, "1 1 2 3 5 8 13 21 34 55");
    matched.structured["expectation_checked"] = true;
    matched.structured["expectation_satisfied"] = true;
    matched.structured["expectation_description"] = "prints the sequence";
    matched.structured["expectation_criterion"] = 1;
    interpret_tool_result(run_call(), matched, ledger, request);
    ledger.reassess_objective();

    CHECK(ledger.status(Stage::Objective) == StageStatus::Fail);
    CHECK(assess_completion(ledger, true).blocked);
}
