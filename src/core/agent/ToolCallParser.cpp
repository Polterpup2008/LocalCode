#include "ToolCallParser.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

// The delimiters LocalCode documents, plus the ones models reach for anyway.
// Being liberal here costs nothing and saves a wasted turn every time a model
// uses the convention it was trained on rather than the one it was told.
struct Delimiter {
    const char* open;
    const char* close;
};

const std::vector<Delimiter>& delimiters() {
    static const std::vector<Delimiter> kDelimiters = {
        {"<tool_call>", "</tool_call>"},
        {"<tool_call>", "<|tool_call_end|>"},
        {"<function_call>", "</function_call>"},
        {"<|tool_call|>", "<|/tool_call|>"},
        {"[TOOL_CALL]", "[/TOOL_CALL]"},
        {"```tool_call", "```"},
        {"```tool_code", "```"},
        {"```json tool_call", "```"},
    };
    return kDelimiters;
}

// Finds a balanced JSON object starting at the first '{' at or after `from`.
// Quote- and escape-aware, so a brace inside a string does not end the object.
std::size_t find_json_object_end(std::string_view text, std::size_t start) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

bool markdown_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim_view(std::string_view text) {
    while (!text.empty() && markdown_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && markdown_space(text.back()))
        text.remove_suffix(1);
    return text;
}

void mask_range(std::string& text, std::size_t begin, std::size_t end) {
    for (auto i = begin; i < end; ++i) {
        if (text[i] != '\n') text[i] = ' ';
    }
}

bool action_fence(std::string_view info) {
    info = trim_view(info);
    if (info == "tool_call" || info == "tool_code") return true;
    if (!info.starts_with("json")) return false;
    info.remove_prefix(4);
    return !info.empty() && markdown_space(info.front()) && trim_view(info) == "tool_call";
}

// Preserve offsets/newlines while removing Markdown examples. Named action
// fences have their own explicit protocol meaning, even if their body is bad JSON.
bool mask_markdown_examples(std::string& text) {
    char fence_char = 0;
    std::size_t fence_length = 0;
    bool is_action = false;
    bool action_body = false;
    bool found_action = false;
    for (std::size_t line = 0; line < text.size();) {
        auto end = text.find('\n', line);
        if (end == std::string::npos) end = text.size();
        auto first = line;
        while (first < end && (text[first] == ' ' || text[first] == '\t'))
            ++first;
        const auto content = std::string_view(text).substr(first, end - first);
        std::size_t run = 0;
        if (!content.empty() && (content.front() == '`' || content.front() == '~')) {
            while (run < content.size() && content[run] == content.front())
                ++run;
        }
        if (fence_char != 0) {
            const bool closes = !content.empty() && content.front() == fence_char &&
                                run >= fence_length && trim_view(content.substr(run)).empty();
            if (closes) {
                found_action = found_action || (is_action && action_body);
                fence_char = 0;
            } else if (!trim_view(content).empty()) {
                action_body = true;
            }
            mask_range(text, line, end);
        } else if (!content.empty() && content.front() == '>') {
            mask_range(text, line, end);
        } else if (run >= 3) {
            fence_char = content.front();
            fence_length = run;
            is_action = action_fence(content.substr(run));
            action_body = false;
            mask_range(text, line, end);
        }
        line = end == text.size() ? end : end + 1;
    }
    found_action = found_action || (fence_char != 0 && is_action && action_body);

    // Pair equal-length backtick runs before masking, so unmatched runs do not
    // hide a later real action and many unmatched delimiters do not rescan text.
    struct TickRun {
        std::size_t start;
        std::size_t length;
        std::size_t next;
    };

    std::vector<TickRun> ticks;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '`') {
            ++i;
            continue;
        }
        const auto start = i;
        while (i < text.size() && text[i] == '`')
            ++i;
        ticks.push_back({start, i - start, std::string::npos});
    }
    std::unordered_map<std::size_t, std::size_t> next_run;
    for (auto i = ticks.size(); i > 0; --i) {
        auto& tick = ticks[i - 1];
        if (const auto found = next_run.find(tick.length); found != next_run.end()) {
            tick.next = found->second;
        }
        next_run[tick.length] = i - 1;
    }
    for (std::size_t i = 0; i < ticks.size();) {
        const auto next = ticks[i].next;
        if (next == std::string::npos) {
            ++i;
            continue;
        }
        mask_range(text, ticks[i].start, ticks[next].start + ticks[next].length);
        i = next + 1;
    }
    return found_action;
}

bool marked_action_body(std::string_view text, std::size_t open, std::size_t body_start,
                        std::size_t body_end, bool closed) {
    const auto body = trim_view(text.substr(body_start, body_end - body_start));
    if (body.empty()) return false;
    // Even an incomplete JSON or Qwen function body is a real attempted action.
    if (body.front() == '{' || body.starts_with("<function=") || body.starts_with("<parameter=")) {
        return true;
    }
    // A stand-alone malformed envelope is still intent. A marker merely named
    // within a prose sentence has neither a structural body nor its own block.
    const auto line_start = text.rfind('\n', open);
    const auto prefix =
        text.substr(line_start == std::string_view::npos ? 0 : line_start + 1,
                    open - (line_start == std::string_view::npos ? 0 : line_start + 1));
    return closed && trim_view(prefix).empty();
}

}  // namespace

bool ToolCallParser::contains_unquoted_tool_intent(std::string_view text,
                                                   const std::vector<std::string>& known_tools) {
    std::string visible(text);
    if (mask_markdown_examples(visible)) return true;
    const std::string_view unquoted(visible);
    for (const auto& delimiter : delimiters()) {
        if (delimiter.open[0] == '`') continue;   // Already handled as fences.
        std::size_t from = 0;
        while (true) {
            const auto open = unquoted.find(delimiter.open, from);
            if (open == std::string_view::npos) break;
            const auto body_start = open + std::strlen(delimiter.open);
            const auto close = unquoted.find(delimiter.close, body_start);
            if (marked_action_body(unquoted, open, body_start,
                                   close == std::string_view::npos ? unquoted.size() : close,
                                   close != std::string_view::npos)) {
                return true;
            }
            from = body_start;
        }
    }
    // Qwen sometimes omits the outer tool_call envelope entirely.
    for (auto from = unquoted.find("<function="); from != std::string_view::npos;
         from = unquoted.find("<function=", from + 10)) {
        const auto tag_end = unquoted.find('>', from + 10);
        if (tag_end == std::string_view::npos) continue;
        const auto body = trim_view(unquoted.substr(tag_end + 1));
        if (body.starts_with("<parameter=") || body.starts_with('{')) return true;
    }
    if (known_tools.empty()) return false;
    for (auto start = unquoted.find('{'); start != std::string_view::npos;) {
        const auto end = find_json_object_end(unquoted, start);
        if (end == std::string_view::npos) break;
        const auto object = Json::parse(unquoted.substr(start, end - start + 1), nullptr, false);
        if (object.is_object()) {
            auto name = json_string(object, "name");
            if (name.empty()) name = json_string(object, "tool");
            if (name.empty()) name = json_string(object, "tool_name");
            if (name.empty()) name = json_string(object, "function");
            if (std::find(known_tools.begin(), known_tools.end(), name) != known_tools.end()) {
                return true;
            }
        }
        start = unquoted.find('{', end + 1);
    }
    return false;
}

bool ToolCallParser::looks_like_tool_call(std::string_view text) {
    for (const auto& delimiter : delimiters()) {
        if (text.find(delimiter.open) != std::string_view::npos) return true;
    }
    // A bare object with both keys is the other common shape.
    return text.find("\"name\"") != std::string_view::npos &&
           text.find("\"arguments\"") != std::string_view::npos;
}

bool ToolCallParser::extract_call(std::string_view block, std::vector<ToolCall>& calls,
                                  std::vector<std::string>& malformed, std::size_t index) {
    const auto brace = block.find('{');
    if (brace == std::string_view::npos) {
        malformed.emplace_back(strings::utf8_truncate(block, 300));
        return false;
    }
    const auto end = find_json_object_end(block, brace);
    if (end == std::string_view::npos) {
        malformed.emplace_back(strings::utf8_truncate(block, 300));
        return false;
    }

    const auto json_text = block.substr(brace, end - brace + 1);
    auto parsed = parse_json(json_text, "tool call");
    if (!parsed || !parsed->is_object()) {
        malformed.emplace_back(strings::utf8_truncate(json_text, 300));
        return false;
    }

    ToolCall call;
    // Accept the several key names models use for the same two fields.
    call.name = json_string(*parsed, "name");
    if (call.name.empty()) call.name = json_string(*parsed, "tool");
    if (call.name.empty()) call.name = json_string(*parsed, "tool_name");
    if (call.name.empty()) call.name = json_string(*parsed, "function");

    if (call.name.empty()) {
        malformed.emplace_back(strings::utf8_truncate(json_text, 300));
        return false;
    }

    const Json* argument_node = nullptr;
    for (const char* key : {"arguments", "args", "parameters", "input"}) {
        if (parsed->contains(key)) {
            argument_node = &(*parsed)[key];
            break;
        }
    }

    if (argument_node) {
        // Some models emit the arguments as a JSON *string* rather than an
        // object; unwrap that rather than failing the call.
        if (argument_node->is_string()) {
            const auto inner = argument_node->get<std::string>();
            auto reparsed = parse_json(inner, "tool call arguments");
            call.arguments_json = (reparsed && reparsed->is_object()) ? reparsed->dump() : inner;
        } else {
            call.arguments_json = argument_node->dump();
        }
    } else {
        // No arguments key: treat every other property as an argument.
        Json arguments = Json::object();
        for (auto it = parsed->begin(); it != parsed->end(); ++it) {
            if (it.key() == "name" || it.key() == "tool" || it.key() == "tool_name" ||
                it.key() == "function") {
                continue;
            }
            arguments[it.key()] = it.value();
        }
        call.arguments_json = arguments.dump();
    }

    call.id = "text_call_" + std::to_string(index);
    calls.push_back(std::move(call));
    return true;
}

ParsedToolCalls ToolCallParser::parse(std::string_view text) {
    ParsedToolCalls result;
    std::string remaining;
    remaining.reserve(text.size());

    std::size_t position = 0;
    std::size_t call_index = 0;

    while (position < text.size()) {
        // Find the earliest opening delimiter from here.
        std::size_t best_open = std::string_view::npos;
        const Delimiter* best = nullptr;
        for (const auto& delimiter : delimiters()) {
            const auto found = text.find(delimiter.open, position);
            if (found != std::string_view::npos && found < best_open) {
                best_open = found;
                best = &delimiter;
            }
        }
        if (!best) break;

        const auto body_start = best_open + std::strlen(best->open);
        auto close = text.find(best->close, body_start);
        // An unterminated block at the end of a stream is common; take the rest.
        const auto body_end = close == std::string_view::npos ? text.size() : close;

        remaining.append(text.substr(position, best_open - position));
        extract_call(text.substr(body_start, body_end - body_start), result.calls,
                     result.malformed, call_index++);

        position = close == std::string_view::npos
                       ? text.size()
                       : close + std::strlen(best->close);
    }

    if (position < text.size()) remaining.append(text.substr(position));

    // No delimited block: look for a bare JSON object that is clearly a call.
    if (result.calls.empty() && result.malformed.empty()) {
        const auto trimmed = strings::trim(text);
        if (!trimmed.empty() && trimmed.front() == '{' &&
            trimmed.find("\"name\"") != std::string::npos &&
            (trimmed.find("\"arguments\"") != std::string::npos ||
             trimmed.find("\"parameters\"") != std::string::npos)) {
            std::vector<ToolCall> calls;
            std::vector<std::string> malformed;
            if (extract_call(trimmed, calls, malformed, 0)) {
                result.calls = std::move(calls);
                remaining.clear();
            }
        }
    }

    result.remaining_text = strings::trim(remaining);
    if (!result.calls.empty()) {
        log_debug(LogChannel::Agent, "parsed text-protocol tool calls",
                  {{"count", std::to_string(result.calls.size())},
                   {"malformed", std::to_string(result.malformed.size())}});
    }
    return result;
}

std::string ToolCallParser::protocol_instructions(const std::vector<ToolDefinition>& tools) {
    std::string out =
        "## Calling tools\n\n"
        "This model does not have built-in tool calling, so use this text format. To call a "
        "tool, write a block exactly like this, on its own lines:\n\n"
        "<tool_call>\n"
        "{\"name\": \"read_file\", \"arguments\": {\"path\": \"src/main.cpp\"}}\n"
        "</tool_call>\n\n"
        "Rules:\n"
        "  - The block contains one JSON object with exactly two keys: \"name\" and \"arguments\".\n"
        "  - \"arguments\" is an object, even when empty: {}.\n"
        "  - Write nothing after the closing tag in that turn; stop and wait for the result.\n"
        "  - You may write a short sentence before the block saying what you are about to do.\n"
        "  - Call one tool per turn. You will be given the result and can then call another.\n\n"
        "The available tools are:\n\n";

    for (const auto& tool : tools) {
        out += "### " + tool.name + "\n";
        out += tool.description + "\n";
        out += "Parameters: " + tool.parameters.dump() + "\n\n";
    }
    return out;
}

}  // namespace localcode
