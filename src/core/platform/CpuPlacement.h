#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../util/Result.h"

namespace localcode {
// Eligible logical CPUs only. physical_id identifies shared execution resources,
// including SMT siblings; larger performance_class values prefer faster cores.
struct CpuLogicalProcessor {
    std::uint32_t logical_id = 0;
    std::uint64_t physical_id = 0;
    std::uint32_t performance_class = 0;
};

struct CpuPlacementPlan {
    std::uint32_t threads = 0;
    std::uint32_t threads_batch = 0;
    std::string mask;
    std::string mask_batch;
};

struct CpuPoolPlan {
    std::uint32_t threads = 0;
    std::uint32_t threads_batch = 0;
    std::vector<std::uint32_t> logical_processors;
};

// Does not change any process/thread affinity. Unsupported topology is an error;
// default profiles never call this provider and keep runtime scheduling.
Result<std::vector<CpuLogicalProcessor>> available_cpu_processors();
Result<CpuPlacementPlan> physical_cpu_placement(
    const std::vector<CpuLogicalProcessor>& processors,
    std::uint32_t threads, std::uint32_t threads_batch);
Result<CpuPlacementPlan> physical_cpu_placement(
    std::uint32_t threads, std::uint32_t threads_batch);
Result<CpuPoolPlan> physical_cpu_pool(
    const std::vector<CpuLogicalProcessor>& processors,
    std::uint32_t threads, std::uint32_t threads_batch);
Result<CpuPoolPlan> physical_cpu_pool(std::uint32_t threads, std::uint32_t threads_batch);
}  // namespace localcode
