#pragma once
// ---------------------------------------------------------------------------
// A warning before the runtime aborts.
//
// Loading a 5.3 GB model with 1.2 GB of video memory free fails, and until now
// the only sign was llama.cpp's own
//
//     failed to fit params to free device memory ... abort
//
// arriving a minute later. That message is correct and stays authoritative -
// this does not refuse anything and does not guess where it cannot know.
//
// File size is a rough weight budget for a full-GPU profile, not the actual
// allocation: some tensors can remain on CPU, while KV and compute buffers add
// memory. A partial offload cannot be estimated from the whole file, so it gets
// no verdict. Positive layer counts require the model's repeating-layer count
// to establish full offload, including the output layer.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <optional>
#include <string>

namespace localcode {

enum class VramVerdict {
    // Nothing to say: missing information, CPU or partial offload.
    Unknown,
    // The full-file weight budget exceeds free video memory: a high-risk load,
    // not proof of its actual allocation or a refusal.
    Insufficient,
    // Little headroom above the file-size budget for cache and buffers.
    Tight,
    // Room to spare against the file-size budget, not proof that the model fits.
    Sufficient,
};

[[nodiscard]] std::string_view to_string(VramVerdict verdict) noexcept;

struct VramCheck {
    VramVerdict verdict = VramVerdict::Unknown;
    // Empty when there is nothing worth saying.
    std::string message;

    std::uint64_t model_bytes = 0;
    std::uint64_t free_bytes = 0;
    std::uint64_t total_bytes = 0;

    [[nodiscard]] bool worth_reporting() const {
        return verdict == VramVerdict::Insufficient || verdict == VramVerdict::Tight;
    }
};

// `free_bytes` and `total_bytes` absent means no GPU was detected, which is not
// a problem - it is an absence of information, and the verdict is Unknown.
//
// The C++ launcher maps negative `gpu_layers` to all layers; 0 means CPU only.
// Full offload with a positive count requires more than the known repeating
// layers, to cover the output layer too. Unknown/partial placement gets no verdict.
[[nodiscard]] bool is_full_gpu_profile(int gpu_layers,
                                       std::optional<std::uint64_t> repeating_layers);

[[nodiscard]] VramCheck check_vram(std::uint64_t model_bytes, int gpu_layers,
                                   std::optional<std::uint64_t> free_bytes,
                                   std::optional<std::uint64_t> total_bytes,
                                   std::optional<std::uint64_t> repeating_layers = std::nullopt);

}   // namespace localcode
