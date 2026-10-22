// ---------------------------------------------------------------------------
// Whether a run may be called finished.
//
// The model's sentence and the ledger's record disagree sometimes. These tests
// pin down which one wins, and - just as importantly - when the gate must stay
// out of the way, because a gate that blocks "what does this function do?" on
// a missing build is no better than one that lets a wrong answer through.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "agent/Verification.h"

using namespace localcode;

namespace {

bool mentions(const CompletionAssessment& assessment, std::string_view fragment) {
    for (const auto& reason : assessment.reasons) {
        if (reason.find(fragment) != std::string::npos) return true;
    }
    return false;
}

VerificationLedger built_and_run() {
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true), "");
    ledger.record(Stage::Execution, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::CommandRun, "fib.exe", "exit 0", true), "");
    return ledger;
}

}  // namespace

TEST_CASE("a run that changed nothing is not gated", "[gate]") {
    // Answering a question is not a build task. Demanding evidence here would
    // make the tool useless for the thing it is asked to do most often.
    VerificationLedger ledger;
    const auto assessment = assess_completion(ledger, /*work_was_done=*/false);
    CHECK_FALSE(assessment.blocked);
    CHECK(assessment.reasons.empty());
}

TEST_CASE("Run 9 would have been blocked", "[gate][fibonacci]") {
    // Built, ran, exit 0, and reported as printing the first ten Fibonacci
    // numbers. Nothing ever compared the output with the answer.
    auto ledger = built_and_run();
    const auto assessment = assess_completion(ledger, true);

    REQUIRE(assessment.blocked);
    CHECK(mentions(assessment, "output was never checked"));
    CHECK(mentions(assessment, "no acceptance criteria"));
    CHECK(assessment.guidance.find("set_acceptance_criteria") != std::string::npos);
}

TEST_CASE("a satisfied plan lets the run finish", "[gate]") {
    auto ledger = built_and_run();
    ledger.set_acceptance_plan({{"the project builds"}, {"it prints the right numbers"}});
    ledger.settle_check(0, true, "E001");
    ledger.record(Stage::Behaviour, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::TestsRun, "behaviour check",
                                      "output matched", true),
                  "prints the right numbers");
    ledger.settle_check(1, true, "E003");
    ledger.reassess_objective();

    const auto assessment = assess_completion(ledger, true);
    INFO(assessment.guidance);
    CHECK_FALSE(assessment.blocked);
}

TEST_CASE("a rejected plan blocks completion even when all its checks pass",
          "[gate][adversarial][regression]") {
    auto ledger = built_and_run();
    ledger.set_acceptance_plan({{"output contains a count field"}});
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E003", "count field exists");
    ledger.settle_check(0, true, "E003");
    ledger.record_plan_gap("the request requires count 4, not just a count field");
    ledger.reassess_objective();
    REQUIRE(ledger.status(Stage::Objective) == StageStatus::Fail);
    const auto assessment = assess_completion(ledger, true);
    CHECK(assessment.blocked);
    CHECK(mentions(assessment, "count 4"));
    CHECK(assessment.guidance.find("set_acceptance_criteria") != std::string::npos);
}

TEST_CASE("verification persistence retains disputed criteria and plan gaps",
          "[gate][persistence][regression]") {
    auto ledger = built_and_run();
    ledger.set_acceptance_plan({{"it prints four numbers"}});
    ledger.ground_check(0, CriterionSource::UserExplicit, "stated in request", "user request");
    ledger.dispute_check(0, "the request actually requires five numbers");
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E003", "four numbers printed");
    ledger.settle_check(0, true, "E003");
    ledger.record_plan_gap("five numbers required");
    auto restored = VerificationLedger::from_json(ledger.to_json());
    REQUIRE(restored.acceptance_plan().size() == 1);
    CHECK(restored.acceptance_plan()[0].source == CriterionSource::UserExplicit);
    CHECK(restored.acceptance_plan()[0].reason == "stated in request");
    CHECK(restored.acceptance_plan()[0].derived_from == "user request");
    CHECK(restored.acceptance_plan()[0].trust() == CriterionTrust::Conflicted);
    CHECK(restored.plan_gap() == "five numbers required");
    restored.reassess_objective();
    CHECK(restored.status(Stage::Objective) == StageStatus::Fail);
    CHECK(assess_completion(restored, true).blocked);
}

TEST_CASE("objective verification headings are distinct from individual checks and caveats",
          "[gate][claims][regression]") {
    CHECK(claims_verified_objective("VERIFIED\n\nI edited the file."));
    CHECK(claims_verified_objective("## **VERIFIED**"));
    CHECK(claims_verified_objective("Verification status: VERIFIED."));
    CHECK_FALSE(claims_verified_objective("NOT VERIFIED"));
    CHECK_FALSE(claims_verified_objective("PARTIALLY VERIFIED"));
    CHECK_FALSE(claims_verified_objective("Criterion 1: VERIFIED"));
    CHECK_FALSE(claims_verified_objective("The task was not verified."));
}

TEST_CASE("an unsettled criterion blocks, and is named", "[gate]") {
    auto ledger = built_and_run();
    ledger.set_acceptance_plan({{"the project builds"}, {"it prints the right numbers"}});
    ledger.settle_check(0, true, "E001");

    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    CHECK(mentions(assessment, "it prints the right numbers"));
    CHECK(assessment.guidance.find("2. it prints the right numbers") != std::string::npos);
}

TEST_CASE("a criterion that failed blocks as a failure, not as an omission", "[gate]") {
    auto ledger = built_and_run();
    ledger.set_acceptance_plan({{"it prints the first ten Fibonacci numbers"}});
    ledger.record(Stage::Behaviour, StageStatus::Fail,
                  ledger.add_evidence(EvidenceKind::TestsRun, "behaviour check",
                                      "expected 0 1 1 2 3, got 1 3 6 12 24", false),
                  "output does not match");
    ledger.settle_check(0, false, "E003");

    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    CHECK(mentions(assessment, "acceptance criterion not met"));
    CHECK(mentions(assessment, "behaviour failed"));
}

TEST_CASE("editing after a successful build blocks the run", "[gate][staleness]") {
    auto ledger = built_and_run();
    ledger.set_acceptance_plan({{"the project builds"}});
    ledger.settle_check(0, true, "E001");
    ledger.record(Stage::Behaviour, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::TestsRun, "check", "ok", true), "");

    // The last-minute tidy-up that nobody rebuilt.
    ledger.note_source_changed("src/main.cpp");

    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    CHECK(mentions(assessment, "files have changed since"));
    CHECK(mentions(assessment, "r0"));
}

TEST_CASE("changing files and building nothing blocks the run", "[gate]") {
    VerificationLedger ledger;
    ledger.note_source_changed("src/main.cpp");

    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    CHECK(mentions(assessment, "no classified build, run or test evidence"));
}

TEST_CASE("an unclassified command remains unverified without denying the attempt",
          "[gate][opaque-powershell]") {
    VerificationLedger ledger;
    ledger.note_source_changed("src/main.cpp");
    ledger.add_evidence(EvidenceKind::CommandRun, "custom-wrapper", "exit 0", true);

    const auto assessment = assess_completion(ledger, true);
    CHECK(assessment.blocked);
    CHECK(mentions(assessment, "no classified build, run or test evidence"));
    CHECK_FALSE(mentions(assessment, "nothing was built, run or tested"));
    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Test) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Execution) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);
}

TEST_CASE("a passing test suite counts instead of a behaviour check", "[gate]") {
    // A project whose own tests cover the change does not also need the program
    // run by hand. Requiring both would be ceremony, not rigour.
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true), "");
    ledger.record(Stage::Test, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::TestsRun, "ctest", "48 passed", true), "");
    ledger.record(Stage::Execution, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::CommandRun, "app.exe", "exit 0", true), "");
    ledger.set_acceptance_plan({{"the test suite passes"}});
    ledger.settle_check(0, true, "E002");
    ledger.reassess_objective();

    const auto assessment = assess_completion(ledger, true);
    INFO(assessment.guidance);
    CHECK_FALSE(assessment.blocked);
}

TEST_CASE("a failing build blocks even when the plan is satisfied", "[gate]") {
    // The plan being settled is not a licence to ignore a broken build; both
    // are checked, so neither can be used to talk over the other.
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Fail,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 1", false),
                  "C2065: undeclared identifier");
    ledger.set_acceptance_plan({{"it builds"}});
    ledger.settle_check(0, true, "E001");

    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    CHECK(mentions(assessment, "C2065"));
}

TEST_CASE("the guidance tells the model what to do, not just what is wrong", "[gate]") {
    auto ledger = built_and_run();
    ledger.set_acceptance_plan({{"it prints the first ten Fibonacci numbers"}});

    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    INFO(assessment.guidance);
    CHECK(assessment.guidance.find("run_command") != std::string::npos);
    CHECK(assessment.guidance.find("expect") != std::string::npos);
    CHECK(assessment.guidance.find("criterion") != std::string::npos);
    // And it must not invite the model to simply say it again.
    CHECK(assessment.guidance.find("Do not restate") != std::string::npos);
}
