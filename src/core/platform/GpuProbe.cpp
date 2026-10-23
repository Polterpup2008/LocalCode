#include "GpuProbe.h"

#include <charconv>
#include <mutex>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <dxgi1_2.h>
#else
#include <unistd.h>
#endif

#include "Process.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace localcode {
namespace {

std::mutex& override_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<GpuMemory>& override_value() {
    static std::optional<GpuMemory> value;
    return value;
}

std::optional<std::uint64_t> parse_mib(std::string_view text) {
    const auto trimmed = strings::trim(text);
    std::uint64_t mib = 0;
    const auto* begin = trimmed.data();
    const auto* end = begin + trimmed.size();
    const auto result = std::from_chars(begin, end, mib);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return mib * 1024ull * 1024ull;
}

}  // namespace

std::vector<GraphicsAdapter> probe_graphics_adapters() {
    std::vector<GraphicsAdapter> adapters;
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return adapters;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            const auto length = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
                                                    nullptr, 0, nullptr, nullptr);
            if (length > 1) {
                std::string name(static_cast<std::size_t>(length), '\0');
                WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
                                    name.data(), length, nullptr, nullptr);
                name.pop_back();
                adapters.push_back({std::move(name), description.DedicatedVideoMemory});
            }
        }
        adapter->Release();
    }
    factory->Release();
#endif
    return adapters;
}

std::optional<std::uint64_t> probe_system_memory_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) return memory.ullTotalPhys;
#else
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && size > 0)
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(size);
#endif
    return std::nullopt;
}

void set_gpu_memory_for_testing(std::optional<GpuMemory> memory) {
    std::lock_guard lock(override_mutex());
    override_value() = std::move(memory);
}

void clear_gpu_memory_for_testing() {
    std::lock_guard lock(override_mutex());
    override_value().reset();
}

std::optional<GpuMemory> probe_gpu_memory() {
    {
        std::lock_guard lock(override_mutex());
        if (override_value().has_value()) return override_value();
    }

    // Resolved against PATH first: CreateProcessW is given an explicit
    // application name, and Windows does not search PATH for that parameter, so
    // a bare "nvidia-smi" never starts. This is why the probe silently reported
    // no GPU on a machine that plainly has one.
    const auto binary = find_executable("nvidia-smi");
    if (!binary.has_value()) return std::nullopt;

    ProcessRequest request;
    request.executable = *binary;
    request.arguments = {"--query-gpu=name,memory.total,memory.free,driver_version", "--format=csv,noheader,nounits"};
    // Short: this runs before a model load and must never be what makes one
    // feel slow. A driver that cannot answer in three seconds is treated as
    // absent.
    request.timeout = std::chrono::milliseconds{3000};
    request.max_captured_bytes = 8u * 1024;

    const auto outcome = ProcessRunner::run(request);
    // nvidia-smi absent is the ordinary case on a machine with no NVIDIA GPU,
    // and is reported the same way as any other failure: as no information.
    if (!outcome) return std::nullopt;
    const auto& result = *outcome;
    if (!result.started || result.exit_code != 0 || result.timed_out) return std::nullopt;

    // One line per device: "NVIDIA GeForce RTX 4060 Laptop GPU, 8188, 7956".
    // The one with the most free memory is the one a load would land on.
    std::optional<GpuMemory> best;
    for (const auto& line : strings::split_lines(result.stdout_text)) {
        const auto fields = strings::split(line, ',');
        if (fields.size() < 3) continue;

        const auto total = parse_mib(fields[1]);
        const auto free = parse_mib(fields[2]);
        if (!total.has_value() || !free.has_value()) continue;

        GpuMemory memory;
        memory.name = std::string{strings::trim(fields[0])};
        memory.total_bytes = *total;
        memory.free_bytes = *free;
        if (fields.size() > 3) memory.driver_version = std::string(strings::trim(fields[3]));
        if (!best.has_value() || memory.free_bytes > best->free_bytes) best = memory;
    }

    if (best.has_value()) {
        log_debug(LogChannel::Backend, "gpu memory probed",
                  {{"device", best->name},
                   {"free_mib", std::to_string(best->free_bytes / (1024 * 1024))},
                   {"total_mib", std::to_string(best->total_bytes / (1024 * 1024))}});
    }
    return best;
}

}  // namespace localcode
