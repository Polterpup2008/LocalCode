#include "RuntimeDiagnostics.h"

#include <regex>

#include "platform/GpuProbe.h"
#include "util/Strings.h"

namespace localcode {

std::string prefer_dedicated_device(const std::vector<std::string>& devices,
                                    const std::vector<GraphicsAdapter>& adapters) {
    std::string selected;
    std::uint64_t dedicated = 0;
    for (const auto& device : devices) {
        const auto lower = strings::to_lower(device);
        for (const auto& adapter : adapters) {
            if (!adapter.name.empty() && adapter.dedicated_bytes > dedicated &&
                lower.find(strings::to_lower(adapter.name)) != std::string::npos) {
                dedicated = adapter.dedicated_bytes;
                selected = device.substr(0, device.find(':'));
            }
        }
    }
    return selected;
}

std::vector<std::string> parse_device_listing(std::string_view output) {
    std::vector<std::string> devices;
    // Device identifiers are opaque runtime values. Restrict parsing to the
    // listing rather than maintaining a GPU-vendor whitelist (ROCm, remote
    // devices and future backends must not silently become CPU fallbacks).
    static const std::regex device(R"(^\s+(\S+):\s*(.+)$)");
    bool listing = false;
    for (const auto& line : strings::split_lines(std::string(output))) {
        if (strings::trim(line) == "Available devices:") {
            listing = true;
            continue;
        }
        if (!listing) continue;
        if (strings::trim(line).empty()) {
            listing = false;
            continue;
        }
        std::smatch match;
        if (std::regex_match(line, match, device) && match[1].str() != "CPU")
            devices.push_back(match[1].str() + ": " + match[2].str());
    }
    return devices;
}

void AccelerationInfo::observe(std::string_view line) {
    static const std::regex offload(R"(offloaded\s+(\d+)\s*/\s*(\d+)\s+layers)");
    static const std::regex buffer(
        R"(\b((?:CUDA|Vulkan|HIP|ROCm|SYCL|Metal|OpenCL|CANN|MUSA|WebGPU)[0-9]*)\s+(?:model|KV|RS|compute)\s+buffer size\s*=\s*([0-9]+(?:\.[0-9]+)?)\s*MiB)");
    const std::string text(line);
    std::smatch match;
    if (std::regex_search(text, match, offload)) {
        try {
            const auto count = std::stoi(match[1].str()), total = std::stoi(match[2].str());
            if (count >= 0 && total > 0 && count <= total) {
                offloaded_layers = count;
                total_layers = total;
            }
        } catch (...) { /* Malformed log numbers are not telemetry. */
        }
    }
    if (std::regex_search(text, match, buffer)) {
        try {
            const auto mib = std::stod(match[2].str());
            device_buffer_mib += mib;
            if (devices.find(match[1].str()) == std::string::npos) {
                if (!devices.empty()) devices += ", ";
                devices += match[1].str();
            }
        } catch (...) { /* Unknown allocation remains unknown. */
        }
    }
}

std::string AccelerationInfo::describe() const {
    if (offloaded_layers < 0)
        return "Acceleration unconfirmed: the runtime did not report an offloaded layer count.";
    if (offloaded_layers == 0)
        return "CPU inference: the runtime reports 0/" + std::to_string(total_layers) +
               " layers offloaded.";
    auto out =
        std::string(offloaded_layers == total_layers ? "Full GPU offload" : "Partial GPU offload");
    out += ": " + std::to_string(offloaded_layers) + "/" + std::to_string(total_layers) + " layers";
    if (!devices.empty()) out += " on " + devices;
    if (device_buffer_mib > 0)
        out += "; " + std::to_string(static_cast<long long>(device_buffer_mib)) +
               " MiB of device buffers reported by the runtime";
    return out + ".";
}

}   // namespace localcode
