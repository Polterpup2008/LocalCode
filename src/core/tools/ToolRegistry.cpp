#include "ToolRegistry.h"
#include "../security/TaskPolicy.h"
#include "../workspace/Workspace.h"

#include <chrono>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {

ToolRegistry::ToolRegistry() = default;

void ToolRegistry::add(ToolPtr tool) {
    if (!tool) return;
    std::lock_guard lock(mutex_);
    tools_[tool->name()] = std::move(tool);
}

void ToolRegistry::remove(std::string_view name) {
    std::lock_guard lock(mutex_);
    const auto it = tools_.find(name);
    if (it != tools_.end()) tools_.erase(it);
}

void ToolRegistry::register_builtin_tools() {
    for (auto& group :
         {make_file_tools(), make_search_tools(), make_edit_tools(), make_process_tools(),
          make_git_tools(), make_web_tools(), make_environment_tools(), make_skill_tools(),
          make_verification_tools(), make_repo_tools()}) {
        for (auto& tool : group)
            add(tool);
    }
    log_info(LogChannel::Tool, "tool set registered", {{"count", std::to_string(names().size())}});
}

ITool* ToolRegistry::find(std::string_view name) const {
    std::lock_guard lock(mutex_);
    const auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : it->second.get();
}

std::vector<std::string> ToolRegistry::names() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    result.reserve(tools_.size());
    for (const auto& [name, tool] : tools_)
        result.push_back(name);
    return result;
}

std::vector<ToolDefinition> ToolRegistry::definitions(bool include_mutating,
                                                      const TaskPolicy* policy) const {
    std::lock_guard lock(mutex_);
    std::vector<ToolDefinition> definitions;
    definitions.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        if (!include_mutating && tool->mutates()) continue;
        if (policy && !policy->allows_tool(name)) continue;
        definitions.push_back(tool->definition());
    }
    return definitions;
}

void ToolRegistry::set_observer(
    std::function<void(const ToolInvocation&, const ToolResult&)> observer) {
    std::lock_guard lock(mutex_);
    observer_ = std::move(observer);
}

std::vector<std::string> ToolRegistry::validate_arguments(const Json& schema,
                                                          const Json& arguments) {
    std::vector<std::string> problems;
    if (!schema.is_object()) return problems;

    if (!arguments.is_object()) {
        problems.emplace_back("arguments must be a JSON object");
        return problems;
    }

    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& entry : schema["required"]) {
            if (!entry.is_string()) continue;
            const auto key = entry.get<std::string>();
            if (!arguments.contains(key) || arguments[key].is_null()) {
                problems.push_back("missing required parameter '" + key + "'");
            }
        }
    }

    if (!schema.contains("properties") || !schema["properties"].is_object()) return problems;
    const auto& properties = schema["properties"];

    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!properties.contains(it.key())) {
            // Report unknown parameters but do not fail on them: the message is
            // enough for the model to correct itself next turn.
            problems.push_back("unknown parameter '" + it.key() + "'");
            continue;
        }
        const auto& definition = properties[it.key()];
        const auto expected = json_string(definition, "type");
        const auto& value = it.value();

        if (expected == "string" && !value.is_string() && !value.is_number() &&
            !value.is_boolean()) {
            problems.push_back("parameter '" + it.key() + "' should be a string");
        } else if (expected == "integer" || expected == "number") {
            const bool numeric_string =
                value.is_string() && !value.get<std::string>().empty() &&
                value.get<std::string>().find_first_not_of("+-0123456789.eE") == std::string::npos;
            if (!value.is_number() && !numeric_string && !value.is_boolean()) {
                problems.push_back("parameter '" + it.key() + "' should be a number");
            }
        } else if (expected == "boolean" && !value.is_boolean()) {
            const auto text = value.is_string() ? strings::to_lower(value.get<std::string>()) : "";
            if (text != "true" && text != "false" && !value.is_number()) {
                problems.push_back("parameter '" + it.key() + "' should be true or false");
            }
        } else if (expected == "array" && !value.is_array()) {
            problems.push_back("parameter '" + it.key() + "' should be an array");
        } else if (expected == "object" && !value.is_object()) {
            problems.push_back("parameter '" + it.key() + "' should be an object");
        }

        if (definition.contains("enum") && definition["enum"].is_array() && value.is_string()) {
            bool matched = false;
            std::vector<std::string> allowed;
            for (const auto& option : definition["enum"]) {
                if (!option.is_string()) continue;
                allowed.push_back(option.get<std::string>());
                if (strings::iequals(option.get<std::string>(), value.get<std::string>())) {
                    matched = true;
                }
            }
            if (!matched) {
                problems.push_back("parameter '" + it.key() +
                                   "' must be one of: " + strings::join(allowed, ", "));
            }
        }
    }
    return problems;
}

ToolResult ToolRegistry::invoke(const ToolInvocation& invocation, ToolContext& context) const {
    const auto started = std::chrono::steady_clock::now();

    auto finish = [&](ToolResult result) {
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        std::function<void(const ToolInvocation&, const ToolResult&)> observer;
        {
            std::lock_guard lock(mutex_);
            observer = observer_;
        }
        log_info(LogChannel::Tool, result.ok ? "tool succeeded" : "tool failed",
                 {{"tool", invocation.tool_name},
                  {"summary", result.summary},
                  {"ms", std::to_string(result.duration_ms)}});
        if (observer) observer(invocation, result);
        return result;
    };

    ITool* tool = find(invocation.tool_name);
    if (!tool) {
        auto available = names();
        if (context.task_policy) {
            std::erase_if(available, [&](const auto& name) {
                return !context.task_policy->allows_tool(name);
            });
        }
        return finish(ToolResult::failure(
            "unknown tool: " + invocation.tool_name,
            "There is no tool called '" + invocation.tool_name + "'. The available tools are: " +
                strings::join(available, ", ") + ". Call one of those instead."));
    }

    if (context.task_policy && !context.task_policy->allows_tool(invocation.tool_name)) {
        return finish(ToolResult::failure(
            invocation.tool_name + " is outside this task's tool scope",
            "The user requested file tools only. This task can use file reads, searches and "
            "edits; Git, environment, web, repository and process helpers are unavailable. "
            "Report any unperformed checks without claiming they passed."));
    }

    if ((read_only_ || (context.task_policy && context.task_policy->read_only())) &&
        tool->mutates()) {
        return finish(ToolResult::failure(
            invocation.tool_name + " is unavailable",
            "LocalCode is in read-only mode, so '" + invocation.tool_name +
                "' cannot be used. Inspect the project and explain what you would change."));
    }

    if (tool->requires_workspace() &&
        (context.workspace == nullptr || !context.workspace->is_open())) {
        return finish(ToolResult::failure(
            "no workspace selected",
            "'" + invocation.tool_name +
                "' needs an open workspace. Ask the user to select a project folder."));
    }

    if (context.cancel.cancelled()) {
        return finish(ToolResult::failure("cancelled", "The operation was cancelled."));
    }

    if (context.workspace && context.task_policy &&
        PathPolicy::canonicalize(context.workspace->root()) !=
            context.task_policy->paths().workspace_root()) {
        return finish(ToolResult::failure(
            "workspace changed during task",
            "The task's workspace changed. Stop this task and start a new one in the selected "
            "workspace; its existing permissions do not transfer to another project."));
    }

    const auto problems = validate_arguments(tool->parameters_schema(), invocation.arguments);
    // Keep scalar coercions lenient, but never silently replace a malformed
    // argument container with an empty list/object. That can run a different
    // command than the caller requested (e.g. Python without its script).
    bool fatal = false;
    for (const auto& problem : problems) {
        if (strings::starts_with(problem, "missing required")) fatal = true;
    }
    const auto schema = tool->parameters_schema();
    if (schema.contains("properties") && schema["properties"].is_object() &&
        invocation.arguments.is_object()) {
        for (auto field = invocation.arguments.begin(); field != invocation.arguments.end(); ++field) {
            if (!schema["properties"].contains(field.key())) continue;
            const auto type = json_string(schema["properties"][field.key()], "type");
            if ((type == "array" && field->is_object()) ||
                (type == "object" && !field->is_object())) fatal = true;
        }
    }
    if (fatal) {
        return finish(ToolResult::failure("invalid arguments for " + invocation.tool_name,
                                          "The call to '" + invocation.tool_name +
                                              "' was rejected: " + strings::join(problems, "; ") +
                                              ".\nThe expected parameters are:\n" +
                                              tool->parameters_schema().dump(2)));
    }

    try {
        auto result = tool->execute(invocation.arguments, context);
        if (!problems.empty() && result.ok) {
            result.content += "\n\n[note: " + strings::join(problems, "; ") + "]";
        }
        return finish(std::move(result));
    } catch (const std::exception& e) {
        // A tool throwing is a bug in LocalCode, not in the model. Report it as
        // a tool failure so the run continues, and log it loudly.
        log_error(LogChannel::Tool, "tool threw an exception",
                  {{"tool", invocation.tool_name}, {"what", e.what()}});
        return finish(ToolResult::failure("internal error in " + invocation.tool_name,
                                          std::string{"internal error: "} + e.what()));
    }
}

}   // namespace localcode
