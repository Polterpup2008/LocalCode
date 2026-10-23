#include "OpenAiCompatBackend.h"
#include "RequestTrace.h"
#include "ToolHistory.h"

#include <algorithm>
#include <map>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

// Accumulates streamed tool calls. The wire format sends a call's arguments in
// arbitrary fragments keyed by index, and llama.cpp emits the id and name only
// on the first fragment - so both have to be remembered across deltas.
struct ToolCallAccumulator {
    std::map<int, ToolCall> by_index;
    std::vector<int> order;

    void apply(const Json& delta_tool_calls) {
        if (!delta_tool_calls.is_array()) return;
        for (const auto& entry : delta_tool_calls) {
            const int index = static_cast<int>(json_int(entry, "index", 0));
            auto it = by_index.find(index);
            if (it == by_index.end()) {
                it = by_index.emplace(index, ToolCall{}).first;
                order.push_back(index);
            }
            ToolCall& call = it->second;

            auto id = json_string(entry, "id");
            if (!id.empty()) call.id = std::move(id);

            if (entry.contains("function") && entry["function"].is_object()) {
                const auto& function = entry["function"];
                auto name = json_string(function, "name");
                if (!name.empty()) call.name = std::move(name);
                // Arguments arrive as a stream of string fragments.
                if (function.contains("arguments") && function["arguments"].is_string()) {
                    call.arguments_json += function["arguments"].get<std::string>();
                }
            }
        }
    }

    [[nodiscard]] std::vector<ToolCall> finish() const {
        std::vector<ToolCall> calls;
        calls.reserve(order.size());
        for (int index : order) {
            auto call = by_index.at(index);
            if (call.name.empty()) continue;   // never emit a nameless call
            if (call.id.empty()) call.id = "call_" + std::to_string(index);
            calls.push_back(std::move(call));
        }
        return calls;
    }
};

// llama.cpp has used more than one field name for reasoning across releases;
// accept any of them rather than requiring a particular build.
std::string extract_reasoning(const Json& object) {
    for (const char* key : {"reasoning_content", "reasoning", "thinking"}) {
        if (object.contains(key) && object[key].is_string()) {
            return object[key].get<std::string>();
        }
    }
    return {};
}

Json message_to_json(const ChatMessage& message) {
    Json out;
    out["role"] = std::string{to_string(message.role)};

    if (message.role == Role::Tool) {
        out["content"] = message.content;
        if (!message.tool_call_id.empty()) out["tool_call_id"] = message.tool_call_id;
        if (!message.name.empty()) out["name"] = message.name;
        return out;
    }

    if (message.role == Role::Assistant && !message.reasoning_content.empty()) {
        out["reasoning_content"] = message.reasoning_content;
    }

    if (!message.tool_calls.empty()) {
        // An assistant turn that made tool calls must be replayed with those
        // calls attached, or the server rejects the following tool messages.
        Json calls = Json::array();
        for (const auto& call : message.tool_calls) {
            Json entry;
            entry["id"] = call.id;
            entry["type"] = "function";
            entry["function"]["name"] = call.name;
            entry["function"]["arguments"] =
                call.arguments_json.empty() ? "{}" : call.arguments_json;
            calls.push_back(std::move(entry));
        }
        out["tool_calls"] = std::move(calls);
        out["content"] = message.content;   // may legitimately be empty
    } else {
        out["content"] = message.content;
    }
    return out;
}

ChatUsage parse_usage(const Json& usage) {
    ChatUsage result;
    if (!usage.is_object()) return result;
    result.prompt_tokens = static_cast<int>(json_int(usage, "prompt_tokens"));
    result.completion_tokens = static_cast<int>(json_int(usage, "completion_tokens"));
    result.total_tokens = static_cast<int>(json_int(usage, "total_tokens"));
    if (result.total_tokens == 0) {
        result.total_tokens = result.prompt_tokens + result.completion_tokens;
    }
    if (usage.contains("prompt_tokens_details") && usage["prompt_tokens_details"].is_object()) {
        result.cached_prompt_tokens =
            static_cast<int>(json_int(usage["prompt_tokens_details"], "cached_tokens"));
    }
    result.from_server = result.total_tokens > 0;
    return result;
}

// llama.cpp and ik_llama.cpp both attach a `timings` object, in the buffered
// response and in the final chunk of a stream. Verified against b10655:
//
//   "timings": { "cache_n": 8, "prompt_n": 22, "prompt_ms": 653.1,
//                "prompt_per_second": 27.1, "predicted_n": 60,
//                "predicted_ms": 2761.1, "predicted_per_second": 21.4 }
//
// prompt_n counts only the tokens actually evaluated; cache_n is what was reused.
ChatTimings parse_timings(const Json& timings) {
    ChatTimings result;
    if (!timings.is_object()) return result;

    result.prompt_tokens_evaluated = static_cast<int>(json_int(timings, "prompt_n"));
    result.prompt_ms = json_double(timings, "prompt_ms");
    result.prompt_tokens_per_second = json_double(timings, "prompt_per_second");

    result.decoded_tokens = static_cast<int>(json_int(timings, "predicted_n"));
    result.decode_ms = json_double(timings, "predicted_ms");
    result.decode_tokens_per_second = json_double(timings, "predicted_per_second");

    // Some builds omit the per-second fields; derive them rather than lose them.
    if (result.prompt_tokens_per_second <= 0.0 && result.prompt_ms > 0.0) {
        result.prompt_tokens_per_second =
            result.prompt_tokens_evaluated * 1000.0 / result.prompt_ms;
    }
    if (result.decode_tokens_per_second <= 0.0 && result.decode_ms > 0.0) {
        result.decode_tokens_per_second = result.decoded_tokens * 1000.0 / result.decode_ms;
    }

    result.from_server = result.decode_ms > 0.0 || result.prompt_ms > 0.0;
    return result;
}

}   // namespace

OpenAiCompatBackend::OpenAiCompatBackend(BackendEndpoint endpoint, std::shared_ptr<HttpClient> http)
    : endpoint_(std::move(endpoint)), http_(std::move(http)) {
    capabilities_.name = display_name();
}

void OpenAiCompatBackend::set_endpoint(BackendEndpoint endpoint) {
    clear_token_counts();
    endpoint_ = std::move(endpoint);
}

void OpenAiCompatBackend::clear_token_counts() {
    std::lock_guard lock(token_counts_mutex_);
    token_counts_.clear();
    token_count_text_bytes_ = 0;
    ++token_count_generation_;
}

void OpenAiCompatBackend::set_liveness_reporter(LivenessReporter reporter) {
    liveness_ = std::move(reporter);
}

Error OpenAiCompatBackend::annotate_transport_failure(Error error) const {
    if (!liveness_) return error;
    auto verdict = liveness_();
    if (verdict.empty()) return error;

    if (error.detail.empty()) {
        error.detail = std::move(verdict);
    } else {
        error.detail += "  " + verdict;
    }
    return error;
}

Error OpenAiCompatBackend::chat_http_error(int status_code, const std::string& body) {
    // The server usually says exactly what went wrong. Reporting "HTTP 500" and
    // dropping the rest turns a complete diagnosis into a shrug.
    std::string server_message;
    if (auto parsed = parse_json(body, "error response")) {
        if (parsed->is_object() && parsed->contains("error")) {
            const auto& error = (*parsed)["error"];
            server_message = error.is_object() ? json_string(error, "message") : error.dump();
        }
    }
    if (strings::trim(server_message).empty()) {
        server_message = strings::utf8_truncate(strings::trim(body), 600);
    }

    // A tool call the server could not parse is the model's doing, not the
    // server's, and it is recoverable - so it is named rather than buried in a
    // transport failure. Measured: a model filled its entire 4096-token output
    // budget with runaway <arg_key> tags, llama.cpp answered "Failed to parse
    // tool call arguments as JSON ... missing closing quote", and the run ended
    // reporting that the backend had failed.
    if (strings::icontains(server_message, "parse tool call") ||
        strings::icontains(server_message, "tool call arguments")) {
        return make_error(ErrorCode::InvalidToolCall,
                          "the model produced a tool call the server could not parse",
                          strings::utf8_truncate(server_message, 600));
    }

    return make_error(ErrorCode::BackendError,
                      "chat completion failed with HTTP " + std::to_string(status_code),
                      strings::utf8_truncate(server_message, 600));
}

BackendCapabilities OpenAiCompatBackend::capabilities() const {
    return capabilities_;
}

std::string OpenAiCompatBackend::url_for(std::string_view path) const {
    std::string base = endpoint_.base_url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    std::string suffix{path};
    if (!suffix.empty() && suffix.front() != '/') suffix.insert(suffix.begin(), '/');
    return base + suffix;
}

std::vector<std::pair<std::string, std::string>> OpenAiCompatBackend::default_headers() const {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
    };
    if (!endpoint_.api_key.empty()) {
        headers.emplace_back("Authorization", "Bearer " + endpoint_.api_key);
    }
    return headers;
}

Json OpenAiCompatBackend::build_payload(const ChatRequest& request) const {
    Json payload;
    payload["model"] = "local";   // llama-server ignores this but requires it
    payload["stream"] = request.stream;
    payload["cache_prompt"] = true;
    if (request.stream) payload["stream_options"] = {{"include_usage", true}};

    Json messages = Json::array();
    // Older releases saved malformed native calls before validating them. The
    // server parses historical arguments while rendering its template, so one
    // broken stored call otherwise makes every subsequent request fail. Omit
    // only those rejected calls and their paired error results on the wire;
    // leave the saved conversation, valid calls and execution evidence intact.
    const auto repaired = repair_tool_history(request.messages);
    for (const auto& message : repaired ? *repaired : request.messages)
        messages.push_back(message_to_json(message));
    payload["messages"] = std::move(messages);

    const auto& sampling = request.sampling;
    payload["temperature"] = sampling.temperature;
    payload["top_p"] = sampling.top_p;
    payload["top_k"] = sampling.top_k;
    payload["min_p"] = sampling.min_p;
    payload["repeat_penalty"] = sampling.repeat_penalty;
    if (sampling.max_tokens > 0) payload["max_tokens"] = sampling.max_tokens;
    if (sampling.seed >= 0) payload["seed"] = sampling.seed;
    if (!sampling.stop_sequences.empty()) payload["stop"] = sampling.stop_sequences;

    if (!request.tools.empty()) {
        Json tools = Json::array();
        for (const auto& tool : request.tools)
            tools.push_back(tool.to_openai_json());
        payload["tools"] = std::move(tools);
        payload["tool_choice"] = "auto";
        if (request.parallel_tool_calls.has_value())
            payload["parallel_tool_calls"] = *request.parallel_tool_calls;
    }

    if (!request.response_schema.is_null()) {
        payload["response_format"] = {
            {"type", "json_schema"},
            {"json_schema", {{"name", "response"}, {"schema", request.response_schema}}}};
    }

    // Reasoning switches are forwarded only when the caller has established that
    // the model actually supports them; LocalCode does not invent effort levels.
    if (request.enable_reasoning) {
        payload["chat_template_kwargs"]["enable_thinking"] = true;
        if (!request.native_reasoning_effort.empty()) {
            payload["reasoning_effort"] = request.native_reasoning_effort;
        }
    }

    // An explicit per-request override wins. Some templates ignore the flag;
    // capable managed llama.cpp servers also receive their native budget key.
    // Older servers may still have a budget-prefill bug, so this remains best
    // effort unless the owner selects a corrected runtime.
    if (request.thinking_override.has_value()) {
        payload["chat_template_kwargs"]["enable_thinking"] = *request.thinking_override;
        if (!*request.thinking_override && endpoint_.native_reasoning_budget) {
            payload["reasoning_budget_tokens"] = 0;
        }
    }
    return payload;
}

Result<ChatResult> OpenAiCompatBackend::chat(const ChatRequest& request,
                                             const StreamHandlers& handlers,
                                             CancellationToken cancel) {
    if (!http_) {
        return make_error(ErrorCode::NotSupported, "no HTTP transport is available in this build");
    }
    const auto payload = build_payload(request);
    return request.stream ? chat_streaming(request, payload, handlers, cancel)
                          : chat_buffered(request, payload, handlers, cancel);
}

Result<ChatResult> OpenAiCompatBackend::chat_streaming(const ChatRequest& request,
                                                       const Json& payload,
                                                       const StreamHandlers& handlers,
                                                       CancellationToken cancel) {
    HttpRequest http_request;
    http_request.method = HttpMethod::Post;
    http_request.url = url_for("/v1/chat/completions");
    http_request.headers = default_headers();
    http_request.headers.emplace_back("Accept", "text/event-stream");
    http_request.body = payload.dump();
    if (RequestTrace::enabled())
        RequestTrace::record_event(
            "http_chat_prepared",
            {{"bytes", http_request.body.size()}, {"stream", true}, {"payload", payload}});
    http_request.timeout = request.timeout;

    ChatResult result;
    result.message.role = Role::Assistant;

    ToolCallAccumulator tool_calls;
    SseParser parser;
    bool saw_first_token = false;
    // When the first token of any kind arrived, so the decode phase can be
    // separated from prompt evaluation if the runtime reports no timings.
    std::chrono::steady_clock::time_point first_token_at{};
    bool done = false;
    std::string protocol_error;
    const auto started = std::chrono::steady_clock::now();

    auto handle_event = [&](const SseParser::Event& event) {
        if (done) return;
        const auto data = strings::trim(event.data);
        if (data.empty()) return;
        if (data == "[DONE]") {
            done = true;
            return;
        }

        auto parsed = parse_json(data, "streamed chat chunk");
        if (!parsed) {
            // A malformed chunk is worth recording but not worth aborting on:
            // the stream usually recovers on the next event.
            log_warn(LogChannel::Backend, "unparsable stream chunk",
                     {{"data", strings::utf8_truncate(data, 200)}});
            return;
        }

        if (parsed->contains("error")) {
            const auto& error = (*parsed)["error"];
            protocol_error =
                error.is_object() ? json_string(error, "message", error.dump()) : error.dump();
            done = true;
            return;
        }

        if (parsed->contains("usage") && (*parsed)["usage"].is_object()) {
            result.usage = parse_usage((*parsed)["usage"]);
            if (handlers.on_usage) handlers.on_usage(result.usage);
        }
        // The same final chunk carries the runtime's own timings.
        if (parsed->contains("timings") && (*parsed)["timings"].is_object()) {
            result.timings = parse_timings((*parsed)["timings"]);
        }

        if (!parsed->contains("choices") || !(*parsed)["choices"].is_array()) return;
        for (const auto& choice : (*parsed)["choices"]) {
            const auto finish = json_string(choice, "finish_reason");
            if (!finish.empty()) result.finish_reason = finish;

            if (!choice.contains("delta") || !choice["delta"].is_object()) continue;
            const auto& delta = choice["delta"];

            const auto reasoning = extract_reasoning(delta);
            if (!reasoning.empty()) {
                if (!saw_first_token) {
                    saw_first_token = true;
                    first_token_at = std::chrono::steady_clock::now();
                    if (handlers.on_first_token) handlers.on_first_token();
                }
                result.message.reasoning_content += reasoning;
                if (handlers.on_reasoning) handlers.on_reasoning(reasoning);
            }

            if (delta.contains("content") && delta["content"].is_string()) {
                const auto content = delta["content"].get<std::string>();
                if (!content.empty()) {
                    if (!saw_first_token) {
                        saw_first_token = true;
                        first_token_at = std::chrono::steady_clock::now();
                        if (handlers.on_first_token) handlers.on_first_token();
                    }
                    result.message.content += content;
                    if (handlers.on_content) handlers.on_content(content);
                }
            }

            if (delta.contains("tool_calls")) tool_calls.apply(delta["tool_calls"]);
        }
    };

    // A non-2xx response is not SSE - it is a JSON error object arriving down
    // the same connection, and the streaming client returns an empty body. Kept
    // here, it is the only place the server's own explanation exists.
    //
    // Measured: llama.cpp answered a runaway tool call with
    // {"error":{"code":500,"message":"Failed to parse tool call arguments as
    // JSON: ... missing closing quote"}} - a complete diagnosis - and LocalCode
    // reported "chat completion failed with HTTP 500" and threw the rest away.
    std::string error_body;
    auto response = http_->stream(
        http_request,
        [&](std::string_view chunk) {
            if (error_body.size() < 8192) error_body.append(chunk);
            parser.feed(chunk, handle_event);
            return !done && !cancel.cancelled();
        },
        cancel);

    if (!response) {
        if (response.error().code == ErrorCode::Cancelled) {
            // A cancelled generation still returns whatever arrived, so the
            // conversation stays consistent instead of losing a partial turn.
            result.finish_reason = "cancelled";
            result.message.tool_calls = tool_calls.finish();
            result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            return result;
        }
        return annotate_transport_failure(response.error());
    }
    parser.finish(handle_event);

    if (!protocol_error.empty()) {
        if (strings::icontains(protocol_error, "parse tool call") ||
            strings::icontains(protocol_error, "tool call arguments"))
            return make_error(ErrorCode::InvalidToolCall,
                              "the model produced a tool call the server could not parse",
                              strings::utf8_truncate(protocol_error, 600));
        return make_error(ErrorCode::BackendError, "model server reported an error",
                          strings::utf8_truncate(protocol_error, 600));
    }
    if (!response->ok()) {
        return chat_http_error(response->status_code,
                               response->body.empty() ? error_body : response->body);
    }

    result.message.tool_calls = tool_calls.finish();
    if (!result.message.tool_calls.empty() && result.finish_reason.empty()) {
        result.finish_reason = "tool_calls";
    }
    result.truncated_by_length = result.finish_reason == "length";
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    // If the runtime did not report timings, fall back to a client-side decode
    // rate - but only over the decoding part of the request. Dividing by the
    // whole duration counts prompt evaluation as if it were generation, which is
    // what this code used to do and why the same model appeared to run at 24.7
    // tok/s on a cold prompt and 94.9 on a warm one.
    if (!result.timings.from_server && result.usage.completion_tokens > 0) {
        const auto first =
            first_token_at.time_since_epoch().count() != 0
                ? std::chrono::duration_cast<std::chrono::milliseconds>(first_token_at - started)
                      .count()
                : 0;
        const auto decoding_ms = result.duration_ms - first;
        if (decoding_ms > 0) {
            result.timings.decoded_tokens = result.usage.completion_tokens;
            result.timings.decode_ms = static_cast<double>(decoding_ms);
            result.timings.decode_tokens_per_second =
                static_cast<double>(result.usage.completion_tokens) * 1000.0 /
                static_cast<double>(decoding_ms);
            result.timings.prompt_ms = static_cast<double>(first);
            result.timings.from_server = false;   // measured here, not reported
        }
    }
    return result;
}

Result<ChatResult> OpenAiCompatBackend::chat_buffered(const ChatRequest& request,
                                                      const Json& payload,
                                                      const StreamHandlers& handlers,
                                                      CancellationToken cancel) {
    HttpRequest http_request;
    http_request.method = HttpMethod::Post;
    http_request.url = url_for("/v1/chat/completions");
    http_request.headers = default_headers();
    http_request.body = payload.dump();
    if (RequestTrace::enabled())
        RequestTrace::record_event(
            "http_chat_prepared",
            {{"bytes", http_request.body.size()}, {"stream", false}, {"payload", payload}});
    http_request.timeout = request.timeout;
    http_request.max_response_bytes = 32u * 1024 * 1024;

    const auto started = std::chrono::steady_clock::now();
    auto response = http_->send(http_request, cancel);
    if (!response) return annotate_transport_failure(response.error());

    auto parsed = parse_json(response->body, "chat completion response");
    if (!parsed) return parsed.error();

    if (parsed->contains("error")) {
        const auto& error = (*parsed)["error"];
        return make_error(
            ErrorCode::BackendError, "model server reported an error",
            error.is_object() ? json_string(error, "message", error.dump()) : error.dump());
    }
    if (!response->ok()) {
        return chat_http_error(response->status_code, response->body);
    }

    ChatResult result;
    result.message.role = Role::Assistant;
    result.usage = parse_usage(parsed->value("usage", Json::object()));
    result.timings = parse_timings(parsed->value("timings", Json::object()));

    if (parsed->contains("choices") && (*parsed)["choices"].is_array() &&
        !(*parsed)["choices"].empty()) {
        const auto& choice = (*parsed)["choices"][0];
        result.finish_reason = json_string(choice, "finish_reason");
        if (choice.contains("message") && choice["message"].is_object()) {
            const auto& message = choice["message"];
            result.message.content = json_string(message, "content");
            result.message.reasoning_content = extract_reasoning(message);

            if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                for (const auto& entry : message["tool_calls"]) {
                    ToolCall call;
                    call.id = json_string(entry, "id");
                    if (entry.contains("function") && entry["function"].is_object()) {
                        call.name = json_string(entry["function"], "name");
                        const auto& arguments = entry["function"]["arguments"];
                        call.arguments_json =
                            arguments.is_string() ? arguments.get<std::string>() : arguments.dump();
                    }
                    if (!call.name.empty()) {
                        if (call.id.empty()) {
                            call.id = "call_" + std::to_string(result.message.tool_calls.size());
                        }
                        result.message.tool_calls.push_back(std::move(call));
                    }
                }
            }
        }
    }

    // Feed the handlers so buffered and streaming callers behave identically.
    if (!result.message.reasoning_content.empty() && handlers.on_reasoning) {
        handlers.on_reasoning(result.message.reasoning_content);
    }
    if (!result.message.content.empty() && handlers.on_content) {
        if (handlers.on_first_token) handlers.on_first_token();
        handlers.on_content(result.message.content);
    }
    for (const auto& call : result.message.tool_calls) {
        if (handlers.on_tool_call) handlers.on_tool_call(call);
    }
    if (handlers.on_usage) handlers.on_usage(result.usage);

    result.truncated_by_length = result.finish_reason == "length";
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    return result;
}

Result<int> OpenAiCompatBackend::count_tokens(std::string_view text, CancellationToken cancel) {
    if (!http_ || !capabilities_.tokenize_endpoint) {
        return estimate_tokens(text);
    }

    std::uint64_t generation;
    {
        std::lock_guard lock(token_counts_mutex_);
        generation = token_count_generation_;
        const auto found =
            std::find_if(token_counts_.begin(), token_counts_.end(),
                         [&](const TokenCountEntry& entry) { return entry.text == text; });
        if (found != token_counts_.end()) {
            const int tokens = found->tokens;
            token_counts_.splice(token_counts_.begin(), token_counts_, found);
            return tokens;
        }
    }

    HttpRequest request;
    request.method = HttpMethod::Post;
    request.url = url_for("/tokenize");
    request.headers = default_headers();
    request.body = Json{{"content", std::string{text}}}.dump();
    request.timeout = std::chrono::seconds(30);

    auto response = http_->send(request, cancel);
    if (!response || !response->ok()) return estimate_tokens(text);

    auto parsed = parse_json(response->body, "tokenize response");
    if (!parsed) return estimate_tokens(text);
    if (parsed->contains("tokens") && (*parsed)["tokens"].is_array()) {
        const int count = static_cast<int>((*parsed)["tokens"].size());
        // Keep exact counts only. Bound retained text independently of entry count.
        constexpr std::size_t max_text_bytes = 256 * 1024;
        constexpr std::size_t max_entries = 32;
        if (text.size() <= max_text_bytes) {
            std::lock_guard lock(token_counts_mutex_);
            if (generation == token_count_generation_) {
                const auto found =
                    std::find_if(token_counts_.begin(), token_counts_.end(),
                                 [&](const TokenCountEntry& entry) { return entry.text == text; });
                if (found != token_counts_.end()) {
                    found->tokens = count;
                    token_counts_.splice(token_counts_.begin(), token_counts_, found);
                } else {
                    while (!token_counts_.empty() &&
                           (token_counts_.size() >= max_entries ||
                            token_count_text_bytes_ > max_text_bytes - text.size())) {
                        token_count_text_bytes_ -= token_counts_.back().text.size();
                        token_counts_.pop_back();
                    }
                    token_counts_.push_front({std::string{text}, count});
                    token_count_text_bytes_ += text.size();
                }
            }
        }
        return count;
    }
    return estimate_tokens(text);
}

Status OpenAiCompatBackend::health(CancellationToken cancel) {
    if (!http_) {
        return make_error(ErrorCode::NotSupported, "no HTTP transport is available in this build");
    }
    HttpRequest request;
    request.url = url_for("/health");
    request.headers = default_headers();
    request.timeout = std::chrono::seconds(5);
    request.connect_timeout = std::chrono::seconds(3);

    auto response = http_->send(request, cancel);
    if (!response) return response.error();
    if (response->status_code == 503) {
        // llama-server answers 503 while the model is still loading.
        return make_error(ErrorCode::BackendError, "model is still loading",
                          strings::utf8_truncate(response->body, 200));
    }
    if (!response->ok()) {
        return make_error(ErrorCode::BackendError,
                          "health check returned HTTP " + std::to_string(response->status_code));
    }
    return ok_status();
}

Result<BackendInfo> OpenAiCompatBackend::info(CancellationToken cancel) {
    clear_token_counts();
    if (!http_) {
        return make_error(ErrorCode::NotSupported, "no HTTP transport is available in this build");
    }

    BackendInfo result;

    HttpRequest request;
    request.url = url_for("/props");
    request.headers = default_headers();
    request.timeout = std::chrono::seconds(15);

    auto response = http_->send(request, cancel);
    if (response && response->ok()) {
        auto parsed = parse_json(response->body, "server properties");
        if (parsed) {
            result.raw = *parsed;
            result.chat_template = json_string(*parsed, "chat_template");
            result.model_path = json_string(*parsed, "model_path");
            result.server_version = json_string(*parsed, "build_info");
            if (parsed->contains("default_generation_settings")) {
                const auto& settings = (*parsed)["default_generation_settings"];
                result.context_size = static_cast<std::uint32_t>(json_int(settings, "n_ctx"));
                if (result.context_size == 0 && settings.contains("params")) {
                    result.context_size =
                        static_cast<std::uint32_t>(json_int(settings["params"], "n_ctx"));
                }
            }
            if (result.context_size == 0) {
                result.context_size = static_cast<std::uint32_t>(json_int(*parsed, "n_ctx"));
            }
            const auto lowered = strings::to_lower(result.chat_template);
            result.supports_tools_via_template = lowered.find("tool") != std::string::npos;
        }
    }

    if (result.model_path.empty()) {
        HttpRequest models;
        models.url = url_for("/v1/models");
        models.headers = default_headers();
        models.timeout = std::chrono::seconds(15);
        auto models_response = http_->send(models, cancel);
        if (models_response && models_response->ok()) {
            auto parsed = parse_json(models_response->body, "model list");
            if (parsed && parsed->contains("data") && (*parsed)["data"].is_array() &&
                !(*parsed)["data"].empty()) {
                result.model_id = json_string((*parsed)["data"][0], "id");
            }
        }
    }
    if (result.model_id.empty() && !result.model_path.empty()) {
        result.model_id = std::filesystem::path{result.model_path}.filename().string();
    }

    // Probe /tokenize once so context accounting can use exact counts when the
    // server offers them, and say "estimated" honestly when it does not.
    {
        HttpRequest probe;
        probe.method = HttpMethod::Post;
        probe.url = url_for("/tokenize");
        probe.headers = default_headers();
        probe.body = Json{{"content", "probe"}}.dump();
        probe.timeout = std::chrono::seconds(10);
        auto probe_response = http_->send(probe, cancel);
        capabilities_.tokenize_endpoint = probe_response && probe_response->ok();
    }

    capabilities_.name = display_name();
    capabilities_.version = result.server_version;
    capabilities_.tools = result.supports_tools_via_template;

    log_info(LogChannel::Backend, "backend probed",
             {{"backend", id()},
              {"model", result.model_id},
              {"n_ctx", std::to_string(result.context_size)},
              {"tokenize", capabilities_.tokenize_endpoint ? "yes" : "no"}});
    return result;
}

}   // namespace localcode
