#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../inference/ChatTypes.h"
#include "../util/Result.h"

namespace localcode {

// A per-run view of the already permitted tools. Loading a schema is metadata
// only: execution still goes through ToolRegistry and the frozen TaskPolicy.
class ToolCatalog {
public:
    explicit ToolCatalog(std::vector<ToolDefinition> permitted, bool stable_dispatch = false);
    [[nodiscard]] ToolDefinition selector() const;
    [[nodiscard]] std::vector<ToolDefinition> definitions() const;
    [[nodiscard]] const std::vector<ToolDefinition>& loaded_definitions() const { return active_; }
    [[nodiscard]] bool stable_dispatch() const { return stable_dispatch_; }
    Result<ToolCall> expand_call(const ToolCall& call) const;
    [[nodiscard]] static std::string text_schemas(const std::vector<ToolDefinition>& tools);
    [[nodiscard]] static std::string instructions();

    // Replace the active set atomically. Never trim properties to make it fit;
    // reject an oversized selection so the model can request fewer tools.
    Result<std::string> select(
        const Json& arguments, int budget,
        const std::function<int(const std::vector<ToolDefinition>&)>& cost);

private:
    std::vector<ToolDefinition> permitted_;
    std::vector<ToolDefinition> active_;
    bool stable_dispatch_ = false;
};

} // namespace localcode
