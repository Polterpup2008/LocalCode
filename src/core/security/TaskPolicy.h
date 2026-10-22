#pragma once

#include "CommandPolicy.h"
#include "PathPolicy.h"

namespace localcode {

// A task uses the permissions selected when it starts. Later Settings/workspace
// changes belong to the next task, never to an already running tool sequence.
class TaskPolicy final {
public:
    enum class ToolScope { AllTools, FileTools, ReadOnlyFileTools };

    TaskPolicy(const PathPolicy& paths, const CommandPolicy& commands, bool read_only = false,
               bool file_tools_only = false)
        : paths_(paths),
          commands_(commands),
          read_only_(read_only),
          file_tools_only_(file_tools_only) {
        commands_.set_path_policy(&paths_);
    }

    TaskPolicy(const TaskPolicy&) = delete;
    TaskPolicy& operator=(const TaskPolicy&) = delete;

    [[nodiscard]] const PathPolicy& paths() const { return paths_; }

    [[nodiscard]] const CommandPolicy& commands() const { return commands_; }

    [[nodiscard]] bool read_only() const { return read_only_; }

    [[nodiscard]] bool file_tools_only() const { return file_tools_only_; }

    [[nodiscard]] bool allows_tool(std::string_view name) const;
    [[nodiscard]] static bool requests_file_tools_only(std::string_view request);
    [[nodiscard]] static ToolScope requested_tool_scope(std::string_view request);

private:
    PathPolicy paths_;
    CommandPolicy commands_;
    bool read_only_;
    bool file_tools_only_;
};

}   // namespace localcode
