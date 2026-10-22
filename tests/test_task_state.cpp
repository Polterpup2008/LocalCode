// Task state and the evidence rules that decide what may be claimed.
#include <catch2/catch_test_macros.hpp>

#include "agent/TaskState.h"
#include "util/Log.h"

using namespace localcode;

namespace {

Evidence evidence_of(EvidenceKind kind, bool positive, std::string subject = "x") {
    Evidence evidence;
    evidence.kind = kind;
    evidence.positive = positive;
    evidence.subject = std::move(subject);
    evidence.at_unix_ms = now_unix_ms();
    return evidence;
}

}   // namespace

TEST_CASE("a fresh task has nothing verified", "[task]") {
    TaskState task;
    task.begin("fix the rendering bug");

    CHECK(task.objective() == "fix the rendering bug");
    CHECK(task.verification() == VerificationState::NotAttempted);
    CHECK(task.verification_summary().find("NOT VERIFIED") != std::string::npos);
}

TEST_CASE("unclassified commands do not become a claim that nothing was attempted",
          "[task][evidence][opaque-powershell]") {
    TaskState task;
    task.begin("fix the decoder");
    task.record_evidence(evidence_of(EvidenceKind::FileWritten, true, "src/decoder.h"));
    task.record_evidence(
        evidence_of(EvidenceKind::CommandRun, true, "pwsh -NoProfile -File ./run-tests.ps1"));
    CommandRecord command;
    command.command = "pwsh -NoProfile -File ./run-tests.ps1";
    command.succeeded = true;
    task.record_command(command);

    CHECK(task.verification() == VerificationState::NotAttempted);
    CHECK_FALSE(task.has_successful_build());
    CHECK_FALSE(task.has_successful_tests());
    const auto summary = task.verification_summary();
    CHECK(summary.find("NOT VERIFIED") != std::string::npos);
    CHECK(summary.find("classified build or test") != std::string::npos);
    CHECK(summary.find("nothing was built or tested") == std::string::npos);
    const auto prompt = task.to_prompt_block();
    CHECK(prompt.find(command.command) != std::string::npos);
    CHECK(prompt.find("classified build or test") != std::string::npos);
}

TEST_CASE("unsupported wrapper claims describe missing evidence without denying execution",
          "[task][evidence][opaque-claim-feedback]") {
    TaskState task;
    task.begin("fix the decoder");
    task.record_evidence(evidence_of(EvidenceKind::FileWritten, true, "src/decoder.h"));
    task.record_evidence(
        evidence_of(EvidenceKind::CommandRun, true, "pwsh -NoProfile -File ./run-tests.ps1"));

    auto problems = task.unsupported_claims("The project builds successfully. Tests pass.");
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].find("no classified successful build evidence") != std::string::npos);
    CHECK(problems[1].find("no classified successful test evidence") != std::string::npos);
    for (const auto& problem : problems) {
        CHECK(problem.find("no successful build was run") == std::string::npos);
        CHECK(problem.find("no successful test run happened") == std::string::npos);
    }
    CHECK(task.verification() == VerificationState::NotAttempted);

    // An explicit successful comparison, recorded by ProcessTools as TestsRun,
    // supports that check without inventing a classified build for the script.
    task.record_evidence(evidence_of(EvidenceKind::TestsRun, true, "explicit sentinel check"));
    CHECK(task.unsupported_claims("The output is correct.").empty());
    CHECK_FALSE(task.unsupported_claims("The project builds successfully.").empty());
    CHECK_FALSE(task.has_successful_build());
    CHECK(task.has_successful_tests());
}

TEST_CASE("the plan tracks which step is active", "[task]") {
    TaskState task;
    task.begin("objective");
    task.set_plan({"inspect", "reproduce", "fix", "verify"});

    REQUIRE(task.steps().size() == 4);
    CHECK(task.current_step() == "inspect");

    task.set_current_step("inspect");
    task.complete_current_step("read three files");
    CHECK(task.steps()[0].status == StepStatus::Done);
    CHECK(task.steps()[0].note == "read three files");

    task.set_current_step("reproduce");
    task.fail_current_step("could not build");
    CHECK(task.steps()[1].status == StepStatus::Failed);

    // Only one step is active at a time.
    task.set_current_step("fix");
    int active = 0;
    for (const auto& step : task.steps()) {
        if (step.status == StepStatus::Active) ++active;
    }
    CHECK(active == 1);
}

TEST_CASE("verification requires success AFTER the last edit", "[task][evidence]") {
    TaskState task;
    task.begin("fix it");

    // A build that succeeded before the change proves nothing about it.
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, true));
    CHECK(task.verification() == VerificationState::Partial);

    task.record_evidence(evidence_of(EvidenceKind::FileWritten, true, "src/main.cpp"));
    CHECK(task.verification() == VerificationState::NotAttempted);

    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, true));
    CHECK(task.verification() == VerificationState::Partial);

    task.record_evidence(evidence_of(EvidenceKind::TestsRun, true));
    CHECK(task.verification() == VerificationState::Verified);
}

TEST_CASE("resumed checks are historical until revalidated", "[task][persistence]") {
    TaskState task;
    task.begin("fix it");
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, true));
    task.record_evidence(evidence_of(EvidenceKind::TestsRun, true));
    REQUIRE(task.verification() == VerificationState::Verified);
    task.invalidate_checks();
    CHECK(task.verification() == VerificationState::NotAttempted);
    CHECK_FALSE(task.has_successful_build());
    CHECK_FALSE(task.has_successful_tests());
    task = TaskState::from_json(task.to_json());
    CHECK(task.verification() == VerificationState::NotAttempted);
    task.record_evidence(evidence_of(EvidenceKind::TestsRun, true));
    CHECK(task.verification_summary().find("tests ran, but no build") != std::string::npos);
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, true));
    CHECK(task.verification() == VerificationState::Verified);
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, false));
    CHECK_FALSE(task.has_successful_build());
}

TEST_CASE("a failure after the last edit means not verified", "[task][evidence]") {
    TaskState task;
    task.begin("fix it");

    task.record_evidence(evidence_of(EvidenceKind::FileWritten, true, "a.cpp"));
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, false));

    CHECK(task.verification() == VerificationState::Failed);
    CHECK(task.has_failed_build_since_last_edit());
    CHECK(task.verification_summary().find("NOT VERIFIED") != std::string::npos);
}

TEST_CASE("unsupported claims are detected", "[task][evidence]") {
    TaskState task;
    task.begin("fix it");
    task.record_evidence(evidence_of(EvidenceKind::FileWritten, true, "a.cpp"));

    SECTION("claiming a build with no build run") {
        auto problems = task.unsupported_claims("I fixed it and the project builds successfully.");
        CHECK_FALSE(problems.empty());
    }
    SECTION("claiming tests with no test run") {
        auto problems = task.unsupported_claims("All tests pass now.");
        CHECK_FALSE(problems.empty());
    }
    SECTION("an honest report raises nothing") {
        auto problems = task.unsupported_claims(
            "I changed the loop bound in render.cpp. I have not rebuilt, so this is unverified.");
        CHECK(problems.empty());
    }
    SECTION("a claim backed by evidence is fine") {
        task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, true));
        auto problems = task.unsupported_claims("The project builds successfully.");
        CHECK(problems.empty());
    }
    SECTION("a build claim is not satisfied by a test run alone") {
        task.record_evidence(evidence_of(EvidenceKind::TestsRun, true));
        auto problems = task.unsupported_claims("It builds successfully.");
        CHECK_FALSE(problems.empty());
    }
}

TEST_CASE("files and commands are recorded and de-duplicated", "[task]") {
    TaskState task;
    task.begin("objective");

    task.record_file("src/a.cpp", "modified");
    task.record_file("src/a.cpp", "modified");
    task.record_file("src/b.cpp", "created");

    REQUIRE(task.files_changed().size() == 2);
    CHECK(task.files_changed()[0].edit_count == 2);

    CommandRecord command;
    command.command = "cmake --build build";
    command.exit_code = 0;
    command.succeeded = true;
    task.record_command(command);
    REQUIRE(task.commands_run().size() == 1);
}

TEST_CASE("the prompt block carries the mission", "[task]") {
    TaskState task;
    task.begin("fix the renderer");
    task.add_requirement("do not change the public API");
    task.set_plan({"inspect", "fix"});
    task.set_current_step("inspect");
    task.record_file("src/render.cpp", "modified");
    task.add_blocker("the shader compiler is not installed");

    const auto block = task.to_prompt_block();
    CHECK(block.find("fix the renderer") != std::string::npos);
    CHECK(block.find("do not change the public API") != std::string::npos);
    CHECK(block.find("src/render.cpp") != std::string::npos);
    CHECK(block.find("shader compiler") != std::string::npos);
    CHECK(block.find("not verified") != std::string::npos);
}

TEST_CASE("an empty task produces no prompt block", "[task]") {
    TaskState task;
    CHECK(task.to_prompt_block().empty());
}

TEST_CASE("task state survives serialisation", "[task]") {
    TaskState task;
    task.begin("the objective");
    task.add_requirement("a requirement");
    task.set_plan({"one", "two"});
    task.set_current_step("one");
    task.complete_current_step("done it");
    task.record_file("f.cpp", "created");
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, true, "cmake"));
    task.note_iteration();

    CommandRecord command;
    command.command = "ninja";
    command.succeeded = true;
    task.record_command(command);

    const auto restored = TaskState::from_json(task.to_json());

    CHECK(restored.objective() == "the objective");
    REQUIRE(restored.requirements().size() == 1);
    REQUIRE(restored.steps().size() == 2);
    CHECK(restored.steps()[0].status == StepStatus::Done);
    REQUIRE(restored.files_changed().size() == 1);
    REQUIRE(restored.commands_run().size() == 1);
    CHECK(restored.has_successful_build());
    CHECK(restored.iterations() == 1);
}

TEST_CASE("the evidence list stays bounded", "[task]") {
    TaskState task;
    task.begin("long run");
    for (int i = 0; i < 1000; ++i) {
        task.record_evidence(evidence_of(EvidenceKind::FileRead, true, "f" + std::to_string(i)));
    }
    CHECK(task.evidence().size() <= 400);
}

TEST_CASE("fresh verification survives eviction of historical evidence", "[task][audit]") {
    TaskState task;
    task.begin("resume a long task");
    for (int i = 0; i < 400; ++i)
        task.record_evidence(evidence_of(EvidenceKind::FileRead, true, "old source"));
    task.invalidate_checks();
    task = TaskState::from_json(task.to_json());
    CHECK(task.verification() == VerificationState::NotAttempted);
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, true));
    CHECK(task.has_successful_build());
    CHECK(task.verification() == VerificationState::Partial);
    task.record_evidence(evidence_of(EvidenceKind::TestsRun, true));
    CHECK(task.has_successful_tests());
    CHECK(task.verification() == VerificationState::Verified);
    CHECK(task.evidence().size() == 400);
}

// ---------------------------------------------------------------------------
// Running a program is not checking its output.
//
// Measured with a 3B model on a Fibonacci task. It created the source, fixed a
// compile error, built with MSVC in Release, and ran the executable to exit
// code 0. The program printed
//
//     1 3 6 12 24 48 96 192 384 768
//
// which is a doubling sequence, not Fibonacci. The reply said "the first 10
// Fibonacci numbers are indeed printed correctly" and "the task is verified",
// and both passed unchallenged, because a build genuinely had succeeded and the
// claim patterns only covered claims about building and testing.
// ---------------------------------------------------------------------------

TEST_CASE("claiming the output is correct requires more than a successful build",
          "[task][evidence][honesty]") {
    TaskState task;
    task.begin("print the first 10 Fibonacci numbers");

    // Everything the run actually established: it built, and it ran.
    Evidence built;
    built.kind = EvidenceKind::BuildAttempted;
    built.subject = "cmake --build . --config Release";
    built.positive = true;
    task.record_evidence(built);

    Evidence ran;
    ran.kind = EvidenceKind::CommandRun;
    ran.subject = "fibonacci.exe";
    ran.positive = true;
    task.record_evidence(ran);

    CHECK(task.has_successful_build());
    CHECK_FALSE(task.has_successful_tests());

    // A claim about the build is supported.
    CHECK(task.unsupported_claims("The project builds successfully.").empty());

    // Claims about the result are not.
    CHECK_FALSE(
        task.unsupported_claims("The first 10 Fibonacci numbers are indeed printed correctly.")
            .empty());
    CHECK_FALSE(task.unsupported_claims("The output is as expected.").empty());
    CHECK_FALSE(task.unsupported_claims("we can now report that the task is verified").empty());
    CHECK_FALSE(task.unsupported_claims("It works correctly.").empty());

    // Reporting what happened, without asserting it was right, is fine.
    CHECK(task.unsupported_claims("The program printed: 1 3 6 12 24 48 96 192 384 768").empty());
}

TEST_CASE("a build that fails and then succeeds is verified, not failing",
          "[task][verification][regression]") {
    // Fixing what a build complained about and building again is the ordinary
    // loop. Only the most recent attempt is evidence about the code as it
    // stands; a superseded failure is not.
    //
    // Measured: a run whose every acceptance criterion was settled, whose
    // program printed exactly the right thing, and whose final answer was
    // accurate, reported "failing" - one earlier command had failed and no
    // edit happened between it and the successful build that replaced it.
    TaskState task;
    task.record_file("main.cpp", "modified");

    Evidence failed;
    failed.kind = EvidenceKind::BuildAttempted;
    failed.subject = "cmake --build build";
    failed.positive = false;
    task.record_evidence(failed);
    CHECK(task.verification() == VerificationState::Failed);

    Evidence passed = failed;
    passed.positive = true;
    task.record_evidence(passed);
    CHECK(task.verification() == VerificationState::Partial);

    Evidence tests;
    tests.kind = EvidenceKind::TestsRun;
    tests.subject = "ctest";
    tests.positive = true;
    task.record_evidence(tests);
    CHECK(task.verification() == VerificationState::Verified);
}

TEST_CASE("a build that succeeds and then breaks is failing again",
          "[task][verification][regression]") {
    // The rule must cut both ways, or it would be a way to keep a stale green
    // light after breaking the build.
    TaskState task;
    task.record_file("main.cpp", "modified");

    Evidence passed;
    passed.kind = EvidenceKind::BuildAttempted;
    passed.subject = "cmake --build build";
    passed.positive = true;
    task.record_evidence(passed);
    CHECK(task.verification() == VerificationState::Partial);

    Evidence broke = passed;
    broke.positive = false;
    task.record_evidence(broke);
    CHECK(task.verification() == VerificationState::Failed);
}

TEST_CASE("the model task projection can omit only its duplicated objective",
          "[task][stable-source-context][candidate8]") {
    TaskState task;
    const std::string objective = "A distinct original objective already protected as human input.";
    task.begin(objective);
    task.add_requirement("Preserve the public API and literal failing inputs.");
    task.set_plan({"inspect", "regression", "fix", "verify"});
    task.set_current_step("inspect");
    task.complete_current_step("Read the real source.");
    task.set_current_step("regression");
    task.record_file("src/renderer.cpp", "modified");
    CommandRecord command;
    command.command = "cmake --build build";
    command.exit_code = 2;
    command.succeeded = false;
    task.record_command(command);
    task.record_evidence(evidence_of(EvidenceKind::BuildAttempted, false));
    task.add_blocker("The compiler rejected the changed source.");
    const auto saved = task.to_json();
    const auto full = task.to_prompt_block();
    CHECK(full == task.to_prompt_block(true));
    REQUIRE(full.find(objective) != std::string::npos);
    const auto projected = task.to_prompt_block(false);
    auto expected = full;
    const std::string objective_line = "Objective: " + objective + "\n";
    const auto at = expected.find(objective_line);
    REQUIRE(at != std::string::npos);
    expected.erase(at, objective_line.size());
    CHECK(projected == expected);
    CHECK(projected.find(objective) == std::string::npos);
    CHECK(projected.find("literal failing inputs") != std::string::npos);
    CHECK(projected.find("regression") != std::string::npos);
    CHECK(projected.find("renderer.cpp") != std::string::npos);
    CHECK(projected.find("exit 2") != std::string::npos);
    CHECK(projected.find("Verification status:") != std::string::npos);
    CHECK(projected.find("The compiler rejected") != std::string::npos);
    CHECK(task.to_json() == saved);
    CHECK(task.objective() == objective);
    TaskState empty;
    CHECK(empty.to_prompt_block().empty());
    CHECK(empty.to_prompt_block(false).empty());
}
