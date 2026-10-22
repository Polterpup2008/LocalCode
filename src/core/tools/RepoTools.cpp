// ---------------------------------------------------------------------------
// The repository tools.
//
// Five of them, not fifteen. A small model given twenty overlapping tools picks
// badly and then justifies the pick; a handful with clear edges is worth more
// than complete coverage of every question that could be asked.
//
// They all answer from the index, and they all say where an answer came from.
// When the index is empty or absent they say that too - answering "no results"
// out of an index that was never built is the most misleading thing they could
// do, because it reads as "that does not exist in this project".
// ---------------------------------------------------------------------------
#include <algorithm>

#include "repo/RepoIndex.h"
#include "repo/RepoQuery.h"
#include "tools/Tool.h"
#include "tools/ToolRegistry.h"
#include "util/Log.h"
#include "util/Strings.h"
#include "workspace/Workspace.h"

namespace localcode {
namespace {

using repo::Evidence;
using repo::RepoIndex;
using repo::RepoQuery;
using repo::SearchHit;

// Every repository tool needs the same two things and fails the same way
// without them.
struct Ready {
    RepoIndex* index = nullptr;
    std::filesystem::path root;
    ToolResult failure;
    bool ok = false;
};

Ready prepare(ToolContext& context, std::string_view tool) {
    Ready ready;
    if (context.workspace == nullptr || !context.workspace->is_open()) {
        ready.failure = ToolResult::failure(std::string{tool},
                                            "no project folder is open, so there is nothing to "
                                            "search. Open one first.");
        return ready;
    }
    if (context.repo == nullptr || !context.repo->is_open()) {
        ready.failure = ToolResult::failure(
            std::string{tool},
            "the repository index is not available for this workspace. Fall back to reading and "
            "searching files directly - do not treat this as the project being empty.");
        return ready;
    }
    if (context.repo->state() == repo::IndexState::Empty) {
        ready.failure = ToolResult::failure(
            std::string{tool},
            "the repository has not been indexed yet. Fall back to reading and searching files "
            "directly - an empty index is not evidence that the project is empty.");
        return ready;
    }
    if (context.repo->state() != repo::IndexState::Ready) {
        ready.failure = ToolResult::failure(
            std::string{tool},
            "the repository index is incomplete, refreshing, or failed. Read and search files "
            "directly until a complete refresh finishes; cached results are not current evidence.");
        return ready;
    }
    ready.index = context.repo;
    ready.root = context.workspace->root();
    ready.ok = true;
    return ready;
}

std::string evidence_note(Evidence evidence) {
    switch (evidence) {
        case Evidence::Structural: return " [structural]";
        case Evidence::Verified: return " [verified]";
        case Evidence::Heuristic: return " [guess]";
        case Evidence::Lexical: break;
    }
    return "";
}

// --- repo_overview ---------------------------------------------------------

class RepoOverviewTool : public ITool {
public:
    std::string name() const override { return "repo_overview"; }

    std::string description() const override {
        return "Languages, build system, source and test roots, entry points, and known "
               "build and test commands. Ask first to orient yourself.";
    }

    Json parameters_schema() const override { return schema::object({}, {}); }

    ToolResult execute(const Json& /*arguments*/, ToolContext& context) override {
        auto ready = prepare(context, "repo_overview");
        if (!ready.ok) return ready.failure;

        auto overview = ready.index->overview();
        if (!overview || !overview->has_value()) {
            return ToolResult::failure("repo_overview", "the repository index has no summary yet");
        }
        const auto& map = **overview;

        std::string out;
        auto line = [&out](std::string_view label, const std::vector<std::string>& values) {
            if (values.empty()) return;
            out += std::string{label} + ": ";
            for (std::size_t i = 0; i < values.size() && i < 12; ++i) {
                if (i != 0) out += ", ";
                out += values[i];
            }
            if (values.size() > 12) out += ", ...";
            out += "\n";
        };

        out += "languages: ";
        for (std::size_t i = 0; i < map.languages.size() && i < 6; ++i) {
            if (i != 0) out += ", ";
            out += map.languages[i].language + " (" + std::to_string(map.languages[i].files) +
                   " files, " + std::to_string(map.languages[i].lines) + " lines)";
        }
        out += "\n";

        line("build systems", map.build_systems);
        line("package managers", map.package_managers);
        line("source roots", map.source_roots);
        line("test roots", map.test_roots);
        line("entry points", map.entry_points);
        line("manifests", map.manifests);
        line("documentation", map.documentation);

        out += "indexed: " + std::to_string(map.indexed_files) + " files, " +
               std::to_string(map.total_lines) + " lines, " + std::to_string(map.symbol_count) +
               " symbols";
        if (map.skipped_files != 0) {
            out += " (" + std::to_string(map.skipped_files) + " skipped as binary or oversized)";
        }
        out += "\n";
        if (map.truncated) {
            out += "NOTE: the scan hit its limit, so these counts are a floor, not a total.\n";
        }

        if (auto commands = ready.index->store().commands(); commands && !commands->empty()) {
            out += "\ncommands found:\n";
            for (const auto& command : *commands) {
                out += "  " + command.purpose + ": " + command.command;
                out += "  (from " + command.source;
                if (command.verified) out += ", VERIFIED by running it";
                out += ")\n";
            }
            out +=
                "Commands marked VERIFIED have been run successfully here. The others are what "
                "the project's files imply and have not been tried.\n";
        }

        return ToolResult::success(
            "Repository overview (" + std::to_string(map.indexed_files) + " files)", out);
    }
};

// --- repo_search -----------------------------------------------------------

class RepoSearchTool : public ITool {
public:
    std::string name() const override { return "repo_search"; }

    std::string description() const override {
        return "Search literal text by default; set regex=true for patterns such as A|B or A.*B. "
               "Search indexed files for text, or find files by name. Use instead of "
               "grep.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"query", schema::string_param("What to look for.")},
             {"kind",
              schema::string_enum("Search contents, or match file paths.", {"text", "file"})},
             {"regex", schema::boolean_param("Treat the query as a regex.", false)},
             {"path_contains", schema::string_param("Restrict to paths containing this.")}},
            {"query"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        auto ready = prepare(context, "repo_search");
        if (!ready.ok) return ready.failure;

        const auto query = arguments.value("query", std::string{});
        if (strings::trim(query).empty()) {
            return ToolResult::failure("repo_search", "give me something to search for");
        }
        const auto kind = arguments.value("kind", std::string{"text"});
        constexpr std::size_t limit = 20;

        RepoQuery search{ready.index->store(), ready.root};
        Result<std::vector<SearchHit>> hits = std::vector<SearchHit>{};
        repo::SearchStats covered;

        if (kind == "file") {
            hits = search.find_files(query, limit);
        } else {
            repo::SearchOptions options;
            options.max_hits = limit;
            options.regex = arguments.value("regex", false);
            options.path_contains = arguments.value("path_contains", std::string{});
            hits = search.search_text(query, options, &covered);
        }
        if (!hits) return ToolResult::failure("repo_search", hits.error().message);
        if (hits->empty()) {
            const std::string mode_hint =
                kind != "file" && !arguments.value("regex", false)
                    ? " This was a literal search; set regex=true if a pattern such as A|B or A.*B "
                      "was intended."
                    : "";
            if (!covered.complete()) {
                return ToolResult::success(
                    "No match for " + query + " in the part that was searched",
                    "No match in the " + std::to_string(covered.files_read) +
                        " file(s) searched, but the search stopped before covering the "
                        "repository. This is NOT evidence that the project does not contain it. "
                        "Narrow the search with path_contains and try again." +
                        mode_hint);
            }
            return ToolResult::success("No match for " + query,
                                       "Nothing in the indexed files matches that. The index "
                                       "covers source, tests, manifests and documentation - not "
                                       "build output or dependencies." +
                                           mode_hint);
        }

        std::string out;
        for (const auto& hit : *hits) {
            out += hit.file;
            if (hit.line != 0) out += ":" + std::to_string(hit.line);
            out += evidence_note(hit.evidence);
            out += "  - " + hit.reason + "\n";
            if (!hit.preview.empty()) out += "    " + hit.preview + "\n";
        }
        // A search that gave up part way through must say so. Otherwise "4
        // matches" reads as "there are four", and the model stops looking.
        if (!covered.complete()) {
            out += "\nINCOMPLETE: this search stopped after reading " +
                   std::to_string(covered.files_read) + " file(s)";
            out += covered.hit_file_limit ? " because it reached its file limit."
                                          : " because it had found enough matches to rank.";
            out +=
                " There may be more. Narrow it with path_contains, or use a more specific "
                "query.\n";
        }
        return ToolResult::success("Found " + std::to_string(hits->size()) + " match(es) for " +
                                       query + (covered.complete() ? "" : " (incomplete)"),
                                   out);
    }
};

// --- repo_symbol -----------------------------------------------------------

class RepoSymbolTool : public ITool {
public:
    std::string name() const override { return "repo_symbol"; }

    std::string description() const override {
        return "Where a function, class, type or build target is defined, or what uses it.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"name", schema::string_param("Symbol name; a fragment falls back to substring.")},
             {"mode", schema::string_enum("Where it is defined, or what uses it.",
                                          {"definition", "references"})},
             {"file", schema::string_param("With mode 'references', a file to find includes of "
                                           "instead of a symbol.")}},
            {});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        auto ready = prepare(context, "repo_symbol");
        if (!ready.ok) return ready.failure;

        const auto name = strings::trim(arguments.value("name", std::string{}));
        const auto file = strings::trim(arguments.value("file", std::string{}));
        const auto mode = arguments.value("mode", std::string{"definition"});
        if (name.empty() && file.empty()) {
            return ToolResult::failure("repo_symbol", "give me a symbol name or a file");
        }

        RepoQuery search{ready.index->store(), ready.root};

        if (mode == "references") {
            std::string out;
            std::size_t count = 0;

            if (!file.empty()) {
                auto references = ready.index->store().references_to_file(file, 30);
                if (!references) {
                    return ToolResult::failure("repo_symbol", references.error().message);
                }
                for (const auto& reference : *references) {
                    out += reference.from_file + ":" + std::to_string(reference.line) +
                           evidence_note(reference.evidence) + "  - includes it\n";
                    ++count;
                }
            }
            if (!name.empty()) {
                auto hits = search.find_references(name, 30);
                if (!hits) return ToolResult::failure("repo_symbol", hits.error().message);
                for (const auto& hit : *hits) {
                    out += hit.file + ":" + std::to_string(hit.line) + evidence_note(hit.evidence) +
                           "  - " + hit.reason + "\n";
                    if (!hit.preview.empty()) out += "    " + hit.preview + "\n";
                    ++count;
                }
            }

            if (count == 0) {
                return ToolResult::success("No references found",
                                           "Nothing in the indexed files refers to that.");
            }
            out +=
                "\nThese are textual matches, not a compiler's call graph: a name appearing in an "
                "unrelated context is listed, and a use through a macro or alias may not be.\n";
            return ToolResult::success("Found " + std::to_string(count) + " reference(s)", out);
        }

        if (name.empty()) {
            return ToolResult::failure("repo_symbol", "give me a symbol name");
        }
        auto symbols = search.find_symbol(name, 20);
        if (!symbols) return ToolResult::failure("repo_symbol", symbols.error().message);
        if (symbols->empty()) {
            return ToolResult::success(
                "No definition of " + name,
                "No definition of that name is in the index. It may be in a language LocalCode "
                "does not read structurally, generated at build time, or from a dependency. "
                "repo_search will find the text if it is there.");
        }

        std::string out;
        for (const auto& symbol : *symbols) {
            out += std::string{to_string(symbol.kind)} + "  " + symbol.name;
            if (!symbol.container.empty()) out += "  in " + symbol.container;
            out += "\n  " + symbol.file + ":" + std::to_string(symbol.line);
            out += evidence_note(symbol.evidence) + "\n";
            if (!symbol.signature.empty()) out += "  " + symbol.signature + "\n";
        }
        out +=
            "\nRead from the source, not compiled: a definition inside a conditional block may be "
            "listed even if it is not the one in force.\n";
        return ToolResult::success(
            "Found " + std::to_string(symbols->size()) + " definition(s) of " + name, out);
    }
};

// --- repo_tests_for --------------------------------------------------------

class RepoTestsForTool : public ITool {
public:
    std::string name() const override { return "repo_tests_for"; }

    std::string description() const override {
        return "Test files covering a source file, and the command that runs them.";
    }

    Json parameters_schema() const override {
        return schema::object({{"file", schema::string_param("Source file, workspace-relative.")}},
                              {"file"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        auto ready = prepare(context, "repo_tests_for");
        if (!ready.ok) return ready.failure;

        const auto file = strings::trim(arguments.value("file", std::string{}));
        if (file.empty()) return ToolResult::failure("repo_tests_for", "give me a file");
        constexpr std::size_t limit = 15;

        RepoQuery search{ready.index->store(), ready.root};
        auto hits = search.tests_for(file, limit);
        if (!hits) return ToolResult::failure("repo_tests_for", hits.error().message);

        std::string out;
        if (hits->empty()) {
            out = "No test file in the index appears to cover that one.\n";
        } else {
            for (const auto& hit : *hits) {
                out += hit.file + evidence_note(hit.evidence) + "  - " + hit.reason + "\n";
            }
            out +=
                "\nThese associations are inferred from includes and naming, not from coverage "
                "data. A test that exercises this file indirectly will not be listed.\n";
        }

        if (auto commands = ready.index->store().commands()) {
            for (const auto& command : *commands) {
                if (command.purpose != "test") continue;
                out += "\ntest command: " + command.command + "  (from " + command.source;
                if (command.verified) out += ", VERIFIED";
                out += ")\n";
                break;
            }
        }
        return ToolResult::success(
            "Tests for " + file + ": " + std::to_string(hits->size()) + " candidate(s)", out);
    }
};

// --- repo_fact -------------------------------------------------------------

class RepoFactTool : public ITool {
public:
    std::string name() const override { return "repo_fact"; }

    std::string description() const override {
        return "Read or record a durable repository fact with its provenance.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"action", schema::string_enum("Default 'list'.", {"get", "list", "set"})},
             {"key", schema::string_param("The fact's name, for 'get' and 'set'.")},
             {"value", schema::string_param("The fact's value, for 'set'.")},
             {"source", schema::string_param("How you established it, for 'set'. Required.")},
             {"depends_on",
              schema::array_of_strings("Paths this rests on; the fact is dropped when they "
                                       "change.")}},
            {});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        auto ready = prepare(context, "repo_fact");
        if (!ready.ok) return ready.failure;

        const auto action = arguments.value("action", std::string{"list"});
        auto& store = ready.index->store();

        if (action == "list") {
            auto facts = store.facts();
            if (!facts) return ToolResult::failure("repo_fact", facts.error().message);
            if (facts->empty()) {
                return ToolResult::success("No repository facts recorded",
                                           "Nothing has been established about this repository "
                                           "yet.");
            }
            // Every other tool here bounds what it returns; this one did not,
            // and facts accumulate across runs.
            constexpr std::size_t kMaxListed = 40;
            std::string out;
            std::size_t shown = 0;
            for (const auto& fact : *facts) {
                if (shown++ >= kMaxListed) break;
                out += fact.key + " = " + fact.value + evidence_note(fact.evidence) + "\n  from " +
                       fact.source + "\n";
            }
            if (facts->size() > kMaxListed) {
                out += "\n... and " + std::to_string(facts->size() - kMaxListed) +
                       " more. Ask for one by name with action 'get'.\n";
            }
            return ToolResult::success(std::to_string(facts->size()) + " repository fact(s)", out);
        }

        const auto key = strings::trim(arguments.value("key", std::string{}));
        if (key.empty()) return ToolResult::failure("repo_fact", "give me a key");

        if (action == "get") {
            auto fact = store.fact(key);
            if (!fact) return ToolResult::failure("repo_fact", fact.error().message);
            if (!fact->has_value()) {
                return ToolResult::success("No fact called " + key,
                                           "Nothing has been recorded under that name.");
            }
            const auto& value = **fact;
            return ToolResult::success(key + " = " + value.value,
                                       value.key + " = " + value.value +
                                           evidence_note(value.evidence) + "\n  from " +
                                           value.source + "\n");
        }

        if (action != "set") {
            return ToolResult::failure("repo_fact", "action must be 'get', 'list' or 'set'");
        }

        const auto value = strings::trim(arguments.value("value", std::string{}));
        const auto source = strings::trim(arguments.value("source", std::string{}));
        if (value.empty()) return ToolResult::failure("repo_fact", "give me a value");
        if (source.empty()) {
            // The whole point of the layer. A fact with no provenance is an
            // assertion, and this store does not keep assertions.
            return ToolResult::failure(
                "repo_fact",
                "say how you established this. A fact with no stated source is a guess, and "
                "guesses are not recorded here.");
        }

        repo::RepoFact fact;
        fact.key = key;
        fact.value = value;
        fact.source = source;
        // Recorded as lexical, never verified: verified means LocalCode watched
        // it work, and writing a fact is not watching anything.
        fact.evidence = Evidence::Lexical;
        fact.recorded_at_unix_ms = now_unix_ms();
        if (arguments.contains("depends_on") && arguments["depends_on"].is_array()) {
            for (const auto& item : arguments["depends_on"]) {
                if (item.is_string()) fact.depends_on.push_back(item.get<std::string>());
            }
        }

        if (auto status = store.put_fact(fact); !status) {
            return ToolResult::failure("repo_fact", status.error().message);
        }
        std::string note = "Recorded " + key + " = " + value + "\n";
        if (fact.depends_on.empty()) {
            note +=
                "No dependencies were given, so this will not be discarded automatically when the "
                "code changes. List the files it rests on if you want it kept honest.\n";
        }
        return ToolResult::success("Recorded " + key, note);
    }
};

}   // namespace

std::vector<ToolPtr> make_repo_tools() {
    return {std::make_shared<RepoOverviewTool>(), std::make_shared<RepoSearchTool>(),
            std::make_shared<RepoSymbolTool>(), std::make_shared<RepoTestsForTool>(),
            std::make_shared<RepoFactTool>()};
}

}   // namespace localcode
