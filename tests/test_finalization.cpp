// ---------------------------------------------------------------------------
// Presentation, and its separation from the objective.
//
// Measured on Qwen3.5-9B: ten runs of ten reached a verified objective with a
// clean ledger, and eight ended with the model producing reasoning and no prose.
// All ten were reported as incomplete, which is false about the engineering.
//
// These tests hold the line that the two are different questions: what was
// built and checked, and whether anyone was told about it. A run may honestly be
// verified and unpresentable, and the record has to say so rather than calling
// finished work a failure.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "agent/Agent.h"
#include "agent/EvidenceInterpreter.h"
#include "agent/Presentation.h"
#include "agent/TaskState.h"
#include "agent/Verification.h"
#include "inference/RequestTrace.h"
#include "platform/Environment.h"
#include "support/MockBackend.h"
#include "support/TempDirectory.h"
#include "tools/ToolRegistry.h"
#include "workspace/Workspace.h"

using namespace localcode;
using localcode::test::MockBackend;
using localcode::test::ScriptedTurn;
using localcode::test::TempDirectory;

namespace {

// Two different things in this codebase are called verification, and mixing
// them up is easy: `AgentRunSummary::verification` is TaskState's build-and-test
// summary, while the objective the finalizer keys on is the ledger's
// Stage::Objective. A run can settle every criterion through a command without
// ever building, which reads as Partial there and Pass here. These tests assert
// the ledger, because that is what "the work is done" means.

// A ledger in the state the finalizer is meant to describe: work done, criteria
// settled, objective established.
VerificationLedger verified_ledger() {
    VerificationLedger ledger;
    ledger.set_acceptance_plan(
        {AcceptanceCheck{"the program prints the first ten Fibonacci numbers"}});
    const auto build =
        ledger.add_evidence(EvidenceKind::BuildAttempted, "cmake --build build", "exit 0", true);
    ledger.record(Stage::Build, StageStatus::Pass, build, "built cleanly");
    const auto ran = ledger.add_evidence(EvidenceKind::CommandRun, "build/fib.exe",
                                         "printed 0 1 1 2 3 5 8 13 21 34", true);
    ledger.record(Stage::Execution, StageStatus::Pass, ran, "ran");
    ledger.record(Stage::Behaviour, StageStatus::Pass, ran, "output matched");
    ledger.settle_check(0, true, ran);
    ledger.reassess_objective();
    return ledger;
}

// The same work, with a criterion nobody ever checked.
VerificationLedger partly_checked_ledger() {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({AcceptanceCheck{"the program builds"},
                                AcceptanceCheck{"every line is a Fibonacci number"}});
    const auto build =
        ledger.add_evidence(EvidenceKind::BuildAttempted, "cmake --build build", "exit 0", true);
    ledger.record(Stage::Build, StageStatus::Pass, build, "built cleanly");
    ledger.settle_check(0, true, build);
    ledger.reassess_objective();
    return ledger;
}

TaskState task_with_files() {
    TaskState task;
    task.begin("write a Fibonacci program and verify it");
    task.record_file("main.cpp", "created");
    return task;
}

struct FinalizationFixture {
    TempDirectory temp{"lc-final"};
    Workspace workspace;
    ToolRegistry registry;
    EnvironmentService environment;
    SkillRegistry skills;
    Agent agent;
    std::shared_ptr<MockBackend> backend = std::make_shared<MockBackend>();

    FinalizationFixture() {
        registry.register_builtin_tools();
        REQUIRE(workspace.open(temp.path()).ok());

        agent.set_backend(backend);
        agent.set_tools(&registry);
        agent.set_workspace(&workspace);
        agent.set_skills(&skills);
        agent.set_environment(&environment);
        agent.set_confirmation_handler([](const ConfirmationRequest&) { return true; });

        ModelProfile profile;
        profile.id = "mock";
        profile.display_name = "Mock";
        profile.context_size = 32768;
        profile.capabilities.supports_tools = true;
        profile.tool_protocol = ToolProtocol::Native;
        agent.set_profile(profile);
    }

    static ToolCall call(const std::string& name, Json arguments, const std::string& id = "c1") {
        ToolCall c;
        c.id = id;
        c.name = name;
        c.arguments_json = arguments.dump();
        return c;
    }

    // Drives the agent to a verified objective, then hands control to `after`
    // for every turn beyond the work. Turn 0 sets one criterion; turn 1 settles
    // it with a command whose success is the criterion.
    AgentRunSummary run_to_verified(std::function<ScriptedTurn(int)> after,
                                    CancellationToken cancel = {}) {
        int step = 0;
        backend->set_responder([&, this](const ChatRequest&, int) -> ScriptedTurn {
            ScriptedTurn turn;
            switch (step++) {
                case 0:
                    turn.content = "Setting out what done means.";
                    turn.tool_calls.push_back(
                        call("set_acceptance_criteria",
                             Json{{"criteria", Json::array({"the command reports success"})}}));
                    break;
                case 1:
                    turn.content = "Checking it.";
                    turn.tool_calls.push_back(
                        call("run_command",
                             Json{{"executable", "cmd"},
                                  {"arguments", Json::array({"/c", "echo done"})},
                                  {"expect", Json{{"criterion", 1},
                                                  {"description", "the command reports success"},
                                                  {"exit_code", 0}}}},
                             "c2"));
                    break;
                default: return after(step - 3);
            }
            return turn;
        });

        return agent.run("write a program and verify it", cancel);
    }
};

}   // namespace

// --- The digest ------------------------------------------------------------

TEST_CASE("the digest separates what was checked from what was not",
          "[finalization][presentation]") {
    const auto ledger = partly_checked_ledger();
    const auto task = task_with_files();
    const auto digest = build_evidence_digest("write a Fibonacci program", ledger, task);

    REQUIRE(digest.settled.size() == 1);
    REQUIRE(digest.unsettled.size() == 1);
    CHECK(digest.settled[0].find("the program builds") != std::string::npos);
    CHECK(digest.unsettled[0].find("every line is a Fibonacci number") != std::string::npos);
    CHECK(digest.unsettled[0].find("never checked") != std::string::npos);
    CHECK_FALSE(digest.objective_verified);
}

TEST_CASE("a finalization request states the criteria nobody checked",
          "[finalization][presentation]") {
    const auto digest =
        build_evidence_digest("write it", partly_checked_ledger(), task_with_files());
    const std::string prompt = finalization_request(digest, 1);

    // The unchecked criterion has to reach the model, and the model has to be
    // told to say so. Handing over only what passed is how a partial result
    // gets described as a complete one.
    CHECK(prompt.find("every line is a Fibonacci number") != std::string::npos);
    CHECK(prompt.find("NOT checked") != std::string::npos);
    CHECK(prompt.find("Say so") != std::string::npos);
    CHECK(prompt.find("Do not call tools") != std::string::npos);
}

TEST_CASE("the strict retry asks for the answer rather than the reasoning",
          "[finalization][presentation]") {
    const auto digest = build_evidence_digest("write it", verified_ledger(), task_with_files());
    const std::string first = finalization_request(digest, 1);
    const std::string retry = finalization_request(digest, 2);

    CHECK(first != retry);
    CHECK(retry.find("only reasoning") != std::string::npos);
    CHECK(retry.find("Do not restate your reasoning") != std::string::npos);
}

TEST_CASE("reasoning with no prose is not a usable answer", "[finalization][presentation]") {
    CHECK_FALSE(is_usable_final_answer(""));
    CHECK_FALSE(is_usable_final_answer("   \n\n  "));
    CHECK_FALSE(is_usable_final_answer("OK"));
    CHECK_FALSE(is_usable_final_answer("."));
    CHECK(is_usable_final_answer("I built the program and it printed the ten numbers."));
}

TEST_CASE("presentation states name themselves", "[finalization][presentation]") {
    CHECK(to_string(PresentationState::Emitted) == "emitted");
    CHECK(to_string(PresentationState::Failed) == "finalization failed");
    CHECK(is_presentation_failure(PresentationState::Failed));
    CHECK_FALSE(is_presentation_failure(PresentationState::Emitted));
}

// --- The loop --------------------------------------------------------------

TEST_CASE("a verified run that answers normally needs no finalization", "[finalization][agent]") {
    FinalizationFixture fixture;
    auto summary = fixture.run_to_verified([](int) {
        ScriptedTurn turn;
        turn.content = "I ran the command and it reported success, which is what was asked.";
        return turn;
    });

    CHECK(summary.completed);
    CHECK(summary.ledger.holds(Stage::Objective));
    CHECK(summary.finalization_attempts == 0);
    CHECK(summary.presentation == PresentationState::None);
}

TEST_CASE("a verified run that returns only reasoning is finalized, not failed",
          "[finalization][agent]") {
    FinalizationFixture fixture;
    auto summary = fixture.run_to_verified([](int extra) -> ScriptedTurn {
        ScriptedTurn turn;
        if (extra < 3) {
            // Thinking, and nothing the user can read. The loop nudges first.
            turn.reasoning = "Considering how to phrase the summary.";
            return turn;
        }
        turn.content = "The command ran and reported success, so the criterion is met.";
        return turn;
    });

    CHECK(summary.ledger.holds(Stage::Objective));
    CHECK(summary.presentation == PresentationState::Emitted);
    CHECK(summary.completed);
    CHECK(summary.finalization_attempts >= 1);
    CHECK(summary.final_message.find("reported success") != std::string::npos);
}

TEST_CASE("finalization recovers on the strict retry when the first attempt is empty",
          "[finalization][agent]") {
    FinalizationFixture fixture;
    int finalization_turns = 0;
    auto summary = fixture.run_to_verified([&](int extra) -> ScriptedTurn {
        ScriptedTurn turn;
        if (extra < 3) {
            turn.reasoning = "thinking";
            return turn;
        }
        // The first finalization attempt also produces nothing; the second works.
        if (++finalization_turns == 1) {
            turn.reasoning = "still thinking about it";
            return turn;
        }
        turn.content = "The command ran successfully and the criterion is settled.";
        return turn;
    });

    CHECK(summary.ledger.holds(Stage::Objective));
    CHECK(summary.presentation == PresentationState::Emitted);
    CHECK(summary.finalization_attempts == 2);
    CHECK(summary.completed);
}

TEST_CASE("verified work survives a finalization that never produces prose",
          "[finalization][agent]") {
    FinalizationFixture fixture;
    auto summary = fixture.run_to_verified([](int) {
        ScriptedTurn turn;
        turn.reasoning = "thinking, endlessly, and saying nothing";
        return turn;
    });

    // The whole point. The engineering verdict is untouched by the fact that
    // nobody could be told about it.
    CHECK(summary.ledger.holds(Stage::Objective));
    CHECK(summary.presentation == PresentationState::Failed);
    CHECK(is_presentation_failure(summary.presentation));
    CHECK_FALSE(summary.completed);
    CHECK(summary.finalization_attempts == kMaxFinalizationAttempts);
    CHECK(summary.stop_reason.find("verified") != std::string::npos);
    CHECK(summary.stop_reason.find("no final answer") != std::string::npos);
}

TEST_CASE("finalization is offered no tools and asks the template not to think",
          "[finalization][agent]") {
    FinalizationFixture fixture;
    fixture.run_to_verified([](int) {
        ScriptedTurn turn;
        turn.reasoning = "thinking";
        return turn;
    });

    // The last requests are the finalization attempts. They must carry no tools
    // - this phase reports, it does not act - and must ask the chat template to
    // skip thinking, which is the only lever measured to work per request.
    const auto& last = fixture.backend->last_request();
    CHECK(last.tools.empty());
    REQUIRE(last.thinking_override.has_value());
    CHECK(*last.thinking_override == false);
    CHECK_FALSE(last.enable_reasoning);
    REQUIRE(last.messages.size() == 2);
    CHECK(last.messages.front().role == Role::System);
    CHECK(last.messages.back().role == Role::User);
    CHECK(last.messages.front().content.find("Actions are closed") != std::string::npos);
}

TEST_CASE("finalization cannot bypass source citation coverage",
          "[finalization][agent][source-grounding][recovery]") {
    FinalizationFixture fixture;
    const auto summary = fixture.run_to_verified([](int extra) {
        ScriptedTurn turn;
        if (extra < 3)
            turn.reasoning = "Considering the summary.";
        else
            turn.content = "The command succeeded. The declaration is in unread.cpp:99.";
        return turn;
    });
    CHECK(summary.ledger.holds(Stage::Objective));
    CHECK(summary.finalization_attempts == 2);
    CHECK_FALSE(summary.completed);
    CHECK_FALSE(summary.unsupported_claims.empty());
}

TEST_CASE("serialized actions are not usable finalization prose",
          "[finalization][navigation][recovery]") {
    CHECK_FALSE(is_usable_final_answer(
        "<tool_call>\n<function=read_file_range>\n<parameter=path>\nmodule.cpp\n"
        "</parameter>\n</function>\n</tool_call>"));
    CHECK_FALSE(is_usable_final_answer(
        "<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"module.cpp\"}}</tool_call>"));
}

TEST_CASE("a finalizer cannot conceal native action intent in otherwise usable prose",
          "[finalization][navigation][recovery]") {
    FinalizationFixture fixture;
    const auto summary = fixture.run_to_verified([](int extra) {
        ScriptedTurn turn;
        if (extra < 3)
            turn.reasoning = "Considering the verified result.";
        else
            turn.content =
                "The command succeeded. I will inspect more source. "
                "{\"name\":\"read_file\",\"arguments\":{\"path\":\"module.cpp\"}}";
        return turn;
    });
    CHECK(summary.ledger.holds(Stage::Objective));
    CHECK(summary.finalization_attempts == 2);
    CHECK_FALSE(summary.completed);
    CHECK(summary.final_message.empty());
}

TEST_CASE("an unverified run is not finalized into looking finished", "[finalization][agent]") {
    FinalizationFixture fixture;
    // No criteria, no work: the objective was never established, so there is
    // nothing to summarise and nothing to rescue.
    fixture.backend->set_responder([](const ChatRequest&, int) -> ScriptedTurn {
        ScriptedTurn turn;
        turn.reasoning = "thinking about it";
        return turn;
    });

    CancellationSource cancel;
    auto summary = fixture.agent.run("do something", cancel.token());

    CHECK_FALSE(summary.completed);
    CHECK(summary.verification != VerificationState::Verified);
    CHECK(summary.finalization_attempts == 0);
    CHECK(summary.presentation == PresentationState::Failed);
}

TEST_CASE("cancellation during finalization does not fabricate an answer",
          "[finalization][agent]") {
    FinalizationFixture fixture;
    CancellationSource cancel;
    int step = 0;
    fixture.backend->set_responder([&](const ChatRequest&, int) -> ScriptedTurn {
        ScriptedTurn turn;
        switch (step++) {
            case 0:
                turn.tool_calls.push_back(FinalizationFixture::call(
                    "set_acceptance_criteria",
                    Json{{"criteria", Json::array({"the command reports success"})}}));
                return turn;
            case 1:
                turn.tool_calls.push_back(FinalizationFixture::call(
                    "run_command",
                    Json{{"executable", "cmd"},
                         {"arguments", Json::array({"/c", "echo done"})},
                         {"expect", Json{{"criterion", 1},
                                         {"description", "the command reports success"},
                                         {"exit_code", 0}}}},
                    "c2"));
                return turn;
            default:
                cancel.cancel();
                turn.reasoning = "thinking";
                return turn;
        }
    });

    auto summary = fixture.agent.run("write a program and verify it", cancel.token());

    CHECK_FALSE(summary.completed);
    CHECK(summary.final_message.empty());
}

TEST_CASE("cancellation of a successful finalization reply cannot become completion",
          "[finalization][agent][cancel][recovery]") {
    FinalizationFixture fixture;
    CancellationSource cancel;
    bool cancel_caller = false;
    SECTION("the caller cancels while the finalizer produces usable prose") {
        cancel_caller = true;
    }
    SECTION("the backend returns usable partial prose with cancelled finish reason") {}

    int finalized_replies = 0;
    int cancelled_events = 0;
    int completed_events = 0;
    fixture.agent.set_event_sink([&](const AgentEvent& event) {
        cancelled_events += event.type == AgentEventType::RunCancelled;
        completed_events += event.type == AgentEventType::RunFinished;
    });
    const auto summary = fixture.run_to_verified(
        [&](int extra) {
            ScriptedTurn turn;
            if (extra < 3) {
                turn.reasoning = "Considering how to phrase the verified result.";
                return turn;
            }
            ++finalized_replies;
            // MockBackend returns this as a successful transport response even
            // when cancellation arrives after the request started. The content
            // would pass final-answer and evidence checks without a cancel gate.
            turn.content = "The command ran and reported success; the acceptance criterion is met.";
            if (cancel_caller)
                cancel.cancel();
            else
                turn.finish_reason = "cancelled";
            return turn;
        },
        cancel.token());

    REQUIRE(summary.ledger.holds(Stage::Objective));
    REQUIRE(summary.finalization_attempts == 1);
    CHECK(finalized_replies == 1);
    CHECK(summary.cancelled);
    CHECK_FALSE(summary.completed);
    CHECK(summary.final_message.empty());
    CHECK(summary.presentation != PresentationState::Emitted);
    CHECK(summary.stop_reason.find("cancelled") != std::string::npos);
    CHECK(cancelled_events == 1);
    CHECK(completed_events == 0);
    CHECK(cancel.cancelled() == cancel_caller);
    REQUIRE_FALSE(fixture.backend->requests().empty());
    CHECK(fixture.backend->requests().back().tools.empty());
    CHECK(fixture.backend->requests().back().thinking_override == false);
}

TEST_CASE("cancellation before the finalizer precheck retains its cancellation outcome",
          "[finalization][agent][cancel][recovery][cancel-before-finalize]") {
    FinalizationFixture fixture;
    CancellationSource cancel;
    int empty_assistant_events = 0;
    int cancelled_events = 0;
    int completed_events = 0;
    int failed_events = 0;
    fixture.agent.set_event_sink([&](const AgentEvent& event) {
        cancelled_events += event.type == AgentEventType::RunCancelled;
        completed_events += event.type == AgentEventType::RunFinished;
        failed_events += event.type == AgentEventType::RunFailed;
        if (event.type == AgentEventType::AssistantMessage && event.message.content.empty() &&
            event.message.tool_calls.empty()) {
            // The first two empty turns are nudged. The third reaches verified
            // presentation finalization, after the main post-chat cancel gate.
            if (++empty_assistant_events == 3) cancel.cancel();
        }
    });
    const auto summary = fixture.run_to_verified(
        [](int) {
            ScriptedTurn turn;
            turn.reasoning = "Considering how to present the verified result.";
            return turn;
        },
        cancel.token());

    REQUIRE(empty_assistant_events == 3);
    REQUIRE(summary.ledger.holds(Stage::Objective));
    CHECK(fixture.backend->requests().size() == 5);
    CHECK(summary.finalization_attempts == 0);
    CHECK(summary.cancelled);
    CHECK_FALSE(summary.completed);
    CHECK(summary.final_message.empty());
    CHECK(summary.presentation != PresentationState::Emitted);
    CHECK(summary.stop_reason.find("cancelled") != std::string::npos);
    CHECK(cancelled_events == 1);
    CHECK(completed_events == 0);
    CHECK(failed_events == 0);
}

// --- Tracing ---------------------------------------------------------------

TEST_CASE("tracing is off unless it is asked for", "[finalization][trace]") {
    RequestTrace::reset_for_testing();
    CHECK_FALSE(RequestTrace::enabled());
    RequestTrace::begin_run("nothing");
    CHECK(RequestTrace::current_file().empty());

    TraceEntry entry;
    entry.phase = "agent";
    RequestTrace::record(entry);   // must be a no-op, not a crash
    RequestTrace::end_run();
}

TEST_CASE("a trace entry carries enough to reproduce the request", "[finalization][trace]") {
    TraceEntry entry;
    entry.phase = "finalization";
    entry.model = "some-model";
    entry.request.messages.push_back(ChatMessage::user("summarise the work"));
    entry.request.sampling.temperature = 0.2F;
    entry.request.sampling.max_tokens = 1024;
    entry.request.thinking_override = false;
    entry.content = "I built it and it ran.";
    entry.reasoning = "deliberation";
    entry.finish_reason = "stop";
    entry.prompt_tokens = 100;
    entry.completion_tokens = 20;
    entry.duration_ms = 1234;

    const Json json = RequestTrace::to_json(entry);
    CHECK(json.at("phase") == "finalization");
    CHECK(json.at("model") == "some-model");
    CHECK(json.at("request").at("thinking_override") == false);
    CHECK(json.at("request").at("sampling").at("max_tokens") == 1024);
    CHECK(json.at("request").at("messages").size() == 1);
    CHECK(json.at("request").at("messages")[0].at("content") == "summarise the work");
    CHECK(json.at("response").at("content") == "I built it and it ran.");
    CHECK(json.at("response").at("reasoning_content") == "deliberation");
    CHECK(json.at("metrics").at("duration_ms") == 1234);
}

TEST_CASE("an enabled trace records the finalization transaction", "[finalization][trace]") {
    TempDirectory trace_dir{"lc-trace"};
    RequestTrace::set_directory_for_testing(trace_dir.path());
    CHECK(RequestTrace::enabled());

    {
        FinalizationFixture fixture;
        fixture.run_to_verified([](int) {
            ScriptedTurn turn;
            turn.reasoning = "thinking";
            return turn;
        });
    }

    // One file per run, one JSON object per interaction, including the
    // finalization attempts that could not be captured before this existed.
    int finalization_entries = 0;
    int agent_entries = 0;
    for (const auto& file : std::filesystem::directory_iterator(trace_dir.path())) {
        std::ifstream in(file.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("\"finalization\"") != std::string::npos) ++finalization_entries;
            if (line.find("\"phase\":\"agent\"") != std::string::npos) ++agent_entries;
        }
    }
    CHECK(agent_entries > 0);
    CHECK(finalization_entries > 0);

    RequestTrace::reset_for_testing();
}

TEST_CASE("stage probes stay separate from replay transactions", "[trace]") {
    TempDirectory directory{"lc-stage-trace"};
    RequestTrace::set_directory_for_testing(directory.path());
    RequestTrace::begin_run("fixture");
    const auto replay = RequestTrace::current_file();
    REQUIRE(!replay.empty());
    auto events = replay;
    events.replace_extension(".events.jsonl");
    RequestTrace::record_event("prompt_built", Json{{"elapsed_ms", 1.25}, {"bytes", 1024}});
    RequestTrace::end_run();
    RequestTrace::record_event("after_run", Json::object());
    std::ifstream source(events);
    std::string line;
    REQUIRE(std::getline(source, line));
    const auto entry = Json::parse(line);
    CHECK(entry.at("stage") == "prompt_built");
    CHECK(entry.at("fields").at("elapsed_ms") == 1.25);
    CHECK(entry.at("at_unix_ms").is_number_integer());
    CHECK_FALSE(static_cast<bool>(std::getline(source, line)));
    CHECK(std::filesystem::file_size(replay) == 0);
    RequestTrace::reset_for_testing();
}

TEST_CASE("closed finalization shares preserved human context without discovery replay",
          "[finalization][agent][closed-report][recovery]") {
    FinalizationFixture fixture;
    const std::string earlier = "Earlier human requirement:\n" + std::string(700, 'z') +
                                "\nConstraint beyond byte500 remains binding.";
    fixture.agent.set_history(
        {ChatMessage::user(earlier), ChatMessage::assistant("A prior proposed reporting format.")});
    const auto result = fixture.run_to_verified([](int extra) {
        ScriptedTurn reply;
        if (extra < 3)
            reply.reasoning = "DISCOVERY_REASONING_NOT_REPORT_EVIDENCE";
        else
            reply.content = "The command reported success, and that recorded criterion was met.";
        return reply;
    });
    const auto& request = fixture.backend->last_request();
    REQUIRE(request.messages.size() == 2);
    const auto report = Json::parse(request.messages.back().content, nullptr, false);
    REQUIRE_FALSE(report.is_discarded());
    REQUIRE(report["conversation"].size() == 3);
    CHECK(report["conversation"][0]["content"] == earlier);
    CHECK(report["conversation"][2]["content"] == "write a program and verify it");
    CHECK(request.messages.back().content.find("DISCOVERY_REASONING_NOT_REPORT_EVIDENCE") ==
          std::string::npos);
    CHECK(request.tools.empty());
    CHECK(request.sampling.max_tokens <= 1024);
    CHECK(result.ledger.holds(Stage::Objective));
    CHECK(result.completed);
    CHECK(result.finalization_attempts >= 1);
}

TEST_CASE("closed reporting instructions do not assert unfinished work is complete",
          "[finalization][closed-report][recovery]") {
    const auto digest =
        build_evidence_digest("write it", partly_checked_ledger(), task_with_files());
    const auto record = finalization_request(digest, 1);
    CHECK(record.find("The work is finished") == std::string::npos);
    CHECK(record.find("Nothing remains to be built") == std::string::npos);
    CHECK(record.find("nothing left to do") == std::string::npos);
    CHECK(record.find("every line is a Fibonacci number") != std::string::npos);
    CHECK(record.find("never checked") != std::string::npos);
}

TEST_CASE("closed finalization refreshes source witnesses after an intervening model call",
          "[finalization][agent][closed-report][recovery]") {
    FinalizationFixture fixture;
    fixture.temp.write("module.cpp", "int original_value = 73;\n");
    fixture.backend->set_responder([&](const ChatRequest&, int turn) {
        ScriptedTurn reply;
        if (turn == 0)
            reply.tool_calls = {FinalizationFixture::call(
                "set_acceptance_criteria",
                {{"criteria", Json::array({"the command reports success"})}}, "criteria")};
        else if (turn == 1)
            reply.tool_calls = {
                FinalizationFixture::call("run_command",
                                          {{"executable", "cmd"},
                                           {"arguments", Json::array({"/c", "echo done"})},
                                           {"expect",
                                            {{"criterion", 1},
                                             {"description", "the command reports success"},
                                             {"exit_code", 0}}}},
                                          "command")};
        else if (turn == 2)
            reply.tool_calls = {
                FinalizationFixture::call("read_file", {{"path", "module.cpp"}}, "source")};
        else if (turn <= 5) {
            if (turn == 5)
                fixture.temp.write("module.cpp",
                                   "int external_revision_has_a_different_length = 91;\n");
            reply.reasoning = "Considering the report.";
        } else
            reply.content =
                "The command criterion was met. Current source details are unavailable after the "
                "external edit.";
        return reply;
    });
    const auto result =
        fixture.agent.run("Run the command check and report source limitations honestly.", {});
    const auto& request = fixture.backend->last_request();
    REQUIRE(result.finalization_attempts >= 1);
    REQUIRE(request.messages.size() == 2);
    const auto report = Json::parse(request.messages.back().content, nullptr, false);
    REQUIRE_FALSE(report.is_discarded());
    CHECK(report["source_excerpts"].empty());
    CHECK(report["source_omissions"]["ambiguous_or_unmatched_receipts"].get<int>() > 0);
    CHECK(request.messages.back().content.find("original_value") == std::string::npos);
    CHECK(result.ledger.holds(Stage::Objective));
}

// The numeric association is a public contract. This fixture supplies only
// an observed exit comparison; it deliberately supplies no project build.
namespace {

ToolResult caller_exit_comparison(std::string description, int criterion = 1,
                                  bool satisfied = true) {
    ToolResult result = ToolResult::success("Recorded helper result", "helper returned 0");
    result.structured = {
        {"command", "pwsh -NoProfile -File ./status-helper.ps1"},
        {"exit_code", 0},
        {"timed_out", false},
        {"cancelled", false},
        {"error_count", 0},
        {"expectation_checked", true},
        {"expectation_satisfied", satisfied},
        {"expectation_description", description},
        {"expectation_checks", Json::array({satisfied ? "exit code is 0" : "exit code is 7"})},
        {"expectation_failures",
         satisfied ? Json::array() : Json::array({"expected exit code 7, got 0"})},
        {"expectation_criterion", criterion}};
    result.evidence.push_back(
        {EvidenceKind::CommandRun, "pwsh -NoProfile -File ./status-helper.ps1", "exit 0", true});
    result.evidence.push_back(
        {EvidenceKind::TestsRun, "behaviour check: " + description,
         satisfied ? "the output matched what was expected" : "the comparison did not match",
         satisfied});
    return result;
}

class CallerExitComparisonTool final : public ITool {
public:
    std::string name() const override { return "run_command"; }

    std::string description() const override { return "Return a recorded helper comparison."; }

    Json parameters_schema() const override { return Json{{"type", "object"}}; }

    bool mutates() const override { return true; }

    ToolResult execute(const Json&, ToolContext&) override {
        return caller_exit_comparison("The project builds with CMake");
    }
};

}   // namespace

TEST_CASE("a caller-associated comparison keeps its scope without changing numeric settlement",
          "[finalization][interpreter][caller-comparison][candidate20]") {
    for (const bool satisfied : {true, false}) {
        VerificationLedger ledger;
        ledger.set_acceptance_plan(
            {{"the untouched criterion"}, {"The project builds with CMake"}});
        auto result = caller_exit_comparison("The project builds with CMake", 2, satisfied);
        const auto call = FinalizationFixture::call("run_command", Json::object());
        interpret_tool_result(call, result, ledger);

        REQUIRE(ledger.acceptance_plan()[1].settled);
        CHECK(ledger.acceptance_plan()[1].satisfied == satisfied);
        CHECK_FALSE(ledger.acceptance_plan()[0].settled);
        CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
        CHECK(ledger.status(Stage::Behaviour) ==
              (satisfied ? StageStatus::Pass : StageStatus::Fail));
        const auto* evidence = ledger.find_evidence(ledger.acceptance_plan()[1].evidence_id);
        REQUIRE(evidence != nullptr);
        CHECK(evidence->kind == EvidenceKind::TestsRun);
        CHECK(evidence->detail.find("The project builds with CMake") != std::string::npos);
        CHECK(evidence->detail.find(satisfied ? "exit code is 0" : "exit code is 7") !=
              std::string::npos);
        CHECK(evidence->detail.find("Caller-associated") != std::string::npos);

        const auto digest =
            build_evidence_digest("Report the observed helper result", ledger, task_with_files());
        REQUIRE(digest.settled.size() == 1);
        CHECK(digest.settled[0].find(satisfied ? "associated check passed"
                                               : "associated check failed") != std::string::npos);
        CHECK(std::find(digest.stages.begin(), digest.stages.end(), "build: not established") !=
              digest.stages.end());
        CHECK(assess_completion(ledger, true).blocked);   // criterion1 is still open
    }
}

TEST_CASE("bounded report evidence retains comparisons before a long caller description",
          "[finalization][presentation][caller-comparison][candidate20]") {
    const std::string description = "The project builds with CMake. " + std::string(700, 'x');
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{description}});
    auto result = caller_exit_comparison(description);
    result.structured["expectation_checks"] =
        Json::array({"exit code is 0", "stdout contains ready"});
    interpret_tool_result(FinalizationFixture::call("run_command", Json::object()), result, ledger);
    const auto digest =
        build_evidence_digest("Report the recorded comparison", ledger, task_with_files());
    const auto request = finalization_request(digest, 1);
    CHECK(request.find("exit code is 0") != std::string::npos);
    CHECK(request.find("stdout contains ready") != std::string::npos);
    CHECK(request.find("Caller-associated comparison") != std::string::npos);
    CHECK(request.find("build: not established") != std::string::npos);
    CHECK(ledger.acceptance_plan()[0].settled);
    CHECK(ledger.acceptance_plan()[0].satisfied);
    CHECK(ledger.holds(Stage::Objective));   // caller-defined checks keep their contract
    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
}

TEST_CASE("recognized build observations retain their classified stage in the digest",
          "[finalization][presentation][caller-comparison][candidate20]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"the build command returns 0"}});
    ToolResult built = ToolResult::success("Build returned 0", "built");
    built.structured = {{"command", "cmake --build build"},
                        {"exit_code", 0},
                        {"timed_out", false},
                        {"cancelled", false},
                        {"error_count", 0}};
    interpret_tool_result(FinalizationFixture::call("run_command", Json::object()), built, ledger);
    REQUIRE(ledger.holds(Stage::Build));
    ledger.settle_check(0, true, ledger.raw(Stage::Build).evidence_id);
    const auto digest = build_evidence_digest("Report the build result", ledger, task_with_files());
    CHECK(std::find(digest.stages.begin(), digest.stages.end(), "build: pass") !=
          digest.stages.end());
    CHECK(std::find(digest.stages.begin(), digest.stages.end(), "build: not established") ==
          digest.stages.end());
    CHECK_FALSE(assess_completion(ledger, true).blocked);
}

TEST_CASE("the actual closed request scopes a caller-associated helper comparison",
          "[finalization][agent][closed-report][caller-comparison][candidate20]") {
    FinalizationFixture fixture;
    fixture.agent.set_environment(nullptr);
    fixture.agent.set_skills(nullptr);
    fixture.registry.remove("run_command");
    fixture.registry.add(std::make_shared<CallerExitComparisonTool>());
    auto config = fixture.agent.config();
    config.max_iterations = 4;
    fixture.agent.set_config(config);
    const std::string human =
        "Write status.txt containing ready, perform the helper exit check, and report what was "
        "observed without claiming a project build.";
    bool captured = false;
    ChatRequest closed;
    fixture.backend->set_responder([&](const ChatRequest& request, int turn) {
        ScriptedTurn reply;
        if (turn == 0)
            reply.tool_calls = {FinalizationFixture::call(
                "set_acceptance_criteria",
                {{"criteria", Json::array({"The project builds with CMake"})}}, "criteria")};
        else if (turn == 1)
            reply.tool_calls = {FinalizationFixture::call(
                "write_file", {{"path", "status.txt"}, {"content", "ready"}}, "source")};
        else if (turn == 2)
            reply.tool_calls = {
                FinalizationFixture::call("run_command",
                                          {{"expect",
                                            {{"criterion", 1},
                                             {"exit_code", 0},
                                             {"description", "The project builds with CMake"}}}},
                                          "comparison")};
        else {
            captured = true;
            closed = request;
            reply.content =
                "The helper returned exit code zero. A project build was not established.";
        }
        return reply;
    });
    const auto summary = fixture.agent.run(human, {});
    REQUIRE(captured);
    REQUIRE(closed.messages.size() == 2);
    REQUIRE(closed.tools.empty());
    const auto report = Json::parse(closed.messages.back().content, nullptr, false);
    REQUIRE_FALSE(report.is_discarded());
    const auto record = report.at("application_record").get<std::string>();
    CHECK(record.find("exit code is 0") != std::string::npos);
    CHECK(record.find("associated check passed") != std::string::npos);
    CHECK(record.find("build: not established") != std::string::npos);
    CHECK(record.find("Criterion associations are supplied by the caller") != std::string::npos);
    REQUIRE(report.at("conversation").size() == 1);
    CHECK(report.at("conversation")[0]["content"] == human);
    REQUIRE(summary.ledger.acceptance_plan().size() == 1);
    CHECK(summary.ledger.acceptance_plan()[0].settled);
    CHECK(summary.ledger.acceptance_plan()[0].satisfied);
    CHECK(summary.ledger.status(Stage::Build) == StageStatus::Unknown);
    CHECK(fixture.agent.task_state().has_successful_build() == false);
    CHECK(summary.tool_calls == 3);
    CHECK(fixture.backend->requests().size() == 4);
    CHECK(std::filesystem::exists(fixture.temp.path() / "status.txt"));
    CHECK(summary.unsupported_claims.empty());
}
