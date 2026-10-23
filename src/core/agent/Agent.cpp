#include "Agent.h"
#include "../security/TaskPolicy.h"

#include "Presentation.h"

#include "../inference/RequestTrace.h"
#include "../repo/RepoQuery.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "../platform/Environment.h"
#include "../repo/RepoIndex.h"
#include "../util/Log.h"
#include "../util/Strings.h"
#include "../web/WebService.h"
#include "../workspace/Workspace.h"
#include "EvidenceInterpreter.h"
#include "FinalResponseBudget.h"
#include "ToolCallParser.h"
#include "ToolCatalog.h"

namespace localcode {
namespace {

// One sleeping waiter per deadline, rather than polling or relying on the
// model/tool returning before the next iteration can check the budget. The
// linked token reaches the existing HTTP and process cancellation callbacks.
class RunDeadline {
public:
    RunDeadline(CancellationToken upstream, std::chrono::steady_clock::time_point deadline)
        : upstream_(upstream, [source = source_]() mutable { source.cancel(); }),
          waiter_([this, deadline](std::stop_token stop) {
              std::unique_lock lock(mutex_);
              condition_.wait_until(lock, stop, deadline, [] { return false; });
              if (stop.stop_requested() || source_.cancelled()) return;
              expired_.store(true);
              lock.unlock();
              source_.cancel();
          }) {}

    [[nodiscard]] CancellationToken token() const { return source_.token(); }

    [[nodiscard]] bool expired() const { return expired_.load(); }

private:
    CancellationSource source_;
    CancellationRegistration upstream_;
    std::atomic<bool> expired_{false};
    std::mutex mutex_;
    std::condition_variable_any condition_;
    // Destroyed first: requests stop, wakes the waiter and joins before any
    // state it accesses is destroyed. The upstream callback owns its source.
    std::jthread waiter_;
};

// Tool names whose success means files on disk changed.
bool is_mutating_tool(const std::string& name) {
    static const char* kNames[] = {"write_file",       "replace_in_file", "apply_patch",
                                   "create_directory", "move_file",       "copy_file",
                                   "download_file",    "delete_path"};
    for (const char* candidate : kNames) {
        if (name == candidate) return true;
    }
    return false;
}

}   // namespace

std::string_view to_string(AgentEventType type) noexcept {
    switch (type) {
        case AgentEventType::RunStarted: return "run_started";
        case AgentEventType::PromptProcessing: return "prompt_processing";
        case AgentEventType::ReasoningDelta: return "reasoning_delta";
        case AgentEventType::ContentDelta: return "content_delta";
        case AgentEventType::AssistantMessage: return "assistant_message";
        case AgentEventType::ToolCallStarted: return "tool_call_started";
        case AgentEventType::ToolProgress: return "tool_progress";
        case AgentEventType::ToolOutput: return "tool_output";
        case AgentEventType::ToolCompleted: return "tool_completed";
        case AgentEventType::IterationFinished: return "iteration_finished";
        case AgentEventType::ContextUpdated: return "context_updated";
        case AgentEventType::VerificationUpdated: return "verification_updated";
        case AgentEventType::GenerationStats: return "generation_stats";
        case AgentEventType::ProgressWarning: return "progress_warning";
        case AgentEventType::FinalizationStarted: return "finalization_started";
        case AgentEventType::TaskStateUpdated: return "task_state_updated";
        case AgentEventType::RunFinished: return "run_finished";
        case AgentEventType::RunFailed: return "run_failed";
        case AgentEventType::RunCancelled: return "run_cancelled";
    }
    return "run_started";
}

Agent::Agent() = default;
Agent::~Agent() = default;

void Agent::set_backend(BackendPtr backend) {
    std::lock_guard lock(mutex_);
    backend_ = std::move(backend);
    context_.set_backend(backend_);
}

void Agent::set_profile(ModelProfile profile) {
    std::lock_guard lock(mutex_);
    profile_ = std::move(profile);

    const int context_size = static_cast<int>(profile_.context_size);
    context_.set_context_size(context_size);

    // Reserving the model's full max_tokens out of a small window leaves no
    // room for the conversation: an 8k model with max_tokens=4096 would have
    // half its context gone before a single message was added. Cap the
    // reservation at a quarter of the window.
    const int reserved =
        std::min(std::max(512, profile_.sampling.max_tokens), std::max(512, context_size / 6));
    context_.set_reserved_output(reserved);
}

void Agent::set_tools(ToolRegistry* tools) {
    std::lock_guard lock(mutex_);
    tools_ = tools;
}

void Agent::set_workspace(Workspace* workspace) {
    std::lock_guard lock(mutex_);
    workspace_ = workspace;
}

void Agent::set_skills(SkillRegistry* skills) {
    std::lock_guard lock(mutex_);
    skills_ = skills;
}

void Agent::set_environment(EnvironmentService* environment) {
    std::lock_guard lock(mutex_);
    environment_ = environment;
}

void Agent::set_web(WebService* web) {
    std::lock_guard lock(mutex_);
    web_ = web;
}

void Agent::set_repo(repo::RepoIndex* repo) {
    std::lock_guard lock(mutex_);
    repo_ = repo;
}

void Agent::set_config(AgentConfig config) {
    std::lock_guard lock(mutex_);
    config_ = config;
}

AgentConfig Agent::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

void Agent::set_event_sink(AgentEventSink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void Agent::set_confirmation_handler(std::function<bool(const ConfirmationRequest&)> handler) {
    std::lock_guard lock(mutex_);
    confirm_ = std::move(handler);
}

void Agent::set_history(std::vector<ChatMessage> history) {
    std::lock_guard lock(mutex_);
    history_ = std::move(history);
}

std::vector<ChatMessage> Agent::history() const {
    std::lock_guard lock(mutex_);
    return history_;
}

void Agent::clear_history() {
    std::lock_guard lock(mutex_);
    history_.clear();
    task_.reset();
    ledger_.reset();
    progress_.reset();
    source_witnesses_.reset({});
}

TaskState Agent::task_state() const {
    std::lock_guard lock(mutex_);
    return task_;
}

void Agent::set_task_state(TaskState state) {
    std::lock_guard lock(mutex_);
    task_ = std::move(state);
}

VerificationLedger Agent::verification_ledger() const {
    std::lock_guard lock(mutex_);
    return ledger_;
}

void Agent::set_verification_ledger(VerificationLedger ledger) {
    std::lock_guard lock(mutex_);
    ledger_ = std::move(ledger);
}

ContextBudget Agent::last_context() const {
    std::lock_guard lock(mutex_);
    return last_context_;
}

void Agent::publish(AgentEvent event) const {
    switch (event.type) {
        case AgentEventType::RunStarted:
        case AgentEventType::ToolCallStarted:
        case AgentEventType::ToolCompleted:
        case AgentEventType::IterationFinished:
        case AgentEventType::FinalizationStarted:
        case AgentEventType::RunFinished:
        case AgentEventType::RunFailed:
        case AgentEventType::RunCancelled:
            if (RequestTrace::enabled()) {
                Json fields{{"type", to_string(event.type)}, {"iteration", event.iteration}};
                if (!event.tool_call.name.empty()) {
                    fields["tool"] = event.tool_call.name;
                    fields["call_id"] = event.tool_call.id;
                    fields["arguments"] = event.tool_call.arguments_json;
                }
                RequestTrace::record_event("agent_event", fields);
            }
            break;
        default: break;
    }
    AgentEventSink sink;
    {
        std::lock_guard lock(mutex_);
        sink = sink_;
    }
    if (!sink) return;
    event.at_unix_ms = now_unix_ms();
    sink(event);
}

void Agent::push_guidance(std::string text) {
    std::lock_guard lock(mutex_);
    // Keep application advice separate from what the human actually wrote.
    // ContextManager folds it onto an adjacent user/tool turn only in the
    // outgoing request when a strict template needs that role sequence.
    history_.push_back(ChatMessage::application_feedback(std::move(text)));
}

void Agent::emit_simple(AgentEventType type, std::string text) const {
    AgentEvent event;
    event.type = type;
    event.text = std::move(text);
    publish(std::move(event));
}

std::vector<SkillMatch> Agent::pick_skills(const std::string& objective) const {
    SkillRegistry* skills = nullptr;
    std::vector<ChatMessage> history;
    {
        std::lock_guard lock(mutex_);
        skills = skills_;
        history = history_;
    }
    if (!skills) return {};

    // Match against the objective plus the last couple of turns, so a task that
    // has drifted into, say, a linker problem picks up the linker skill.
    std::string recent;
    const std::size_t start = history.size() > 4 ? history.size() - 4 : 0;
    for (std::size_t i = start; i < history.size(); ++i) {
        recent += history[i].content;
        recent.push_back('\n');
    }
    // Fewer skills on a small window. Three full procedures cost roughly 2500
    // tokens, which is most of an 8k budget once the tool schemas are counted.
    std::size_t budget_count = 3;
    {
        std::lock_guard lock(mutex_);
        if (profile_.context_size <= 8192)
            budget_count = 1;
        else if (profile_.context_size <= 16384)
            budget_count = 2;
    }
    return skills->select(objective, strings::utf8_truncate(recent, 4000), budget_count);
}

std::string Agent::build_system_prompt(const std::vector<ToolDefinition>& tools,
                                       bool text_protocol) const {
    PromptContext prompt_context;
    RepositoryMap map;
    bool have_map = false;
    {
        std::lock_guard lock(mutex_);
        prompt_context.workspace = workspace_;
        prompt_context.environment = environment_;
        prompt_context.profile = &profile_;
        prompt_context.compact_instructions =
            config_.experimental_compact_prompt || config_.experimental_stable_tool_dispatch;
        prompt_context.read_only =
            task_policy_ ? task_policy_->read_only() : (tools_ && tools_->read_only());
        prompt_context.file_tools_only = task_policy_ && task_policy_->file_tools_only();
        prompt_context.web_enabled = web_ && web_->enabled() && !prompt_context.file_tools_only;
    }
    prompt_context.text_tool_protocol = text_protocol;
    prompt_context.tools = tools;

    if (prompt_context.workspace && prompt_context.workspace->is_open()) {
        // The repository map is the single cheapest thing that stops a model
        // guessing at a project's layout, so it is worth the scan.
        auto built = prompt_context.workspace->build_repository_map(20000);
        if (built) {
            map = *built;
            have_map = true;
        }
    }
    prompt_context.repository_map = have_map ? &map : nullptr;

    // The prompt says the index exists only when it does, and only when it has
    // something in it. Tools a model is never told about are tools it does not
    // use: with the repository tools registered but unmentioned, a run asked
    // which files include a header reached for search_text twice and never
    // touched the index that could answer it directly.
    RepositoryIndexSummary index_summary;
    if (!prompt_context.file_tools_only && repo_ != nullptr && repo_->is_open() &&
        repo_->state() == repo::IndexState::Ready) {
        if (auto overview = repo_->overview(); overview && overview->has_value()) {
            index_summary.available = (**overview).indexed_files != 0;
            index_summary.files = (**overview).indexed_files;
            index_summary.symbols = (**overview).symbol_count;
            index_summary.truncated = (**overview).truncated;
        }
    }
    prompt_context.repository_index = index_summary.available ? &index_summary : nullptr;

    return SystemPrompt::build(prompt_context);
}

ChatMessage Agent::execute_tool(const ToolCall& call, CancellationToken cancel, bool& changed_files,
                                bool& settled_criterion, std::optional<bool>& source_novelty,
                                ToolProgress& progress_result) {
    progress_result = {};
    changed_files = false;
    settled_criterion = false;

    ToolRegistry* tools = nullptr;
    ToolContext tool_context;
    {
        std::lock_guard lock(mutex_);
        tools = tools_;
        tool_context.workspace = workspace_;
        tool_context.task_policy = task_policy_;
        tool_context.environment = environment_;
        tool_context.skills = skills_;
        tool_context.web = web_;
        tool_context.repo = repo_;
        tool_context.max_read_bytes =
            std::max<std::size_t>(2048, static_cast<std::size_t>(profile_.context_size) / 4);
        tool_context.confirm = confirm_;
        // Copy bounded task-local metadata while locked. No registry-global
        // state or pointer into mutable witnesses escapes this invocation.
        tool_context.whole_file_write_check =
            [observations = source_witnesses_](const std::filesystem::path& authorized_path) {
                return observations.check_whole_file_write(authorized_path);
            };
    }
    tool_context.cancel = cancel;
    tool_context.on_output = [this](std::string_view chunk, bool is_stderr) {
        AgentEvent event;
        event.type = AgentEventType::ToolOutput;
        event.text = std::string{chunk};
        event.is_stderr = is_stderr;
        publish(std::move(event));
    };
    tool_context.on_progress = [this](std::string_view note) {
        emit_simple(AgentEventType::ToolProgress, std::string{note});
    };
    tool_context.acceptance_criteria = [this] {
        std::vector<CriterionSummary> summary;
        const auto& plan = ledger_.acceptance_plan();
        for (std::size_t i = 0; i < plan.size(); ++i) {
            summary.push_back({static_cast<int>(i) + 1, plan[i].description, plan[i].settled});
        }
        return summary;
    };

    if (!tools) {
        return ChatMessage::tool_result(call.id, call.name,
                                        "ERROR: no tool registry is available in this session.");
    }

    auto arguments = call.parsed_arguments();
    if (!arguments) {
        // Malformed JSON from the model: hand back the parse error verbatim so
        // it can fix the call rather than guessing what went wrong.
        auto text = "ERROR: the arguments for '" + call.name + "' were not valid JSON.\n" +
                    arguments.error().to_string() +
                    "\nSend the arguments as a single well-formed JSON object.";
        ToolResult failure = ToolResult::failure("invalid arguments for " + call.name, text);
        AgentEvent event;
        event.type = AgentEventType::ToolCompleted;
        event.tool_call = call;
        event.tool_result = failure;
        publish(std::move(event));
        return ChatMessage::tool_result(call.id, call.name, text);
    }

    {
        AgentEvent event;
        event.type = AgentEventType::ToolCallStarted;
        event.tool_call = call;
        publish(std::move(event));
    }

    ToolInvocation invocation;
    invocation.tool_name = call.name;
    invocation.arguments = *arguments;
    invocation.call_id = call.id;

    const auto tool_started = std::chrono::steady_clock::now();
    auto result = tools->invoke(invocation, tool_context);
    if (RequestTrace::enabled()) {
        RequestTrace::record_event(
            "tool_result", {{"name", call.name},
                            {"call_id", call.id},
                            {"ok", result.ok},
                            {"result_bytes", result.content.size() + result.error.size()},
                            {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                               std::chrono::steady_clock::now() - tool_started)
                                               .count()}});
    }
    const auto verification_started = std::chrono::steady_clock::now();
    std::string navigation_guidance;
    ToolInterpretation interpretation;

    // Fold what happened into the task state, which is what the next request
    // will actually be built from.
    {
        std::lock_guard lock(mutex_);
        for (const auto& evidence : result.evidence) {
            task_.record_evidence(evidence);
            if (evidence.kind == EvidenceKind::FileWritten && evidence.positive) {
                task_.record_file(evidence.subject, "modified");
                source_witnesses_.invalidate(evidence.subject);
                changed_files = true;
            }
        }
        // What this tool result actually proves - decided in one place, and
        // never by reading exit codes at the point of use.
        const auto settled_before =
            std::count_if(ledger_.acceptance_plan().begin(), ledger_.acceptance_plan().end(),
                          [](const AcceptanceCheck& check) { return check.settled; });
        interpret_tool_result(call, result, ledger_, task_.objective(), &interpretation);
        const auto source = source_witnesses_.observe(result, call.name, call.id);
        if (call.name == "read_file" || call.name == "read_file_range")
            source_novelty = source.new_evidence;
        navigation_guidance = source.guidance;
        settled_criterion =
            std::count_if(ledger_.acceptance_plan().begin(), ledger_.acceptance_plan().end(),
                          [](const AcceptanceCheck& check) { return check.settled; }) >
            settled_before;
        if ((call.name == "run_command" || call.name == "run_artifact") &&
            result.structured.is_object()) {
            CommandRecord record;
            record.command = json_string(result.structured, "command");
            record.exit_code = static_cast<int>(json_int(result.structured, "exit_code"));
            record.succeeded = record.exit_code == 0;
            record.duration_ms = json_int(result.structured, "duration_ms");
            record.at_unix_ms = now_unix_ms();
            task_.record_command(record);
        }
    }
    progress_result.succeeded = result.ok;
    if (call.name == "run_command" || call.name == "run_artifact") {
        const auto& details = result.structured;
        const bool completed_process =
            result.ok && details.is_object() && details.contains("exit_code") &&
            details["exit_code"].is_number_integer() && !json_bool(details, "timed_out") &&
            !json_bool(details, "cancelled");
        progress_result.succeeded =
            completed_process && !interpretation.no_tests_found &&
            (json_bool(details, "expectation_checked") ? json_bool(details, "expectation_satisfied")
                                                       : json_int(details, "exit_code") == 0);
        if (!progress_result.succeeded) {
            const auto tail = [](std::string_view text, std::size_t limit) {
                auto start = text.size() > limit ? text.size() - limit : 0;
                while (start < text.size() &&
                       (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                    ++start;
                return std::string{text.substr(start)};
            };
            const auto error = tail(result.error, 512);
            const auto stderr_tail = details.is_object()
                                         ? tail(json_string(details, "progress_stderr_tail"), 512)
                                         : std::string{};
            const auto stdout_tail = details.is_object()
                                         ? tail(json_string(details, "progress_stdout_tail"), 512)
                                         : std::string{};
            Json failures = Json::array();
            if (details.is_object() && details.contains("expectation_failures") &&
                details["expectation_failures"].is_array()) {
                for (const auto& failure : details["expectation_failures"]) {
                    if (failure.is_string())
                        failures.push_back(strings::utf8_truncate(failure.get<std::string>(), 160));
                    if (failures.size() == 4) break;
                }
            }
            Json identity{{"invocation_ok", result.ok},
                          {"error", error},
                          {"stderr_tail", stderr_tail},
                          {"stdout_tail", stdout_tail},
                          {"expectation_failures", failures},
                          {"no_tests_found", interpretation.no_tests_found}};
            if (details.is_object()) {
                for (const auto* key : {"exit_code", "timed_out", "cancelled"})
                    if (details.contains(key)) identity[key] = details[key];
            }
            if (interpretation.no_tests_found) {
                // This typed outcome is already established by the interpreter.
                // Filter names and CTest timing/output do not change its identity.
                identity = {{"category", "no_tests_found"},
                            {"exit_code", json_int(details, "exit_code")},
                            {"timed_out", json_bool(details, "timed_out")},
                            {"cancelled", json_bool(details, "cancelled")}};
            } else if (error.empty() && stderr_tail.empty() && stdout_tail.empty() &&
                       failures.empty()) {
                // A silent exit is not enough to equate different commands.
                // This is a bounded equality hint, not a security hash.
                identity["command_hint"] = std::hash<std::string>{}(
                    details.is_object() ? json_string(details, "command") : call.arguments_json);
            }
            progress_result.failure_identity = identity.dump();
            progress_result.summary =
                !result.ok ? "Invocation failed: " + error
                : interpretation.no_tests_found
                    ? "The test command found no tests."
                    : "Command exit " + std::to_string(json_int(details, "exit_code"));
            if (details.is_object() && json_bool(details, "timed_out"))
                progress_result.summary += " (timed out)";
            if (details.is_object() && json_bool(details, "cancelled"))
                progress_result.summary += " (cancelled)";
            if (!failures.empty()) progress_result.summary += " " + failures.dump();
            if (!stderr_tail.empty())
                progress_result.summary += "\nstderr: " + stderr_tail;
            else if (!stdout_tail.empty())
                progress_result.summary += "\nstdout: " + stdout_tail;
            progress_result.summary = strings::utf8_truncate(progress_result.summary, 900);
        }
    }
    if (is_mutating_tool(call.name) && result.ok) changed_files = true;
    if (!navigation_guidance.empty()) {
        result.content += "\n\nNavigation feedback: " + navigation_guidance;
        emit_simple(AgentEventType::ProgressWarning, navigation_guidance);
    }
    if (RequestTrace::enabled()) {
        Json fields{{"name", call.name},
                    {"call_id", call.id},
                    {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - verification_started)
                                       .count()}};
        if (source_novelty) {
            fields["source_read_receipt"] =
                result.structured.is_object() && json_bool(result.structured, "source_read");
            fields["source_version_stable"] = result.structured.is_object() &&
                                              json_bool(result.structured, "source_version_stable");
            fields["source_new_evidence"] = *source_novelty;
        }
        RequestTrace::record_event("tool_verification", fields);
    }

    {
        AgentEvent event;
        event.type = AgentEventType::ToolCompleted;
        event.tool_call = call;
        event.tool_result = result;
        publish(std::move(event));
    }

    auto text = result.to_model_text();

    // Running the program without saying what its output should be is the
    // single step that separates Run 9 from a correct run, and it is invisible
    // at the moment it happens - the command succeeds, the numbers scroll past,
    // and the mistake only surfaces at the end when nothing can be confirmed.
    //
    // Observed in Run 10: a model that had written three good acceptance
    // criteria ran the program four times without once stating an expectation,
    // then ran out of steps with all three still open. So the reminder goes
    // here, next to the output it is about, rather than in a system prompt read
    // twenty turns ago.
    if ((call.name == "run_command" || call.name == "run_artifact") &&
        result.structured.is_object() && !json_bool(result.structured, "expectation_checked") &&
        !json_bool(result.structured, "cancelled") && !json_bool(result.structured, "timed_out")) {
        const auto command = json_string(result.structured, "command");
        if (classify_intent(command) == CommandIntent::Execution &&
            json_int(result.structured, "exit_code") == 0) {
            text +=
                "\n\nCommand verification: exit 0 records execution, not correctness; its "
                "purpose is not classified as a build or test. If this command checks an "
                "acceptance criterion, supply the expected exit/output in 'expect' and its "
                "matching 'expect.criterion'. LocalCode checks the actual result; leave "
                "unrelated criteria open.";
        }
    }

    return ChatMessage::tool_result(call.id, call.name, text);
}

std::string Agent::apply_evidence_check(const std::string& message,
                                        std::vector<std::string>& problems) const {
    std::lock_guard lock(mutex_);
    problems = task_.unsupported_claims(message);
    const auto citations = source_witnesses_.citation_problems(message);
    problems.insert(problems.end(), citations.begin(), citations.end());
    if (claims_verified_objective(message) && !ledger_.holds(Stage::Objective)) {
        problems.emplace_back(
            "the answer says VERIFIED, but the current evidence does not "
            "establish the objective");
    }
    if (problems.empty()) return message;

    // The model asserted something the record does not support. Rather than
    // suppressing the answer, the discrepancy is stated openly underneath it.
    std::string amended = message;
    amended += "\n\n---\n**LocalCode could not confirm part of this answer.**\n";
    for (const auto& problem : problems)
        amended += "- " + problem + "\n";
    amended += "\n" + task_.verification_summary() + "\n";
    return amended;
}

AgentRunSummary Agent::run(const std::string& user_message, CancellationToken cancel) {
    const auto started = std::chrono::steady_clock::now();
    AgentRunSummary summary;

    if (running_.exchange(true)) {
        summary.stop_reason = "An agent task is already running. Stop it or wait for it to finish.";
        return summary;
    }

    struct RunningGuard {
        std::atomic<bool>& flag;
        std::mutex& mutex;
        std::shared_ptr<const TaskPolicy>& policy;

        ~RunningGuard() {
            std::shared_ptr<const TaskPolicy> finished;
            {
                std::lock_guard lock(mutex);
                finished = std::move(policy);
            }
            finished.reset();
            flag.store(false);
        }
    } guard{running_, mutex_, task_policy_};

    // Off unless LOCALCODE_TRACE_DIR names somewhere to write. When on, every
    // model interaction in this run is recorded well enough to replay it.
    RequestTrace::begin_run("agent");
    RequestTrace::record_event("task_submitted", Json::object());

    struct TraceScope {
        ~TraceScope() { RequestTrace::end_run(); }
    } trace_scope;

    BackendPtr backend;
    ToolRegistry* tools = nullptr;
    Workspace* workspace = nullptr;
    ModelProfile profile;
    AgentConfig config;
    const auto requested_scope = TaskPolicy::requested_tool_scope(user_message);
    {
        std::lock_guard lock(mutex_);
        backend = backend_;
        tools = tools_;
        workspace = workspace_;
        profile = profile_;
        config = config_;
        const bool read_only = (tools && tools->read_only()) ||
                               requested_scope == TaskPolicy::ToolScope::ReadOnlyFileTools;
        const bool file_tools_only = requested_scope != TaskPolicy::ToolScope::AllTools;
        task_policy_ =
            workspace ? std::make_shared<const TaskPolicy>(
                            workspace->paths(), workspace->commands(), read_only, file_tools_only)
                      : std::make_shared<const TaskPolicy>(PathPolicy{}, CommandPolicy{}, read_only,
                                                           file_tools_only);
    }

    if (!backend) {
        summary.stop_reason =
            "No model is loaded. Select a model and wait for it to finish "
            "loading before sending a request.";
        emit_simple(AgentEventType::RunFailed, summary.stop_reason);
        return summary;
    }

    RunDeadline run_deadline(cancel, started + config.wall_clock_budget);
    cancel = run_deadline.token();

    if (use_conversation_path(user_message, config.request_mode))
        return run_conversation(user_message, backend, profile, config, cancel, started);

    // --- Set up the task -------------------------------------------------
    {
        std::lock_guard lock(mutex_);
        if (task_.objective().empty()) {
            ledger_.reset();
        } else {
            // A follow-up message is a new request against the same tree. What
            // was proved about the previous one is not carried over as proof
            // about this one, so the plan is cleared while the stages keep
            // their revisions. Task bookkeeping below belongs to this request;
            // prior edits/builds cannot count as action in the new request.
            ledger_.clear_acceptance_plan();
        }
        task_.begin(strings::utf8_truncate(strings::trim(user_message), 500));
        source_witnesses_.reset(workspace ? workspace->root() : std::filesystem::path{});
        history_.push_back(ChatMessage::user(user_message));
        progress_.reset();
    }

    // Record which files were already dirty, so the agent's own changes can be
    // told apart from the user's uncommitted work.
    if (workspace && workspace->is_open()) {
        if (repo_ && repo_->is_open() && !task_policy_->file_tools_only()) {
            const auto refresh_started = std::chrono::steady_clock::now();
            const auto refreshed = repo_->refresh(workspace->ignore_rules(), cancel);
            if (RequestTrace::enabled()) {
                RequestTrace::record_event(
                    "repository_refresh",
                    {{"ok", refreshed.ok()},
                     {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - refresh_started)
                                        .count()}});
            }
            if (!refreshed)
                log_debug(LogChannel::Agent, "repository refresh unavailable",
                          {{"reason", refreshed.error().message}});
        }
        const auto snapshot_started = std::chrono::steady_clock::now();
        auto snapshot = workspace->snapshot_baseline(cancel, !task_policy_->file_tools_only());
        if (RequestTrace::enabled()) {
            RequestTrace::record_event(
                "workspace_snapshot",
                {{"ok", snapshot.ok()},
                 {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - snapshot_started)
                                    .count()}});
        }
        if (!snapshot) {
            log_debug(LogChannel::Agent, "baseline snapshot unavailable",
                      {{"reason", snapshot.error().message}});
        }
    }

    emit_simple(AgentEventType::RunStarted, user_message);

    const bool text_protocol = profile.tool_protocol == ToolProtocol::TextFallback;
    const bool native_tools = profile.tool_protocol == ToolProtocol::Native;

    // Twenty-three full schemas cost roughly 4,800 tokens. That is most of an 8k
    // window before anything useful is in it, so the set is compacted when it
    // would otherwise crowd out the conversation.
    auto permitted_tools =
        tools ? tools->definitions(!task_policy_ || !task_policy_->read_only(), task_policy_.get())
              : std::vector<ToolDefinition>{};
    const auto tool_definitions =
        fit_tool_definitions(permitted_tools, static_cast<int>(profile.context_size));
    // Production runs do not retain a second, full schema set for an unused
    // prototype. They have the same compact definitions as before.
    if (!config.experimental_deferred_tools && !config.experimental_stable_tool_dispatch)
        permitted_tools.clear();
    std::optional<ToolCatalog> tool_catalog;

    // The system message is built once and never rebuilt.
    //
    // This is a performance decision, not a tidiness one. Every runtime keeps a
    // KV cache keyed on the longest matching prefix of the prompt; a system
    // message that changes between turns invalidates all of it. Measured against
    // an 8B model at 8k on this machine, re-selecting skills each turn moved the
    // divergence point to token ~2001 and forced a 4,400-token re-prefill on six
    // of ten turns, at roughly two minutes each. Pinning the block for the run
    // costs a little adaptivity and buys back most of the wall clock.
    const auto setup_started = std::chrono::steady_clock::now();
    auto system_prompt = build_system_prompt(tool_definitions, text_protocol);
    std::string pinned_skills;
    {
        std::string objective;
        {
            std::lock_guard lock(mutex_);
            objective = task_.objective();
        }
        // A skills block is guidance; guidance that crowds out the problem is
        // worse than none, so it is capped at an eighth of the window.
        const int skill_budget = std::max(400, static_cast<int>(profile.context_size) / 8);
        pinned_skills = SkillRegistry::render(pick_skills(objective), skill_budget);
    }
    if (RequestTrace::enabled()) {
        RequestTrace::record_event(
            "prompt_setup", {{"system_bytes", system_prompt.size()},
                             {"skill_bytes", pinned_skills.size()},
                             {"tool_count", tool_definitions.size()},
                             {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - setup_started)
                                                .count()}});
    }
    std::string initial_sources;
    std::vector<SourceAnchor> initial_source_anchors;
    int initial_sources_revision = 0;
    {
        std::lock_guard lock(mutex_);
        initial_sources_revision = ledger_.revision();
    }
    if (!task_policy_->file_tools_only() && repo_ && repo_->is_open() &&
        repo_->state() == repo::IndexState::Ready && workspace) {
        const auto retrieval_started = std::chrono::steady_clock::now();
        repo::RepoQuery query(repo_->store(), workspace->root());
        auto sources = query.task_sources(user_message, 6);
        if (sources && !sources->empty()) {
            initial_sources =
                "\n## Initial repository leads (lexical, not proof)\n"
                "Prefer read_file_range at these line numbers, then follow the relevant calls. "
                "These initial candidates must be confirmed against source; they are not build "
                "or test evidence.\n";
            for (const auto& source : *sources) {
                const auto signature = strings::utf8_truncate(
                    strings::replace_all(strings::replace_all(source.preview, "\n", " "), "\r",
                                         " "),
                    140);
                auto ranges = std::to_string(source.line);
                if (source.end_line > source.line) ranges += "-" + std::to_string(source.end_line);
                for (const auto& [line, end] : source.related_ranges)
                    ranges += ", " + std::to_string(line) + "-" + std::to_string(end);
                const auto entry =
                    source.file + ":" + ranges + " — " +
                    (signature.empty() ? source.reason : signature + " [" + source.reason + "]") +
                    "\n";
                if (initial_sources.size() + entry.size() > 1800) break;
                initial_sources += entry;
                initial_source_anchors.push_back({source.file, source.line, source.end_line});
                for (const auto& [line, end] : source.related_ranges)
                    initial_source_anchors.push_back({source.file, line, end});
            }
        }
        if (RequestTrace::enabled())
            RequestTrace::record_event(
                "initial_repository_leads",
                {{"bytes", initial_sources.size()},
                 {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - retrieval_started)
                                    .count()}});
    }

    const auto deadline = started + config.wall_clock_budget;
    FinalResponseBudget response_budget(config.wall_clock_budget, context_.context_size(),
                                        context_.reserved_output(), profile.sampling.max_tokens);
    auto run_elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    };
    int iteration = 0;
    // How many times in a row the model has been told to act instead of
    // describing. Counted consecutively and reset by any productive turn: a
    // model that reasons without answering at step 3, recovers, and does it
    // again at step 15 has had two isolated hiccups, not failed twice. Run 10
    // died on the global count with 14 steps and half its context unspent.
    int nudges = 0;
    constexpr int kMaxNudges = 2;
    // How many times the model has claimed to be finished and been sent back
    // to produce the missing evidence. Capped, because a model that cannot
    // verify its work will not learn to on the fifth attempt, and an honest
    // "unverified" is better than an endless loop.
    int verification_returns = 0;
    int citation_returns = 0;
    constexpr int kMaxVerificationReturns = 2;
    // How many times the model has produced a tool call the server could not
    // parse. Recoverable, and capped: a model that cannot write a short call
    // will not learn to on the fourth attempt.
    int malformed_calls = 0;
    constexpr int kMaxMalformedCalls = 2;
    // RunFailed is emitted at the point of failure; the tail must not repeat it.
    bool failure_reported = false;
    std::vector<std::string> unexecuted_actions;
    bool progress_reporting = false;
    bool progress_report_requested = false;
    const std::string progress_stop_reason =
        "stopped after repeated warnings: the same unproductive pattern kept repeating";

    while (iteration < config.max_iterations) {
        if (cancel.cancelled()) {
            if (run_deadline.expired()) {
                summary.stop_reason = "the time budget for this run was reached";
                break;
            }
            summary.cancelled = true;
            summary.stop_reason = "cancelled by the user";
            emit_simple(AgentEventType::RunCancelled, summary.stop_reason);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            summary.stop_reason = "the time budget for this run was reached";
            emit_simple(AgentEventType::RunFailed, summary.stop_reason);
            failure_reported = true;
            break;
        }
        // A progress stop closes actions, but should not discard the user's
        // report while time and iterations remain. Never turn this into an
        // additional recovery loop when the one report is unusable.
        if (progress_reporting && progress_report_requested) break;
        if (response_budget.final_attempted()) {
            summary.stop_reason = "the reserved final response attempt did not produce an answer";
            break;
        }

        ++iteration;
        // Reserve a response inside the configured budget. Discovery cannot
        // silently consume the last turn, and this phase cannot perform a late
        // edit whose verification would have no remaining turn.
        // Native completed discovery timings can move this cutoff earlier.
        // Forecast a cold report; cache reuse and future throughput are unknown.
        // The hard task deadline and existing final output cap stay unchanged.
        const bool time_for_response = config.wall_clock_budget >= std::chrono::seconds{30} &&
                                       !response_budget.actions_open(run_elapsed());
        const bool synthesis_turn =
            progress_reporting || time_for_response ||
            (config.max_iterations >= 3 && iteration == config.max_iterations);
        {
            std::lock_guard lock(mutex_);
            task_.note_iteration();
        }
        progress_.note_iteration();

        // --- Assemble the request ----------------------------------------
        const auto build_started = std::chrono::steady_clock::now();
        std::vector<ChatMessage> history;
        std::string task_block;
        std::string navigation_checkpoint;
        bool has_acceptance_plan = false;
        bool sources_current = false;
        std::vector<SourceAnchor> source_anchors;
        const bool index_current = repo_ && repo_->state() == repo::IndexState::Ready;
        {
            std::lock_guard lock(mutex_);
            history = history_;
            task_block = task_.to_prompt_block(false) + "\n" + ledger_.render_states();
            const auto plan = ledger_.render_plan();
            if (!plan.empty()) task_block += "\n" + plan;
            has_acceptance_plan = !ledger_.acceptance_plan().empty();
            // Retrieval is bounded and performed once. Keep its navigation
            // candidates across discovery calls, but discard their old ranges
            // as soon as source changes or the index is invalidated.
            sources_current = index_current && ledger_.revision() == initial_sources_revision;
            task_block += source_witnesses_.render();
            navigation_checkpoint = source_witnesses_.navigation_checkpoint();
            source_anchors = source_witnesses_.anchors();
            if (sources_current)
                source_anchors.insert(source_anchors.end(), initial_source_anchors.begin(),
                                      initial_source_anchors.end());
        }
        task_block += "\nTurn " + std::to_string(iteration) + "/" +
                      std::to_string(config.max_iterations) + "; " +
                      std::to_string(config.max_iterations - iteration) + " left. ";
        const auto seconds_left =
            std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                                          deadline - std::chrono::steady_clock::now())
                                          .count());
        auto action_deadline = started + response_budget.action_cutoff();
        const auto action_seconds_left =
            synthesis_turn
                ? 0
                : std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                                                action_deadline - std::chrono::steady_clock::now())
                                                .count());
        task_block += "Actions: " + std::to_string(action_seconds_left) +
                      "s; Deadline: " + std::to_string(seconds_left) + "s.\n";
        if (time_for_response)
            task_block +=
                "The time reserve has been reached; use this turn for the final response.\n";
        if (synthesis_turn)
            task_block +=
                "FINAL RESPONSE TURN: tools are unavailable. Answer the user's request now "
                "from inspected evidence. Cover each requested part concisely; prefer claims "
                "with source citations over long copied source blocks. Cite only source lines "
                "actually read in this task. "
                "State any unanswered part or missing verification explicitly; do not claim "
                "unperformed work is complete.\n";
        else if (config.max_iterations - iteration <= 3)
            task_block +=
                "Budget is nearly spent. Stop broad discovery; close the objective's specific "
                "gaps and verify any edits before the final response.\n";

        // Once a plan exists, an explicit check must identify the criterion
        // it proves. Keep ordinary reads and commands available without an
        // expectation; those are discovery, not acceptance evidence.
        auto prepare_tools = [&] {
            if (synthesis_turn) return std::vector<ToolDefinition>{};
            auto definitions =
                tool_catalog ? (tool_catalog->stable_dispatch() ? tool_catalog->loaded_definitions()
                                                                : tool_catalog->definitions())
                             : tool_definitions;
            if (has_acceptance_plan) {
                for (auto& tool : definitions) {
                    if (tool.name != "read_file" && tool.name != "run_command" &&
                        tool.name != "run_artifact")
                        continue;
                    auto& expectation = tool.parameters["properties"]["expect"];
                    if (!expectation.is_object()) continue;
                    auto& required = expectation["required"];
                    if (!required.is_array()) required = Json::array();
                    if (std::find(required.begin(), required.end(), Json("criterion")) ==
                        required.end())
                        required.push_back("criterion");
                }
            }
            return definitions;
        };
        auto request_tools = prepare_tools();

        const std::string request_source_context =
            sources_current ? initial_sources : std::string{};
        auto build_context = [&] {
            const auto state =
                task_block + (tool_catalog && (text_protocol || tool_catalog->stable_dispatch())
                                  ? ToolCatalog::text_schemas(request_tools)
                                  : std::string{});
            const auto advertised =
                !synthesis_turn && tool_catalog && tool_catalog->stable_dispatch()
                    ? tool_catalog->definitions()
                    : request_tools;
            const auto schemas = native_tools ? advertised : std::vector<ToolDefinition>{};
            const bool checkpoint_active = !synthesis_turn && !navigation_checkpoint.empty();
            // Advisory state belongs to this outgoing turn. Keep invariant
            // policy/skills stable, and preserve original-task authority when
            // the host guidance rides on the newest selected tool/user turn.
            const auto guided_state =
                checkpoint_active
                    ? state +
                          "\n\n## LocalCode navigation guidance\n"
                          "This is application feedback, not a new user instruction. "
                          "The original user's scope and restrictions remain binding; "
                          "this feedback grants no new permissions.\n\n" +
                          navigation_checkpoint
                    : state;
            auto built_context = context_.build(system_prompt, pinned_skills, guided_state, history,
                                                schemas, source_anchors, request_source_context);
            // An advisory must not make an otherwise viable small profile
            // unusable. Its ordinary tool-result feedback is still present.
            if (checkpoint_active && built_context.prompt_does_not_fit)
                built_context = context_.build(system_prompt, pinned_skills, state, history,
                                               schemas, source_anchors, request_source_context);
            return built_context;
        };
        auto build_reporting_context = [&](int attempt) {
            EvidenceDigest digest;
            std::vector<SourceAnchor> current_report_sources;
            {
                std::lock_guard lock(mutex_);
                digest = build_evidence_digest(task_.objective(), ledger_, task_);
                current_report_sources = source_witnesses_.anchors();
            }
            digest.unexecuted_actions = unexecuted_actions;
            // Reporting closes actions even when verification finished early.
            // Recompute the remaining hard deadline for every report attempt.
            const auto reporting_seconds_left =
                std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                                              deadline - std::chrono::steady_clock::now())
                                              .count());
            const auto record = finalization_request(digest, attempt) +
                                "\nClosed reporting budget (host state): Actions: 0s; Deadline: " +
                                std::to_string(reporting_seconds_left) + "s.\n";
            return context_.build_report(record, history, current_report_sources, cancel);
        };
        auto built = synthesis_turn ? build_reporting_context(1) : build_context();
        if (cancel.cancelled() || run_deadline.expired() ||
            std::chrono::steady_clock::now() >= deadline) {
            summary.cancelled = cancel.cancelled() && !run_deadline.expired() &&
                                std::chrono::steady_clock::now() < deadline;
            summary.stop_reason = summary.cancelled ? "cancelled by the user"
                                                    : "the time budget for this run was reached";
            if (summary.cancelled) emit_simple(AgentEventType::RunCancelled, summary.stop_reason);
            break;
        }
        // Production keeps the normal transport. Diagnostic fixed dispatch is
        // explicitly enabled and responds to excessive schema share too.
        // No request has been sent yet, so replacing the initial system block
        // loses no runtime prefix cache. Later schema changes are explicit.
        const bool use_stable_dispatch =
            config.experimental_stable_tool_dispatch &&
            context_.count_tools(tool_definitions) >
                std::max(600, static_cast<int>(profile.context_size) / 5);
        if (!synthesis_turn && iteration == 1 && !permitted_tools.empty() &&
            ((config.experimental_deferred_tools && built.prompt_does_not_fit) ||
             use_stable_dispatch) &&
            (native_tools || text_protocol) && tools && !tools->find("select_tools") &&
            (!use_stable_dispatch || !tools->find("call_tool"))) {
            tool_catalog.emplace(permitted_tools, use_stable_dispatch);
            system_prompt = build_system_prompt(tool_catalog->definitions(), text_protocol) +
                            ToolCatalog::instructions();
            if (use_stable_dispatch)
                system_prompt +=
                    "Call loaded tools through call_tool: name is the tool name, "
                    "arguments is its canonical JSON object. Selection runs nothing.\n";
            request_tools = prepare_tools();
            built = build_context();
            if (RequestTrace::enabled())
                RequestTrace::record_event("tool_catalog",
                                           {{"permitted_count", permitted_tools.size()},
                                            {"initial_tool_count", request_tools.size()},
                                            {"prompt_fits", !built.prompt_does_not_fit}});
        }
        const double prompt_build_ms = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - build_started)
                                           .count();
        if (RequestTrace::enabled()) {
            RequestTrace::record_event(
                "prompt_built", {{"iteration", iteration},
                                 {"messages", built.messages.size()},
                                 {"budget_tokens", built.budget.used},
                                 {"retained_source_excerpts", built.retained_source_excerpts},
                                 {"elapsed_ms", prompt_build_ms}});
        }
        {
            std::lock_guard lock(mutex_);
            last_context_ = built.budget;
        }
        {
            AgentEvent event;
            event.type = AgentEventType::ContextUpdated;
            event.context = built.budget;
            event.text = built.note;
            event.iteration = iteration;
            publish(std::move(event));
        }
        if (built.prompt_does_not_fit) {
            summary.stop_reason = built.note;
            emit_simple(AgentEventType::RunFailed, summary.stop_reason);
            failure_reported = true;
            break;
        }

        // --- Call the model ----------------------------------------------
        ChatRequest request;
        request.messages = std::move(built.messages);
        if (native_tools && !synthesis_turn)
            request.tools = tool_catalog && tool_catalog->stable_dispatch()
                                ? tool_catalog->definitions()
                                : std::move(request_tools);
        request.sampling = profile.sampling;
        if (malformed_calls > 0) request.parallel_tool_calls = false;
        request.stream = config.stream;
        // Reasoning switches are only sent when the model genuinely has them.
        request.enable_reasoning =
            profile.capabilities.supports_native_reasoning && profile.reasoning_enabled;
        if (profile.capabilities.supports_native_reasoning) {
            request.thinking_override = profile.reasoning_enabled;
        }
        request.native_reasoning_effort = profile.native_reasoning_effort;
        if (synthesis_turn) {
            // The shared closed frame already replaces discovery policy,
            // skills and action-shaped history. This is the same reserved call,
            // with the original hard deadline and output cap, not another try.
            request.enable_reasoning = false;
            if (profile.capabilities.supports_native_reasoning) request.thinking_override = false;
            if (request.sampling.max_tokens <= 0 || request.sampling.max_tokens > 2048)
                request.sampling.max_tokens = 2048;
        }

        emit_simple(AgentEventType::PromptProcessing, "");

        // --- Finalization -------------------------------------------------
        //
        // Reached when the model has stopped producing usable replies. If the
        // objective is established and the criteria are settled, the engineering
        // is done and only the summary is missing, so the run is not over: it
        // moves to a phase whose whole job is to describe what the ledger
        // already holds.
        //
        // Deliberately narrow. No tools, no history, no room to start work
        // again - a model handed the full conversation at this point tends to
        // resume the task rather than describe it. Thinking is switched off per
        // request where the template honours it, because a model that thinks
        // instead of answering has already cost the user the answer once.
        std::string finalization_context_failure;
        auto attempt_finalization = [&](std::vector<std::string>& claims) -> std::string {
            if (progress_reporting || synthesis_turn) return {};
            EvidenceDigest digest;
            {
                std::lock_guard lock(mutex_);
                digest = build_evidence_digest(task_.objective(), ledger_, task_);
            }
            if (!digest.has_anything()) return {};

            std::string best;
            const int attempts_available =
                std::min(kMaxFinalizationAttempts, config.max_iterations - iteration);
            for (int attempt = 1; attempt <= attempts_available; ++attempt) {
                if (cancel.cancelled()) return {};
                auto reporting = build_reporting_context(attempt);
                if (reporting.prompt_does_not_fit) {
                    finalization_context_failure = reporting.note;
                    return {};
                }
                if (cancel.cancelled()) return {};
                ++iteration;
                ++summary.finalization_attempts;
                {
                    std::lock_guard lock(mutex_);
                    last_context_ = reporting.budget;
                }

                ChatRequest fr;
                fr.messages = std::move(reporting.messages);
                // No tools: this phase may not act, only report.
                fr.sampling = profile.sampling;
                fr.stream = false;
                fr.enable_reasoning = false;
                fr.thinking_override = false;
                // A summary does not need the whole reply budget, and a smaller
                // one bounds how long a thinking model can stall before the
                // attempt is spent.
                if (fr.sampling.max_tokens <= 0 || fr.sampling.max_tokens > 1024) {
                    fr.sampling.max_tokens = 1024;
                }

                emit_simple(AgentEventType::FinalizationStarted,
                            attempt == 1 ? "The work is verified; asking for the final answer."
                                         : "Still no answer; asking once more, strictly.");

                StreamHandlers quiet;
                const auto finalize_sent_at = std::chrono::steady_clock::now();
                auto reply = backend->chat(fr, quiet, cancel);
                if (RequestTrace::enabled()) {
                    TraceEntry entry;
                    entry.phase = attempt == 1 ? "finalization" : "finalization-retry";
                    entry.model = profile.id;
                    entry.request = fr;
                    entry.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - finalize_sent_at)
                                            .count();
                    if (reply) {
                        entry.content = reply->message.content;
                        entry.reasoning = reply->message.reasoning_content;
                        entry.finish_reason = reply->finish_reason;
                        entry.prompt_tokens = reply->usage.prompt_tokens;
                        entry.completion_tokens = reply->usage.completion_tokens;
                        entry.usage_from_server = reply->usage.from_server;
                    } else {
                        entry.error = reply.error().to_string();
                    }
                    RequestTrace::record(entry);
                }
                if (cancel.cancelled() || (reply && reply->finish_reason == "cancelled")) {
                    summary.cancelled = !run_deadline.expired();
                    summary.stop_reason = summary.cancelled
                                              ? "cancelled by the user"
                                              : "the time budget for this run was reached";
                    return {};
                }
                if (!reply) {
                    log_warn(LogChannel::Agent, "finalization request failed",
                             {{"attempt", std::to_string(attempt)},
                              {"error", reply.error().to_string()}});
                    continue;
                }

                const std::string content = strings::collapse_blank_lines(reply->message.content);
                if (!is_usable_final_answer(content) ||
                    ToolCallParser::contains_unquoted_tool_intent(
                        content, tools ? tools->names() : std::vector<std::string>{})) {
                    summary.presentation = PresentationState::ReasoningOnly;
                    log_info(LogChannel::Agent, "finalization produced no answer",
                             {{"attempt", std::to_string(attempt)},
                              {"reasoning_characters",
                               std::to_string(reply->message.reasoning_content.size())}});
                    continue;
                }

                // Presentation is not an exemption from verification. The
                // summary goes through the same claim check as any other final
                // answer, and a summary that claims more than the ledger holds
                // spends the remaining attempt rather than being accepted.
                std::vector<std::string> found;
                const std::string checked = apply_evidence_check(content, found);
                if (found.empty()) {
                    claims.clear();
                    return checked;
                }
                log_warn(LogChannel::Agent, "finalization claimed more than the evidence",
                         {{"attempt", std::to_string(attempt)},
                          {"claims", std::to_string(found.size())}});
                best = checked;
                claims = found;
            }
            return best;
        };

        // Timing for this turn. The clock starts before the request goes out, so
        // prompt processing - which on a long context is most of the wait - is
        // inside the measurement rather than hidden before it.
        const auto turn_started = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point first_token_at{};
        std::chrono::steady_clock::time_point first_reasoning_at{};
        std::chrono::steady_clock::time_point last_reasoning_at{};
        int reasoning_characters = 0;
        int reasoning_delta_count = 0;

        // Both ends must have been stamped. A turn that returns only tool calls
        // fires neither the content nor the reasoning handler, so the first-token
        // mark is never set - and subtracting a zero time point from `now`
        // reported a time to first token of minus five thousand seconds.
        auto elapsed_ms = [](std::chrono::steady_clock::time_point from,
                             std::chrono::steady_clock::time_point to) -> std::int64_t {
            if (from.time_since_epoch().count() == 0) return 0;
            if (to.time_since_epoch().count() == 0) return 0;
            const auto span = std::chrono::duration_cast<std::chrono::milliseconds>(to - from);
            return span.count() < 0 ? 0 : span.count();
        };

        StreamHandlers handlers;
        handlers.on_first_token = [&, this] {
            if (first_token_at.time_since_epoch().count() == 0) {
                first_token_at = std::chrono::steady_clock::now();
            }
            emit_simple(AgentEventType::ContentDelta, "");
        };
        handlers.on_content = [&, this](std::string_view delta) {
            emit_simple(AgentEventType::ContentDelta, std::string{delta});
        };
        // The handler is installed whenever the user wants reasoning shown, not
        // only when the model file claims to have it.
        //
        // Capability detection reads the chat template, and the template is not
        // the whole story: llama.cpp knows model families its templates do not
        // mark, and splits their thinking out anyway. Ministral-3B-Reasoning is
        // detected as having no native reasoning and its every reply arrives
        // with a populated reasoning_content - so LocalCode threw that text
        // away, showed no thinking, reported zero reasoning tokens, and called a
        // reply that was entirely reasoning an "empty answer".
        //
        // Installing the handler is not pretending the model reasons: it fires
        // only if reasoning actually arrives on the wire, which is evidence.
        if (profile.show_reasoning) {
            handlers.on_reasoning = [&, this](std::string_view delta) {
                const auto now = std::chrono::steady_clock::now();
                if (first_reasoning_at.time_since_epoch().count() == 0) {
                    first_reasoning_at = now;
                }
                // Span between first and last reasoning delta, not first
                // reasoning to first content: the gap before content arrives is
                // the model starting its answer, not still thinking.
                last_reasoning_at = now;
                ++reasoning_delta_count;
                reasoning_characters += static_cast<int>(delta.size());
                emit_simple(AgentEventType::ReasoningDelta, std::string{delta});
            };
        }

        const auto request_sent_at = std::chrono::steady_clock::now();
        RequestTrace::record_event("model_call_started", {{"iteration", iteration}});
        std::optional<RunDeadline> action_phase;
        auto request_cancel = cancel;
        if (!synthesis_turn && config.wall_clock_budget >= std::chrono::seconds{30}) {
            action_phase.emplace(cancel, action_deadline);
            request_cancel = action_phase->token();
        }
        auto action_time_closed = [&] {
            return action_phase.has_value() && std::chrono::steady_clock::now() >= action_deadline;
        };
        auto close_action_phase = [&](const ChatMessage* rejected, std::size_t from,
                                      bool record_assistant) {
            if (rejected && !rejected->tool_calls.empty()) {
                std::lock_guard lock(mutex_);
                if (record_assistant) {
                    auto recorded = *rejected;
                    recorded.reasoning_content.clear();
                    history_.push_back(std::move(recorded));
                }
                for (std::size_t i = from; i < rejected->tool_calls.size(); ++i) {
                    const auto& call = rejected->tool_calls[i];
                    unexecuted_actions.push_back(call.name + " [call " + call.id +
                                                 "]: not executed because the action time closed.");
                    history_.push_back(
                        ChatMessage::tool_result(call.id, call.name,
                                                 "Not executed: the action time is closed; only "
                                                 "the final response remains."));
                }
            }
            const std::string guidance =
                "The action time has expired. No further tool calls will be executed. "
                "Produce the final response from existing evidence and state unfinished work "
                "honestly.";
            emit_simple(AgentEventType::ProgressWarning, guidance);
            push_guidance(guidance);
            RequestTrace::record_event("action_phase_closed", {{"iteration", iteration}});
        };
        if (progress_reporting) progress_report_requested = true;
        if (synthesis_turn && !response_budget.begin_final(run_elapsed())) {
            summary.stop_reason = "the reserved final response attempt did not produce an answer";
            break;
        }
        auto response = backend->chat(request, handlers, request_cancel);
        const auto response_received_at = std::chrono::steady_clock::now();
        if (!synthesis_turn && response) {
            FinalResponseBudget::Sample sample;
            sample.completed_success =
                !cancel.cancelled() && !run_deadline.expired() && response_received_at < deadline &&
                !request_cancel.cancelled() && !response->truncated_by_length &&
                (response->finish_reason == "stop" || response->finish_reason == "tool_calls");
            sample.timings_from_server = response->timings.from_server;
            sample.evaluated_prompt_tokens = response->timings.prompt_tokens_evaluated;
            sample.prompt_ms = response->timings.prompt_ms;
            sample.decoded_tokens = response->timings.decoded_tokens;
            sample.decode_ms = response->timings.decode_ms;
            // Build ends before this request starts, so it is added once only.
            sample.request_ms =
                std::chrono::duration<double, std::milli>(response_received_at - request_sent_at)
                    .count();
            sample.prompt_build_ms = prompt_build_ms;
            sample.usage_from_server = response->usage.from_server;
            sample.server_prompt_tokens = response->usage.prompt_tokens;
            sample.estimated_prompt_tokens = built.budget.used;
            const auto previous_action_deadline = action_deadline;
            response_budget.observe(sample, run_elapsed());
            action_deadline = started + response_budget.action_cutoff();
            if (action_phase && action_deadline < previous_action_deadline) {
                // The transport has returned. Retire its old waiter, then bind
                // the earlier timer to the token that pending tools will get.
                // A passed cutoff is also checked synchronously before dispatch.
                request_cancel = cancel;
                action_phase.reset();
                action_phase.emplace(cancel, action_deadline);
                request_cancel = action_phase->token();
            }
        }
        if (RequestTrace::enabled()) {
            TraceEntry entry;
            entry.phase = "agent";
            entry.model = profile.id;
            entry.request = request;
            entry.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - request_sent_at)
                                    .count();
            if (response) {
                entry.content = response->message.content;
                entry.reasoning = response->message.reasoning_content;
                entry.finish_reason = response->finish_reason;
                entry.tool_calls = static_cast<int>(response->message.tool_calls.size());
                entry.prompt_tokens = response->usage.prompt_tokens;
                entry.completion_tokens = response->usage.completion_tokens;
                entry.usage_from_server = response->usage.from_server;
            } else {
                entry.error = response.error().to_string();
            }
            RequestTrace::record(entry);
        }
        // Parent user/hard cancellation takes priority over a phase-only expiry.
        if (cancel.cancelled() || run_deadline.expired() ||
            std::chrono::steady_clock::now() >= deadline) {
            summary.cancelled = cancel.cancelled() && !run_deadline.expired() &&
                                std::chrono::steady_clock::now() < deadline;
            const bool has_partial = response && !strings::trim(response->message.content).empty();
            if (has_partial) {
                auto partial = response->message;
                partial.reasoning_content.clear();
                partial.tool_calls.clear();
                std::lock_guard lock(mutex_);
                history_.push_back(std::move(partial));
            }
            summary.stop_reason = !summary.cancelled ? "the time budget for this run was reached"
                                  : has_partial
                                      ? "cancelled by the user; the partial reply was kept"
                                      : "cancelled by the user";
            if (summary.cancelled) emit_simple(AgentEventType::RunCancelled, summary.stop_reason);
            break;
        }
        if (action_time_closed()) {
            const auto names = tools ? tools->names() : std::vector<std::string>{};
            const bool complete_prose =
                response && response->finish_reason == "stop" && !response->truncated_by_length &&
                response->message.tool_calls.empty() &&
                !strings::trim(response->message.content).empty() &&
                !ToolCallParser::contains_unquoted_tool_intent(response->message.content, names);
            // A fully returned answer may win the cutoff race. Interrupted output
            // and unexecuted actions cannot become a completed answer.
            // Known length-limited no-tools prose must reach the terminal
            // incomplete-answer guard below, not earn a new reserved request.
            // Serialized/native tool intent keeps its existing handling.
            const bool length_limited_prose =
                response && response->message.tool_calls.empty() &&
                !strings::trim(response->message.content).empty() &&
                (response->finish_reason == "length" || response->truncated_by_length) &&
                !ToolCallParser::contains_unquoted_tool_intent(response->message.content, names);
            if (!complete_prose && !length_limited_prose) {
                close_action_phase(response ? &response->message : nullptr, 0, true);
                continue;
            }
        }
        auto recover_tool_turn = [&](bool cut_off) {
            if (!synthesis_turn && malformed_calls < kMaxMalformedCalls) {
                ++malformed_calls;
                push_guidance(
                    "Your previous tool call could not be parsed or was cut off. The rejected "
                    "batch was discarded before execution; do not assume any of it ran.\n\n"
                    "Make the next one short: one complete tool call with a valid JSON object. "
                    "For a large new program, create a small working module first, then add "
                    "other modules or extend it with small apply_patch edits in later turns. "
                    "Do not put another tool call's markup inside an argument string.");
                emit_simple(AgentEventType::ProgressWarning,
                    cut_off ? "The tool call reached the reply limit. Discarded it before execution; "
                              "retrying with a smaller step."
                            : "A malformed tool call was discarded before execution; "
                              "retrying with one complete call.");
                return true;
            }
            summary.completed = false;
            summary.presentation = PresentationState::Failed;
            summary.stop_reason =
                "The model repeatedly returned incomplete or malformed tool calls. The rejected "
                "batches were not executed. Try a smaller first step or increase the model's "
                "reply-token budget in Models.";
            emit_simple(AgentEventType::RunFailed, summary.stop_reason);
            failure_reported = true;
            return false;
        };
        if (!response) {
            if (run_deadline.expired()) {
                summary.stop_reason = "the time budget for this run was reached";
            } else if (response.error().code == ErrorCode::Cancelled) {
                summary.cancelled = true;
                summary.stop_reason = "cancelled by the user";
                emit_simple(AgentEventType::RunCancelled, summary.stop_reason);
            } else if (response.error().code == ErrorCode::InvalidToolCall ||
                       strings::icontains(response.error().message, "could not parse")) {
                // The model filled its whole output budget with a runaway tool
                // call and the server refused to parse it. That is one bad turn,
                // not a broken server: nothing was corrupted, the history is
                // intact, and saying so gets a usable turn back.
                //
                // Measured: a run whose C++ build had already succeeded and
                // whose criteria were nearly settled died here, reported as
                // "the model backend failed".
                log_warn(LogChannel::Agent, "the model produced an unparseable tool call",
                         {{"attempt", std::to_string(malformed_calls)},
                          {"detail", response.error().detail}});
                if (recover_tool_turn(false)) continue;
            } else {
                summary.stop_reason = "the model backend failed: " + response.error().to_string();
                emit_simple(AgentEventType::RunFailed, summary.stop_reason);
                failure_reported = true;

                // The shape of the conversation we sent, because several chat
                // templates reject a sequence rather than a message and the
                // error says only that they did. Without this the only way to
                // find out is to guess.
                std::string shape;
                for (const auto& message : request.messages) {
                    if (!shape.empty()) shape += ",";
                    shape += to_string(message.role);
                    if (!message.tool_calls.empty()) {
                        shape += "(" + std::to_string(message.tool_calls.size()) + " calls)";
                    }
                    if (message.role != Role::System && message.content.empty() &&
                        message.tool_calls.empty()) {
                        shape += "(empty)";
                    }
                }
                log_error(LogChannel::Agent, "backend call failed",
                          {{"error", response.error().to_string()}, {"conversation", shape}});
            }
            break;
        }

        ChatMessage assistant = response->message;

        // A cancelled generation returns successfully, carrying whatever arrived
        // before the stop. That is deliberate - the partial turn is worth
        // keeping - but it must be reported as a cancellation and not as an
        // answer. Pressing Stop while the model was reasoning was reported as
        // "the model returned an empty answer", which blames the model for the
        // user's own decision.
        if (cancel.cancelled() || response->finish_reason == "cancelled") {
            if (!strings::trim(assistant.content).empty()) {
                std::lock_guard lock(mutex_);
                assistant.reasoning_content.clear();
                history_.push_back(assistant);
            }
            summary.cancelled = !run_deadline.expired();
            summary.stop_reason = strings::trim(assistant.content).empty()
                                      ? "cancelled by the user before the model produced any output"
                                      : "cancelled by the user; the partial reply was kept";
            if (summary.cancelled) emit_simple(AgentEventType::RunCancelled, summary.stop_reason);
            break;
        }

        // --- What the turn cost ------------------------------------------
        {
            const auto now = std::chrono::steady_clock::now();

            TurnStats stats;

            // Reported by the runtime.
            stats.usage_from_server = response->usage.from_server;
            stats.prompt_tokens = response->usage.prompt_tokens;
            stats.cached_prompt_tokens = response->usage.cached_prompt_tokens;
            stats.completion_tokens = response->usage.completion_tokens;

            stats.timings_from_server = response->timings.from_server;
            stats.prompt_tokens_evaluated = response->timings.prompt_tokens_evaluated;
            stats.prompt_ms = response->timings.prompt_ms;
            stats.prompt_tokens_per_second = response->timings.prompt_tokens_per_second;
            stats.decode_ms = response->timings.decode_ms;
            stats.decode_tokens_per_second = response->timings.decode_tokens_per_second;

            // Measured here.
            stats.total_request_ms =
                response->duration_ms > 0 ? response->duration_ms : elapsed_ms(turn_started, now);
            stats.time_to_first_token_ms = elapsed_ms(turn_started, first_token_at);

            // Observed, not reported. See the comment on TurnStats.
            stats.reasoning_characters = reasoning_characters;
            stats.reasoning_delta_count = reasoning_delta_count;
            stats.observed_reasoning_stream_ms = elapsed_ms(first_reasoning_at, last_reasoning_at);
            if (reasoning_characters > 0) {
                stats.estimated_reasoning_tokens = estimate_tokens(
                    assistant.reasoning_content.empty()
                        ? std::string(static_cast<std::size_t>(reasoning_characters), 'x')
                        : assistant.reasoning_content);
            }

            AgentEvent event;
            event.type = AgentEventType::GenerationStats;
            event.stats = stats;
            event.iteration = iteration;
            publish(event);
        }

        // A native model can emit serialized calls as ordinary content when
        // the final request has no tools. They were never executed and cannot
        // count as an answer. Quoted/fenced documentation examples stay prose;
        // unquoted action intent is rejected even when surrounded by prose.
        if (native_tools && assistant.tool_calls.empty()) {
            const auto known_tools = tools ? tools->names() : std::vector<std::string>{};
            if (ToolCallParser::contains_unquoted_tool_intent(assistant.content, known_tools)) {
                {
                    std::lock_guard lock(mutex_);
                    history_.push_back(assistant);
                }
                if (!synthesis_turn && malformed_calls < kMaxMalformedCalls) {
                    ++malformed_calls;
                    push_guidance(
                        "You returned tool-call markup as answer text. Nothing ran. "
                        "Use the advertised native tool interface for an action, or write the "
                        "answer itself from the available evidence. Put documented syntax "
                        "examples in ordinary code fences or inline backticks.");
                    continue;
                }
                summary.completed = false;
                summary.presentation = PresentationState::Failed;
                summary.final_message =
                    "The model returned tool calls instead of an answer. "
                    "Those calls were not executed.";
                summary.stop_reason =
                    "the model returned serialized tool calls instead of a final answer";
                break;
            }
        }

        // --- Text-protocol tool calls ------------------------------------
        if (text_protocol && assistant.tool_calls.empty()) {
            auto parsed = ToolCallParser::parse(assistant.content);
            if (!parsed.calls.empty()) {
                assistant.tool_calls = parsed.calls;
                assistant.content = parsed.remaining_text;
            } else if (!parsed.malformed.empty()) {
                // Tell the model exactly what was wrong rather than treating a
                // botched call as a final answer.
                assistant.content = parsed.remaining_text;
                {
                    std::lock_guard lock(mutex_);
                    history_.push_back(assistant);
                }
                push_guidance("That looked like a tool call but could not be parsed:\n" +
                              strings::utf8_truncate(parsed.malformed.front(), 400) +
                              "\nUse exactly this form:\n<tool_call>\n"
                              "{\"name\": \"tool_name\", \"arguments\": {}}\n</tool_call>");
                continue;
            }
        }

        // Never store or execute part of a cut-off/invalid native batch. A
        // later template render parses historical arguments before generation;
        // storing these would poison every retry with the same HTTP 500.
        if (!assistant.tool_calls.empty()) {
            const bool cut_off = response->truncated_by_length || response->finish_reason == "length";
            const bool malformed = std::any_of(assistant.tool_calls.begin(), assistant.tool_calls.end(),
                [](const ToolCall& call) { return !call.parsed_arguments(); });
            if (cut_off || malformed) {
                if (recover_tool_turn(cut_off)) continue;
                break;
            }
        }

        // An assistant turn with no content and no tool calls is not a turn.
        //
        // It happens when the model spends the whole reply reasoning: the
        // reasoning is stripped on the way in, leaving an empty message that
        // says nothing. Keeping it is worse than useless - Mistral's template
        // filters empty assistant messages out and then applies its alternation
        // check to what remains, so the user turn before it and the corrective
        // turn after it become adjacent and the whole request is refused with
        // HTTP 500. The role sequence looked legal; the template disagreed after
        // its own filtering.
        const bool assistant_says_nothing =
            strings::trim(assistant.content).empty() && assistant.tool_calls.empty();
        if (!assistant_says_nothing) {
            std::lock_guard lock(mutex_);
            // Tool-call reasoning belongs to the ongoing request. Qwen's native
            // template replays it between tool results so the next step retains
            // the model's plan; only completed-answer scratchpads are optional.
            if (!config.include_reasoning_in_history && assistant.tool_calls.empty()) {
                assistant.reasoning_content.clear();
            }
            history_.push_back(assistant);
        }
        {
            AgentEvent event;
            event.type = AgentEventType::AssistantMessage;
            event.message = assistant;
            event.iteration = iteration;
            publish(std::move(event));
        }

        // An observer can press Stop while this turn is being delivered,
        // after the post-chat cancellation check above. Preserve its partial
        // text, but do not finalize it or dispatch any of its tool calls.
        if (cancel.cancelled()) {
            std::lock_guard lock(mutex_);
            for (const auto& pending : assistant.tool_calls)
                history_.push_back(ChatMessage::tool_result(
                    pending.id, pending.name,
                    "Not executed: the user cancelled before tool dispatch."));
            summary.final_message = assistant.content;
            break;
        }

        // --- No tools requested: this is the final answer -----------------
        if (assistant.tool_calls.empty()) {
            if (!strings::trim(assistant.content).empty() &&
                (response->finish_reason == "length" || response->truncated_by_length)) {
                // Prose being present does not prove the report finished.
                // Keep effects, evidence and partial history; do not retry or
                // promote output stopped at its cap to a completed answer.
                summary.completed = false;
                summary.presentation = PresentationState::Failed;
                summary.final_message = assistant.content;
                summary.stop_reason = "the model reply reached its output limit before completion";
                emit_simple(AgentEventType::RunFailed, summary.stop_reason);
                failure_reported = true;
                break;
            }
            // An empty answer is not an answer. It happens when the model spends
            // its whole output budget reasoning and is cut off before writing
            // anything, and reporting it as "the model produced a final answer"
            // leaves the user staring at a blank turn wondering what went wrong.
            // Observed live: a 20-point essay request at 8k, six seconds, reply
            // stored with empty content and reported as success.
            if (strings::trim(assistant.content).empty()) {
                // Everything it produced went into the think block and nothing
                // came out as an answer or a tool call. That is a recoverable
                // turn, not a dead run: ask for the answer once. Measured with a
                // 3B model whose 102 decoded tokens were entirely reasoning.
                // assistant.reasoning_content has already been cleared on the
                // way into the history; the response still has it.
                if (!strings::trim(response->message.reasoning_content).empty() &&
                    nudges < kMaxNudges && !synthesis_turn) {
                    ++nudges;
                    push_guidance(
                        "You produced reasoning but no answer and no tool call, so nothing "
                        "reached me and nothing happened.\n\nEither call a tool to do the "
                        "next concrete step, or write the answer itself. Keep the thinking "
                        "short.");
                    log_info(LogChannel::Agent, "the reply was entirely reasoning",
                             {{"nudge", std::to_string(nudges)},
                              {"reasoning_characters",
                               std::to_string(response->message.reasoning_content.size())}});
                    emit_simple(AgentEventType::ProgressWarning,
                                "The model produced only reasoning; asking for an answer.");
                    continue;
                }

                // The engineering may well be finished. Measured on Qwen3.5-9B:
                // ten runs of ten reached a verified objective with a clean
                // ledger, and eight of them ended here, recorded as incomplete
                // although the programs had been written, built, run and checked.
                bool work_stands = false;
                {
                    std::lock_guard lock(mutex_);
                    work_stands = ledger_.holds(Stage::Objective) && ledger_.plan_is_complete() &&
                                  !ledger_.plan_has_failure();
                }

                if (work_stands) {
                    summary.presentation = PresentationState::Pending;
                    std::vector<std::string> claims;
                    const std::string finalized = attempt_finalization(claims);
                    if (summary.cancelled || cancel.cancelled()) {
                        // Cancellation can arrive in the AssistantMessage
                        // observer before the finalizer sends its first call.
                        // Classify that preflight cancellation here as well.
                        summary.cancelled = !run_deadline.expired();
                        summary.stop_reason = summary.cancelled
                                                  ? "cancelled by the user"
                                                  : "the time budget for this run was reached";
                        summary.completed = false;
                        summary.final_message.clear();
                        summary.presentation = PresentationState::Failed;
                        if (summary.cancelled)
                            emit_simple(AgentEventType::RunCancelled, summary.stop_reason);
                        break;
                    }
                    if (!finalized.empty()) {
                        summary.final_message = finalized;
                        summary.unsupported_claims = claims;
                        summary.presentation = PresentationState::Emitted;
                        summary.completed = claims.empty();
                        summary.stop_reason =
                            claims.empty() ? "the model produced a final answer when asked directly"
                                           : "the final answer requested directly still contains "
                                             "unsupported claims";
                        log_info(LogChannel::Agent, "finalization recovered the answer",
                                 {{"attempts", std::to_string(summary.finalization_attempts)},
                                  {"claims", std::to_string(claims.size())}});
                        break;
                    }

                    // Out of attempts. The work still stands: what failed is the
                    // description of it, and the two must not be reported as one.
                    summary.presentation = PresentationState::Failed;
                    summary.completed = false;
                    summary.final_message.clear();
                    summary.stop_reason = !finalization_context_failure.empty()
                                              ? finalization_context_failure
                                              : "the work was verified, but the model produced no "
                                                "final answer to show "
                                                "for it";
                    log_warn(LogChannel::Agent, "verified work with no final answer",
                             {{"attempts", std::to_string(summary.finalization_attempts)}});
                    emit_simple(AgentEventType::ProgressWarning, summary.stop_reason);
                    failure_reported = true;
                    break;
                }

                summary.completed = false;
                summary.final_message.clear();
                summary.presentation = PresentationState::Failed;
                summary.stop_reason =
                    response->truncated_by_length
                        ? "the model used its entire output budget on reasoning and produced no "
                          "answer - raise the reply budget or the context size for this model"
                        : "the model returned an empty answer";
                log_warn(LogChannel::Agent, "empty final answer",
                         {{"finish_reason", response->finish_reason},
                          {"reasoning_characters", std::to_string(reasoning_characters)}});
                emit_simple(AgentEventType::RunFailed, summary.stop_reason);
                failure_reported = true;
                break;
            }

            // --- Announced work is not performed work ---------------------
            //
            // A reply with no tool calls normally means the model has finished.
            // But a model that thinks out loud produces a plan, and when its
            // chat template does not separate reasoning that plan arrives as the
            // answer. Accepting it ends the run having done nothing: measured
            // with a 3B model on a build task - one iteration, zero tool calls,
            // 114 seconds of planning recorded as a completed task.
            //
            // So: if the model announced work, performed none all run, and the
            // tools to do it are available, say so and let it act. Twice at
            // most, then stop and report honestly rather than looping.
            const bool announced = TaskState::describes_intended_work(assistant.content);
            const bool can_act = tools && !tool_definitions.empty();

            // "Did nothing" means nothing changed, not "called no tools".
            //
            // Requiring zero tool calls was too weak: a run that listed the
            // directory, read a file and checked the environment - six calls,
            // all read-only - then announced it was creating the project and
            // stopped, leaving the workspace empty and reporting a final answer.
            // Looking is not doing.
            bool changed_anything = false;
            {
                std::lock_guard lock(mutex_);
                changed_anything = !task_.files_changed().empty() || task_.has_successful_build();
            }
            const bool did_nothing = !changed_anything;

            if (announced && did_nothing && can_act && nudges < kMaxNudges && !synthesis_turn) {
                ++nudges;
                push_guidance(
                    "You described what you intend to do but did not do it. Nothing has "
                    "changed on disk and no command has run, so no progress has been made.\n\n"
                    "Stop planning. Carry out the first concrete step now by calling a tool. "
                    "If you need to know what is installed or what the project contains, call "
                    "a tool to find out rather than assuming. Report only what the tool "
                    "results actually show.");
                log_info(LogChannel::Agent, "the model announced work without doing any",
                         {{"nudge", std::to_string(nudges)}});
                emit_simple(AgentEventType::ProgressWarning,
                            "The model described work without performing any; asking it to act.");
                continue;
            }

            if (announced && did_nothing && can_act) {
                // Nudged and still talking. Ending on "the model produced a
                // final answer" would be a lie about what happened.
                summary.completed = false;
                summary.final_message = assistant.content;
                summary.stop_reason =
                    "the model described the work repeatedly but never carried any of it out - "
                    "nothing was created, built or run";
                emit_simple(AgentEventType::RunFailed, summary.stop_reason);
                failure_reported = true;
                break;
            }

            // --- The completion gate --------------------------------------
            //
            // The model says it is finished. The ledger says what was actually
            // established. Where they disagree the ledger wins, and the model
            // gets the chance to close the gap rather than having its answer
            // quietly reworded.
            CompletionAssessment assessment;
            {
                std::lock_guard lock(mutex_);
                ledger_.reassess_objective();
                assessment = assess_completion(ledger_, changed_anything);
            }
            std::vector<std::string> problems;
            const auto checked_message =
                apply_evidence_check(strings::collapse_blank_lines(assistant.content), problems);
            std::vector<std::string> citation_problems;
            {
                std::lock_guard lock(mutex_);
                citation_problems = source_witnesses_.citation_problems(assistant.content);
            }
            if (!citation_problems.empty()) {
                if (!synthesis_turn && citation_returns < 2) {
                    ++citation_returns;
                    std::string guidance = "Correct these source references before answering.\n";
                    for (const auto& problem : citation_problems)
                        guidance += problem + "\n";
                    push_guidance(std::move(guidance));
                    emit_simple(AgentEventType::ProgressWarning,
                                "Source citations were not supported by inspected current lines; "
                                "asking for correction.");
                    continue;
                }
                summary.final_message = checked_message;
                summary.unsupported_claims = problems;
                summary.completed = false;
                summary.stop_reason = "the final answer contains unsupported source citations";
                break;
            }
            const bool file_only = task_policy_ && task_policy_->file_tools_only();
            // Missing runtime proof remains missing. When execution is outside
            // the user's scope, an honest account can finish without asking the
            // model twice for evidence it cannot obtain. False claims still get
            // the bounded correction path and cannot become verified.
            if (assessment.blocked && !synthesis_turn &&
                verification_returns < kMaxVerificationReturns &&
                (!file_only || !problems.empty())) {
                ++verification_returns;
                push_guidance(assessment.guidance);
                log_info(LogChannel::Agent, "completion blocked - evidence missing",
                         {{"attempt", std::to_string(verification_returns)},
                          {"reasons", std::to_string(assessment.reasons.size())}});
                emit_simple(AgentEventType::ProgressWarning,
                            "Reported as finished, but " +
                                std::to_string(assessment.reasons.size()) +
                                " thing(s) could not be confirmed; asking for the evidence.");
                continue;
            }

            // A degenerating model emits newlines. One reply in Run 10 carried
            // several hundred blank lines between two sentences, which pushed
            // the note explaining what had not been verified several screens
            // below the answer it belonged to.
            summary.final_message = checked_message;
            summary.unsupported_claims = problems;

            if (assessment.blocked) {
                if (file_only && problems.empty()) {
                    summary.final_message +=
                        "\n\n*Not verified: execution checks were outside this file-only task.*";
                    summary.completed = true;
                    summary.stop_reason =
                        "the model produced a final answer; runtime verification was outside "
                        "the file-only task scope and the work itself was not verified";
                    log_info(LogChannel::Agent,
                             "file-only task answered without runtime verification");
                    break;
                }
                // Asked and still nothing. The answer stands, with what could
                // not be confirmed stated under it - a silent "completed" is
                // the one outcome that must never happen here.
                summary.final_message +=
                    "\n\n---\n**Not verified.** Nothing in this run "
                    "establishes that the task is done:\n";
                for (const auto& reason : assessment.reasons) {
                    summary.final_message += "- " + reason + "\n";
                }

                // Whether the run counts as completed turns on what the model
                // said, not on how much it verified. "I changed the return
                // value; I have not built it, so this is unverified" is a good
                // answer to an unverified run and must not be recorded as a
                // failure. "It builds and all tests pass" on the same evidence
                // is a false claim, and is.
                if (problems.empty()) {
                    summary.completed = true;
                    summary.stop_reason =
                        "the model produced a final answer; the work itself was not verified";
                    log_info(LogChannel::Agent, "run finished without verification",
                             {{"reasons", std::to_string(assessment.reasons.size())}});
                } else {
                    summary.completed = false;
                    summary.stop_reason =
                        "the model reported the task as finished, but claimed more than the "
                        "evidence supports";
                    log_warn(LogChannel::Agent, "run ended on an unsupported completion claim",
                             {{"claims", std::to_string(problems.size())},
                              {"reasons", std::to_string(assessment.reasons.size())}});
                    emit_simple(AgentEventType::ProgressWarning, summary.stop_reason);
                }
                summary.unsupported_claims.insert(summary.unsupported_claims.end(),
                                                  assessment.reasons.begin(),
                                                  assessment.reasons.end());
                break;
            }

            // Things that are true and are not grounds to refuse the answer.
            // A run whose every criterion was the model's own reading of the
            // task has shown itself consistent with that reading - which is
            // worth saying, and is not worth blocking, because for most work
            // that reading is the only definition anyone has.
            if (!assessment.caveats.empty()) {
                summary.final_message += "\n\n---\n**Worth knowing.**\n";
                for (const auto& caveat : assessment.caveats) {
                    summary.final_message += "- " + caveat + "\n";
                }
            }

            summary.completed = true;
            summary.stop_reason = problems.empty()
                                      ? "the model produced a final answer"
                                      : "the model produced a final answer, with unverified claims";
            if (!problems.empty()) {
                log_warn(LogChannel::Agent, "final answer contained unsupported claims",
                         {{"count", std::to_string(problems.size())}});
            }
            break;
        }

        // The model asked for tools, which is the behaviour the nudges exist to
        // produce. Whatever happened earlier is no longer a streak.
        nudges = 0;

        if (synthesis_turn) {
            // Preserve native tool-call/result pairing even for a model that
            // ignores the final-turn instruction. Never invoke these calls.
            std::lock_guard lock(mutex_);
            for (const auto& call : assistant.tool_calls)
                history_.push_back(ChatMessage::tool_result(
                    call.id, call.name,
                    "Not executed: this was the final response turn; the tool budget is closed."));
            summary.completed = false;
            summary.final_message =
                progress_reporting
                    ? "The progress guard stopped repeated actions. The model did not supply a "
                      "final report."
                    : "The step budget was reached before the model supplied an answer.";
            summary.stop_reason =
                "the model requested tools during the reserved final response turn";
            break;
        }

        // --- Execute the tools -------------------------------------------
        std::size_t tool_position = 0;
        for (const auto& call : assistant.tool_calls) {
            if (cancel.cancelled()) {
                std::lock_guard lock(mutex_);
                for (std::size_t i = tool_position; i < assistant.tool_calls.size(); ++i) {
                    const auto& pending = assistant.tool_calls[i];
                    history_.push_back(ChatMessage::tool_result(
                        pending.id, pending.name,
                        "Not executed: the user cancelled the remaining tool calls."));
                }
                break;
            }
            if (tool_position >=
                static_cast<std::size_t>(std::max(0, config.max_tool_calls_per_turn))) {
                // The complete assistant batch is already in history. Keep
                // every native call paired, while enforcing the action cap.
                std::lock_guard lock(mutex_);
                for (std::size_t i = tool_position; i < assistant.tool_calls.size(); ++i) {
                    const auto& pending = assistant.tool_calls[i];
                    unexecuted_actions.push_back(
                        pending.name + " [call " + pending.id +
                        "]: not executed because the per-turn tool limit was reached.");
                    history_.push_back(ChatMessage::tool_result(
                        pending.id, pending.name,
                        "Not executed: the per-turn tool limit was reached."));
                }
                break;
            }
            if (action_time_closed()) {
                close_action_phase(&assistant, tool_position, false);
                break;
            }
            ++tool_position;

            bool changed_files = false;
            bool settled_criterion = false;
            ToolProgress tool_progress;
            std::optional<bool> source_novelty;
            auto observed_call = call;
            ChatMessage tool_message;
            if (tool_catalog && call.name == "select_tools") {
                AgentEvent started_event;
                started_event.type = AgentEventType::ToolCallStarted;
                started_event.tool_call = call;
                publish(std::move(started_event));
                auto arguments = call.parsed_arguments();
                // Keep room for the human request, tool call/result chain and
                // current ledger. A selection failure leaves the old set intact.
                const int schema_budget =
                    static_cast<int>(profile.context_size) - context_.reserved_output() -
                    context_.count_tokens(system_prompt) - context_.count_tokens(pinned_skills) -
                    context_.count_tokens(task_block) -
                    (native_tools && tool_catalog->stable_dispatch()
                         ? context_.count_tools(tool_catalog->definitions())
                         : 0) -
                    std::max(600, context_.count_tokens(user_message) + 128) - 6;
                auto selection =
                    arguments ? tool_catalog->select(
                                    *arguments, schema_budget,
                                    [&](const auto& candidate) {
                                        return native_tools && !tool_catalog->stable_dispatch()
                                                   ? context_.count_tools(candidate)
                                                   : context_.count_tokens(
                                                         ToolCatalog::text_schemas(candidate));
                                    })
                              : Result<std::string>{arguments.error()};
                auto result = selection ? ToolResult::success("Loaded tool schemas", *selection)
                                        : ToolResult::failure("Could not load tool schemas",
                                                              selection.error().message);
                tool_progress.succeeded = result.ok;
                AgentEvent event;
                event.type = AgentEventType::ToolCompleted;
                event.tool_call = call;
                event.tool_result = result;
                publish(std::move(event));
                if (RequestTrace::enabled())
                    RequestTrace::record_event(
                        "tool_selection",
                        {{"ok", result.ok},
                         {"schema_budget", schema_budget},
                         {"active_count", tool_catalog->loaded_definitions().size()}});
                tool_message = ChatMessage::tool_result(call.id, call.name, result.to_model_text());
            } else if (tool_catalog && tool_catalog->stable_dispatch() &&
                       call.name == "call_tool") {
                auto expanded = tool_catalog->expand_call(call);
                if (expanded) {
                    observed_call = *expanded;
                    tool_message = execute_tool(*expanded, request_cancel, changed_files,
                                                settled_criterion, source_novelty, tool_progress);
                } else {
                    auto result =
                        ToolResult::failure("Could not call tool", expanded.error().message);
                    AgentEvent event;
                    event.type = AgentEventType::ToolCompleted;
                    event.tool_call = call;
                    event.tool_result = result;
                    publish(std::move(event));
                    tool_message =
                        ChatMessage::tool_result(call.id, call.name, result.to_model_text());
                }
            } else {
                tool_message = execute_tool(call, request_cancel, changed_files, settled_criterion,
                                            source_novelty, tool_progress);
            }
            ++summary.tool_calls;

            {
                std::lock_guard lock(mutex_);
                history_.push_back(tool_message);
            }

            if (action_time_closed() && !cancel.cancelled()) {
                // Actual effects/evidence remain recorded. Cooperative phase
                // cancellation is not a productivity failure or a user Stop.
                close_action_phase(&assistant, tool_position, false);
                break;
            }
            if (tool_progress.summary.empty())
                tool_progress.summary = strings::utf8_truncate(tool_message.content, 300);
            auto arguments = observed_call.parsed_arguments();
            auto assessment = progress_.observe(
                observed_call.name, arguments ? *arguments : Json::object(), tool_progress.summary,
                tool_progress.succeeded, changed_files, settled_criterion, source_novelty,
                tool_progress.failure_identity);

            if (assessment.verdict != ProgressVerdict::Fine) {
                emit_simple(AgentEventType::ProgressWarning, assessment.guidance);
                push_guidance(assessment.guidance);
                if (assessment.should_stop) {
                    // The guidance has just been shown as a warning. Repeating
                    // the whole paragraph as the stop reason printed it twice
                    // in a row in the transcript; the reason is a summary, and
                    // the warning above it carries the detail.
                    summary.stop_reason = progress_stop_reason;
                    progress_reporting = true;
                    // The assistant turn is already in history. Pair every
                    // unexecuted call without invoking it, including writes.
                    {
                        std::lock_guard lock(mutex_);
                        for (std::size_t i = tool_position; i < assistant.tool_calls.size(); ++i) {
                            const auto& pending = assistant.tool_calls[i];
                            unexecuted_actions.push_back(
                                pending.name + " [call " + pending.id +
                                "]: not executed because the progress guard closed actions.");
                            history_.push_back(ChatMessage::tool_result(
                                pending.id, pending.name,
                                "Not executed: the progress guard closed the action phase; "
                                "only a final report remains."));
                        }
                    }
                    push_guidance(
                        "The progress guard has closed the action phase. No more tools may "
                        "run. Give a concise final report from the recorded changes and "
                        "results; preserve unresolved checks and do not claim completion.");
                    break;
                }
            }
        }

        {
            // Render under the lock, emit outside it: emit() takes the same
            // mutex, and std::mutex is not recursive.
            AgentEvent event;
            event.type = AgentEventType::TaskStateUpdated;
            {
                std::lock_guard lock(mutex_);
                event.text = task_.to_prompt_block();
            }
            publish(std::move(event));
        }
        {
            // The interface is shown the same verification record the model
            // is, not a friendlier summary of it. If the two ever disagreed,
            // the user would be the last person to find out.
            AgentEvent event;
            event.type = AgentEventType::VerificationUpdated;
            {
                std::lock_guard lock(mutex_);
                ledger_.reassess_objective();
                event.ledger = std::make_shared<const VerificationLedger>(ledger_);
            }
            publish(std::move(event));
        }
        {
            AgentEvent event;
            event.type = AgentEventType::IterationFinished;
            event.iteration = iteration;
            publish(std::move(event));
        }
    }

    if (cancel.cancelled() && !run_deadline.expired() && !summary.cancelled) {
        summary.completed = false;
        summary.cancelled = true;
        summary.stop_reason = "cancelled by the user; the partial work was kept";
        emit_simple(AgentEventType::RunCancelled, summary.stop_reason);
    }
    if (progress_reporting && !summary.cancelled && !run_deadline.expired()) {
        // Delivering a report does not promote stopped work to completion or
        // settle any missing proof. Cancellation and the hard deadline win.
        summary.completed = false;
        summary.stop_reason = progress_stop_reason;
    }
    if (run_deadline.expired()) {
        summary.completed = false;
        summary.cancelled = false;
        summary.stop_reason = "the time budget for this run was reached";
    }
    if (!summary.completed && summary.stop_reason.empty()) {
        summary.stop_reason = "reached the maximum of " + std::to_string(config.max_iterations) +
                              " steps without finishing";
    }

    summary.iterations = iteration;
    summary.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
    {
        std::lock_guard lock(mutex_);
        summary.verification = task_.verification();
        ledger_.reassess_objective();
        summary.ledger = ledger_;
        summary.context = last_context_;
    }

    // The gate above only runs when the model produces a final answer. A run
    // that ends any other way - out of steps, out of time, on an empty reply -
    // still changed files, and reporting it with nothing said about what was
    // established would leave the most misleading case unlabelled.
    //
    // Observed in Run 10: the model fixed every fault correctly, ran out of
    // context, returned an empty answer, and the run was reported as
    // "partially verified" while the ledger held no behaviour check at all.
    if (!summary.completed) {
        const bool work_was_done = !task_state().files_changed().empty();
        const auto assessment = assess_completion(summary.ledger, work_was_done);
        for (const auto& reason : assessment.reasons) {
            if (std::find(summary.unsupported_claims.begin(), summary.unsupported_claims.end(),
                          reason) == summary.unsupported_claims.end()) {
                summary.unsupported_claims.push_back(reason);
            }
        }
    }

    if (summary.completed) {
        AgentEvent event;
        event.type = AgentEventType::RunFinished;
        event.text = summary.final_message;
        event.iteration = iteration;
        publish(std::move(event));
    } else if (!summary.cancelled && !failure_reported) {
        emit_simple(AgentEventType::RunFailed, summary.stop_reason);
    }

    log_info(LogChannel::Agent, "run finished",
             {{"iterations", std::to_string(summary.iterations)},
              {"tool_calls", std::to_string(summary.tool_calls)},
              {"verification", std::string{to_string(summary.verification)}},
              {"ms", std::to_string(summary.duration_ms)},
              {"reason", summary.stop_reason}});
    return summary;
}

}   // namespace localcode
