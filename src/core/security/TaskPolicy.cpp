#include "TaskPolicy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>
#include <vector>

#include "../util/Strings.h"

namespace localcode {

bool TaskPolicy::allows_tool(std::string_view name) const {
    if (!file_tools_only_) return true;
    static constexpr std::array<std::string_view, 13> names{
        "read_file",  "read_file_range", "list_directory", "file_info",    "create_directory",
        "move_file",  "copy_file",       "delete_path",    "search_files", "search_text",
        "write_file", "replace_in_file", "apply_patch"};
    return std::find(names.begin(), names.end(), name) != names.end();
}

TaskPolicy::ToolScope TaskPolicy::requested_tool_scope(std::string_view request) {
    // Recognize explicit scope only in the genuine request. Examples in quoted
    // text/code and subsequent model/tool feedback do not grant permissions.
    std::string instructions;
    bool fenced = false;
    char quote = 0;
    for (std::size_t i = 0; i < request.size(); ++i) {
        if (!quote && request.substr(i, 3) == "```") {
            fenced = !fenced;
            instructions += ' ';
            i += 2;
            continue;
        }
        if (fenced) continue;
        const char c = request[i];
        if (quote) {
            if (c == quote && (i == 0 || request[i - 1] != '\\')) quote = 0;
            continue;
        }
        const bool apostrophe = c == '\'' && i > 0 && i + 1 < request.size() &&
                                std::isalnum(static_cast<unsigned char>(request[i - 1])) &&
                                std::isalnum(static_cast<unsigned char>(request[i + 1]));
        if ((c == '"' || c == '\'' || c == '`') && !apostrophe) {
            quote = c;
            instructions += ' ';
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (instructions.empty() || instructions.back() != ' ') instructions += ' ';
        } else {
            instructions += c;
        }
    }
    const auto lower = strings::to_lower(instructions);
    struct ScopePhrase { std::string_view text; ToolScope scope; };
    std::vector<std::pair<std::size_t, std::size_t>> negated_ranges;
    // Reading-only phrases must precede the generic substring "file tools only".
    for (const auto entry : {
             ScopePhrase{"only read-only file tools", ToolScope::ReadOnlyFileTools},
             ScopePhrase{"read-only file tools only", ToolScope::ReadOnlyFileTools},
             ScopePhrase{"only file reading tools", ToolScope::ReadOnlyFileTools},
             ScopePhrase{"file reading tools only", ToolScope::ReadOnlyFileTools},
             ScopePhrase{"seulement les outils de lecture de fichiers", ToolScope::ReadOnlyFileTools},
             ScopePhrase{"uniquement les outils de lecture de fichiers", ToolScope::ReadOnlyFileTools},
             ScopePhrase{"only file tools", ToolScope::FileTools},
             ScopePhrase{"file tools only", ToolScope::FileTools},
             ScopePhrase{"uniquement les outils de fichiers", ToolScope::FileTools},
             ScopePhrase{"uniquement des outils de fichiers", ToolScope::FileTools},
             ScopePhrase{"seulement les outils de fichiers", ToolScope::FileTools},
             ScopePhrase{"seulement les outils de lecture et de modification de fichiers", ToolScope::FileTools},
             ScopePhrase{"uniquement les outils de lecture et de modification de fichiers", ToolScope::FileTools}}) {
        const auto phrase = entry.text;
        std::size_t start = 0;
        while ((start = lower.find(phrase, start)) != std::string::npos) {
            const auto prefix = std::string_view(lower).substr(0, start);
            const auto end = start + std::string_view(phrase).size();
            // A negated longer phrase must not reappear as a positive generic
            // substring later ("don't use read-only file tools only").
            if (std::any_of(negated_ranges.begin(), negated_ranges.end(), [&](const auto& range) {
                    return start >= range.first && end <= range.second;
                })) {
                start = end;
                continue;
            }
            if ((start && std::isalnum(static_cast<unsigned char>(lower[start - 1]))) ||
                (end < lower.size() && std::isalnum(static_cast<unsigned char>(lower[end])))) {
                start = end;
                continue;
            }
            bool negated = false;
            for (const auto negative :
                 {"do not use ", "don't use ", "never use ", "not limited to ",
                  "not restricted to ", "do not restrict to ", "don't restrict to ",
                  "n'utilise pas ", "ne limite pas aux ", "ne te limite pas aux "}) {
                if (prefix.ends_with(negative)) negated = true;
            }
            if (!negated) return entry.scope;
            negated_ranges.emplace_back(start, end);
            start += std::string_view(phrase).size();
        }
    }
    return ToolScope::AllTools;
}

bool TaskPolicy::requests_file_tools_only(std::string_view request) {
    return requested_tool_scope(request) != ToolScope::AllTools;
}

}   // namespace localcode
