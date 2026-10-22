#pragma once
// ---------------------------------------------------------------------------
// Text-protocol tool calling.
//
// When a model's chat template declares tool support, the runtime produces
// proper `tool_calls` and this file is not used. Plenty of good local models
// have no such template, though, and a coding agent that cannot call tools is
// not an agent - so LocalCode offers one documented, generic fallback: the model
// writes a small JSON object inside a marked block, and this parser lifts it out.
//
// This is the only emulation in the codebase, and it is explicit: the profile
// says `tool_protocol: text-fallback`, the UI shows it, and the system prompt
// describes the format. Nothing here pretends a model has a capability it does
// not have.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include "../inference/ChatTypes.h"

namespace localcode {

struct ParsedToolCalls {
    // The reply with the tool-call blocks removed, i.e. what the user should see.
    std::string           remaining_text;
    std::vector<ToolCall> calls;
    // Blocks that looked like a tool call but could not be parsed. Fed back to
    // the model so it can correct the syntax instead of silently being ignored.
    std::vector<std::string> malformed;

    [[nodiscard]] bool empty() const { return calls.empty() && malformed.empty(); }
};

class ToolCallParser {
public:
    // Extracts tool calls written in the text protocol.
    static ParsedToolCalls parse(std::string_view text);

    // The instructions handed to a model that has no native tool calling.
    static std::string protocol_instructions(const std::vector<ToolDefinition>& tools);

    // True when the text contains something that looks like an attempted call.
    static bool looks_like_tool_call(std::string_view text);

    // Detects unquoted action syntax without parsing or executing a call. Ordinary
    // Markdown code/quotes are examples; explicitly named action fences are not.
    // Bare JSON requires a top-level name from known_tools.
    static bool contains_unquoted_tool_intent(std::string_view text,
                                              const std::vector<std::string>& known_tools = {});

private:
    static bool extract_call(std::string_view block, std::vector<ToolCall>& calls,
                             std::vector<std::string>& malformed, std::size_t index);
};

}  // namespace localcode
