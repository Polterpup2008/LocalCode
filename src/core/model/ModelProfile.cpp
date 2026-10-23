#include "ModelProfile.h"

#include <algorithm>
#include "../platform/PathEncoding.h"

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

// FNV-1a: short, stable, and good enough to disambiguate two paths.
std::string short_hash(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%08x", static_cast<unsigned>(hash & 0xFFFFFFFFull));
    return buffer;
}

bool template_mentions(const std::string& lowered, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (lowered.find(needle) != std::string::npos) return true;
    }
    return false;
}

}   // namespace

std::string_view to_string(BackendPreference preference) noexcept {
    switch (preference) {
        case BackendPreference::Auto: return "auto";
        case BackendPreference::LlamaCpp: return "llama.cpp";
        case BackendPreference::IkLlama: return "ik_llama.cpp";
    }
    return "auto";
}

BackendPreference backend_preference_from_string(std::string_view text) {
    const auto lowered = strings::to_lower(text);
    if (lowered == "llama.cpp" || lowered == "llamacpp" || lowered == "llama") {
        return BackendPreference::LlamaCpp;
    }
    if (lowered == "ik_llama.cpp" || lowered == "ik_llama" || lowered == "ikllama" ||
        lowered == "ik") {
        return BackendPreference::IkLlama;
    }
    return BackendPreference::Auto;
}

std::string_view to_string(ToolProtocol protocol) noexcept {
    switch (protocol) {
        case ToolProtocol::None: return "none";
        case ToolProtocol::Native: return "native";
        case ToolProtocol::TextFallback: return "text-fallback";
    }
    return "native";
}

std::string_view to_string(CpuPlacement placement) noexcept {
    switch (placement) {
        case CpuPlacement::System: return "os";
        case CpuPlacement::PhysicalCores: return "physical_cores";
        case CpuPlacement::PhysicalPool: return "physical_pool";
        case CpuPlacement::Invalid: return "invalid";
    }
    return "invalid";
}

CpuPlacement cpu_placement_from_string(std::string_view text) noexcept {
    if (text == "os") return CpuPlacement::System;
    if (text == "physical_cores") return CpuPlacement::PhysicalCores;
    if (text == "physical_pool") return CpuPlacement::PhysicalPool;
    return CpuPlacement::Invalid;
}

ModelCapabilities ModelCapabilities::infer(const GgufMetadata& metadata) {
    ModelCapabilities capabilities;
    std::vector<std::string> evidence;

    const auto chat_template = strings::to_lower(metadata.chat_template);

    if (!chat_template.empty()) {
        // A chat template that renders a `tools` variable is the model telling
        // us, in its own words, that it was trained for tool calling.
        if (template_mentions(chat_template, {"tool_calls", "tool_call", "\"tools\"", "tools %}",
                                              "tools is defined", "tools|", "tools %", "toolcall",
                                              "available_tools", "functions"})) {
            capabilities.supports_tools = true;
            evidence.emplace_back("chat template renders tool calls");
        }

        if (template_mentions(chat_template, {"<think>", "</think>", "reasoning_content",
                                              "<thinking>", "enable_thinking", "thinking %}",
                                              "<|thought|>", "analysis<|", "reasoning %}"})) {
            capabilities.supports_native_reasoning = true;
            evidence.emplace_back("chat template contains a reasoning section");
            // A dedicated field or delimiter means reasoning can be shown apart
            // from the answer instead of being mixed into it.
            if (template_mentions(chat_template,
                                  {"reasoning_content", "<think>", "<thinking>", "<|thought|>"})) {
                capabilities.supports_reasoning_content = true;
                evidence.emplace_back("reasoning is delimited and can be displayed separately");
            }
        }
    } else {
        evidence.emplace_back("no chat template in the GGUF header");
    }

    // Vision towers are recorded either in the architecture or as clip.* keys.
    const auto architecture = strings::to_lower(metadata.architecture);
    if (architecture.find("clip") != std::string::npos ||
        architecture.find("vision") != std::string::npos ||
        architecture.find("mllama") != std::string::npos) {
        capabilities.supports_vision = true;
        evidence.emplace_back("vision architecture in the header");
    } else {
        for (const auto& [key, value] : metadata.values) {
            if (strings::starts_with(key, "clip.") || key.find(".vision.") != std::string::npos) {
                capabilities.supports_vision = true;
                evidence.emplace_back("vision tensors described in the header");
                break;
            }
        }
    }

    if (metadata.context_length >= 32768) {
        capabilities.supports_long_context = true;
        evidence.emplace_back("trained context is " + std::to_string(metadata.context_length) +
                              " tokens");
    }

    // Multi-token prediction heads show up as nextn / mtp keys.
    for (const auto& [key, value] : metadata.values) {
        const auto lowered = strings::to_lower(key);
        if (lowered.find("nextn") != std::string::npos ||
            lowered.find(".mtp") != std::string::npos) {
            capabilities.supports_mtp = true;
            evidence.emplace_back("multi-token prediction heads in the header");
            break;
        }
    }

    // Constrained decoding is a property of the runtime, not of the weights:
    // llama.cpp and ik_llama.cpp both apply GBNF grammars and JSON schemas to
    // any model they can load.
    capabilities.supports_grammar = true;
    capabilities.supports_json_schema = true;
    evidence.emplace_back("grammar and JSON schema are provided by the runtime");

    // Draft-model speculative decoding likewise depends on the server being
    // started with a draft model, so it is opt-in per profile rather than a
    // property we can read here.
    capabilities.supports_speculative_decoding = false;

    capabilities.evidence = strings::join(evidence, "; ");
    return capabilities;
}

std::vector<std::pair<std::string, std::string>> ModelCapabilities::describe() const {
    auto yes_no = [](bool value) { return std::string{value ? "yes" : "no"}; };
    return {
        {"tools", yes_no(supports_tools)},
        {"native reasoning", yes_no(supports_native_reasoning)},
        {"reasoning content", yes_no(supports_reasoning_content)},
        {"vision", yes_no(supports_vision)},
        {"long context", yes_no(supports_long_context)},
        {"json schema", yes_no(supports_json_schema)},
        {"grammar", yes_no(supports_grammar)},
        {"speculative decoding", yes_no(supports_speculative_decoding)},
        {"multi-token prediction", yes_no(supports_mtp)},
        {"evidence", evidence},
    };
}

std::string ModelProfile::make_id(const std::filesystem::path& path) {
    auto stem = path_to_utf8(path.stem());
    std::string sanitized;
    sanitized.reserve(stem.size());
    for (char c : stem) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc))
            sanitized.push_back(static_cast<char>(std::tolower(uc)));
        else if (!sanitized.empty() && sanitized.back() != '-')
            sanitized.push_back('-');
    }
    while (!sanitized.empty() && sanitized.back() == '-')
        sanitized.pop_back();
    if (sanitized.size() > 48) sanitized.resize(48);
    if (sanitized.empty()) sanitized = "model";
    return sanitized + "-" + short_hash(path_to_utf8(path));
}

ModelProfile ModelProfile::from_gguf(const GgufMetadata& metadata,
    std::optional<std::uint64_t> ram_bytes, std::optional<std::uint64_t> vram_bytes) {
    ModelProfile profile;
    profile.model_path = metadata.path;
    profile.id = make_id(metadata.path);

    profile.display_name =
        metadata.name.empty() ? path_to_utf8(metadata.path.stem()) : metadata.name;
    if (!metadata.quantization.empty()) {
        profile.display_name += " (" + metadata.quantization + ")";
    }

    profile.architecture = metadata.architecture;
    profile.quantization = metadata.quantization;
    profile.file_size = metadata.file_size;
    profile.trained_context = metadata.context_length;
    profile.created_unix_ms = now_unix_ms();

    // A starting recommendation for NEW imports, not an inference speed claim.
    // Saved profiles/manual overrides are retained by Application::import_model.
    constexpr std::uint64_t gib = 1024ull * 1024 * 1024;
    std::uint64_t context = 8192;
    const bool low_memory = (ram_bytes && *ram_bytes <= 16 * gib) ||
                            (vram_bytes && *vram_bytes <= 4 * gib);
    if (low_memory)
        context = 4096;
    else if (ram_bytes && *ram_bytes >= 30 * gib && vram_bytes && *vram_bytes >= 7 * gib)
        context = *vram_bytes >= 15 * gib ? 32768 : 16384;
    if (metadata.context_length > 0) context = std::min(context, metadata.context_length);
    profile.context_size = static_cast<std::uint32_t>(context);
    profile.auto_gpu_layers = true;
    profile.prompt_cache_mib = low_memory ? 128 : 512;

    profile.capabilities = ModelCapabilities::infer(metadata);
    profile.tool_protocol =
        profile.capabilities.supports_tools ? ToolProtocol::Native : ToolProtocol::TextFallback;
    profile.reasoning_enabled = profile.capabilities.supports_native_reasoning;
    profile.show_reasoning = profile.capabilities.supports_reasoning_content;

    // Mixture-of-experts models are where ik_llama.cpp's specialised kernels are
    // most often worth trying, so surface that as a hint rather than a hard
    // preference - AUTO still checks which backends actually exist.
    if (metadata.expert_count > 0) {
        profile.notes = "Mixture-of-experts model (" + std::to_string(metadata.expert_count) +
                        " experts). ik_llama.cpp may perform differently here; benchmark both.";
    }

    return profile;
}

// ---------------------------------------------------------------------------
// Serialisation. Written field by field rather than with a macro so that adding
// a field to the struct without handling it here shows up as a compile-time
// gap in review rather than as silently dropped user configuration.
// ---------------------------------------------------------------------------
Json to_json(const ModelProfile& profile) {
    Json out;
    out["id"] = profile.id;
    out["display_name"] = profile.display_name;
    out["model_path"] = path_to_utf8(profile.model_path);
    out["backend"] = std::string{to_string(profile.backend)};
    out["server_binary"] = path_to_utf8(profile.server_binary);
    out["extra_server_arguments"] = profile.extra_server_arguments;

    out["context_size"] = profile.context_size;
    out["gpu_layers"] = profile.gpu_layers;
    out["auto_gpu_layers"] = profile.auto_gpu_layers;
    out["batch_size"] = profile.batch_size;
    out["ubatch_size"] = profile.ubatch_size;
    out["threads"] = profile.threads;
    out["threads_batch"] = profile.threads_batch;
    if (profile.cpu_placement != CpuPlacement::System)
        out["cpu_placement"] = std::string(to_string(profile.cpu_placement));
    if (profile.prompt_cache_mib) out["prompt_cache_mib"] = *profile.prompt_cache_mib;
    if (profile.weight_repacking != WeightRepacking::RuntimeDefault)
        out["weight_repacking"] = profile.weight_repacking == WeightRepacking::Invalid
                                      ? Json("invalid")
                                      : Json(profile.weight_repacking == WeightRepacking::Enabled);
    out["ngram_speculation"] = profile.ngram_speculation;
    out["flash_attention"] = profile.flash_attention;
    out["mmap"] = profile.mmap;
    out["mlock"] = profile.mlock;
    out["cache_type_k"] = profile.cache_type_k;
    out["cache_type_v"] = profile.cache_type_v;

    const auto& s = profile.sampling;
    out["sampling"] = {{"temperature", s.temperature},
                       {"top_p", s.top_p},
                       {"top_k", s.top_k},
                       {"min_p", s.min_p},
                       {"repeat_penalty", s.repeat_penalty},
                       {"max_tokens", s.max_tokens},
                       {"seed", s.seed},
                       {"stop_sequences", s.stop_sequences}};

    const auto& c = profile.capabilities;
    out["capabilities"] = {{"tools", c.supports_tools},
                           {"native_reasoning", c.supports_native_reasoning},
                           {"reasoning_content", c.supports_reasoning_content},
                           {"vision", c.supports_vision},
                           {"long_context", c.supports_long_context},
                           {"json_schema", c.supports_json_schema},
                           {"grammar", c.supports_grammar},
                           {"speculative_decoding", c.supports_speculative_decoding},
                           {"mtp", c.supports_mtp},
                           {"evidence", c.evidence}};

    out["tool_protocol"] = std::string{to_string(profile.tool_protocol)};
    out["reasoning_enabled"] = profile.reasoning_enabled;
    out["show_reasoning"] = profile.show_reasoning;
    out["native_reasoning_effort"] = profile.native_reasoning_effort;

    out["notes"] = profile.notes;
    out["architecture"] = profile.architecture;
    out["quantization"] = profile.quantization;
    out["file_size"] = profile.file_size;
    out["trained_context"] = profile.trained_context;
    out["created_unix_ms"] = profile.created_unix_ms;
    out["last_used_unix_ms"] = profile.last_used_unix_ms;
    return out;
}

ModelProfile model_profile_from_json(const Json& json) {
    ModelProfile profile;
    if (!json.is_object()) return profile;

    profile.id = json_string(json, "id");
    profile.display_name = json_string(json, "display_name");
    profile.model_path = path_from_utf8(json_string(json, "model_path"));
    profile.backend = backend_preference_from_string(json_string(json, "backend", "auto"));
    profile.server_binary = path_from_utf8(json_string(json, "server_binary"));
    if (json.contains("extra_server_arguments") && json["extra_server_arguments"].is_array()) {
        for (const auto& item : json["extra_server_arguments"]) {
            if (item.is_string()) profile.extra_server_arguments.push_back(item.get<std::string>());
        }
    }

    profile.context_size = static_cast<std::uint32_t>(json_int(json, "context_size", 8192));
    profile.gpu_layers = static_cast<int>(json_int(json, "gpu_layers", -1));
    // Legacy unbounded offload was the old default. Fit placement on upgrade;
    // explicit layer counts and any saved 1.1 override remain manual.
    profile.auto_gpu_layers = json_bool(json, "auto_gpu_layers", profile.gpu_layers < 0);
    profile.batch_size = static_cast<std::uint32_t>(json_int(json, "batch_size", 512));
    profile.ubatch_size = static_cast<std::uint32_t>(json_int(json, "ubatch_size", 0));
    profile.threads = static_cast<std::uint32_t>(json_int(json, "threads", 0));
    profile.threads_batch = static_cast<std::uint32_t>(json_int(json, "threads_batch", 0));
    if (json.contains("cpu_placement") && !json["cpu_placement"].is_null())
        profile.cpu_placement = json["cpu_placement"].is_string()
            ? cpu_placement_from_string(json["cpu_placement"].get<std::string>())
            : CpuPlacement::Invalid;
    if (json.contains("prompt_cache_mib") && !json["prompt_cache_mib"].is_null()) {
        // Keep malformed explicit values invalid so launch refuses them, rather
        // than silently restoring the runtime's potentially much larger budget.
        profile.prompt_cache_mib = json["prompt_cache_mib"].is_number_integer()
                                       ? json_int(json, "prompt_cache_mib", -1)
                                       : -1;
    }
    if (json.contains("weight_repacking") && !json["weight_repacking"].is_null()) {
        const auto& value = json["weight_repacking"];
        profile.weight_repacking = !value.is_boolean() ? WeightRepacking::Invalid
                                 : value.get<bool>() ? WeightRepacking::Enabled
                                                     : WeightRepacking::Disabled;
    }
    profile.ngram_speculation = json_bool(json, "ngram_speculation", false);
    profile.flash_attention = json_bool(json, "flash_attention", true);
    profile.mmap = json_bool(json, "mmap", true);
    profile.mlock = json_bool(json, "mlock", false);
    profile.cache_type_k = json_string(json, "cache_type_k");
    profile.cache_type_v = json_string(json, "cache_type_v");

    if (json.contains("sampling") && json["sampling"].is_object()) {
        const auto& s = json["sampling"];
        auto& out = profile.sampling;
        out.temperature = static_cast<float>(json_double(s, "temperature", out.temperature));
        out.top_p = static_cast<float>(json_double(s, "top_p", out.top_p));
        out.top_k = static_cast<int>(json_int(s, "top_k", out.top_k));
        out.min_p = static_cast<float>(json_double(s, "min_p", out.min_p));
        out.repeat_penalty =
            static_cast<float>(json_double(s, "repeat_penalty", out.repeat_penalty));
        out.max_tokens = static_cast<int>(json_int(s, "max_tokens", out.max_tokens));
        out.seed = json_int(s, "seed", out.seed);
        if (s.contains("stop_sequences") && s["stop_sequences"].is_array()) {
            for (const auto& item : s["stop_sequences"]) {
                if (item.is_string()) out.stop_sequences.push_back(item.get<std::string>());
            }
        }
    }

    if (json.contains("capabilities") && json["capabilities"].is_object()) {
        const auto& c = json["capabilities"];
        auto& out = profile.capabilities;
        out.supports_tools = json_bool(c, "tools");
        out.supports_native_reasoning = json_bool(c, "native_reasoning");
        out.supports_reasoning_content = json_bool(c, "reasoning_content");
        out.supports_vision = json_bool(c, "vision");
        out.supports_long_context = json_bool(c, "long_context");
        out.supports_json_schema = json_bool(c, "json_schema", true);
        out.supports_grammar = json_bool(c, "grammar", true);
        out.supports_speculative_decoding = json_bool(c, "speculative_decoding");
        out.supports_mtp = json_bool(c, "mtp");
        out.evidence = json_string(c, "evidence");
    }

    const auto protocol = json_string(json, "tool_protocol", "native");
    profile.tool_protocol = protocol == "none"            ? ToolProtocol::None
                            : protocol == "text-fallback" ? ToolProtocol::TextFallback
                                                          : ToolProtocol::Native;

    profile.reasoning_enabled = json_bool(json, "reasoning_enabled", true);
    profile.show_reasoning = json_bool(json, "show_reasoning", true);
    profile.native_reasoning_effort = json_string(json, "native_reasoning_effort");

    profile.notes = json_string(json, "notes");
    profile.architecture = json_string(json, "architecture");
    profile.quantization = json_string(json, "quantization");
    profile.file_size = static_cast<std::uint64_t>(json_int(json, "file_size"));
    profile.trained_context = static_cast<std::uint64_t>(json_int(json, "trained_context"));
    profile.created_unix_ms = json_int(json, "created_unix_ms");
    profile.last_used_unix_ms = json_int(json, "last_used_unix_ms");
    return profile;
}

}   // namespace localcode
