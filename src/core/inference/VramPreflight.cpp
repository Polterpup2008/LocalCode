#include "VramPreflight.h"

#include <array>
#include <cstdio>

namespace localcode {
namespace {

// A rule of thumb above the file-size budget, not an allocation estimate.
constexpr double kHeadroomFraction = 0.15;

std::string gib(std::uint64_t bytes) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.1f GiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return std::string{buffer.data()};
}

}   // namespace

std::string_view to_string(VramVerdict verdict) noexcept {
    switch (verdict) {
        case VramVerdict::Unknown: return "unknown";
        case VramVerdict::Insufficient: return "insufficient";
        case VramVerdict::Tight: return "tight";
        case VramVerdict::Sufficient: return "sufficient";
    }
    return "unknown";
}

bool is_full_gpu_profile(int gpu_layers, std::optional<std::uint64_t> repeating_layers) {
    return gpu_layers < 0 || (gpu_layers > 0 && repeating_layers && *repeating_layers > 0 &&
                              static_cast<std::uint64_t>(gpu_layers) > *repeating_layers);
}

VramCheck check_vram(std::uint64_t model_bytes, int gpu_layers,
                     std::optional<std::uint64_t> free_bytes,
                     std::optional<std::uint64_t> total_bytes,
                     std::optional<std::uint64_t> repeating_layers) {
    VramCheck check;
    check.model_bytes = model_bytes;
    check.free_bytes = free_bytes.value_or(0);
    check.total_bytes = total_bytes.value_or(0);

    // Whole-file size says nothing about a partial offload's GPU allocation.
    if (!is_full_gpu_profile(gpu_layers, repeating_layers)) return check;
    // No GPU, no reading, or a model of unknown size: say nothing rather than
    // manufacture a verdict out of a missing number.
    if (!free_bytes.has_value() || model_bytes == 0) return check;

    const auto free = *free_bytes;
    if (model_bytes > free) {
        check.verdict = VramVerdict::Insufficient;
        check.message = "this full-GPU profile uses a model file of " + gib(model_bytes) +
                        ", while only " + gib(free) +
                        " of video memory is free. File size is a rough weight budget, not the "
                        "runtime allocation; cache and compute buffers also need memory. The "
                        "load may fail. Close other GPU workloads or lower GPU layers to keep "
                        "part of the model in system memory.";
        return check;
    }

    const auto headroom = free - model_bytes;
    if (static_cast<double>(headroom) < static_cast<double>(model_bytes) * kHeadroomFraction) {
        check.verdict = VramVerdict::Tight;
        check.message = "this full-GPU profile uses a model file of " + gib(model_bytes) +
                        " and only " + gib(free) +
                        " of video memory is free. File size is a rough weight budget, not the "
                        "runtime allocation; little headroom remains for cache and compute "
                        "buffers. The load may still fail.";
        return check;
    }

    check.verdict = VramVerdict::Sufficient;
    return check;
}

}   // namespace localcode
