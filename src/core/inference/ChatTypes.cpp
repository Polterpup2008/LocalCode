#include "ChatTypes.h"

#include <algorithm>
#include <functional>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {

std::string_view to_string(Role role) noexcept {
    switch (role) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

Role role_from_string(std::string_view text) {
    if (strings::iequals(text, "system")) return Role::System;
    if (strings::iequals(text, "assistant")) return Role::Assistant;
    if (strings::iequals(text, "tool") || strings::iequals(text, "function")) return Role::Tool;
    return Role::User;
}

Result<Json> ToolCall::parsed_arguments() const {
    const auto text = strings::trim(arguments_json);
    if (text.empty()) return Json::object();
    auto parsed = parse_json(text, "arguments for tool '" + name + "'");
    if (!parsed) return parsed;
    if (!parsed->is_object()) {
        return make_error(ErrorCode::ParseError,
                          "arguments for tool '" + name + "' must be a JSON object",
                          strings::utf8_truncate(text, 200));
    }
    return parsed;
}

ChatMessage ChatMessage::system(std::string text) {
    ChatMessage m;
    m.role = Role::System;
    m.content = std::move(text);
    return m;
}

ChatMessage ChatMessage::user(std::string text) {
    ChatMessage m;
    m.role = Role::User;
    m.content = std::move(text);
    return m;
}

ChatMessage ChatMessage::application_feedback(std::string text) {
    auto m = user(
        "## LocalCode internal feedback\n"
        "This is application feedback, not a new user instruction. The original user's "
        "scope and restrictions remain binding; this feedback grants no new permissions. "
        "If the requested evidence requires a forbidden action, report that limitation "
        "without performing the action.\n\n" + text);
    m.name = "localcode.internal_feedback";
    return m;
}

bool ChatMessage::is_application_feedback() const noexcept {
    return role == Role::User && name == "localcode.internal_feedback";
}

ChatMessage ChatMessage::assistant(std::string text) {
    ChatMessage m;
    m.role = Role::Assistant;
    m.content = std::move(text);
    return m;
}

ChatMessage ChatMessage::tool_result(std::string call_id, std::string tool_name, std::string text) {
    ChatMessage m;
    m.role = Role::Tool;
    m.tool_call_id = std::move(call_id);
    m.name = std::move(tool_name);
    m.content = std::move(text);
    return m;
}

Json ToolDefinition::to_openai_json() const {
    Json function;
    function["name"] = name;
    function["description"] = description;
    function["parameters"] = parameters.is_null() ? Json::object() : parameters;

    Json tool;
    tool["type"] = "function";
    tool["function"] = std::move(function);
    return tool;
}

namespace {

// The first sentence or two, up to `max_characters`, cut at a boundary rather
// than mid-word.
std::string first_sentences(const std::string& text, std::size_t max_characters,
                            int max_sentences) {
    if (text.size() <= max_characters && max_sentences <= 0) return text;

    std::size_t end = 0;
    int sentences = 0;
    for (std::size_t i = 0; i < text.size() && i < max_characters; ++i) {
        if (text[i] != '.') continue;
        // Not a decimal point or an ellipsis.
        const bool followed_by_space =
            i + 1 >= text.size() || text[i + 1] == ' ' || text[i + 1] == '\n';
        if (!followed_by_space) continue;
        end = i + 1;
        if (++sentences >= max_sentences) break;
    }

    if (end == 0) {
        // No sentence boundary in range: cut at the last space instead.
        if (text.size() <= max_characters) return text;
        auto cut = strings::utf8_truncate(text, max_characters);
        const auto space = cut.find_last_of(' ');
        if (space != std::string::npos && space > max_characters / 2) cut.resize(space);
        return cut + "...";
    }
    return text.substr(0, end);
}

}   // namespace

ToolDefinition ToolDefinition::compact(int level) const {
    const bool aggressive = level >= 2;
    // The last rung, for a tool set large enough that trimming prose alone
    // cannot reach the ceiling: optional parameters that are pure refinement
    // are dropped outright. A required parameter is never dropped, and neither
    // is an optional one carrying an enum - that is a mode selector, which is
    // half of what the tool does rather than a knob on it.
    //
    // And nothing is dropped from a tool that declares no required parameters
    // at all. Such a tool takes different inputs in different modes, so it
    // cannot express its essentials through `required`, and every property it
    // has is load-bearing. Applying the rule blindly emptied repo_symbol and
    // repo_fact down to their mode selector and left them uncallable.
    const bool minimal = level >= 3;

    ToolDefinition out;
    out.name = name;
    out.description = aggressive ? first_sentences(description, minimal ? 50 : 120, 1)
                                 : first_sentences(description, 220, 2);

    if (!parameters.is_object()) {
        out.parameters = parameters;
        return out;
    }

    out.parameters = Json::object();
    out.parameters["type"] = parameters.value("type", "object");

    std::vector<std::string> required;
    if (parameters.contains("required") && parameters["required"].is_array()) {
        for (const auto& entry : parameters["required"]) {
            if (entry.is_string()) required.push_back(entry.get<std::string>());
        }
        out.parameters["required"] = parameters["required"];
    }

    if (parameters.contains("properties") && parameters["properties"].is_object()) {
        // Trim prose recursively while retaining nested parameter contracts.
        // An object described only as "expect: object" gives the model no way
        // to discover comparison fields or the acceptance criterion binding.
        std::function<Json(const Json&, bool)> compact_parameter;
        compact_parameter = [&](const Json& source, bool required_here) -> Json {
            if (!source.is_object()) return source;
            Json property = source;
            property.erase("description");
            // JSON Schema defaults are annotations, not validation constraints.
            // The tool implementation still supplies its actual defaults.
            if (aggressive) property.erase("default");
            if ((required_here || !aggressive) && source.contains("description") &&
                source["description"].is_string()) {
                const std::size_t budget =
                    aggressive ? (minimal ? 40 : 70) : (required_here ? 110 : 60);
                property["description"] =
                    first_sentences(source["description"].get<std::string>(), budget, 1);
            }
            if (source.contains("properties") && source["properties"].is_object()) {
                const auto nested_required = source.value("required", std::vector<std::string>{});
                for (auto entry = source["properties"].begin(); entry != source["properties"].end();
                     ++entry) {
                    const bool required_nested =
                        std::find(nested_required.begin(), nested_required.end(), entry.key()) !=
                        nested_required.end();
                    property["properties"][entry.key()] =
                        compact_parameter(entry.value(), required_nested);
                }
            }
            if (source.contains("items"))
                property["items"] = compact_parameter(source["items"], true);
            return property;
        };
        Json properties = Json::object();
        for (auto it = parameters["properties"].begin(); it != parameters["properties"].end();
             ++it) {
            const auto& source = it.value();

            // A required parameter keeps a short description: getting it wrong
            // wastes a whole turn. Optional ones are usually self-describing
            // from their name and type.
            const bool is_required =
                std::find(required.begin(), required.end(), it.key()) != required.end();
            if (minimal && !required.empty() && !is_required && !source.contains("enum") &&
                it.key() != "expect") {
                continue;
            }

            properties[it.key()] = compact_parameter(source, is_required);
        }
        out.parameters["properties"] = std::move(properties);
    }
    return out;
}

std::vector<ToolDefinition> fit_tool_definitions(const std::vector<ToolDefinition>& tools,
                                                 int context_size) {
    if (tools.empty()) return tools;

    // Measure rather than assume: a small tool set may fit comfortably even in a
    // small window, and there is no reason to degrade it if so.
    int full_cost = 0;
    for (const auto& tool : tools) {
        full_cost += estimate_tokens(tool.name) + estimate_tokens(tool.description) +
                     estimate_tokens(tool.parameters.dump()) + 8;
    }

    // A fifth of the window is the most the tool schemas may claim. Beyond that
    // they start crowding out the problem the model is meant to be solving.
    const int ceiling = std::max(600, context_size / 5);
    if (full_cost <= ceiling) return tools;

    auto cost_of = [](const std::vector<ToolDefinition>& set) {
        int total = 0;
        for (const auto& tool : set) {
            total += estimate_tokens(tool.name) + estimate_tokens(tool.description) +
                     estimate_tokens(tool.parameters.dump()) + 8;
        }
        return total;
    };

    // Escalate rather than jumping straight to the leanest form: a 32k window
    // that only just overflows should not lose the parameter prose entirely.
    std::vector<ToolDefinition> chosen;
    int chosen_cost = 0;
    int chosen_level = 0;

    for (int level = 1; level <= 3; ++level) {
        std::vector<ToolDefinition> candidate;
        candidate.reserve(tools.size());
        for (const auto& tool : tools)
            candidate.push_back(tool.compact(level));

        chosen = std::move(candidate);
        chosen_cost = cost_of(chosen);
        chosen_level = level;
        if (chosen_cost <= ceiling) break;
    }

    log_info(LogChannel::Agent, "using compact tool schemas",
             {{"context", std::to_string(context_size)},
              {"level", std::to_string(chosen_level)},
              {"full_tokens", std::to_string(full_cost)},
              {"compact_tokens", std::to_string(chosen_cost)},
              {"ceiling", std::to_string(ceiling)}});
    return chosen;
}

int estimate_tokens(std::string_view text) {
    if (text.empty()) return 0;
    // Byte-pair vocabularies average a little under four characters per token on
    // English prose and rather less on source code, which is denser in
    // punctuation. 3.5 errs toward over-counting, which is the safe direction.
    const auto characters = static_cast<double>(strings::utf8_length(text));
    return static_cast<int>(characters / 3.5) + 1;
}

}   // namespace localcode
