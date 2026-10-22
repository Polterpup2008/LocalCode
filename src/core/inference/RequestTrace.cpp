#include "RequestTrace.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <system_error>

#include "../util/Strings.h"

namespace localcode {
namespace {

std::mutex g_mutex;
std::filesystem::path g_directory;   // set by the environment or by a test
std::filesystem::path g_current;
bool g_checked_environment = false;

std::filesystem::path environment_directory() {
    const char* value = std::getenv("LOCALCODE_TRACE_DIR");
    if (value == nullptr || *value == '\0') return {};
    return std::filesystem::path{value};
}

// Reads the environment once. Tracing is a debugging aid; re-reading it per
// request would let a run change behaviour halfway through for no benefit.
std::filesystem::path resolve_directory() {
    if (!g_checked_environment) {
        g_checked_environment = true;
        if (g_directory.empty()) g_directory = environment_directory();
    }
    return g_directory;
}

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Json message_to_json(const ChatMessage& message) {
    Json out;
    out["role"] = std::string{to_string(message.role)};
    out["content"] = message.content;
    if (!message.reasoning_content.empty()) out["reasoning_content"] = message.reasoning_content;
    if (!message.tool_call_id.empty()) out["tool_call_id"] = message.tool_call_id;
    if (!message.name.empty()) out["name"] = message.name;
    if (!message.tool_calls.empty()) {
        Json calls = Json::array();
        for (const auto& call : message.tool_calls) {
            calls.push_back(
                {{"id", call.id}, {"name", call.name}, {"arguments", call.arguments_json}});
        }
        out["tool_calls"] = std::move(calls);
    }
    return out;
}

}   // namespace

bool RequestTrace::enabled() {
    std::lock_guard lock(g_mutex);
    return !resolve_directory().empty();
}

std::filesystem::path RequestTrace::directory() {
    std::lock_guard lock(g_mutex);
    return resolve_directory();
}

std::filesystem::path RequestTrace::current_file() {
    std::lock_guard lock(g_mutex);
    return g_current;
}

void RequestTrace::set_directory_for_testing(std::filesystem::path directory) {
    std::lock_guard lock(g_mutex);
    g_directory = std::move(directory);
    g_checked_environment = true;
    g_current.clear();
}

void RequestTrace::reset_for_testing() {
    std::lock_guard lock(g_mutex);
    g_directory.clear();
    g_current.clear();
    g_checked_environment = true;   // stays off until a test asks for it again
}

void RequestTrace::begin_run(const std::string& label) {
    std::lock_guard lock(g_mutex);
    const auto dir = resolve_directory();
    if (dir.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        // Tracing is optional. Losing it must never take the run with it.
        g_current.clear();
        return;
    }

    std::string safe;
    for (const char c : label) {
        safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '-';
    }
    if (safe.empty()) safe = "run";
    g_current = dir / (std::to_string(now_unix_ms()) + "-" + safe + ".jsonl");

    std::ofstream out(g_current, std::ios::app);
    if (!out) g_current.clear();
}

void RequestTrace::end_run() {
    std::lock_guard lock(g_mutex);
    g_current.clear();
}

Json RequestTrace::to_json(const TraceEntry& entry) {
    Json out;
    out["at_unix_ms"] = now_unix_ms();
    out["phase"] = entry.phase;
    out["model"] = entry.model;

    Json request;
    request["stream"] = entry.request.stream;
    request["enable_reasoning"] = entry.request.enable_reasoning;
    if (entry.request.thinking_override.has_value()) {
        request["thinking_override"] = *entry.request.thinking_override;
    }
    if (!entry.request.native_reasoning_effort.empty()) {
        request["reasoning_effort"] = entry.request.native_reasoning_effort;
    }

    Json sampling;
    sampling["temperature"] = entry.request.sampling.temperature;
    sampling["top_p"] = entry.request.sampling.top_p;
    sampling["top_k"] = entry.request.sampling.top_k;
    sampling["min_p"] = entry.request.sampling.min_p;
    sampling["repeat_penalty"] = entry.request.sampling.repeat_penalty;
    sampling["max_tokens"] = entry.request.sampling.max_tokens;
    sampling["seed"] = entry.request.sampling.seed;
    request["sampling"] = std::move(sampling);

    Json messages = Json::array();
    for (const auto& message : entry.request.messages) {
        messages.push_back(message_to_json(message));
    }
    request["messages"] = std::move(messages);

    // Tool schemas matter for reproduction: the same conversation with a
    // different tool list is a different request.
    Json tools = Json::array();
    for (const auto& tool : entry.request.tools) {
        tools.push_back({{"name", tool.name},
                         {"description", tool.description},
                         {"parameters", tool.parameters}});
    }
    request["tools"] = std::move(tools);
    out["request"] = std::move(request);

    Json response;
    response["content"] = entry.content;
    response["reasoning_content"] = entry.reasoning;
    response["finish_reason"] = entry.finish_reason;
    response["tool_calls"] = entry.tool_calls;
    if (!entry.error.empty()) response["error"] = entry.error;
    out["response"] = std::move(response);

    Json metrics;
    metrics["prompt_tokens"] = entry.prompt_tokens;
    metrics["completion_tokens"] = entry.completion_tokens;
    metrics["usage_from_server"] = entry.usage_from_server;
    metrics["duration_ms"] = entry.duration_ms;
    out["metrics"] = std::move(metrics);

    return out;
}

void RequestTrace::record(const TraceEntry& entry) {
    std::lock_guard lock(g_mutex);
    if (g_current.empty()) return;

    const Json json = to_json(entry);
    // Prompts carry whatever the workspace contained, which on a real project
    // includes configuration files. Redaction runs over the serialised form so
    // it covers message bodies and tool arguments alike.
    const std::string line = strings::redact_secrets(json.dump());

    std::ofstream out(g_current, std::ios::app);
    if (!out) return;
    out << line << '\n';
}

void RequestTrace::record_event(std::string_view stage, const Json& fields) {
    std::lock_guard lock(g_mutex);
    if (g_current.empty()) return;
    auto destination = g_current;
    destination.replace_extension(".events.jsonl");
    const Json entry{{"at_unix_ms", now_unix_ms()}, {"stage", stage}, {"fields", fields}};
    std::ofstream out(destination, std::ios::app);
    if (out) out << strings::redact_secrets(entry.dump()) << '\n';
}

}   // namespace localcode
