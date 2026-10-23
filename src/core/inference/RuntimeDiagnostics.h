#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace localcode {

struct GraphicsAdapter;

struct DeviceProbe {
    bool supported = false;
    bool succeeded = false;
    std::vector<std::string> devices;
    std::string detail;
};

struct AccelerationInfo {
    int offloaded_layers = -1;
    int total_layers = -1;
    double device_buffer_mib = 0;
    std::string devices;
    void observe(std::string_view line);
    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] std::vector<std::string> parse_device_listing(std::string_view output);
[[nodiscard]] std::string prefer_dedicated_device(const std::vector<std::string>& devices,
                                                  const std::vector<GraphicsAdapter>& adapters);

}   // namespace localcode
