// ---------------------------------------------------------------------------
// What a finished run gets called.
//
// This is the decision the GUI used to make implicitly, by having only one word
// to say it with. It lives in the engine now, so it can be tested without a
// window and so the headless tool and the desktop application cannot describe
// the same result differently.
//
// The case that matters is the second one: an objective that holds, with no
// summary to show for it. That is not a failure, and every assertion about it
// here exists because reporting it as one discards work that was done, checked
// and recorded.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "agent/Outcome.h"

using namespace localcode;

TEST_CASE("an honest unverified coding answer retains the unverified outcome", "[outcome][audit]") {
    AgentRunSummary summary;
    summary.completed = true;
    summary.final_message = "The file was changed; I have not run the tests.";
    summary.ledger.note_source_changed("src/main.cpp");
    SECTION("with an unsettled plan") {
        summary.ledger.set_acceptance_plan({{"the corrected program passes its tests"}});
    }
    SECTION("with no plan") {}
    const auto report = describe_outcome(summary);
    CHECK(report.completed);
    CHECK_FALSE(report.objective_verified);
    CHECK(report.kind == OutcomeKind::NotVerified);
    CHECK(report.headline.find("Nothing was claimed") == std::string::npos);
}

namespace {

// A summary with an objective that holds, built the way the ledger builds one:
// a plan whose criteria are settled and satisfied, then recomputed. Nothing here
// sets Objective directly, because nothing in the engine is allowed to.
AgentRunSummary verified_summary() {
    AgentRunSummary summary;
    AcceptanceCheck check;
    check.description = "the program prints the first ten Fibonacci numbers";
    check.source = CriterionSource::UserExplicit;
    check.reason = "stated in the request";
    check.settled = true;
    check.satisfied = true;
    check.evidence_id = "run:1";
    summary.ledger.set_acceptance_plan({check});
    summary.ledger.record(Stage::Source, StageStatus::Pass, "edit:1", "written");
    summary.ledger.record(Stage::Build, StageStatus::Pass, "build:1", "built");
    summary.ledger.record(Stage::Execution, StageStatus::Pass, "run:1", "ran");
    summary.ledger.record(Stage::Behaviour, StageStatus::Pass, "run:1", "output matched");
    summary.ledger.reassess_objective();
    return summary;
}

}  // namespace

TEST_CASE("a verified run that produced its summary is simply verified", "[outcome]") {
    auto summary = verified_summary();
    summary.completed = true;
    summary.presentation = PresentationState::Emitted;
    summary.final_message = "Wrote, built and ran it; the ten numbers printed.";

    const auto report = describe_outcome(summary);
    REQUIRE(report.kind == OutcomeKind::Verified);
    CHECK(report.objective_verified);
    CHECK_FALSE(report.finalization_gated_off);
    CHECK(report.headline == "The work is verified.");
    CHECK(to_string(report.kind) == "verified");
}

TEST_CASE("verified work with no summary is not reported as failure", "[outcome]") {
    auto summary = verified_summary();
    summary.completed = false;
    summary.presentation = PresentationState::Failed;
    summary.finalization_attempts = kMaxFinalizationAttempts;
    summary.stop_reason = "the work was verified, but the model produced no final answer";

    const auto report = describe_outcome(summary);
    REQUIRE(report.kind == OutcomeKind::VerifiedWithoutSummary);
    CHECK(report.objective_verified);
    // It was tried, and it failed. That is a different thing from never trying.
    CHECK_FALSE(report.finalization_gated_off);
    CHECK(report.finalization_attempts == kMaxFinalizationAttempts);
    CHECK(report.headline == "The work is verified. The summary of it is not.");
    // The word "failed" must not be what the user is handed about their work.
    CHECK(report.headline.find("failed") == std::string::npos);
}

TEST_CASE("a reply that was only reasoning still leaves the objective standing", "[outcome]") {
    auto summary = verified_summary();
    summary.presentation = PresentationState::ReasoningOnly;
    summary.finalization_attempts = 1;

    const auto report = describe_outcome(summary);
    CHECK(report.kind == OutcomeKind::VerifiedWithoutSummary);
    CHECK(report.objective_verified);
    CHECK(is_presentation_failure(report.presentation));
}

TEST_CASE("an unestablished objective is reported as not verified", "[outcome]") {
    AgentRunSummary summary;
    summary.completed = false;
    summary.presentation = PresentationState::None;
    summary.stop_reason = "the model returned an empty answer";

    const auto report = describe_outcome(summary);
    REQUIRE(report.kind == OutcomeKind::NotVerified);
    CHECK_FALSE(report.objective_verified);
    CHECK(report.headline == "the model returned an empty answer");
}

TEST_CASE("finalization that never ran is told apart from finalization that failed",
          "[outcome]") {
    // Two criteria, one never settled: the plan is incomplete, so the finalizer
    // is gated off rather than run and failed.
    AgentRunSummary summary;
    AcceptanceCheck settled;
    settled.description = "it builds";
    settled.settled = true;
    settled.satisfied = true;
    AcceptanceCheck open;
    open.description = "each number on its own line";
    summary.ledger.set_acceptance_plan({settled, open});
    summary.ledger.record(Stage::Build, StageStatus::Pass, "build:1", "built");
    summary.ledger.reassess_objective();

    summary.presentation = PresentationState::Failed;
    summary.finalization_attempts = 0;

    const auto report = describe_outcome(summary);
    CHECK(report.finalization_gated_off);
    CHECK(report.finalization_attempts == 0);
    CHECK_FALSE(report.plan_complete);
    REQUIRE(report.unsettled.size() == 1);
    CHECK(report.unsettled.front() == "each number on its own line");
}

TEST_CASE("a disputed criterion is carried into the report", "[outcome]") {
    auto summary = verified_summary();
    auto plan = summary.ledger.acceptance_plan();
    plan.front().conflict = "expected 3 10 1 1 2 3, the request determines 0 1 1 2 3";
    summary.ledger.set_acceptance_plan(plan);

    const auto report = describe_outcome(summary);
    REQUIRE(report.disputed.size() == 1);
    CHECK(report.disputed.front().find("the request determines") != std::string::npos);
}

TEST_CASE("a gated-off run says so instead of blaming the finalizer", "[outcome]") {
    auto summary = verified_summary();
    summary.presentation = PresentationState::Failed;
    summary.finalization_attempts = 0;

    const auto report = describe_outcome(summary);
    CHECK(report.kind == OutcomeKind::VerifiedWithoutSummary);
    CHECK(report.finalization_gated_off);
    CHECK(report.headline == "The work is verified, and no summary was attempted.");
}

TEST_CASE("cancellation outranks everything else", "[outcome]") {
    auto summary = verified_summary();
    summary.cancelled = true;
    summary.presentation = PresentationState::Failed;

    const auto report = describe_outcome(summary);
    REQUIRE(report.kind == OutcomeKind::Cancelled);
    CHECK(report.headline.find("preserved") != std::string::npos);
    // Cancelled or not, what was established stays established.
    CHECK(report.objective_verified);
}

TEST_CASE("an answered question is not a failed task", "[outcome]") {
    // Found in live validation: asking what a CMakeLists.txt file is for
    // rendered the answer and then an ERROR reading "the model produced a final
    // answer". A request that sets no acceptance criteria has nothing to
    // verify, and reporting that as a failure is wrong twice over.
    AgentRunSummary summary;
    summary.completed = true;
    summary.final_message = "A CMakeLists.txt file tells CMake how to build a project.";
    summary.stop_reason = "the model produced a final answer";
    summary.presentation = PresentationState::None;

    const auto report = describe_outcome(summary);
    REQUIRE(report.kind == OutcomeKind::Answered);
    CHECK_FALSE(report.had_plan);
    CHECK_FALSE(report.objective_verified);
    CHECK(report.completed);
    CHECK(report.kind != OutcomeKind::NotVerified);
    // It answered, and it says so without borrowing a word it did not earn.
    CHECK(report.headline.find("Answered") != std::string::npos);
    CHECK(report.headline.find("Nothing was claimed") != std::string::npos);
}

TEST_CASE("a task that set criteria still reports them", "[outcome]") {
    auto summary = verified_summary();
    summary.completed = true;
    summary.presentation = PresentationState::Emitted;

    const auto report = describe_outcome(summary);
    CHECK(report.kind == OutcomeKind::Verified);
    CHECK(report.had_plan);
}

TEST_CASE("every outcome kind has a distinct word", "[outcome]") {
    CHECK(to_string(OutcomeKind::Verified) == "verified");
    CHECK(to_string(OutcomeKind::VerifiedWithoutSummary) == "verified, no summary");
    CHECK(to_string(OutcomeKind::Answered) == "answered");
    CHECK(to_string(OutcomeKind::NotVerified) == "not verified");
    CHECK(to_string(OutcomeKind::Cancelled) == "cancelled");
}
