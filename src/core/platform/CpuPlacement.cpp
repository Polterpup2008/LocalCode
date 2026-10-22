#include "CpuPlacement.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace localcode {
namespace {
Error unavailable(std::string detail) {
    return make_error(ErrorCode::NotSupported,
                      "Physical-core placement is unavailable. Choose system scheduling.",
                      std::move(detail));
}

std::string mask_for(const std::vector<CpuLogicalProcessor>& cores, std::size_t count) {
    std::array<unsigned char, 128> digits{};  // GGML masks support up to512 logical CPUs.
    for (std::size_t i = 0; i < count; ++i)
        digits[cores[i].logical_id / 4] |= static_cast<unsigned char>(1u << (cores[i].logical_id % 4));
    std::string mask;
    bool started = false;
    for (auto it = digits.crbegin(); it != digits.crend(); ++it) {
        if (!started && *it == 0) continue;
        started = true;
        mask.push_back("0123456789abcdef"[*it]);
    }
    return "0x" + mask;
}
Result<std::vector<CpuLogicalProcessor>> eligible_physical_cores(
    const std::vector<CpuLogicalProcessor>& processors,
    std::uint32_t logical_limit) {
    std::set<std::uint32_t> logical_ids;
    std::map<std::uint64_t, CpuLogicalProcessor> physical;
    for (const auto& cpu : processors) {
        if (cpu.logical_id >= logical_limit || !logical_ids.insert(cpu.logical_id).second)
            return unavailable("The logical CPU topology is invalid or exceeds the supported CPU set.");
        const auto found = physical.find(cpu.physical_id);
        if (found == physical.end() || cpu.performance_class > found->second.performance_class ||
            (cpu.performance_class == found->second.performance_class &&
             cpu.logical_id < found->second.logical_id))
            physical[cpu.physical_id] = cpu;
    }
    std::vector<CpuLogicalProcessor> cores;
    for (const auto& [id, cpu] : physical) { (void)id; cores.push_back(cpu); }
    if (cores.empty()) return unavailable("No eligible physical cores were found.");
    std::sort(cores.begin(), cores.end(), [](const auto& a, const auto& b) {
        if (a.performance_class != b.performance_class) return a.performance_class > b.performance_class;
        if (a.physical_id != b.physical_id) return a.physical_id < b.physical_id;
        return a.logical_id < b.logical_id;
    });
    return cores;
}
}  // namespace

Result<CpuPlacementPlan> physical_cpu_placement(
    const std::vector<CpuLogicalProcessor>& processors,
    std::uint32_t threads, std::uint32_t threads_batch) {
    if (threads > 256 || threads_batch > 256)
        return make_error(ErrorCode::InvalidArgument, "CPU thread counts must be within 0..256.");
    auto eligible = eligible_physical_cores(processors, 512);
    if (!eligible) return eligible.error();
    const auto& cores = *eligible;
    if (threads == 0)
        threads = static_cast<std::uint32_t>(std::min<std::size_t>(256, std::count_if(
            cores.begin(), cores.end(), [&](const auto& c) {
                return c.performance_class == cores.front().performance_class;
            })));
    if (threads_batch == 0) threads_batch = threads;
    // The compatible OpenMP runtime skips affinity in its single-worker path.
    // Accepting its echoed CPU flags would falsely claim enforced placement.
    if (threads < 2 || threads_batch < 2)
        return unavailable("Physical placement requires at least 2 generation and prompt threads. "
                           "For one thread, choose system scheduling.");
    if (threads > cores.size() || threads_batch > cores.size())
        return make_error(ErrorCode::InvalidArgument,
                          "Physical-core placement needs one eligible physical core per CPU thread.",
                          "Reduce CPU generation/prompt threads or choose system scheduling.");
    return CpuPlacementPlan{threads, threads_batch, mask_for(cores, threads),
                            mask_for(cores, threads_batch)};
}

Result<CpuPlacementPlan> physical_cpu_placement(std::uint32_t threads, std::uint32_t threads_batch) {
    auto processors = available_cpu_processors();
    if (!processors) return processors.error();
    return physical_cpu_placement(*processors, threads, threads_batch);
}

Result<CpuPoolPlan> physical_cpu_pool(const std::vector<CpuLogicalProcessor>& processors,
                                     std::uint32_t threads, std::uint32_t threads_batch) {
    if (threads > 256 || threads_batch > 256)
        return make_error(ErrorCode::InvalidArgument, "CPU thread counts must be within 0..256.");
    // The process launcher supports a bounded Linux CPU set, without GGML's
    // narrower worker-mask representation. Windows topology remains one group.
    auto eligible = eligible_physical_cores(processors, 1024);
    if (!eligible) return eligible.error();
    const auto& cores = *eligible;
    const auto fast = static_cast<std::size_t>(std::count_if(cores.begin(), cores.end(), [&](const auto& cpu) {
        return cpu.performance_class == cores.front().performance_class;
    }));
    if (threads == 0) threads = static_cast<std::uint32_t>(std::min<std::size_t>(256, fast));
    if (threads_batch == 0) threads_batch = threads;
    if (threads > cores.size() || threads_batch > cores.size())
        return make_error(ErrorCode::InvalidArgument,
                          "The CPU pool needs one eligible physical core per generation/prompt thread.");
    const auto size = std::max({fast, static_cast<std::size_t>(threads), static_cast<std::size_t>(threads_batch)});
    CpuPoolPlan plan{threads, threads_batch, {}};
    for (std::size_t i = 0; i < size; ++i) plan.logical_processors.push_back(cores[i].logical_id);
    std::sort(plan.logical_processors.begin(), plan.logical_processors.end());
    return plan;
}

Result<CpuPoolPlan> physical_cpu_pool(std::uint32_t threads, std::uint32_t threads_batch) {
    auto processors = available_cpu_processors();
    if (!processors) return processors.error();
    return physical_cpu_pool(*processors, threads, threads_batch);
}

Result<std::vector<CpuLogicalProcessor>> available_cpu_processors() {
#ifdef _WIN32
    // Dynamic lookup preserves startup on systems without CPU Set APIs.
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    using QuerySets = BOOL(WINAPI*)(void*, ULONG, PULONG, HANDLE, ULONG);
    using QueryDefaults = BOOL(WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
    const auto query = reinterpret_cast<QuerySets>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    const auto defaults = reinterpret_cast<QueryDefaults>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    if (!query || !defaults || GetActiveProcessorGroupCount() != 1)
        return unavailable("This runtime requires one Windows processor group and CPU Set APIs.");
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        return unavailable("Cannot query the process CPU allowance.");
    ULONG size = 0;
    if (query(nullptr, 0, &size, GetCurrentProcess(), 0) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return unavailable("Cannot query CPU Set buffer size.");
    if (size == 0 || size > 1024 * 1024) return unavailable("Invalid CPU Set buffer size.");
    std::vector<unsigned char> bytes(size);
    if (!query(bytes.data(), size, &size, GetCurrentProcess(), 0) || size > bytes.size())
        return unavailable("CPU topology changed while it was being queried.");
    ULONG default_count = 0;
    if (!defaults(GetCurrentProcess(), nullptr, 0, &default_count) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return unavailable("Cannot query process CPU Sets.");
    if (default_count > 4096) return unavailable("Process CPU Set count is too large.");
    std::vector<ULONG> allowed(default_count);
    if (default_count && !defaults(GetCurrentProcess(), allowed.data(), default_count, &default_count))
        return unavailable("Process CPU Sets changed while being queried.");
    if (default_count > allowed.size()) return unavailable("Process CPU Set count changed.");
    allowed.resize(default_count);
    // Fixed Windows SDK CPU Set prefix; records may append fields in later OS versions.
    struct Entry {
        DWORD size, type, id;
        WORD group;
        BYTE logical, core, cache, numa, performance, flags;
        DWORD reserved;
        DWORD64 tag;
    };
    static_assert(sizeof(Entry) == 32 && offsetof(Entry, performance) == 18);
    std::vector<CpuLogicalProcessor> result;
    std::size_t offset = 0;
    while (offset < size) {
        if (size - offset < 8) return unavailable("Incomplete CPU Set header.");
        DWORD header[2];
        std::memcpy(header, bytes.data() + offset, sizeof(header));
        if (header[0] < 8 || header[0] > size - offset) return unavailable("Invalid CPU Set record.");
        if (header[1] == 0) {
            if (header[0] < sizeof(Entry)) return unavailable("Incomplete CPU Set record.");
            Entry cpu{};
            std::memcpy(&cpu, bytes.data() + offset, sizeof(cpu));
            if (cpu.group != 0 || cpu.logical >= sizeof(DWORD_PTR) * 8)
                return unavailable("CPU group cannot be expressed by this Windows runtime.");
            const bool in_default = allowed.empty() || std::find(allowed.begin(), allowed.end(), cpu.id) != allowed.end();
            // Reserved CPUs may belong exclusively to another process; do not use them.
            if ((process_mask & (DWORD_PTR{1} << cpu.logical)) && !(cpu.flags & 3) && in_default)
                result.push_back({cpu.logical, cpu.core, cpu.performance});
        }
        offset += header[0];
    }
    if (result.empty()) return unavailable("No unparked, unreserved allowed CPU Sets.");
    return result;
#elif defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        return unavailable("Cannot query the process CPU allowance.");
    auto read_number = [](const std::string& path) -> std::optional<std::uint32_t> {
        std::ifstream file(path);
        std::int64_t value = -1;
        if (!(file >> value) || value < 0 || value > 0xffffffffLL) return std::nullopt;
        return static_cast<std::uint32_t>(value);
    };
    std::vector<CpuLogicalProcessor> result;
    bool all_capacities = true;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (!CPU_ISSET(i, &allowed)) continue;
        if (i >= 512) return unavailable("This runtime supports at most512 CPU mask bits.");
        const auto root = "/sys/devices/system/cpu/cpu" + std::to_string(i) + '/';
        const auto package = read_number(root + "topology/physical_package_id");
        const auto core = read_number(root + "topology/core_id");
        if (!package || !core) return unavailable("Physical CPU topology is not exposed by this kernel.");
        const auto capacity = read_number(root + "cpu_capacity");
        all_capacities = all_capacities && capacity.has_value() && *capacity > 0;
        result.push_back({static_cast<std::uint32_t>(i), (std::uint64_t{*package} << 32) | *core, capacity.value_or(0)});
    }
    if (!all_capacities) for (auto& cpu : result) cpu.performance_class = 0;
    if (result.empty()) return unavailable("No allowed logical processors.");
    return result;
#else
    return unavailable("Physical-core masks are not supported on this operating system.");
#endif
}
}  // namespace localcode
