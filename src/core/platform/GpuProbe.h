#pragma once
// ---------------------------------------------------------------------------
// How much video memory is free, when that can be found out at all.
//
// This asks `nvidia-smi`, which exists only where an NVIDIA driver is
// installed. Every other machine gets an empty answer, and an empty answer must
// stay empty: a preflight warning that fires because a reading was missing would
// be worse than no preflight at all.
//
// Deliberately not a dependency on CUDA or NVML. The binary is already on PATH
// wherever the driver is, it costs one short process, and a missing tool is
// indistinguishable from a machine with no NVIDIA GPU - which is the right
// behaviour either way.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <optional>
#include <string>

namespace localcode {

struct GpuMemory {
    std::string   name;
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
};

// The device with the most free memory, or nothing at all. Never throws, never
// blocks for long: the query is given a short timeout and a failure of any kind
// is reported as absence.
[[nodiscard]] std::optional<GpuMemory> probe_gpu_memory();

// Test seam. While a value is installed, probe_gpu_memory() returns it without
// running anything - there is no way to make a real GPU report 1.2 GB free on
// demand, and the preflight has to be testable.
void set_gpu_memory_for_testing(std::optional<GpuMemory> memory);
void clear_gpu_memory_for_testing();

}  // namespace localcode
