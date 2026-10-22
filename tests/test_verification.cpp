// ---------------------------------------------------------------------------
// The difference between doing something and proving it was done right.
//
// A run built a program, ran it to exit code 0, and reported that the first ten
// Fibonacci numbers were "printed correctly". It had printed
// 1 3 6 12 24 48 96 192 384 768. Every piece of evidence was true; the
// conclusion was false, because none of the evidence was about the answer.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "agent/Verification.h"

using namespace localcode;

TEST_CASE("a later failed stage revokes an already settled objective", "[verification][audit]") {
    for (const auto stage :
         {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution, Stage::Behaviour}) {
        INFO(to_string(stage));
        VerificationLedger ledger;
        ledger.set_acceptance_plan({{"the requested result is present"}});
        ledger.settle_check(0, true, "first-check");
        REQUIRE(ledger.holds(Stage::Objective));

        ledger.record(stage, StageStatus::Fail, "later-check", "fresh failure");
        ledger.reassess_objective();
        CHECK_FALSE(ledger.holds(Stage::Objective));
        CHECK(ledger.status(Stage::Objective) == StageStatus::Fail);
        CHECK(assess_completion(ledger, true).blocked);

        ledger.record(stage, StageStatus::Pass, "retry", "fresh successful retry");
        ledger.reassess_objective();
        CHECK(ledger.holds(Stage::Objective));
    }
}

TEST_CASE("stale tests cannot be hidden by newly settled file criteria", "[verification][audit]") {
    VerificationLedger ledger;
    ledger.record(Stage::Test, StageStatus::Pass, "old-tests", "passed before editing");
    ledger.note_source_changed("src/main.cpp");
    ledger.set_acceptance_plan({{"the requested literal is present"}});
    ledger.settle_check(0, true, "new-file-check");
    ledger.reassess_objective();
    CHECK(ledger.status(Stage::Test) == StageStatus::Stale);
    CHECK_FALSE(ledger.holds(Stage::Objective));
    CHECK(ledger.status(Stage::Objective) == StageStatus::Stale);
}

TEST_CASE("marking one failed stage inapplicable cannot hide another failure",
          "[verification][audit]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"the requested result is present"}});
    ledger.settle_check(0, true, "file-check");
    ledger.record(Stage::Build, StageStatus::Fail, "build-failure", "compiler failed");
    ledger.record(Stage::Test, StageStatus::Fail, "test-failure", "runner failed");
    CHECK_FALSE(ledger.holds(Stage::Objective));
    ledger.mark_not_applicable(Stage::Test, "the runner found no tests");
    CHECK(ledger.status(Stage::Test) == StageStatus::NotApplicable);
    CHECK_FALSE(ledger.holds(Stage::Objective));
    ledger.record(Stage::Build, StageStatus::Pass, "rebuilt", "compiler succeeded");
    CHECK(ledger.holds(Stage::Objective));
}

namespace {

// A criterion the user asked for, as opposed to one the model thought up. The
// difference decides whether settling it can carry the objective.
AcceptanceCheck user_criterion(std::string description) {
    AcceptanceCheck check;
    check.description = std::move(description);
    check.source = CriterionSource::UserExplicit;
    check.reason = "stated in the request";
    return check;
}

}  // namespace

TEST_CASE("completion feedback does not grant permission to run checks", "[verification][feedback]") {
    VerificationLedger ledger;
    ledger.note_source_changed("calculator.py");
    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    CHECK(assessment.guidance.find("original user's scope and restrictions") != std::string::npos);
    CHECK(assessment.guidance.find("finish with an explicit verification limitation") != std::string::npos);
    CHECK(assessment.guidance.find("by running something") == std::string::npos);
}

TEST_CASE("a stage starts unestablished", "[verification]") {
    VerificationLedger ledger;
    for (const auto stage : {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution,
                             Stage::Behaviour, Stage::Objective}) {
        INFO("stage " << to_string(stage));
        CHECK(ledger.status(stage) == StageStatus::Unknown);
        CHECK_FALSE(ledger.holds(stage));
    }
    CHECK(ledger.revision() == 0);
}

TEST_CASE("editing a source file makes a passing build stale", "[verification][staleness]") {
    VerificationLedger ledger;

    const auto configured = ledger.add_evidence(EvidenceKind::CommandRun, "cmake -S . -B build",
                                                "exit 0", true);
    ledger.record(Stage::Configure, StageStatus::Pass, configured, "");
    const auto built = ledger.add_evidence(EvidenceKind::BuildAttempted, "cmake --build build",
                                           "exit 0", true);
    ledger.record(Stage::Build, StageStatus::Pass, built, "");
    CHECK(ledger.holds(Stage::Build));

    // The edit that invalidates it.
    ledger.note_source_changed("src/main.cpp");

    CHECK(ledger.revision() == 1);
    CHECK(ledger.status(Stage::Build) == StageStatus::Stale);
    CHECK_FALSE(ledger.holds(Stage::Build));
    CHECK(ledger.is_stale(Stage::Build));

    // The history is not erased: the build did pass, at revision 0.
    CHECK(ledger.raw(Stage::Build).status == StageStatus::Pass);
    CHECK(ledger.raw(Stage::Build).established_at_revision == 0);

    // Rebuilding against the new revision restores it.
    const auto rebuilt = ledger.add_evidence(EvidenceKind::BuildAttempted,
                                             "cmake --build build", "exit 0", true);
    ledger.record(Stage::Build, StageStatus::Pass, rebuilt, "");
    CHECK(ledger.holds(Stage::Build));
}

TEST_CASE("every downstream stage goes stale together", "[verification][staleness]") {
    VerificationLedger ledger;
    for (const auto stage : {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution,
                             Stage::Behaviour}) {
        ledger.record(stage, StageStatus::Pass, ledger.add_evidence(
            EvidenceKind::CommandRun, std::string{to_string(stage)}, "exit 0", true), "");
    }
    // Stated by the user: this test is about staleness, not about how much a
    // criterion is worth.
    ledger.set_acceptance_plan({user_criterion("the program prints the right numbers")});
    ledger.settle_check(0, true, "E001");
    CHECK(ledger.holds(Stage::Objective));

    ledger.note_source_changed("src/main.cpp");

    for (const auto stage : {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution,
                             Stage::Behaviour}) {
        INFO("stage " << to_string(stage));
        CHECK(ledger.status(stage) == StageStatus::Stale);
    }
    // And the objective with them - a passing build from revision 0 must never
    // verify revision 1.
    ledger.reassess_objective();
    CHECK_FALSE(ledger.holds(Stage::Objective));
}

TEST_CASE("a failure does not silently become stale", "[verification][staleness]") {
    // A stale pass is "we no longer know". A failure stays a failure until
    // something establishes otherwise.
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Fail,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 1", false),
                  "C2065");
    ledger.note_source_changed("src/main.cpp");
    CHECK(ledger.status(Stage::Build) == StageStatus::Fail);
}

TEST_CASE("building is not running and running is not behaving", "[verification][inference]") {
    // The Fibonacci case, in the abstract: every lower stage passes and the
    // higher ones stay unknown until something establishes them.
    VerificationLedger ledger;
    ledger.record(Stage::Configure, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::CommandRun, "cmake", "exit 0", true), "");
    ledger.record(Stage::Build, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true), "");
    ledger.record(Stage::Execution, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::CommandRun, "fib.exe", "exit 0", true), "");

    CHECK(ledger.holds(Stage::Build));
    CHECK(ledger.holds(Stage::Execution));

    // Nothing checked the answer.
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Test) == StageStatus::Unknown);
}

TEST_CASE("the Fibonacci case ends as a behaviour failure", "[verification][behaviour]") {
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true), "");
    ledger.record(Stage::Execution, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::CommandRun, "fib.exe",
                                      "1 3 6 12 24 48 96 192 384 768", true),
                  "");

    ledger.set_acceptance_plan({{"stdout is the first ten Fibonacci numbers"}});

    // The comparison that Run 9 never made.
    const auto checked = ledger.add_evidence(
        EvidenceKind::TestsRun, "compare stdout with the first ten Fibonacci numbers",
        "expected 0 1 1 2 3 5 8 13 21 34, actual 1 3 6 12 24 48 96 192 384 768", false);
    ledger.record(Stage::Behaviour, StageStatus::Fail, checked, "output does not match");
    ledger.settle_check(0, false, checked);

    CHECK(ledger.holds(Stage::Build));
    CHECK(ledger.holds(Stage::Execution));
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Fail);
    CHECK(ledger.status(Stage::Objective) == StageStatus::Fail);
    CHECK(ledger.plan_has_failure());
}

TEST_CASE("the objective is derived, never asserted", "[verification][objective]") {
    VerificationLedger ledger;

    // No plan means nothing to satisfy, so the objective cannot be claimed.
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);

    // Stated by the user, so this test is about derivation rather than trust.
    ledger.set_acceptance_plan({user_criterion("it builds"),
                               user_criterion("it prints the right numbers")});
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);

    ledger.settle_check(0, true, "E001");
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);   // one still open

    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E002", "");
    ledger.settle_check(1, true, "E002");
    CHECK(ledger.holds(Stage::Objective));
}

TEST_CASE("a compile-only task can mark behaviour not applicable", "[verification][objective]") {
    // "Make this compile" is satisfied by compiling. Saying behaviour is
    // "unknown" there would be pedantry, not honesty - but it has to be stated
    // deliberately rather than assumed.
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true), "");
    ledger.mark_not_applicable(Stage::Behaviour, "the request was only to make it compile");
    ledger.set_acceptance_plan({user_criterion("the project compiles")});
    ledger.settle_check(0, true, "E001");

    CHECK(ledger.status(Stage::Behaviour) == StageStatus::NotApplicable);
    CHECK(ledger.holds(Stage::Objective));
}

TEST_CASE("evidence is addressable by id", "[verification][evidence]") {
    VerificationLedger ledger;
    const auto first = ledger.add_evidence(EvidenceKind::FileRead, "main.cpp", "19 lines", true);
    const auto second = ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true);

    CHECK(first == "E001");
    CHECK(second == "E002");

    const auto* found = ledger.find_evidence("E002");
    REQUIRE(found != nullptr);
    CHECK(found->subject == "build");
    CHECK(found->positive);
    CHECK(ledger.find_evidence("E999") == nullptr);

    // Evidence remembers which revision it describes.
    ledger.note_source_changed("main.cpp");
    const auto third = ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true);
    CHECK(ledger.find_evidence(third)->revision == 1);
    CHECK(ledger.find_evidence(first)->revision == 0);
}

TEST_CASE("the ledger survives a round trip", "[verification][persistence]") {
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true), "");
    ledger.set_acceptance_plan({{"it builds"}, {"it runs"}});
    ledger.settle_check(0, true, "E001");
    ledger.note_source_changed("src/main.cpp");

    const auto restored = VerificationLedger::from_json(ledger.to_json());

    CHECK(restored.revision() == ledger.revision());
    CHECK(restored.status(Stage::Build) == StageStatus::Stale);
    CHECK(restored.acceptance_plan().size() == 2);
    CHECK(restored.acceptance_plan()[0].satisfied);
    REQUIRE(restored.find_evidence("E001") != nullptr);
    CHECK(restored.find_evidence("E001")->subject == "build");
}

TEST_CASE("the rendered block states staleness in words", "[verification][rendering]") {
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass,
                  ledger.add_evidence(EvidenceKind::BuildAttempted, "build", "exit 0", true), "");
    ledger.note_source_changed("src/main.cpp");

    const auto text = ledger.render_states();
    INFO(text);
    CHECK(text.find("stale") != std::string::npos);
    CHECK(text.find("r0") != std::string::npos);   // the revision it passed at
    CHECK(text.find("source revision: 1") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Criterion provenance
//
// Writing a criterion down does not make it true. Asked for the first ten
// Fibonacci numbers, a model wrote its own criterion as
// "1, 1, 2, 3, 5, 8, 13, 21, 34, 55" - a defensible convention, and not the one
// the user asked for. It would then have verified against its own definition of
// the answer, passed every check honestly, and been wrong.
// ---------------------------------------------------------------------------

TEST_CASE("where a criterion came from decides what it is worth",
          "[verification][provenance]") {
    AcceptanceCheck stated;
    stated.source = CriterionSource::UserExplicit;
    CHECK(stated.trust() == CriterionTrust::Trusted);

    AcceptanceCheck computed;
    computed.source = CriterionSource::UserDerived;
    CHECK(computed.trust() == CriterionTrust::Trusted);

    AcceptanceCheck from_reference;
    from_reference.source = CriterionSource::ReferenceDerived;
    CHECK(from_reference.trust() == CriterionTrust::Derived);

    AcceptanceCheck guessed;   // the default, deliberately
    CHECK(guessed.source == CriterionSource::ModelInferred);
    CHECK(guessed.trust() == CriterionTrust::ModelProposed);
}

TEST_CASE("a disagreement outranks where the criterion came from",
          "[verification][provenance]") {
    AcceptanceCheck stated;
    stated.source = CriterionSource::UserExplicit;
    stated.conflict = "the request says the sequence starts at 0";
    CHECK(stated.trust() == CriterionTrust::Conflicted);
}

TEST_CASE("a conflicted criterion cannot carry the objective, however well settled",
          "[verification][provenance]") {
    // The check was real. The expectation was not.
    VerificationLedger ledger;
    AcceptanceCheck wrong;
    wrong.description = "prints 1 1 2 3 5 8 13 21 34 55";
    wrong.source = CriterionSource::ModelInferred;
    wrong.conflict =
        "the request asked for the sequence starting from 0; a reference gives "
        "0 1 1 2 3 5 8 13 21 34";
    ledger.set_acceptance_plan({wrong});

    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E002", "");
    ledger.settle_check(0, true, "E002");
    ledger.reassess_objective();

    CHECK(ledger.status(Stage::Objective) == StageStatus::Fail);
    CHECK(ledger.raw(Stage::Objective).detail.find("disputed") != std::string::npos);
}

TEST_CASE("a plan made only of the model's own reading passes, but says so",
          "[verification][provenance]") {
    // It cannot be refused outright: for most work the model's reading of the
    // task is the only definition available, and blocking that forever would
    // make the gate useless rather than strict. What it cannot do is pass
    // silently, because the run has shown itself consistent with its own idea
    // of the task and not that the idea was right.
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"it builds"}, {"it runs"}});   // both ModelInferred
    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E002", "");
    ledger.settle_check(0, true, "E001");
    ledger.settle_check(1, true, "E002");
    ledger.reassess_objective();

    CHECK(ledger.holds(Stage::Objective));
    INFO(ledger.raw(Stage::Objective).detail);
    CHECK(ledger.raw(Stage::Objective).detail.find("proposed by") != std::string::npos);

    // And the answer carries it as a caveat rather than a refusal.
    const auto assessment = assess_completion(ledger, true);
    CHECK_FALSE(assessment.blocked);
    REQUIRE(assessment.caveats.size() == 1);
    CHECK(assessment.caveats[0].find("proposed by the model") != std::string::npos);
}

TEST_CASE("one grounded criterion is enough to let a plan pass",
          "[verification][provenance]") {
    // A run may legitimately add "it builds" of its own accord alongside what
    // the user actually asked for. Requiring every criterion to be
    // user-stated would make the rule unusable.
    VerificationLedger ledger;
    ledger.set_acceptance_plan(
        {user_criterion("prints the first ten Fibonacci numbers from 0"), {"it builds"}});
    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E002", "");
    ledger.settle_check(0, true, "E002");
    ledger.settle_check(1, true, "E001");
    ledger.reassess_objective();

    CHECK(ledger.holds(Stage::Objective));
}

// ---------------------------------------------------------------------------
// Staleness, case by case.
//
// One rule with several faces: a stage is only as current as the source it was
// established against. Each of these is a way a run could otherwise carry an
// old green result forward past the change that invalidated it.
// ---------------------------------------------------------------------------

TEST_CASE("build passes, source changes, build is stale", "[verification][staleness]") {
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    REQUIRE(ledger.holds(Stage::Build));

    ledger.note_source_changed("src/main.cpp");
    CHECK(ledger.status(Stage::Build) == StageStatus::Stale);
}

TEST_CASE("tests pass, a test file changes, the tests are stale",
          "[verification][staleness]") {
    // Editing the tests invalidates the test result exactly as editing the code
    // does - and it is the case most worth getting right, because a run that
    // weakened a test would otherwise keep the green from before it did.
    VerificationLedger ledger;
    ledger.record(Stage::Test, StageStatus::Pass, "E001", "48 passed");
    REQUIRE(ledger.holds(Stage::Test));

    ledger.note_source_changed("tests/test_stats.cpp");
    CHECK(ledger.status(Stage::Test) == StageStatus::Stale);
}

TEST_CASE("the build system counts as source", "[verification][staleness]") {
    VerificationLedger ledger;
    ledger.record(Stage::Configure, StageStatus::Pass, "E001", "");
    ledger.record(Stage::Build, StageStatus::Pass, "E002", "");

    ledger.note_source_changed("CMakeLists.txt");
    CHECK(ledger.status(Stage::Configure) == StageStatus::Stale);
    CHECK(ledger.status(Stage::Build) == StageStatus::Stale);
}

TEST_CASE("execution passes, source changes, that execution is stale",
          "[verification][staleness]") {
    // The binary that ran is not the binary the source now describes, whether
    // or not anyone has rebuilt yet.
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    ledger.record(Stage::Execution, StageStatus::Pass, "E002", "");

    ledger.note_source_changed("src/main.cpp");
    CHECK(ledger.status(Stage::Execution) == StageStatus::Stale);

    // Rebuilding restores the build and leaves the execution stale: nothing has
    // run the new binary yet.
    ledger.record(Stage::Build, StageStatus::Pass, "E003", "");
    CHECK(ledger.holds(Stage::Build));
    CHECK(ledger.status(Stage::Execution) == StageStatus::Stale);
}

TEST_CASE("behaviour passes, source changes, behaviour is stale",
          "[verification][staleness]") {
    VerificationLedger ledger;
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E001", "output matched");
    REQUIRE(ledger.holds(Stage::Behaviour));

    ledger.note_source_changed("src/fib.cpp");
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Stale);
}

TEST_CASE("a verified objective does not stay green through a change",
          "[verification][staleness]") {
    // The one that matters most: a run that finished, then touched a file, must
    // not still be finished.
    VerificationLedger ledger;
    ledger.set_acceptance_plan({user_criterion("it prints the right numbers")});
    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    ledger.record(Stage::Behaviour, StageStatus::Pass, "E002", "");
    ledger.settle_check(0, true, "E002");
    REQUIRE(ledger.holds(Stage::Objective));

    ledger.note_source_changed("src/fib.cpp");
    ledger.reassess_objective();

    CHECK_FALSE(ledger.holds(Stage::Objective));
    CHECK(ledger.status(Stage::Objective) == StageStatus::Stale);

    // And the gate says so rather than letting the run finish on it.
    const auto assessment = assess_completion(ledger, true);
    REQUIRE(assessment.blocked);
    bool mentioned = false;
    for (const auto& reason : assessment.reasons) {
        if (reason.find("files have changed since") != std::string::npos) mentioned = true;
    }
    CHECK(mentioned);
}

TEST_CASE("a stale pass keeps the revision it passed at",
          "[verification][staleness]") {
    // Staleness is "we no longer know", not "it never happened". The history
    // has to stay inspectable or the report cannot explain itself.
    VerificationLedger ledger;
    ledger.record(Stage::Build, StageStatus::Pass, "E001", "");
    ledger.note_source_changed("a.cpp");
    ledger.note_source_changed("b.cpp");

    CHECK(ledger.revision() == 2);
    CHECK(ledger.status(Stage::Build) == StageStatus::Stale);
    CHECK(ledger.raw(Stage::Build).status == StageStatus::Pass);
    CHECK(ledger.raw(Stage::Build).established_at_revision == 0);
}
