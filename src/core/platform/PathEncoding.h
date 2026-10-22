#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace localcode {
// JSON, GUI strings and process arguments use UTF-8. Native Windows paths use
// UTF-16; path.string() uses the system code page and is not this wire format.
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
}   // namespace localcode
