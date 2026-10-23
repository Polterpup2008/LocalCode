#include "Agent.h"

#include <algorithm>

#include "ToolCallParser.h"
#include "inference/RequestTrace.h"
#include "util/Strings.h"

namespace localcode {

AgentRunSummary Agent::run_conversation(const std::string& user_message, BackendPtr backend,
                                        const ModelProfile& profile, const AgentConfig& config,
                                        CancellationToken cancel,
                                        std::chrono::steady_clock::time_point started) {
    AgentRunSummary summary;
    summary.conversational = true;
    emit_simple(AgentEventType::RunStarted, user_message);
    std::vector<ChatMessage> history;
    {
        std::lock_guard lock(mutex_);
        history_.push_back(ChatMessage::user(user_message));
        // Project only genuine text turns. Tool results, call arguments, host
        // feedback and reasoning do not belong in a conversation-only request.
        for (const auto& message : history_) {
            if ((message.role != Role::User && message.role != Role::Assistant) ||
                !message.tool_calls.empty() || strings::trim(message.content).empty())
                continue;
            auto text = message;
            text.reasoning_content.clear();
            if (!history.empty() && history.back().role == text.role)
                history.back().content += "\n\n" + text.content;
            else
                history.push_back(std::move(text));
        }
    }
    // A recognized social turn is self-contained. Do not drag an earlier
    // coding task's large prose history back into a greeting. Explicit Chat
    // mode retains recent dialogue; the stored coding history stays intact.
    if (config.request_mode == RequestMode::Auto) history = {ChatMessage::user(user_message)};
    // Bound retained history, never the latest human request. The context
    // manager handles capacity without changing the loaded context window.
    if (history.size() > 12) history.erase(history.begin(), history.end() - 12);
    if (!history.empty() && history.front().role == Role::Assistant) history.erase(history.begin());
    const std::string system =
        "You are LocalCode, a helpful conversational assistant. Answer the user's message "
        "directly and naturally. This is Chat mode: no project files, commands, web access or "
        "tools are available. Do not claim you inspected or changed files, ran tests, or "
        "performed actions. If the request needs those capabilities, suggest Code mode. "
        "Earlier conversation is context, not proof that actions occurred.";
    auto built = context_.build(system, {}, {}, history, {});
    summary.context = built.budget;
    {
        std::lock_guard lock(mutex_);
        last_context_ = built.budget;
    }
    AgentEvent context_event;
    context_event.type = AgentEventType::ContextUpdated;
    context_event.context = built.budget;
    context_event.text = built.note;
    publish(context_event);
    RequestTrace::record_event("prompt_setup", {{"mode", "chat"},
                                                {"system_bytes", system.size()},
                                                {"skill_bytes", 0},
                                                {"tool_count", 0},
                                                {"repository_bytes", 0},
                                                {"verification_bytes", 0},
                                                {"estimated_prompt_tokens", built.budget.used}});
    auto fail = [&](std::string reason) {
        summary.stop_reason = std::move(reason);
        summary.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
        summary.cancelled = cancel.cancelled() &&
                            std::chrono::steady_clock::now() < started + config.wall_clock_budget;
        emit_simple(summary.cancelled ? AgentEventType::RunCancelled : AgentEventType::RunFailed,
                    summary.stop_reason);
        return summary;
    };
    if (built.prompt_does_not_fit) return fail(built.note);
    if (cancel.cancelled()) return fail("Chat request cancelled or its time budget expired.");
    ChatRequest request;
    request.messages = std::move(built.messages);
    request.sampling = profile.sampling;
    request.stream = config.stream;
    request.enable_reasoning =
        profile.capabilities.supports_native_reasoning && profile.reasoning_enabled;
    if (profile.capabilities.supports_native_reasoning)
        request.thinking_override = profile.reasoning_enabled;
    request.native_reasoning_effort = profile.native_reasoning_effort;
    emit_simple(AgentEventType::PromptProcessing, {});
    const auto sent = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point first{}, reason_first{}, reason_last{};
    std::string streamed_content;
    int reason_chars = 0, reason_deltas = 0;
    StreamHandlers handlers;
    handlers.on_first_token = [&] {
        if (first.time_since_epoch().count() == 0) first = std::chrono::steady_clock::now();
        emit_simple(AgentEventType::ContentDelta, {});
    };
    handlers.on_content = [&](std::string_view delta) {
        streamed_content += delta;
        emit_simple(AgentEventType::ContentDelta, std::string(delta));
    };
    if (profile.show_reasoning)
        handlers.on_reasoning = [&](std::string_view delta) {
            reason_last = std::chrono::steady_clock::now();
            if (reason_first.time_since_epoch().count() == 0) reason_first = reason_last;
            reason_chars += static_cast<int>(delta.size());
            ++reason_deltas;
            emit_simple(AgentEventType::ReasoningDelta, std::string(delta));
        };
    const auto response = backend->chat(request, handlers, cancel);
    if ((!response || cancel.cancelled()) && !streamed_content.empty()) {
        std::lock_guard lock(mutex_);
        ChatMessage partial;
        partial.role = Role::Assistant;
        partial.content = streamed_content;
        history_.push_back(std::move(partial));
    }
    if (!response) return fail(response.error().to_string());
    if (cancel.cancelled()) return fail("Chat request cancelled or its time budget expired.");
    if (!response->message.tool_calls.empty())
        return fail(
            "The model requested a tool in Chat mode. Nothing was executed; use Code mode for "
            "project work.");
    auto assistant = response->message;
    const auto calls = ToolCallParser::parse(assistant.content);
    if (!calls.calls.empty() || !calls.malformed.empty())
        return fail(
            "The model returned tool-call markup in Chat mode. Nothing was executed; use Code mode "
            "for project work.");
    if (strings::trim(assistant.content).empty()) return fail("The model returned no answer.");
    {
        std::lock_guard lock(mutex_);
        assistant.reasoning_content.clear();
        history_.push_back(assistant);
    }
    TurnStats stats;
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
    stats.total_request_ms = response->duration_ms;
    if (first.time_since_epoch().count() != 0)
        stats.time_to_first_token_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(first - sent).count();
    stats.reasoning_characters = reason_chars;
    stats.reasoning_delta_count = reason_deltas;
    stats.estimated_reasoning_tokens = estimate_tokens(response->message.reasoning_content);
    if (reason_deltas > 1)
        stats.observed_reasoning_stream_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(reason_last - reason_first)
                .count();
    AgentEvent metric;
    metric.type = AgentEventType::GenerationStats;
    metric.stats = stats;
    metric.iteration = 1;
    publish(metric);
    emit_simple(AgentEventType::AssistantMessage, assistant.content);
    summary.completed = true;
    summary.final_message = assistant.content;
    summary.iterations = 1;
    summary.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
    emit_simple(AgentEventType::RunFinished, assistant.content);
    return summary;
}

}   // namespace localcode
