#include "ToolHistory.h"
#include <algorithm>
#include <unordered_set>

namespace localcode {
std::optional<std::vector<ChatMessage>> repair_tool_history(
    const std::vector<ChatMessage>& history) {
    bool malformed = false;
    for (const auto& message : history)
        for (const auto& call : message.tool_calls)
            if (!call.parsed_arguments()) malformed = true;
    if (!malformed) return std::nullopt;

    std::vector<ChatMessage> repaired;
    repaired.reserve(history.size());
    std::unordered_set<std::string> rejected;
    for (const auto& original : history) {
        // Some servers reuse call_0 in later turns. Rejection belongs only to
        // this assistant batch, never to a conversation-wide ID blacklist.
        if (original.role == Role::Assistant) rejected.clear();
        if (original.role == Role::Tool && rejected.contains(original.tool_call_id)) continue;
        auto message = original;
        const auto count = message.tool_calls.size();
        std::erase_if(message.tool_calls, [&](const ToolCall& call) {
            if (call.parsed_arguments()) return false;
            if (!call.id.empty()) rejected.insert(call.id);
            return true;
        });
        if (message.tool_calls.size() != count)
            message.content += "\n[LocalCode omitted malformed historical tool calls from replay. "
                               "Their invalid arguments could not be executed. Valid tool results "
                               "and the task's evidence remain unchanged.]";
        repaired.push_back(std::move(message));
    }
    return repaired;
}
}
