#pragma once
// ---------------------------------------------------------------------------
// The tool registry.
//
// Holds the compact tool set, dispatches calls by name, and is the single place
// where a tool invocation is validated, timed, logged and recorded in history.
// Nothing calls a tool's execute() directly.
// ---------------------------------------------------------------------------
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Tool.h"

namespace localcode {

struct ToolInvocation {
    std::string tool_name;
    Json arguments;
    std::string call_id;
};

class ToolRegistry {
public:
    ToolRegistry();

    // Registers the standard tool set.
    void register_builtin_tools();
    void add(ToolPtr tool);
    void remove(std::string_view name);

    [[nodiscard]] ITool* find(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> names() const;

    // Definitions to advertise to the model. Read-only mode drops the mutating
    // tools entirely rather than letting the model call them and be refused.
    [[nodiscard]] std::vector<ToolDefinition> definitions(bool include_mutating = true,
                                                          const TaskPolicy* policy = nullptr) const;

    // Executes one call: resolves the tool, validates arguments against the
    // declared schema, runs it, and returns a result that is always safe to feed
    // back to the model - an unknown tool or bad arguments come back as an
    // instructive error rather than an exception.
    ToolResult invoke(const ToolInvocation& invocation, ToolContext& context) const;

    void set_read_only(bool read_only) { read_only_ = read_only; }

    [[nodiscard]] bool read_only() const { return read_only_; }

    // Called after every invocation, for the history repository and the UI.
    void set_observer(std::function<void(const ToolInvocation&, const ToolResult&)> observer);

    // Minimal JSON Schema check: required properties present, types plausible.
    // Deliberately lenient about number-vs-string, because small models emit
    // "8" for an integer often enough that being strict would waste turns.
    static std::vector<std::string> validate_arguments(const Json& schema, const Json& arguments);

private:
    mutable std::mutex mutex_;
    std::map<std::string, ToolPtr, std::less<>> tools_;
    std::atomic<bool> read_only_{false};
    std::function<void(const ToolInvocation&, const ToolResult&)> observer_;
};

// Factory functions for the built-in tools, grouped by file.
std::vector<ToolPtr> make_file_tools();
std::vector<ToolPtr> make_search_tools();
std::vector<ToolPtr> make_edit_tools();
std::vector<ToolPtr> make_process_tools();
std::vector<ToolPtr> make_git_tools();
std::vector<ToolPtr> make_repo_tools();
std::vector<ToolPtr> make_web_tools();
std::vector<ToolPtr> make_environment_tools();
std::vector<ToolPtr> make_skill_tools();
std::vector<ToolPtr> make_verification_tools();

}   // namespace localcode
