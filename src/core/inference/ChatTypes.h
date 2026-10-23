#pragma once
// ---------------------------------------------------------------------------
// The conversation wire format and the inference backend interface.
//
// This is the seam that keeps LocalCode from being welded to one runtime. The
// agent, the tools, the GUI and storage all speak these types; only the classes
// under inference/ know that a llama.cpp server exists. Adding a direct
// libllama backend later means implementing IInferenceBackend and nothing else.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../model/ModelProfile.h"
#include "../util/Cancellation.h"
#include "../util/Json.h"
#include "../util/Result.h"

namespace localcode {

enum class Role { System, User, Assistant, Tool };

std::string_view to_string(Role role) noexcept;
Role role_from_string(std::string_view text);

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;   // raw JSON object text as the model produced it

    [[nodiscard]] Result<Json> parsed_arguments() const;
};

struct ChatMessage {
    Role                  role = Role::User;
    std::string           content;
    // Populated only when the model genuinely exposes reasoning separately.
    // LocalCode never synthesises this field.
    std::string           reasoning_content;
    std::vector<ToolCall> tool_calls;
    std::string           tool_call_id;   // set on Role::Tool replies
    std::string           name;           // tool name, or the internal-feedback provenance marker

    static ChatMessage system(std::string text);
    static ChatMessage user(std::string text);
    // Uses a user role on the wire for strict alternating chat templates, but
    // is never a user instruction. Provenance survives conversation storage.
    static ChatMessage application_feedback(std::string text);
    [[nodiscard]] bool is_application_feedback() const noexcept;
    static ChatMessage assistant(std::string text);
    static ChatMessage tool_result(std::string call_id, std::string tool_name, std::string text);
};

struct ToolDefinition {
    std::string name;
    std::string description;
    Json        parameters;   // JSON Schema object

    [[nodiscard]] Json to_openai_json() const;

    // A leaner form of the same tool, for models with a small context window.
    //
    // The full descriptions exist because small local models call tools badly
    // without them - but twenty-three of them cost around 4,800 tokens, which is
    // most of an 8k window before anything useful is in it. The compact form
    // keeps what decides whether a call is correct - what the tool does, which
    // parameters are required, what values an enum accepts - and drops the
    // elaboration on everything else.
    // `level` 1 keeps a two-sentence summary and short parameter descriptions;
    // level 2 keeps one sentence and describes only the required parameters.
    [[nodiscard]] ToolDefinition compact(int level = 1) const;
};

// Returns the definitions to advertise for a given window: the full schemas when
// there is room, the compact ones when there is not.
[[nodiscard]] std::vector<ToolDefinition> fit_tool_definitions(
    const std::vector<ToolDefinition>& tools, int context_size);

struct ChatRequest {
    std::vector<ChatMessage>    messages;
    std::vector<ToolDefinition> tools;
    SamplingParams              sampling;
    bool                        stream = true;
    // During malformed-call recovery, ask native runtimes for one call. Unset
    // retains the provider/template's ordinary parallel-call behavior.
    std::optional<bool>          parallel_tool_calls;

    // Only sent when the profile says the model actually supports it.
    bool                        enable_reasoning = false;
    std::string                 native_reasoning_effort;

    // Explicitly turn the chat template's thinking on or off for this one
    // request. Distinct from `enable_reasoning`, which can only ever assert
    // that thinking is wanted: unset, the template keeps its own default.
    //
    // Used by finalization, where a model that thinks instead of answering has
    // already cost the user the answer. Templates that do not read the flag
    // ignore it, so this asks rather than assumes.
    std::optional<bool>         thinking_override;

    // Optional JSON schema for constrained output.
    Json                        response_schema;

    std::chrono::milliseconds   timeout{600000};
};

struct ChatUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    // Prompt tokens the runtime served from its cache instead of re-evaluating.
    int cached_prompt_tokens = 0;
    // True when these came from the server. False means they are LocalCode's
    // estimate and must never be presented as measurements.
    bool from_server = false;
};

// Timings as the runtime itself reports them.
//
// This exists because dividing completion tokens by the whole request duration
// produces a number that says more about the prompt length than about the model:
// the same model measured 24.7 tok/s on a cold prompt and 94.9 tok/s on a warm
// one. llama.cpp separates the two phases and its figures are authoritative, so
// they are used whenever they are present.
struct ChatTimings {
    bool   from_server = false;

    // Prompt evaluation - turning the prompt into KV cache entries.
    int    prompt_tokens_evaluated = 0;   // excludes tokens reused from cache
    double prompt_ms = 0.0;
    double prompt_tokens_per_second = 0.0;

    // Decoding - generating the completion, one token at a time. This covers
    // reasoning and the visible answer together: they come off the same decode
    // loop and the runtime does not distinguish them.
    int    decoded_tokens = 0;
    double decode_ms = 0.0;
    double decode_tokens_per_second = 0.0;
};

struct ChatResult {
    ChatMessage  message;
    ChatUsage    usage;
    ChatTimings  timings;
    std::string  finish_reason;      // "stop", "tool_calls", "length", ...

    // Measured by this client: request sent to last byte received. Always
    // larger than prompt_ms + decode_ms, by the network and scheduling overhead.
    std::int64_t duration_ms = 0;

    bool         truncated_by_length = false;
};

// Streaming callbacks. All are optional; all are invoked on the calling thread.
struct StreamHandlers {
    std::function<void(std::string_view)> on_content;
    std::function<void(std::string_view)> on_reasoning;
    // Fired once per tool call, after its arguments have finished streaming.
    std::function<void(const ToolCall&)>  on_tool_call;
    std::function<void(const ChatUsage&)> on_usage;
    // Fired on the first token, so the UI can distinguish prompt processing
    // (which can take a while on a long context) from generation.
    std::function<void()>                 on_first_token;
};

struct BackendCapabilities {
    bool streaming = true;
    bool tools = true;
    bool reasoning_content = false;
    bool json_schema = true;
    bool grammar = true;
    bool tokenize_endpoint = false;   // /tokenize for exact context accounting
    bool embeddings = false;
    bool infill = false;
    std::string name;
    std::string version;
};

struct BackendInfo {
    std::string model_id;
    std::string model_path;
    std::uint32_t context_size = 0;
    std::string server_version;
    std::string chat_template;
    bool        supports_tools_via_template = false;
    Json        raw;
};

class IInferenceBackend {
public:
    virtual ~IInferenceBackend() = default;

    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::string display_name() const = 0;
    [[nodiscard]] virtual BackendCapabilities capabilities() const = 0;

    // Runs one completion. With `request.stream` set, the handlers are called as
    // output arrives and the same content is also accumulated into the result.
    virtual Result<ChatResult> chat(const ChatRequest& request, const StreamHandlers& handlers,
                                    CancellationToken cancel) = 0;

    // Exact token count when the runtime offers it. Falls back to an estimate,
    // which is flagged so the context display can say so.
    virtual Result<int> count_tokens(std::string_view text, CancellationToken cancel = {}) = 0;

    // Cheap liveness probe.
    virtual Status health(CancellationToken cancel = {}) = 0;

    virtual Result<BackendInfo> info(CancellationToken cancel = {}) = 0;
};

using BackendPtr = std::shared_ptr<IInferenceBackend>;

// Rough token estimate used when no tokenizer endpoint is available. Deliberately
// conservative: it is better to trim the context early than to overflow it.
[[nodiscard]] int estimate_tokens(std::string_view text);

}  // namespace localcode
