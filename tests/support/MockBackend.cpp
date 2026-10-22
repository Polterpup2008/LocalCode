#include "MockBackend.h"

namespace localcode::test {

MockBackend::MockBackend(std::vector<ScriptedTurn> script) : script_(std::move(script)) {}

MockBackend& MockBackend::then_call(std::string tool_name, Json arguments, std::string preamble) {
    ScriptedTurn turn;
    turn.content = std::move(preamble);
    ToolCall call;
    call.id = "call_" + std::to_string(script_.size());
    call.name = std::move(tool_name);
    call.arguments_json = arguments.dump();
    turn.tool_calls.push_back(std::move(call));
    turn.finish_reason = "tool_calls";
    script_.push_back(std::move(turn));
    return *this;
}

MockBackend& MockBackend::then_say(std::string content) {
    ScriptedTurn turn;
    turn.content = std::move(content);
    script_.push_back(std::move(turn));
    return *this;
}

void MockBackend::set_responder(std::function<ScriptedTurn(const ChatRequest&, int)> responder) {
    responder_ = std::move(responder);
}

BackendCapabilities MockBackend::capabilities() const {
    BackendCapabilities capabilities;
    capabilities.name = "mock";
    capabilities.tools = true;
    capabilities.tokenize_endpoint = false;
    return capabilities;
}

Result<ChatResult> MockBackend::chat(const ChatRequest& request, const StreamHandlers& handlers,
                                     CancellationToken cancel) {
    requests_.push_back(request);

    if (pending_error_) {
        auto error = *pending_error_;
        pending_error_.reset();
        return error;
    }
    if (cancel.cancelled()) {
        return make_error(ErrorCode::Cancelled, "cancelled");
    }
    if (before_reply_) before_reply_(cancel);
    if (cancel.cancelled()) {
        return make_error(ErrorCode::Cancelled, "cancelled during reply");
    }

    ScriptedTurn turn;
    if (responder_) {
        turn = responder_(request, turn_);
    } else if (static_cast<std::size_t>(turn_) < script_.size()) {
        turn = script_[static_cast<std::size_t>(turn_)];
    } else {
        // Running off the end of the script means the loop asked for more turns
        // than the test expected; answering rather than hanging makes that
        // failure legible.
        turn.content = "(mock backend: no more scripted turns)";
    }
    ++turn_;

    // Deliver through the handlers exactly as a streaming backend would, so the
    // tests exercise the same path the real one takes.
    if (!turn.reasoning.empty() && handlers.on_reasoning) handlers.on_reasoning(turn.reasoning);
    if (!turn.content.empty()) {
        if (handlers.on_first_token) handlers.on_first_token();
        if (handlers.on_content) handlers.on_content(turn.content);
    }
    for (const auto& call : turn.tool_calls) {
        if (handlers.on_tool_call) handlers.on_tool_call(call);
    }

    ChatResult result;
    result.message.role = Role::Assistant;
    result.message.content = turn.content;
    result.message.reasoning_content = turn.reasoning;
    result.message.tool_calls = turn.tool_calls;
    result.finish_reason = turn.finish_reason;
    result.usage.prompt_tokens = 100;
    result.usage.completion_tokens = 20;
    result.usage.total_tokens = 120;
    if (handlers.on_usage) handlers.on_usage(result.usage);
    return result;
}

Result<int> MockBackend::count_tokens(std::string_view text, CancellationToken) {
    return estimate_tokens(text);
}

Status MockBackend::health(CancellationToken) {
    return ok_status();
}

Result<BackendInfo> MockBackend::info(CancellationToken) {
    BackendInfo info;
    info.model_id = "mock";
    info.context_size = 8192;
    info.server_version = "mock";
    return info;
}

}   // namespace localcode::test
