#pragma once
// Internal bounded inspection receipt shared by reads and failed exact edits.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "../platform/FileSystem.h"
#include "../util/Strings.h"
#include "Tool.h"

namespace localcode::tool_support {

inline void annotate_source_controls(ToolResult& result, const fs::SourceRead& source) {
    // A regex containing a literal backspace can look like a word boundary in
    // ordinary source output. Preserve the actual source and its hash, but make
    // nonprinting bytes visible in a separate, bounded inspection receipt.
    constexpr std::size_t kMaxControlDetails = 16;
    Json details = Json::array();
    std::size_t total = 0;
    std::size_t line = source.start_line;
    std::size_t column = 1;
    std::vector<std::string> locations;
    const auto hidden_control = [](unsigned char byte) {
        return (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') || byte == 0x7f;
    };
    for (const unsigned char byte : source.file.content) {
        if (hidden_control(byte)) {
            ++total;
            if (details.size() < kMaxControlDetails) {
                char codepoint[7];
                std::snprintf(codepoint, sizeof codepoint, "U+%04X", static_cast<unsigned>(byte));
                details.push_back(
                    {{"line", line}, {"utf8_byte_column", column}, {"codepoint", codepoint}});
                locations.push_back("line " + std::to_string(line) + ", UTF-8 byte column " +
                                    std::to_string(column) + ": " + codepoint);
            }
        }
        if (byte == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    result.structured["source_control_count"] = total;
    result.structured["source_controls"] = std::move(details);
    if (total != 0) {
        // Keep diagnostic details at the tail for ordinary condensation.
        // These annotations are not numbered source lines.
        result.content += "\nNonprinting ASCII controls in returned source (first " +
                          std::to_string(locations.size()) + " of " + std::to_string(total) +
                          "; source bytes preserved):\n" + strings::join(locations, "; ") + "\n";
    }
    if (total == 0) return;

    // An exact edit needs the entire source line, including printable text
    // around a hidden byte. Offer a JSON literal separately from the unchanged
    // source. Decoding it once yields source text, not a fuzzy edit pattern.
    constexpr std::size_t kMaxQuotedLines = 4;
    constexpr std::size_t kMaxQuotedBytes = 1024;
    Json quoted = Json::array();
    std::size_t affected_lines = 0, encoded_bytes = 0;
    std::size_t line_limit = 0, partial = 0, byte_limit = 0;
    const auto& content = source.file.content;
    line = source.start_line;
    for (std::size_t start = 0; start < content.size(); ++line) {
        const auto end = content.find('\n', start);
        const auto length = end == std::string::npos ? content.size() - start : end - start;
        const std::string_view source_line{content.data() + start, length};
        if (std::any_of(source_line.begin(), source_line.end(), hidden_control)) {
            ++affected_lines;
            if (affected_lines > kMaxQuotedLines) {
                ++line_limit;
            } else if (source.last_line_partial && line == source.end_line) {
                ++partial;
            } else if (source_line.size() + 2 > kMaxQuotedBytes - encoded_bytes) {
                ++byte_limit;
            } else {
                auto encoded = Json(std::string{source_line}).dump();
                // DEL is valid unescaped JSON, but still invisible in source.
                encoded = strings::replace_all(encoded, std::string(1, '\x7f'), "\\u007f");
                if (encoded.size() > kMaxQuotedBytes - encoded_bytes) {
                    ++byte_limit;
                } else {
                    encoded_bytes += encoded.size();
                    quoted.push_back({{"line", line}, {"json_string", std::move(encoded)}});
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    result.structured["source_control_line_count"] = affected_lines;
    result.structured["source_control_lines"] = quoted;
    result.structured["source_control_lines_omitted"] = {
        {"line_limit", line_limit}, {"partial", partial}, {"byte_limit", byte_limit}};
    result.content +=
        "\nExact source line JSON (decode once; excludes LF; up to 4 lines / 1024 encoded "
        "bytes):\n";
    for (const auto& entry : quoted)
        result.content += "line " + std::to_string(entry.at("line").get<std::size_t>()) + ": " +
                          entry.at("json_string").get<std::string>() + "\n";
    if (line_limit != 0 || partial != 0 || byte_limit != 0)
        result.content += "JSON lines omitted: " + std::to_string(line_limit) +
                          " beyond first 4; " + std::to_string(partial) + " partial; " +
                          std::to_string(byte_limit) +
                          " over byte limit. No JSON line is truncated.\n";
}

}   // namespace localcode::tool_support
