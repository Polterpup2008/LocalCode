#include "Hashing.h"

#include <array>
#include <cctype>
#include <cstdio>

namespace localcode::repo {

std::uint64_t fnv1a(std::string_view data) noexcept {
    constexpr std::uint64_t kOffset = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffset;
    for (const unsigned char byte : data) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kPrime;
    }
    return hash;
}

std::string fnv1a_hex(std::string_view data) {
    std::array<char, 24> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%016llx",
                  static_cast<unsigned long long>(fnv1a(data)));
    return std::string{buffer.data()};
}

std::string slug(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool previous_dash = false;
    for (const char c : text) {
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool keep = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
                          lower == '.' || lower == '_' || lower == '-';
        if (keep) {
            out.push_back(lower);
            previous_dash = false;
        } else if (!previous_dash && !out.empty()) {
            out.push_back('-');
            previous_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string{"workspace"} : out;
}

}  // namespace localcode::repo
