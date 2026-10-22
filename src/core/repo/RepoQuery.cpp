#include "RepoQuery.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>
#include <unordered_set>

#include "Languages.h"
#include "platform/FileSystem.h"
#include "security/PathPolicy.h"
#include "util/Strings.h"

namespace localcode::repo {
namespace {

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_ident(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Case-insensitive find that allocates nothing. `needle` must already be lower
// case, which is how the caller holds it.
//
// The obvious version - lower(line).find(needle) - builds a string per line,
// which is sixty-five thousand allocations for one search of this repository
// and made searching slower than the grep the tool tells the model to stop
// using.
std::size_t find_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || needle.size() > haystack.size()) return std::string_view::npos;
    const auto found = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                   [](char left, char right) {
                                       return std::tolower(static_cast<unsigned char>(left)) ==
                                              static_cast<unsigned char>(right);
                                   });
    if (found == haystack.end()) return std::string_view::npos;
    return static_cast<std::size_t>(found - haystack.begin());
}

// The needle appears as a whole word rather than inside a longer identifier.
bool whole_word_at(std::string_view haystack, std::size_t pos, std::size_t length) {
    const bool left = pos == 0 || !is_ident(haystack[pos - 1]);
    const bool right = pos + length >= haystack.size() || !is_ident(haystack[pos + length]);
    return left && right;
}

std::string trimmed_line(std::string_view line, std::size_t limit = 200) {
    auto text = strings::trim(line);
    if (text.size() > limit) text = text.substr(0, limit) + "...";
    return text;
}

// A file's role decides how interesting it is when everything else is equal.
double role_weight(FileRole role) {
    switch (role) {
        case FileRole::Source: return 1.0;
        case FileRole::Build: return 0.8;
        case FileRole::Test: return 0.7;
        case FileRole::Config: return 0.5;
        case FileRole::Documentation: return 0.4;
        case FileRole::Generated:
        case FileRole::Vendored: return 0.1;
        default: return 0.3;
    }
}

void sort_and_trim(std::vector<SearchHit>& hits, std::size_t limit) {
    std::stable_sort(hits.begin(), hits.end(),
                     [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    if (hits.size() > limit) hits.resize(limit);
}

}   // namespace

RepoQuery::RepoQuery(const RepoStore& store, std::filesystem::path root)
    : store_(&store), root_(std::move(root)) {
    canonical_root_ = PathPolicy::canonicalize(root_);
}

Result<std::vector<SearchHit>> RepoQuery::task_sources(std::string_view objective,
                                                       std::size_t limit) const {
    if (limit == 0) return std::vector<SearchHit>{};
    static const std::unordered_set<std::string> stop = {
        "about",   "actual",   "also",     "answer",         "appropriate", "before", "cite",
        "code",    "commands", "concise",  "determine",      "does",        "each",   "edit",
        "execute", "explain",  "file",     "files",          "find",        "first",  "from",
        "give",    "have",     "identify", "implementation", "functions",   "keep",   "line",
        "lines",   "locate",   "methods",  "name",           "number",      "only",   "owns",
        "ranges",  "related",  "relevant", "repository",     "show",        "source", "specific",
        "that",    "their",    "starts",   "stops",          "there",       "these",  "this",
        "tools",   "what",     "when",     "where",          "which",       "with",   "would"};
    std::vector<std::string> terms;
    std::unordered_set<std::string> seen;
    std::string current;
    auto finish = [&] {
        if (current.size() >= 4 && !stop.contains(current) && seen.insert(current).second &&
            terms.size() < 16)
            terms.push_back(current);
        current.clear();
    };
    for (const char character : objective.substr(0, 1024)) {
        if (is_ident(character))
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        else
            finish();
    }
    finish();
    if (terms.empty()) return std::vector<SearchHit>{};
    auto files = store_->files();
    if (!files) return files.error();
    std::unordered_set<std::string> eligible_files;
    for (const auto& file : *files) {
        if (!file.skipped && file.role != FileRole::Generated && file.role != FileRole::Vendored)
            eligible_files.insert(file.path);
    }

    struct Candidate {
        SearchHit hit;
        std::unordered_set<std::string> matched;
        double symbol_score = 0.0;
    };

    std::map<std::string, Candidate> candidates;
    for (const auto& term : terms) {
        const auto frequency = std::count_if(
            files->begin(), files->end(),
            [&](const RepoFile& file) { return find_ci(file.path, term) != std::string::npos; });
        const double weight =
            std::log1p(static_cast<double>(files->size()) / static_cast<double>(frequency + 1));
        for (const auto& file : *files) {
            if (file.skipped || file.role == FileRole::Generated ||
                file.role == FileRole::Vendored || find_ci(file.path, term) == std::string::npos)
                continue;
            auto& candidate = candidates[file.path];
            candidate.hit.file = file.path;
            if (candidate.hit.line == 0) candidate.hit.line = 1;
            candidate.hit.score += weight * role_weight(file.role);
            candidate.matched.insert(term);
        }
        auto symbols = store_->symbols_like(term, 12);
        if (!symbols) return symbols.error();
        for (const auto& symbol : *symbols) {
            if (!eligible_files.contains(symbol.file)) continue;
            auto& candidate = candidates[symbol.file];
            candidate.hit.file = symbol.file;
            if (candidate.matched.insert(term).second) candidate.hit.score += weight;
            const auto identity = lower(symbol.container + " " + symbol.name);
            double symbol_score = 0.0;
            for (const auto& query : terms)
                if (identity.find(query) != std::string::npos) symbol_score += 1.0;
            if (symbol.kind == SymbolKind::Function || symbol.kind == SymbolKind::Method)
                symbol_score += .25;
            if (symbol_score > candidate.symbol_score) {
                candidate.symbol_score = symbol_score;
                candidate.hit.line = symbol.line;
                candidate.hit.preview = symbol.signature;
            }
        }
    }
    std::vector<SearchHit> hits;
    for (auto& [path, candidate] : candidates) {
        std::vector<std::string> matched;
        for (const auto& term : terms)
            if (candidate.matched.contains(term)) matched.push_back(term);
        candidate.hit.reason = "indexed path/symbol matches: " + strings::join(matched, ", ");
        candidate.hit.evidence = Evidence::Lexical;
        candidate.hit.score += candidate.symbol_score;
        hits.push_back(std::move(candidate.hit));
    }
    sort_and_trim(hits, std::min<std::size_t>(limit, 12));
    // Path/name retrieval identifies the owner, but a constructor often wins a
    // class-name lookup even when the question asks about start/stop/clear. Pick
    // ranges from that owner's indexed definitions, using action words only
    // here so generic helpers cannot displace the subject files in retrieval.
    auto range_terms = terms;
    std::unordered_set<std::string> requested_actions;
    const auto lowered_objective = lower(objective);
    for (const auto* action : {"start", "stop", "clear", "load", "unload", "cancel", "close"}) {
        for (const auto& form :
             {std::string(action), std::string(action) + "s", std::string(action) + "ing"}) {
            const auto at = lowered_objective.find(form);
            if (at != std::string::npos && whole_word_at(lowered_objective, at, form.size())) {
                requested_actions.insert(action);
                if (std::find(range_terms.begin(), range_terms.end(), action) == range_terms.end())
                    range_terms.emplace_back(action);
            }
        }
    }
    auto qualified_owner = [](const RepoSymbol& symbol) {
        const auto signature = lower(symbol.signature);
        const auto at = signature.find("::" + lower(symbol.name) + "(");
        if (at == std::string::npos) return std::string{};
        auto first = at;
        while (first > 0 && is_ident(signature[first - 1]))
            --first;
        return signature.substr(first, at - first);
    };
    for (auto& hit : hits) {
        auto definitions = store_->symbols_in(hit.file);
        if (!definitions) return definitions.error();
        std::vector<std::pair<double, const RepoSymbol*>> ranked;
        for (const auto& symbol : *definitions) {
            const bool callable =
                symbol.kind == SymbolKind::Function || symbol.kind == SymbolKind::Method;
            const auto name = lower(symbol.name);
            const auto container = lower(symbol.container);
            const auto signature = lower(symbol.signature);
            const auto owner = qualified_owner(symbol);
            // Out-of-class definitions may have only a namespace as container.
            // Recognize construction/destruction from the declaration too.
            const bool constructor = (!container.empty() && name == container) ||
                                     signature.starts_with(name + "(") ||
                                     signature.starts_with("~" + name + "(") || owner == name ||
                                     signature.find(name + "::~" + name + "(") != std::string::npos;
            double score = callable ? .05 : 0.0;
            for (const auto& term : range_terms) {
                if (name.find(term) != std::string::npos) {
                    // An exact action (clear/start/stop) should outrank a subject
                    // name repeated in a constructor or a display helper. The
                    // lexical parser may only know the surrounding namespace.
                    const auto at = name.find(term);
                    const bool action_component =
                        (at == 0 || name[at - 1] == '_') &&
                        (at + term.size() == name.size() || name[at + term.size()] == '_');
                    score += callable && !constructor ? (requested_actions.contains(term)
                                                             ? (action_component ? 12.0 : 0.0)
                                                             : 4.0)
                                                      : 1.0;
                }
                if (container.find(term) != std::string::npos)
                    score += .25;
                else if (owner.find(term) != std::string::npos)
                    score += .25;
            }
            if (constructor) score *= .05;
            if (callable && score > .05) ranked.emplace_back(score, &symbol);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            return left.first > right.first;
        });
        auto span_end = [&](const RepoSymbol& symbol) {
            // The lexical index currently often knows only a definition's first
            // line. Stop the candidate span at the next callable, bounded to 128
            // lines; this is a navigation window, not a proven function extent.
            auto end = symbol.line + std::min<std::size_t>(127, SIZE_MAX - symbol.line);
            if (symbol.end_line > symbol.line) end = std::min(end, symbol.end_line);
            const auto owner = qualified_owner(symbol);
            for (const auto& next : *definitions)
                if (next.line > symbol.line &&
                    (next.kind == SymbolKind::Function || next.kind == SymbolKind::Method) &&
                    ((!owner.empty() && qualified_owner(next) == owner) ||
                     (owner.empty() && next.container == symbol.container &&
                      (next.signature.find('{') != std::string::npos ||
                       next.language == "python"))))
                    end = std::min(end, next.line - 1);
            return end;
        };
        if (!ranked.empty()) {
            const auto& primary = *ranked.front().second;
            hit.line = primary.line;
            hit.end_line = span_end(primary);
            hit.preview = primary.signature;
            for (const auto& [score, symbol] : ranked) {
                if (hit.related_ranges.size() == 2) break;
                if (symbol->line == hit.line ||
                    std::any_of(hit.related_ranges.begin(), hit.related_ranges.end(),
                                [&](const auto& range) { return range.first == symbol->line; }))
                    continue;
                hit.related_ranges.emplace_back(symbol->line, span_end(*symbol));
            }
        }
    }
    return hits;
}

Result<std::string> RepoQuery::read_file(std::string_view relative, std::size_t max_bytes) const {
    // The index only ever holds workspace-relative paths, and this is the one
    // place they become absolute. A path that climbs out of the root is refused
    // rather than read: an index entry is not a licence to open anything.
    const std::filesystem::path candidate = root_ / std::filesystem::path{std::string{relative}};
    // The candidate is resolved every time, because that is what catches a
    // symlink pointing out of the tree; only the base it is compared against
    // is cached.
    const auto resolved = PathPolicy::canonicalize(candidate);
    if (!PathPolicy::is_within(canonical_root_, resolved)) {
        return make_error(ErrorCode::PermissionDenied,
                          "that path is outside the workspace: " + std::string{relative});
    }
    return fs::read_bytes(candidate, max_bytes);
}

Result<std::vector<SearchHit>> RepoQuery::find_files(std::string_view fragment,
                                                     std::size_t limit) const {
    auto files = store_->files();
    if (!files) return files.error();

    const auto needle = lower(fragment);
    std::vector<SearchHit> hits;
    for (const auto& file : *files) {
        // Filter before allocating. Lowering every path and taking every file
        // name up front cost two strings per record, which on a forty thousand
        // file repository was most of the query.
        if (find_ci(file.path, needle) == std::string_view::npos) continue;
        const auto name = file_name_of(file.path);

        SearchHit hit;
        hit.file = file.path;
        hit.evidence = Evidence::Structural;   // the path is a fact, not a guess
        if (name == needle) {
            hit.score = 100.0;
            hit.reason = "the file is named exactly that";
        } else if (name.find(needle) == 0) {
            hit.score = 70.0;
            hit.reason = "the file name starts with it";
        } else if (name.find(needle) != std::string::npos) {
            hit.score = 50.0;
            hit.reason = "the file name contains it";
        } else {
            hit.score = 20.0;
            hit.reason = "the path contains it";
        }
        hit.score += role_weight(file.role);
        // Shorter paths are usually the more central ones.
        hit.score -= static_cast<double>(file.path.size()) / 500.0;
        hits.push_back(std::move(hit));
    }
    sort_and_trim(hits, limit);
    return hits;
}

Result<std::vector<SearchHit>> RepoQuery::search_text(std::string_view needle,
                                                      const SearchOptions& options,
                                                      SearchStats* stats) const {
    SearchStats local;
    SearchStats& counted = stats != nullptr ? *stats : local;
    counted = SearchStats{};
    if (needle.empty()) return std::vector<SearchHit>{};

    auto files = store_->files();
    if (!files) return files.error();

    std::optional<std::regex> pattern;
    if (options.regex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!options.case_sensitive) flags |= std::regex::icase;
            pattern.emplace(std::string{needle}, flags);
        } catch (const std::regex_error& error) {
            return make_error(ErrorCode::InvalidArgument, "that is not a valid regular expression",
                              error.what());
        }
    }
    const auto plain = options.case_sensitive ? std::string{needle} : lower(needle);

    std::vector<SearchHit> hits;
    std::size_t files_read = 0;
    for (const auto& file : *files) {
        if (hits.size() >= options.max_hits * 4) {   // enough to rank well
            counted.hit_match_limit = true;
            break;
        }
        if (files_read >= options.max_files) {
            counted.hit_file_limit = true;
            break;
        }
        ++counted.files_considered;
        if (file.skipped) continue;
        if (!options.include_tests && file.role == FileRole::Test) continue;
        if (!options.include_non_source && file.role != FileRole::Source) continue;
        if (file.role == FileRole::Generated || file.role == FileRole::Vendored) continue;
        if (!options.path_contains.empty() &&
            lower(file.path).find(lower(options.path_contains)) == std::string::npos) {
            continue;
        }

        auto contents = read_file(file.path);
        if (!contents) continue;
        ++files_read;
        ++counted.files_read;

        std::size_t line_number = 0;
        std::size_t start = 0;
        const std::string& text = *contents;
        while (start <= text.size()) {
            auto newline = text.find('\n', start);
            if (newline == std::string::npos) newline = text.size();
            const std::string_view line{text.data() + start, newline - start};
            ++line_number;

            bool matched = false;
            bool whole_word = false;
            if (pattern) {
                matched = std::regex_search(line.begin(), line.end(), *pattern);
            } else {
                const auto at = options.case_sensitive ? line.find(plain) : find_ci(line, plain);
                matched = at != std::string_view::npos;
                // Delimiters are the same characters either way, so this reads
                // the line itself rather than a lowered copy of it.
                if (matched) whole_word = whole_word_at(line, at, plain.size());
            }

            if (matched) {
                SearchHit hit;
                hit.file = file.path;
                hit.line = line_number;
                hit.preview = trimmed_line(line);
                hit.evidence = Evidence::Lexical;
                hit.reason =
                    whole_word ? "the text appears here as a whole word" : "the text appears here";
                hit.score = (whole_word ? 10.0 : 5.0) + role_weight(file.role);
                hits.push_back(std::move(hit));
                if (hits.size() >= options.max_hits * 4) break;
            }

            if (newline == text.size()) break;
            start = newline + 1;
        }
    }

    sort_and_trim(hits, options.max_hits);
    return hits;
}

Result<std::vector<RepoSymbol>> RepoQuery::find_symbol(std::string_view name,
                                                       std::size_t limit) const {
    auto exact = store_->symbols_named(name, limit);
    if (!exact) return exact.error();
    if (!exact->empty()) return exact;
    // Nothing named exactly that; fall back to a substring so a half-remembered
    // name still lands somewhere useful.
    return store_->symbols_like(name, limit);
}

Result<std::vector<SearchHit>> RepoQuery::find_references(std::string_view symbol,
                                                          std::size_t limit) const {
    // Where it is defined, so those can be told apart from where it is used.
    std::unordered_set<std::string> definitions;
    if (auto defined = store_->symbols_named(symbol, 100)) {
        for (const auto& found : *defined)
            definitions.insert(found.file);
    }

    SearchOptions options;
    options.max_hits = limit;
    options.case_sensitive = true;
    auto mentions = search_text(symbol, options);
    if (!mentions) return mentions.error();

    for (auto& hit : *mentions) {
        if (definitions.count(hit.file) != 0) {
            hit.score += 8.0;
            hit.reason = "this file defines it, and names it here";
        } else {
            hit.reason = "this file names it";
        }
    }
    sort_and_trim(*mentions, limit);
    return mentions;
}

Result<std::vector<SearchHit>> RepoQuery::tests_for(std::string_view path,
                                                    std::size_t limit) const {
    auto files = store_->files();
    if (!files) return files.error();

    const auto name = file_name_of(path);
    auto stem = name;
    if (const auto dot = stem.find_last_of('.'); dot != std::string::npos)
        stem = stem.substr(0, dot);

    // The symbols this file defines - a test that names one of them is
    // exercising it, which is better evidence than a similar file name.
    std::unordered_set<std::string> defined;
    if (auto symbols = store_->symbols_in(path)) {
        for (const auto& symbol : *symbols) {
            if (symbol.name.size() >= 4) defined.insert(symbol.name);
        }
    }

    std::vector<SearchHit> hits;
    for (const auto& file : *files) {
        if (file.role != FileRole::Test || file.skipped) continue;

        SearchHit hit;
        hit.file = file.path;
        hit.evidence = Evidence::Heuristic;

        // 1. It includes the file.
        if (auto references = store_->references_from_file(file.path)) {
            for (const auto& reference : *references) {
                if (reference.kind != ReferenceKind::Include) continue;
                if (reference.to_file == path) {
                    hit.score = 100.0;
                    hit.reason = "it includes " + std::string{name};
                    hit.evidence = Evidence::Lexical;
                    hit.line = reference.line;
                    break;
                }
            }
        }

        // 2. Its name echoes the file's.
        if (hit.score == 0.0 && !stem.empty() &&
            file_name_of(file.path).find(stem) != std::string::npos) {
            hit.score = 60.0;
            hit.reason = "its name matches " + stem;
        }

        // 3. It names something the file defines.
        if (hit.score == 0.0 && !defined.empty()) {
            auto contents = read_file(file.path);
            if (contents) {
                for (const auto& symbol : defined) {
                    const auto at = contents->find(symbol);
                    if (at == std::string::npos) continue;
                    if (!whole_word_at(*contents, at, symbol.size())) continue;
                    hit.score = 40.0;
                    hit.reason = "it names " + symbol + ", which " + std::string{name} + " defines";
                    hit.evidence = Evidence::Lexical;
                    break;
                }
            }
        }

        if (hit.score > 0.0) hits.push_back(std::move(hit));
    }

    sort_and_trim(hits, limit);
    return hits;
}

Result<CodeExcerpt> RepoQuery::excerpt(std::string_view path, std::size_t around_line,
                                       std::size_t context) const {
    auto contents = read_file(path);
    if (!contents) return contents.error();

    std::vector<std::string_view> lines;
    std::size_t start = 0;
    const std::string& text = *contents;
    while (start <= text.size()) {
        auto newline = text.find('\n', start);
        if (newline == std::string::npos) newline = text.size();
        lines.emplace_back(text.data() + start, newline - start);
        if (newline == text.size()) break;
        start = newline + 1;
    }

    CodeExcerpt excerpt;
    excerpt.file = std::string{path};
    if (lines.empty()) return excerpt;

    const std::size_t centre = around_line == 0 ? 1 : around_line;
    excerpt.first_line = centre > context ? centre - context : 1;
    excerpt.last_line = std::min(lines.size(), centre + context);

    for (std::size_t i = excerpt.first_line; i <= excerpt.last_line; ++i) {
        excerpt.text += lines[i - 1];
        excerpt.text += '\n';
    }
    return excerpt;
}

}   // namespace localcode::repo
