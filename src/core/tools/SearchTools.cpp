// ---------------------------------------------------------------------------
// Finding files by name and text within files.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <atomic>
#include <cctype>
#include <regex>

#include "../util/Log.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

constexpr std::size_t kMaxSearchFileBytes = 4u * 1024 * 1024;
constexpr std::size_t kMaxSearchCandidates = 100000;

// This is a navigation preference based only on metadata, not evidence that a
// file is authoritative or relevant. In particular, archived code under docs
// must not outrank ordinary product source solely because of its extension.
int search_file_kind(const std::string& path) {
    const auto slash = path.find('/');
    const auto first = path.substr(0, slash);
    if (first == "logs" || first == "log" || first == "artifacts" || first == "artifact" ||
        first == "proofs" || first == "captures" || first == "snapshots")
        return 3;
    if (first == "docs" || first == "doc" || first == "documentation") return 2;

    const auto name = slash == std::string::npos ? path : path.substr(path.find_last_of('/') + 1);
    const auto dot = name.find_last_of('.');
    const auto extension = dot == std::string::npos ? std::string{} : name.substr(dot);
    if (name == "cmakelists.txt" || name == "makefile" || name == "meson.build" ||
        name == "build" || name == "build.bazel" || name == "dockerfile")
        return 0;
    for (const auto* value :
         {".c",     ".cc",    ".cpp",     ".cxx",    ".h",    ".hh",   ".hpp",   ".hxx",
          ".m",     ".mm",    ".rs",      ".py",     ".pyi",  ".js",   ".jsx",   ".ts",
          ".tsx",   ".go",    ".java",    ".cs",     ".lua",  ".luau", ".swift", ".kt",
          ".kts",   ".scala", ".rb",      ".php",    ".sh",   ".ps1",  ".bat",   ".cmd",
          ".cmake", ".sln",   ".vcxproj", ".csproj", ".html", ".css",  ".scss",  ".sql"}) {
        if (extension == value) {
            // Only a top-level auxiliary directory changes this preference;
            // product paths such as src/core/tools retain their source role.
            return (first == "scripts" || first == "script" || first == "tools" ||
                    first == "tool" || first == "tests" || first == "test" || first == "spec" ||
                    first == "specs" || first == "benchmarks" || first == "benchmark" ||
                    first == "bench")
                       ? 1
                       : 0;
        }
    }
    for (const auto* value : {".json", ".toml", ".yaml", ".yml", ".ini", ".cfg", ".conf", ".xml"})
        if (extension == value) return 1;
    for (const auto* value : {".md", ".rst", ".adoc", ".txt"})
        if (extension == value) return 2;
    return 3;
}

int search_path_affinity(const std::string& path, const std::string& query) {
    if (query.empty() || query.size() > 256) return 0;
    const auto slash = path.find_last_of('/');
    const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    int score = name == query                           ? 1000
                : name.find(query) != std::string::npos ? 200
                : path.find(query) != std::string::npos ? 100
                                                        : 0;
    std::string token;
    int accepted = 0;
    const auto consume = [&] {
        if (token.size() >= 3 && token.size() <= 64 && accepted++ < 8)
            score += name.find(token) != std::string::npos   ? 20
                     : path.find(token) != std::string::npos ? 5
                                                             : 0;
        token.clear();
    };
    for (const auto c : query) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            token += c;
        else
            consume();
    }
    consume();
    return score;
}

void order_literal_search_candidates(std::vector<WorkspaceFile>& files, const std::string& query) {
    struct Ranked {
        WorkspaceFile file;
        int affinity;
        int kind;
    };

    std::vector<Ranked> ranked;
    ranked.reserve(files.size());
    const auto key = strings::to_lower(query);
    for (auto& file : files) {
        const auto path = strings::to_lower(file.relative_path);
        const auto affinity = search_path_affinity(path, key);
        const auto kind = search_file_kind(path);
        ranked.push_back({std::move(file), affinity, kind});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.affinity != b.affinity ? a.affinity > b.affinity : a.kind < b.kind;
    });
    files.clear();
    for (auto& candidate : ranked)
        files.push_back(std::move(candidate.file));
}

// --- search_files ----------------------------------------------------------
class SearchFilesTool final : public ITool {
public:
    std::string name() const override { return "search_files"; }

    std::string description() const override {
        return "Finds files in the workspace whose path matches a glob pattern, for example "
               "'*.cpp', 'src/**/render*' or 'CMakeLists.txt'. Use this when you know roughly "
               "what a file is called but not where it lives. To search inside file contents, "
               "use search_text instead.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"pattern", schema::string_param(
                             "Glob pattern matched against the workspace-relative path. "
                             "'*' matches any run of characters, '?' matches one. Matching is "
                             "case-insensitive.")},
             {"max_results",
              schema::integer_param("Maximum matches to return. Defaults to 100.", 1, 2000)}},
            {"pattern"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto pattern = arg_string(arguments, "pattern");
        if (pattern.empty()) {
            return ToolResult::failure("no pattern given", "The 'pattern' parameter is required.");
        }
        const auto max_results = static_cast<std::size_t>(arg_int(arguments, "max_results", 100));

        auto files = context.workspace->all_files(100000);
        if (!files) return ToolResult::failure("search failed", files.error().message);

        std::vector<std::string> matches;
        for (const auto& file : *files) {
            if (context.cancel.cancelled()) break;
            const auto& path = file.relative_path;
            // Match against the full relative path and against the bare filename,
            // so both "src/*.cpp" and "*.cpp" do what a user expects.
            const auto slash = path.find_last_of('/');
            const auto filename = slash == std::string::npos ? path : path.substr(slash + 1);
            if (strings::glob_match(pattern, path) || strings::glob_match(pattern, filename)) {
                matches.push_back(path);
                if (matches.size() >= max_results) break;
            }
        }

        std::string listing;
        for (const auto& match : matches)
            listing += "  " + match + "\n";

        auto result = ToolResult::success(
            "Found " + std::to_string(matches.size()) + " file(s) matching " + pattern,
            matches.empty() ? "No file in the workspace matches '" + pattern + "'."
                            : std::to_string(matches.size()) + " file(s) matching '" + pattern +
                                  "':\n" + listing);
        result.evidence.push_back(make_evidence(EvidenceKind::SearchPerformed, pattern,
                                                std::to_string(matches.size()) + " matches", true));
        return result;
    }
};

// --- search_text -----------------------------------------------------------
class SearchTextTool final : public ITool {
public:
    std::string name() const override { return "search_text"; }

    std::string description() const override {
        return "Searches literal text by default; set regex=true for patterns such as A|B or A.*B. "
               "Searches the contents of files in the workspace and returns matching lines with "
               "their file, line number and surrounding context. This is the fastest way to "
               "locate a symbol, an error message, a TODO or a call site. Prefer it over reading "
               "many files one by one. Binary files are skipped automatically.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"query", schema::string_param("Text to search for.")},
             {"regex", schema::boolean_param(
                           "Treat the query as an ECMAScript regular expression. Defaults to "
                           "false, which searches for the text literally.",
                           false)},
             {"case_sensitive", schema::boolean_param("Defaults to false.", false)},
             {"file_pattern",
              schema::string_param("Optional single glob, e.g. '*.cpp'. Spaces are literal, "
                                   "not glob separators.")},
             {"context_lines",
              schema::integer_param("Lines of context around each match. Defaults to 2.", 0, 10)},
             {"max_results",
              schema::integer_param("Maximum matches to return. Defaults to 60.", 1, 500)}},
            {"query"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto query = arg_string(arguments, "query");
        if (query.empty()) {
            return ToolResult::failure("no query given", "The 'query' parameter is required.");
        }

        const bool use_regex = arg_bool(arguments, "regex", false);
        const bool case_sensitive = arg_bool(arguments, "case_sensitive", false);
        const auto file_pattern = arg_string(arguments, "file_pattern");
        const auto context_lines = static_cast<std::size_t>(arg_int(arguments, "context_lines", 2));
        const auto max_results = static_cast<std::size_t>(arg_int(arguments, "max_results", 60));

        std::optional<std::regex> expression;
        if (use_regex) {
            try {
                auto flags = std::regex::ECMAScript | std::regex::optimize;
                if (!case_sensitive) flags |= std::regex::icase;
                expression.emplace(query, flags);
            } catch (const std::regex_error& e) {
                return ToolResult::failure(
                    "invalid regular expression",
                    std::string{"The regular expression could not be compiled: "} + e.what() +
                        "\nEither fix the expression or set regex=false to search literally.");
            }
        }

        auto files = context.workspace->all_files(kMaxSearchCandidates);
        if (!files) return ToolResult::failure("search failed", files.error().message);
        const bool enumeration_capped = files->size() >= kMaxSearchCandidates;
        if (!use_regex && !context.cancel.cancelled())
            order_literal_search_candidates(*files, query);

        std::string output;
        std::size_t match_count = 0;
        std::size_t files_with_matches = 0;
        std::size_t files_scanned = 0;
        bool hit_limit = false;

        for (const auto& file : *files) {
            if (context.cancel.cancelled()) break;
            if (hit_limit) break;
            if (!file_pattern.empty()) {
                const auto slash = file.relative_path.find_last_of('/');
                const auto filename = slash == std::string::npos
                                          ? file.relative_path
                                          : file.relative_path.substr(slash + 1);
                if (!strings::glob_match(file_pattern, file.relative_path) &&
                    !strings::glob_match(file_pattern, filename)) {
                    continue;
                }
            }
            if (file.size > kMaxSearchFileBytes) continue;

            const auto absolute = context.workspace->root() / file.relative_path;
            auto text = fs::read_text(absolute, kMaxSearchFileBytes);
            if (!text) continue;   // binary or unreadable: skip silently
            ++files_scanned;

            const auto lines = strings::split_lines(text->content);
            bool file_header_written = false;

            for (std::size_t index = 0; index < lines.size(); ++index) {
                const auto& line = lines[index];
                bool matched = false;
                if (expression) {
                    matched = std::regex_search(line, *expression);
                } else if (case_sensitive) {
                    matched = line.find(query) != std::string::npos;
                } else {
                    matched = strings::icontains(line, query);
                }
                if (!matched) continue;

                if (!file_header_written) {
                    output += "\n" + file.relative_path + "\n";
                    file_header_written = true;
                    ++files_with_matches;
                }

                const std::size_t first = index >= context_lines ? index - context_lines : 0;
                const std::size_t last = std::min(lines.size() - 1, index + context_lines);
                for (std::size_t k = first; k <= last; ++k) {
                    char prefix[24];
                    std::snprintf(prefix, sizeof prefix, "%s%6zu\t", k == index ? ">" : " ", k + 1);
                    output += prefix;
                    output += strings::utf8_truncate(lines[k], 400);
                    output.push_back('\n');
                }
                output += "  --\n";

                if (++match_count >= max_results) {
                    hit_limit = true;
                    break;
                }
            }
        }

        if (context.cancel.cancelled()) {
            return ToolResult::failure(
                "Search was cancelled",
                "The search was interrupted; no complete absence result was established.");
        }
        if (files_scanned == 0) {
            const std::string enumeration_note =
                enumeration_capped
                    ? " Workspace file enumeration reached its 100000-candidate cap; further "
                      "files may be unlisted."
                    : "";
            return ToolResult::failure(
                "No text files were searched",
                "No text files were inspected, so this search did not establish that the query "
                "is absent. Check the file filter, workspace and ignored/binary/large files. "
                "file_pattern accepts a single glob; spaces are literal filename characters. "
                "Use one glob per call or omit the filter to search eligible workspace text "
                "files." +
                    enumeration_note);
        }

        std::string summary_line = "Found " + std::to_string(match_count) + " match(es) in " +
                                   std::to_string(files_with_matches) + " file(s)";
        if (hit_limit) summary_line += " (stopped at the result limit)";

        std::string content =
            summary_line + " for " + (use_regex ? "regex " : "") + "'" + query + "'";
        if (!file_pattern.empty()) content += " in files matching '" + file_pattern + "'";
        content += ".\nSearched " + std::to_string(files_scanned) + " text file(s).\n";
        if (!use_regex)
            content +=
                "Order: literal path affinity, then ordinary source/build, "
                "auxiliary/config, prose and artifacts; stable ties.\n";
        if (hit_limit)
            content +=
                "Result cap reached; other files or matches may be uninspected. This is "
                "not exhaustive workspace coverage.\n";
        if (enumeration_capped)
            content +=
                "Workspace file enumeration reached its 100000-candidate cap; further "
                "files may be unlisted.\n";
        content += match_count == 0
                       ? "\nNo matches. The text does not appear in any searched file - do not "
                         "assume it exists elsewhere without checking."
                       : output;
        if (match_count == 0 && !use_regex)
            content +=
                "\nThis was a literal search. If you intended a pattern such as A|B or "
                "A.*B, set regex=true. Otherwise narrow the literal query.";

        auto result = ToolResult::success(summary_line, content);
        result.truncated = hit_limit || enumeration_capped;
        result.evidence.push_back(make_evidence(
            EvidenceKind::SearchPerformed, query,
            std::to_string(match_count) + " matches in " + std::to_string(files_scanned) + " files",
            true));
        return result;
    }
};

}   // namespace

std::vector<ToolPtr> make_search_tools() {
    return {std::make_shared<SearchFilesTool>(), std::make_shared<SearchTextTool>()};
}

}   // namespace localcode
