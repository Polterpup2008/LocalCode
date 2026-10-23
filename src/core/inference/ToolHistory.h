#pragma once
#include "ChatTypes.h"

namespace localcode {
// A request-only projection for pre-1.2 conversations. No allocation when all
// native arguments are valid. Saved messages and task evidence are not edited.
[[nodiscard]] std::optional<std::vector<ChatMessage>> repair_tool_history(
    const std::vector<ChatMessage>& history);
}
