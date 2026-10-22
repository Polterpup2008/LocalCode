#include "Tool.h"

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {

std::string_view to_string(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::FileRead:           return "file_read";
        case EvidenceKind::FileWritten:        return "file_written";
        case EvidenceKind::DirectoryListed:    return "directory_listed";
        case EvidenceKind::SearchPerformed:    return "search";
        case EvidenceKind::CommandRun:         return "command_run";
        case EvidenceKind::BuildAttempted:     return "build";
        case EvidenceKind::TestsRun:           return "tests";
        case EvidenceKind::EnvironmentChecked: return "environment";
        case EvidenceKind::GitInspected:       return "git";
        case EvidenceKind::WebFetched:         return "web";
    }
    return "command_run";
}

std::string Evidence::describe() const {
    std::string out{to_string(kind)};
    out += positive ? " ok: " : " failed: ";
    out += subject;
    if (!detail.empty()) out += " (" + detail + ")";
    return out;
}

ToolResult ToolResult::success(std::string summary, std::string content) {
    ToolResult result;
    result.ok = true;
    result.summary = std::move(summary);
    result.content = std::move(content);
    return result;
}

ToolResult ToolResult::failure(std::string summary, std::string error) {
    ToolResult result;
    result.ok = false;
    result.summary = std::move(summary);
    result.error = std::move(error);
    return result;
}

std::string ToolResult::to_model_text() const {
    if (!ok) {
        std::string out = "ERROR: " + error;
        if (!content.empty()) out += "\n\n" + content;
        return out;
    }
    std::string out = content;
    if (!full_log_path.empty()) {
        out += "\n\n[full output: " + full_log_path.string() + "]";
    }
    return out;
}

namespace schema {

Json object(std::initializer_list<std::pair<std::string, Json>> properties,
            std::vector<std::string> required) {
    Json result;
    result["type"] = "object";
    result["properties"] = Json::object();
    for (const auto& [name, definition] : properties) {
        result["properties"][name] = definition;
    }
    if (!required.empty()) result["required"] = required;
    // Small models improvise parameter names; refusing extras makes the server's
    // schema validation catch it instead of the tool silently ignoring input.
    result["additionalProperties"] = false;
    return result;
}

Json string_param(std::string description) {
    return Json{{"type", "string"}, {"description", std::move(description)}};
}

Json string_enum(std::string description, std::vector<std::string> values) {
    return Json{{"type", "string"},
                {"description", std::move(description)},
                {"enum", std::move(values)}};
}

Json integer_param(std::string description, std::optional<long long> minimum,
                   std::optional<long long> maximum) {
    Json result{{"type", "integer"}, {"description", std::move(description)}};
    if (minimum) result["minimum"] = *minimum;
    if (maximum) result["maximum"] = *maximum;
    return result;
}

Json boolean_param(std::string description, bool default_value) {
    return Json{{"type", "boolean"},
                {"description", std::move(description)},
                {"default", default_value}};
}

Json array_of_strings(std::string description) {
    return Json{{"type", "array"},
                {"description", std::move(description)},
                {"items", Json{{"type", "string"}}}};
}

}  // namespace schema
}  // namespace localcode
