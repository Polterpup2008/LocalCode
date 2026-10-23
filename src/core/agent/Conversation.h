#pragma once

#include <string_view>

namespace localcode {

enum class RequestMode { Auto, Chat, Code };

// Auto is deliberately conservative: only complete, recognizable social turns
// skip the coding agent. Ambiguous requests retain all tools and verification.
[[nodiscard]] bool use_conversation_path(std::string_view message, RequestMode mode);

}   // namespace localcode
