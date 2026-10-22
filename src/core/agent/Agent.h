#pragma once
// ---------------------------------------------------------------------------
// The agent loop.
//
//   user message
//     -> assemble context (system prompt + skills + task state + history)
//     -> model
//     -> tool calls?  yes: execute them, append the results, go again
//                     no : final answer
//
// It keeps going. One tool call is not a turn; the loop runs until the model
// stops asking for tools, the iteration budget is spent, the user cancels, or
// the progress monitor concludes nothing is changing.
//
// This class knows nothing about Qt. It runs on a worker thread and reports
// through an event sink, which is what lets the same engine drive the GUI, the
// tests and the headless CLI.
// ---------------------------------------------------------------------------
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../inference/ChatTypes.h"
#include "../skills/SkillRegistry.h"
#include "../tools/ToolRegistry.h"
#include "ContextManager.h"
#include "Presentation.h"
#include "ProgressMonitor.h"
#include "SourceWitnesses.h"
#include "SystemPrompt.h"
#include "TaskState.h"
#include "Verification.h"

namespace localcode {

class Workspace;
class EnvironmentService;
class WebService;

namespace repo {
class RepoIndex;
}

enum class AgentEventType {
    RunStarted,
    PromptProcessing,   // request sent; waiting for the first token
    ReasoningDelta,     // native reasoning text, only when the model has it
    ContentDelta,
    AssistantMessage,   // a complete assistant turn
    ToolCallStarted,
    ToolProgress,
    ToolOutput,   // live stdout/stderr from a running command
    ToolCompleted,
    IterationFinished,
    ContextUpdated,
    VerificationUpdated,   // the stages, the plan and the evidence behind them
    GenerationStats,       // throughput and timing for one completed turn
    ProgressWarning,
    // The run is asking for its final summary. Distinct from ProgressWarning
    // because it is not a stall being redirected: the work is already
    // established and what remains is describing it. Told apart here so the
    // interface does not have to recognise the wording.
    FinalizationStarted,
    TaskStateUpdated,
    RunFinished,
    RunFailed,
    RunCancelled,
};

std::string_view to_string(AgentEventType type) noexcept;

// What one turn cost. Every field states where its number came from, because
// mixing a measurement with an estimate produces a display that cannot be
// trusted for either.
//
//   prompt_tokens            tokens in the request, including any the runtime
//                            served from cache
//   cached_prompt_tokens     of those, the ones it did not have to evaluate
//   completion_tokens        tokens decoded this turn - reasoning AND the
//                            visible answer together, because they come off the
//                            same decode loop and no runtime separates them
//   prompt_ms / decode_ms    the runtime's own phase timings
//   *_tokens_per_second      derived from those phases, never from the whole
//                            request
//   total_request_ms         measured here: request sent to last byte. Always
//                            larger than prompt_ms + decode_ms
//   time_to_first_token_ms   measured here: request sent to the first delta of
//                            any kind, reasoning included
//
// Reasoning has no measured duration. The runtime reports one decode phase and
// does not say which part of it was the think block, so all that can be observed
// from this side is how long reasoning deltas were arriving - and that is a
// property of network buffering as much as of the model. It is carried as
// `observed_reasoning_stream_ms` and must never be presented as the time the
// model spent thinking.
struct TurnStats {
    // --- Reported by the runtime -----------------------------------------
    bool usage_from_server = false;
    int prompt_tokens = 0;
    int cached_prompt_tokens = 0;
    int completion_tokens = 0;

    bool timings_from_server = false;
    int prompt_tokens_evaluated = 0;
    double prompt_ms = 0.0;
    double prompt_tokens_per_second = 0.0;
    double decode_ms = 0.0;
    double decode_tokens_per_second = 0.0;

    // --- Measured by this client -----------------------------------------
    std::int64_t total_request_ms = 0;
    std::int64_t time_to_first_token_ms = 0;

    // --- Reasoning: observation, not telemetry ---------------------------
    int reasoning_characters = 0;
    // LocalCode's estimate from those characters. Never a server figure.
    int estimated_reasoning_tokens = 0;
    // Wall time between the first and last reasoning delta. A lower bound on
    // how long the think block took to arrive, not how long it took to produce.
    std::int64_t observed_reasoning_stream_ms = 0;
    // How many separate deltas carried it. One delta means the whole block
    // arrived in a single read and the span above measures nothing useful.
    int reasoning_delta_count = 0;
};

struct AgentEvent {
    AgentEventType type = AgentEventType::RunStarted;
    std::string text;   // delta text, message body, or explanation
    ToolCall tool_call;
    ToolResult tool_result;
    ContextBudget context;
    ChatMessage message;
    // Populated on VerificationUpdated and RunFinished. Shared rather than
    // copied: AgentEvent is passed for every content delta, and the ledger
    // grows for the whole run.
    std::shared_ptr<const VerificationLedger> ledger;
    TurnStats stats;
    int iteration = 0;
    bool is_stderr = false;
    std::int64_t at_unix_ms = 0;
};

using AgentEventSink = std::function<void(const AgentEvent&)>;

struct AgentConfig {
    int max_iterations = 40;
    // Stops a runaway loop from spending the whole evening on one request.
    std::chrono::milliseconds wall_clock_budget{std::chrono::minutes(45)};
    bool stream = true;
    bool include_reasoning_in_history = false;
    int max_tool_calls_per_turn = 8;
    // Diagnostic prototype. Native template schema changes can invalidate a
    // large prefix; retain the normal transport until CPU quality/latency pass.
    bool experimental_deferred_tools = false;
    bool experimental_stable_tool_dispatch = false;
    bool experimental_compact_prompt = false;
};

struct AgentRunSummary {
    bool completed = false;
    bool cancelled = false;
    std::string final_message;
    std::string stop_reason;
    int iterations = 0;
    int tool_calls = 0;
    std::int64_t duration_ms = 0;
    VerificationState verification = VerificationState::NotAttempted;

    // How far the run got in telling the user what happened, tracked apart from
    // whether the work succeeded. A run can honestly be verified and still have
    // failed to produce a sentence about it, and collapsing the two reports
    // finished engineering as a failure.
    PresentationState presentation = PresentationState::None;
    // Finalization attempts actually made: 0 when the model answered normally.
    int finalization_attempts = 0;

    std::vector<std::string> unsupported_claims;
    // What was actually established, stage by stage, and against which source
    // revision. The honest answer to "is this done?" is read from here.
    VerificationLedger ledger;
    ContextBudget context;
};

class Agent {
public:
    Agent();
    ~Agent();

    // --- Wiring ----------------------------------------------------------
    void set_backend(BackendPtr backend);
    void set_profile(ModelProfile profile);
    void set_tools(ToolRegistry* tools);
    void set_workspace(Workspace* workspace);
    void set_skills(SkillRegistry* skills);
    void set_environment(EnvironmentService* environment);
    void set_web(WebService* web);
    // The repository index, when one is open. The agent never indexes; it only
    // passes this to the tools that query it.
    void set_repo(repo::RepoIndex* repo);
    void set_config(AgentConfig config);
    [[nodiscard]] AgentConfig config() const;

    void set_event_sink(AgentEventSink sink);
    void set_confirmation_handler(std::function<bool(const ConfirmationRequest&)> handler);

    // --- Conversation ----------------------------------------------------
    void set_history(std::vector<ChatMessage> history);
    [[nodiscard]] std::vector<ChatMessage> history() const;
    void clear_history();

    [[nodiscard]] TaskState task_state() const;
    void set_task_state(TaskState state);

    [[nodiscard]] VerificationLedger verification_ledger() const;
    void set_verification_ledger(VerificationLedger ledger);

    // --- Running ---------------------------------------------------------
    // Blocking. Runs the whole loop for one user message. Call on a worker
    // thread; the GUI never calls this directly from the UI thread.
    AgentRunSummary run(const std::string& user_message, CancellationToken cancel);

    [[nodiscard]] bool running() const { return running_.load(); }

    [[nodiscard]] ContextBudget last_context() const;

private:
    // Named `publish` rather than `emit`: Qt defines `emit` as a macro, and the
    // GUI includes this header after Qt's.
    void publish(AgentEvent event) const;
    void emit_simple(AgentEventType type, std::string text) const;

    // Adds a corrective instruction to the conversation without breaking role
    // alternation.
    //
    // Several chat templates - Mistral's among them - raise rather than tolerate
    // two consecutive user turns: "conversation roles must alternate user and
    // assistant roles except for tool calls and results", returned as HTTP 500.
    // Guidance is therefore merged into the previous user message when the last
    // message is already one. Caller must not hold mutex_.
    void push_guidance(std::string text);

    [[nodiscard]] std::string build_system_prompt(const std::vector<ToolDefinition>& tools,
                                                  bool text_protocol) const;
    [[nodiscard]] std::vector<SkillMatch> pick_skills(const std::string& objective) const;

    // Runs one tool call and turns the outcome into a tool message.
    // `settled_criterion` comes back true when this call settled an
    // acceptance criterion, which is progress even though nothing on disk
    // moved. Without it the progress monitor tells a model that is correctly
    // verifying its work to stop investigating and make a change.
    struct ToolProgress {
        bool succeeded = false;
        std::string summary;
        std::optional<std::string> failure_identity;
    };

    ChatMessage execute_tool(const ToolCall& call, CancellationToken cancel, bool& changed_files,
                             bool& settled_criterion, std::optional<bool>& source_novelty,
                             ToolProgress& progress_result);

    // Appends an honest caveat when the model claims more than the evidence
    // supports. Returns the (possibly amended) message.
    [[nodiscard]] std::string apply_evidence_check(const std::string& message,
                                                   std::vector<std::string>& problems) const;

    mutable std::mutex mutex_;
    BackendPtr backend_;
    ModelProfile profile_;
    ToolRegistry* tools_ = nullptr;
    Workspace* workspace_ = nullptr;
    std::shared_ptr<const TaskPolicy> task_policy_;
    SkillRegistry* skills_ = nullptr;
    EnvironmentService* environment_ = nullptr;
    WebService* web_ = nullptr;
    repo::RepoIndex* repo_ = nullptr;

    AgentConfig config_;
    AgentEventSink sink_;
    std::function<bool(const ConfirmationRequest&)> confirm_;

    std::vector<ChatMessage> history_;
    TaskState task_;
    VerificationLedger ledger_;
    ContextManager context_;
    ProgressMonitor progress_;
    SourceWitnesses source_witnesses_;
    ContextBudget last_context_;

    std::atomic<bool> running_{false};
};

}   // namespace localcode
