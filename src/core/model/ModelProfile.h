#pragma once
// ---------------------------------------------------------------------------
// Model profiles and capabilities.
//
// LocalCode is model-agnostic by construction. There is no list of blessed
// families anywhere in this codebase and no `if (model == "qwen")`. What a model
// can do is either read out of its own GGUF header - above all its chat
// template, which is the only trustworthy evidence of whether it was trained to
// emit tool calls or reasoning - or set explicitly by the user in the profile.
//
// Capabilities are never emulated. If a model has no native reasoning, LocalCode
// does not invent a "thinking" section for it; the reasoning UI simply does not
// appear. The one exception is tool calling, where a documented plain-text
// protocol is offered as a generic fallback, because a coding agent that cannot
// call tools is not an agent at all. That fallback is explicit and visible in
// the profile, not a silent pretence.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../util/Json.h"
#include "GgufMetadata.h"

namespace localcode {

enum class BackendPreference { Auto, LlamaCpp, IkLlama };
enum class WeightRepacking { RuntimeDefault, Enabled, Disabled, Invalid };
enum class CpuPlacement { System, PhysicalCores, PhysicalPool, Invalid };
std::string_view to_string(CpuPlacement placement) noexcept;
CpuPlacement cpu_placement_from_string(std::string_view text) noexcept;

std::string_view to_string(BackendPreference preference) noexcept;
BackendPreference backend_preference_from_string(std::string_view text);

// How tool calls are exchanged with the model.
enum class ToolProtocol {
    None,          // the model is not used for tool calling
    Native,        // OpenAI-style tool_calls, produced by the model's chat template
    TextFallback   // documented plain-text protocol parsed out of the reply
};

std::string_view to_string(ToolProtocol protocol) noexcept;

struct ModelCapabilities {
    bool supports_tools = false;
    bool supports_native_reasoning = false;
    bool supports_reasoning_content = false;   // reasoning is exposed separately
    bool supports_vision = false;
    bool supports_long_context = false;
    bool supports_json_schema = false;   // provided by the runtime
    bool supports_grammar = false;       // provided by the runtime (GBNF)
    bool supports_speculative_decoding = false;
    bool supports_mtp = false;   // multi-token prediction heads

    // Free-text record of *why* each capability was set, shown in the model
    // inspector so the user can see this was evidence and not a guess.
    std::string evidence;

    // Reads capabilities out of the model file itself.
    [[nodiscard]] static ModelCapabilities infer(const GgufMetadata& metadata);

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> describe() const;
};

struct SamplingParams {
    float temperature = 0.6f;
    float top_p = 0.95f;
    int top_k = 40;
    float min_p = 0.05f;
    float repeat_penalty = 1.05f;
    int max_tokens = 4096;
    std::int64_t seed = -1;   // -1 = random
    std::vector<std::string> stop_sequences;
};

struct ModelProfile {
    std::string id;   // stable key, derived from the file
    std::string display_name;
    std::filesystem::path model_path;

    BackendPreference backend = BackendPreference::Auto;
    // Overrides the backend's own default binary when set.
    std::filesystem::path server_binary;
    std::vector<std::string> extra_server_arguments;

    std::uint32_t context_size = 8192;
    // -1 offloads every layer to the GPU; 0 is CPU-only.
    int gpu_layers = -1;
    bool auto_gpu_layers = false;  // new profiles can fit layers without changing context
    std::uint32_t batch_size = 512;
    std::uint32_t ubatch_size = 0;     // 0 = runtime's physical batch default
    std::uint32_t threads = 0;         // 0 = let the runtime decide
    std::uint32_t threads_batch = 0;   // 0 = inherit generation threads
    // Unset preserves the runtime default; zero disables saved prompt states.
    std::optional<std::int64_t> prompt_cache_mib;
    WeightRepacking weight_repacking = WeightRepacking::RuntimeDefault;
    CpuPlacement cpu_placement = CpuPlacement::System;
    bool ngram_speculation = false;   // legacy selection; blocked after integrity failures
    bool flash_attention = true;
    bool mmap = true;
    bool mlock = false;
    std::string cache_type_k;   // e.g. "q8_0", empty = runtime default
    std::string cache_type_v;

    SamplingParams sampling;
    ModelCapabilities capabilities;
    ToolProtocol tool_protocol = ToolProtocol::Native;

    // Native reasoning switches. Only meaningful when the capability is present;
    // the UI hides them otherwise rather than offering a lever that does nothing.
    bool reasoning_enabled = true;
    bool show_reasoning = true;
    // Some runtimes expose a model-native effort control. This is passed through
    // verbatim when non-empty; LocalCode never invents effort levels of its own.
    std::string native_reasoning_effort;

    // Free-form notes and the GGUF facts we captured at import time.
    std::string notes;
    std::string architecture;
    std::string quantization;
    std::uint64_t file_size = 0;
    std::uint64_t trained_context = 0;
    std::int64_t created_unix_ms = 0;
    std::int64_t last_used_unix_ms = 0;

    // Builds a sensible default profile from a model file's own metadata.
    [[nodiscard]] static ModelProfile from_gguf(const GgufMetadata& metadata,
        std::optional<std::uint64_t> ram_bytes = {},
        std::optional<std::uint64_t> vram_bytes = {});

    // A stable identifier: the file stem plus a short hash of the full path, so
    // two quantizations of the same model in different folders stay distinct.
    [[nodiscard]] static std::string make_id(const std::filesystem::path& path);

    [[nodiscard]] bool valid() const { return !id.empty() && !model_path.empty(); }
};

// Round-trip serialisation for storage and for import/export of profiles.
[[nodiscard]] Json to_json(const ModelProfile& profile);
[[nodiscard]] ModelProfile model_profile_from_json(const Json& json);

}   // namespace localcode
