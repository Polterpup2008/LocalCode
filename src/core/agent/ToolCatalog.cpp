#include "ToolCatalog.h"

#include <algorithm>
#include <set>

#include "../util/Strings.h"

namespace localcode {

ToolCatalog::ToolCatalog(std::vector<ToolDefinition> permitted, bool stable_dispatch)
    : permitted_(std::move(permitted)), stable_dispatch_(stable_dispatch) {}

ToolDefinition ToolCatalog::selector() const {
    Json names = Json::array();
    for (const auto& tool : permitted_) names.push_back(tool.name);
    return {"select_tools",
            "Load tool schemas for the next turn; executes nothing. Replaces the active set.",
            {{"type", "object"},
             {"properties", {{"names", {{"type", "array"},
                                         {"items", {{"type", "string"}, {"enum", names}}}}}}},
             {"required", Json::array({"names"})}}};
}

std::vector<ToolDefinition> ToolCatalog::definitions() const {
    std::vector<ToolDefinition> out{selector()};
    if (stable_dispatch_) {
        Json names = Json::array();
        for (const auto& tool : permitted_) names.push_back(tool.name);
        out.push_back({"call_tool", "Execute a loaded tool with its canonical arguments.",
            {{"type", "object"}, {"properties", {
                {"name", {{"type", "string"}, {"enum", names}}},
                {"arguments", {{"type", "object"}, {"additionalProperties", true}}}}},
             {"required", Json::array({"name", "arguments"})}}});
        return out;
    }
    out.insert(out.end(), active_.begin(), active_.end());
    return out;
}

Result<ToolCall> ToolCatalog::expand_call(const ToolCall& call) const {
    auto arguments = call.parsed_arguments();
    if (!arguments) return arguments.error();
    if (!arguments->contains("name") || !(*arguments)["name"].is_string() ||
        !arguments->contains("arguments") || !(*arguments)["arguments"].is_object())
        return make_error(ErrorCode::InvalidArgument,
                          "call_tool requires a name string and an arguments object");
    const auto name = (*arguments)["name"].get<std::string>();
    if (std::none_of(permitted_.begin(), permitted_.end(),
                     [&](const auto& tool) { return tool.name == name; }))
        return make_error(ErrorCode::InvalidArgument,
                          "Tool is unavailable in this task: " + name);
    if (std::none_of(active_.begin(), active_.end(),
                     [&](const auto& tool) { return tool.name == name; }))
        return make_error(ErrorCode::InvalidArgument,
                          "Select this permitted tool's schema before calling it: " + name);
    return ToolCall{call.id, name, (*arguments)["arguments"].dump()};
}

std::string ToolCatalog::text_schemas(const std::vector<ToolDefinition>& tools) {
    if (tools.empty()) return {};
    std::string out = "\n## Active tool schemas\n";
    for (const auto& tool : tools) {
        if (tool.name == "select_tools" || tool.name == "call_tool") continue;
        out += tool.name + ": " + tool.description + "\nParameters: " +
               tool.parameters.dump() + "\n";
    }
    return out == "\n## Active tool schemas\n" ? std::string{} : out;
}

std::string ToolCatalog::instructions() {
    return "\n## Tool catalogue\n"
           "This window cannot hold all tool schemas together. The names enum in select_tools "
           "lists every permitted tool. Select the tools needed next, wait for their schemas, "
           "then call them. Selection replaces the previous set and executes nothing. Request "
           "fewer tools if a selection is too large; an empty array clears it. Loading schemas "
           "grants no new permissions.\n";
}

Result<std::string> ToolCatalog::select(
    const Json& arguments, int budget,
    const std::function<int(const std::vector<ToolDefinition>&)>& cost) {
    if (!arguments.is_object() || !arguments.contains("names") ||
        !arguments["names"].is_array())
        return make_error(ErrorCode::InvalidArgument, "select_tools requires a names array");

    std::set<std::string> requested;
    for (const auto& name : arguments["names"]) {
        if (!name.is_string())
            return make_error(ErrorCode::InvalidArgument, "Every selected name must be a string");
        const auto text = name.get<std::string>();
        if (std::none_of(permitted_.begin(), permitted_.end(),
                         [&](const auto& tool) { return tool.name == text; }))
            return make_error(ErrorCode::InvalidArgument,
                              "Tool is unavailable in this task: " + text);
        requested.insert(text);
    }

    std::vector<ToolDefinition> selected;
    for (const auto& tool : permitted_)
        if (requested.contains(tool.name)) selected.push_back(tool);
    for (int level = 0; level <= 2; ++level) {
        std::vector<ToolDefinition> candidate;
        if (!stable_dispatch_) candidate.push_back(selector());
        for (const auto& tool : selected) {
            auto definition = level == 0 ? tool : tool.compact(level);
            // compact() retains nested contracts but its old root form only
            // carried type/properties/required. Extensions can use root-level
            // constraints too; discovery must preserve those unchanged.
            if (tool.parameters.is_object()) {
                for (auto property = tool.parameters.begin(); property != tool.parameters.end();
                     ++property) {
                    if (property.key() == "properties" || property.key() == "description" ||
                        property.key() == "default") continue;
                    definition.parameters[property.key()] = property.value();
                }
            }
            candidate.push_back(std::move(definition));
        }
        if (cost(candidate) <= budget) {
            active_.assign(candidate.begin() + (stable_dispatch_ ? 0 : 1), candidate.end());
            std::vector<std::string> names;
            for (const auto& tool : active_) names.push_back(tool.name);
            return names.empty() ? std::string("Active tools cleared. No tool was executed.")
                                 : "Loaded: " + strings::join(names, ", ") +
                                       ". Call these tools next; no tool was executed.";
        }
    }
    return make_error(ErrorCode::InvalidArgument,
                      "Selected schemas exceed this window's tool budget. Request fewer tools "
                      "(one at a time if necessary). The previous selection is unchanged.");
}

} // namespace localcode
