#include "SymbolIndexer.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

#include "Languages.h"
#include "util/Strings.h"

namespace localcode::repo {
namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}
bool is_ident(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

std::string trim_copy(std::string_view text) { return strings::trim(text); }

// One line of a file with the comment and string content already removed, plus
// how deeply nested it is. Stripping first is what stops a name inside a
// comment from being indexed as a definition.
struct CodeLine {
    std::string text;       // comments and string bodies blanked out
    std::string raw;        // as written, for signatures
    std::size_t number = 0; // 1-based
    int depth = 0;          // brace depth at the START of the line
};

// Blanks out comments and string literals while tracking brace depth. Handles
// // and # line comments, /* */ blocks, and ' " ` quoting with backslash
// escapes. Not a lexer for any particular language - a common denominator that
// is right for the ones below.
std::vector<CodeLine> read_lines(std::string_view content, bool hash_comments,
                                 bool slash_comments) {
    std::vector<CodeLine> lines;
    bool in_block_comment = false;
    char in_string = '\0';
    int depth = 0;

    std::size_t start = 0;
    std::size_t number = 0;
    while (start <= content.size()) {
        auto newline = content.find('\n', start);
        if (newline == std::string_view::npos) newline = content.size();
        const auto raw = content.substr(start, newline - start);
        ++number;

        std::string stripped;
        stripped.reserve(raw.size());
        const int depth_at_line_start = depth;

        for (std::size_t i = 0; i < raw.size(); ++i) {
            const char c = raw[i];
            const char next = i + 1 < raw.size() ? raw[i + 1] : '\0';

            if (in_block_comment) {
                if (c == '*' && next == '/') { in_block_comment = false; ++i; }
                stripped.push_back(' ');
                continue;
            }
            if (in_string != '\0') {
                if (c == '\\') { stripped.append("  "); ++i; continue; }
                if (c == in_string) in_string = '\0';
                stripped.push_back(' ');
                continue;
            }
            if (slash_comments && c == '/' && next == '/') break;      // rest is comment
            if (hash_comments && c == '#') break;
            if (slash_comments && c == '/' && next == '*') {
                in_block_comment = true;
                stripped.append("  ");
                ++i;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') {
                in_string = c;
                stripped.push_back(' ');
                continue;
            }
            if (c == '{') ++depth;
            if (c == '}') depth = std::max(0, depth - 1);
            stripped.push_back(c);
        }

        lines.push_back(CodeLine{std::move(stripped), std::string{raw}, number, depth_at_line_start});
        if (newline == content.size()) break;
        start = newline + 1;
    }
    return lines;
}

// A word appears in `text` as a whole identifier.
bool has_word(std::string_view text, std::string_view word) {
    std::size_t from = 0;
    while ((from = text.find(word, from)) != std::string_view::npos) {
        const bool left = from == 0 || !is_ident(text[from - 1]);
        const auto after = from + word.size();
        const bool right = after >= text.size() || !is_ident(text[after]);
        if (left && right) return true;
        from = after;
    }
    return false;
}

// The identifier immediately before `pos`, reading backwards.
std::string identifier_before(std::string_view text, std::size_t pos) {
    while (pos > 0 && std::isspace(static_cast<unsigned char>(text[pos - 1]))) --pos;
    const auto end = pos;
    while (pos > 0 && is_ident(text[pos - 1])) --pos;
    if (pos == end) return {};
    if (!is_ident_start(text[pos])) return {};
    return std::string{text.substr(pos, end - pos)};
}

// The identifier at or after `pos`.
std::string identifier_at(std::string_view text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size() || !is_ident_start(text[pos])) return {};
    const auto start = pos;
    while (pos < text.size() && is_ident(text[pos])) ++pos;
    return std::string{text.substr(start, pos - start)};
}

void add(IndexedFile& out, std::string name, SymbolKind kind, const CodeLine& line,
         std::string_view file, std::string_view language, std::string container,
         Evidence evidence = Evidence::Lexical) {
    if (name.empty()) return;
    RepoSymbol symbol;
    symbol.name = std::move(name);
    symbol.kind = kind;
    symbol.file = std::string{file};
    symbol.line = line.number;
    symbol.end_line = line.number;
    symbol.language = std::string{language};
    symbol.container = std::move(container);
    symbol.signature = trim_copy(line.raw).substr(0, 200);
    symbol.evidence = evidence;
    out.symbols.push_back(std::move(symbol));
}

// --- Containers ------------------------------------------------------------
// The class or namespace a line sits inside, tracked by brace depth so a method
// belongs to its class instead of floating at file scope.
struct ContainerStack {
    // Whether the container is a type matters. A function inside a namespace is
    // a function; only one inside a class is a method. Tracking just the name
    // made every free function in this codebase a "method", because all of it
    // lives inside `namespace localcode`.
    struct Entry { std::string name; int depth; bool is_type; };
    std::vector<Entry> entries;

    void close_to(int depth) {
        while (!entries.empty() && entries.back().depth >= depth) entries.pop_back();
    }
    void open(std::string name, int depth, bool is_type = true) {
        entries.push_back({std::move(name), depth, is_type});
    }

    // True when the innermost container is a class, struct or equivalent.
    [[nodiscard]] bool inside_type() const {
        return !entries.empty() && entries.back().is_type;
    }
    [[nodiscard]] std::string current() const {
        std::string out;
        for (const auto& entry : entries) {
            if (!out.empty()) out += "::";
            out += entry.name;
        }
        return out;
    }
};

// --- C and C++ -------------------------------------------------------------

void index_cpp(std::string_view file, std::string_view language, std::string_view content,
               IndexedFile& out) {
    const auto lines = read_lines(content, false, true);
    ContainerStack containers;

    static const std::regex include_re(R"(^\s*#\s*include\s*[<"]([^">]+)[">])");
    static const std::regex type_re(
        R"(^\s*(?:template\s*<[^>]*>\s*)?(class|struct|union|enum(?:\s+class)?)\s+(?:[A-Z_][A-Z0-9_]*\s+)?([A-Za-z_]\w*))");
    static const std::regex namespace_re(R"(^\s*namespace\s+([A-Za-z_][\w:]*))");
    static const std::regex using_re(R"(^\s*using\s+([A-Za-z_]\w*)\s*=)");
    static const std::regex define_re(R"(^\s*#\s*define\s+([A-Za-z_]\w*))");

    for (const auto& line : lines) {
        containers.close_to(line.depth);
        const auto& text = line.text;
        if (text.empty()) continue;

        std::smatch match;

        // The four regexes below each need one of these words. Checking first
        // costs one scan of the line and skips four std::regex searches on the
        // vast majority that have none - the same guard the include match has
        // always had, applied to the rest.
        const bool maybe_define = has_word(text, "define");
        const bool maybe_namespace = has_word(text, "namespace");
        const bool maybe_type = has_word(text, "class") || has_word(text, "struct") ||
                                has_word(text, "union") || has_word(text, "enum");
        const bool maybe_using = has_word(text, "using");

        const std::string owned{text};

        // The target sits inside quotes or angle brackets, and read_lines blanks
        // string contents - so this matches the raw line, having first confirmed
        // from the stripped one that the directive is really there rather than
        // sitting inside a comment.
        if (has_word(text, "include") && std::regex_search(line.raw, match, include_re)) {
            RepoReference reference;
            reference.from_file = std::string{file};
            reference.to_file = match[1].str();
            reference.kind = ReferenceKind::Include;
            reference.line = line.number;
            reference.evidence = Evidence::Lexical;
            out.references.push_back(std::move(reference));
            continue;
        }
        if (maybe_define && std::regex_search(owned, match, define_re)) {
            add(out, match[1].str(), SymbolKind::Macro, line, file, language, containers.current());
            continue;
        }
        if (maybe_namespace && std::regex_search(owned, match, namespace_re)) {
            add(out, match[1].str(), SymbolKind::Namespace, line, file, language,
                containers.current());
            if (owned.find('{') != std::string::npos) {
                containers.open(match[1].str(), line.depth, /*is_type=*/false);
            }
            continue;
        }
        if (maybe_type && std::regex_search(owned, match, type_re)) {
            // A forward declaration ends in ';' and defines nothing here.
            if (owned.find(';') != std::string::npos && owned.find('{') == std::string::npos) {
                continue;
            }
            const auto keyword = match[1].str();
            const auto name = match[2].str();
            const auto kind = keyword.rfind("enum", 0) == 0  ? SymbolKind::Enum
                              : keyword == "class"          ? SymbolKind::Class
                                                            : SymbolKind::Struct;
            add(out, name, kind, line, file, language, containers.current());
            if (owned.find('{') != std::string::npos) containers.open(name, line.depth);
            continue;
        }
        if (maybe_using && std::regex_search(owned, match, using_re)) {
            add(out, match[1].str(), SymbolKind::TypeAlias, line, file, language,
                containers.current());
            continue;
        }

        // Functions: an identifier followed by '(' where the line is a
        // definition or declaration rather than a call. Calls are excluded by
        // requiring the parenthesis to be preceded by a name that is itself
        // preceded by a type or a qualifier, which in practice means the line
        // does not start with the name.
        const auto paren = owned.find('(');
        if (paren == std::string::npos) continue;
        if (owned.find('=') != std::string::npos && owned.find('=') < paren) continue;
        const auto name = identifier_before(owned, paren);
        if (name.empty()) continue;
        static const std::unordered_set<std::string> keywords = {
            "if", "for", "while", "switch", "catch", "return", "sizeof", "throw",
            "and", "or", "not", "alignof", "decltype", "static_assert", "noexcept",
            "requires", "constexpr", "explicit", "operator",
        };
        if (keywords.count(name) != 0) continue;

        auto name_end = paren;
        while (name_end > 0 && std::isspace(static_cast<unsigned char>(owned[name_end - 1])))
            --name_end;
        const auto before = trim_copy(owned.substr(0, name_end - name.size()));
        // A definition needs something before the name - a return type, or a
        // qualified scope. A bare `foo(x);` is a call.
        if (before.empty()) continue;
        if (before.find_first_of("{};") != std::string::npos) continue;
        // Skip control-flow bodies and initialiser lists.
        if (before.back() == ',' || before.back() == '(') continue;

        // A call through an object is not a definition. `a.f(x)` and `p->f(x)`
        // both have something before the name, which was enough to get them
        // indexed: asking where refresh_repository_index was defined returned
        // its call sites in the GUI alongside the real one.
        if (before.back() == '.') continue;
        if (before.size() >= 2 && before.compare(before.size() - 2, 2, "->") == 0) continue;

        // A scope alone is a qualified call, not a return type. Strip scope
        // segments before deciding whether a declaration prefix remains.
        auto declaration_prefix = before;
        while (strings::ends_with(declaration_prefix, "::")) {
            const auto scope = identifier_before(declaration_prefix, declaration_prefix.size() - 2);
            declaration_prefix = trim_copy(
                declaration_prefix.substr(0, declaration_prefix.size() - 2 - scope.size()));
            if (scope.empty()) break;
        }
        if (declaration_prefix.empty()) continue;

        // Nor is an expression. `return make_thing(x);` has "return" before the
        // name, and the keyword check above only looks at the name itself.
        static const std::unordered_set<std::string> leading_keywords = {
            "return", "if", "while", "for", "switch", "case", "else", "do",
            "new", "delete", "throw", "co_return", "co_await", "co_yield", "using",
        };
        if (leading_keywords.count(
                identifier_before(declaration_prefix, declaration_prefix.size())) != 0)
            continue;

        // A qualified name (Type::method) or a body inside a class is a method.
        // Sitting inside a namespace is not enough.
        const bool is_member = name.find("::") != std::string::npos || containers.inside_type();
        add(out, name, is_member ? SymbolKind::Method : SymbolKind::Function, line, file,
            language, containers.current());
    }
}

// --- Python ----------------------------------------------------------------

void index_python(std::string_view file, std::string_view language, std::string_view content,
                  IndexedFile& out) {
    const auto lines = read_lines(content, true, false);
    // Python nests by indentation, not braces.
    struct Scope { std::string name; std::size_t indent; };
    std::vector<Scope> scopes;

    static const std::regex def_re(R"(^(\s*)(?:async\s+)?def\s+([A-Za-z_]\w*))");
    static const std::regex class_re(R"(^(\s*)class\s+([A-Za-z_]\w*))");
    static const std::regex import_re(R"(^\s*(?:from\s+([\w.]+)\s+import|import\s+([\w.]+)))");

    for (const auto& line : lines) {
        const std::string owned{line.text};
        std::smatch match;

        if (std::regex_search(owned, match, import_re)) {
            RepoReference reference;
            reference.from_file = std::string{file};
            reference.to_file = match[1].matched ? match[1].str() : match[2].str();
            reference.kind = ReferenceKind::Include;
            reference.line = line.number;
            out.references.push_back(std::move(reference));
            continue;
        }

        const bool is_class = std::regex_search(owned, match, class_re);
        if (is_class || std::regex_search(owned, match, def_re)) {
            const auto indent = match[1].str().size();
            while (!scopes.empty() && scopes.back().indent >= indent) scopes.pop_back();

            std::string container;
            for (const auto& scope : scopes) {
                if (!container.empty()) container += ".";
                container += scope.name;
            }
            const auto name = match[2].str();
            add(out, name, is_class ? SymbolKind::Class
                                    : (container.empty() ? SymbolKind::Function
                                                         : SymbolKind::Method),
                line, file, language, container);
            scopes.push_back({name, indent});
            continue;
        }

        // Module-level constants: NAME = value at column zero.
        if (!owned.empty() && is_ident_start(owned.front())) {
            const auto equals = owned.find('=');
            if (equals != std::string::npos && equals + 1 < owned.size() &&
                owned[equals + 1] != '=') {
                const auto name = trim_copy(owned.substr(0, equals));
                if (!name.empty() &&
                    std::all_of(name.begin(), name.end(), [](char c) { return is_ident(c); })) {
                    add(out, name, SymbolKind::Variable, line, file, language, {});
                }
            }
        }
    }
}

// --- Rust ------------------------------------------------------------------

void index_rust(std::string_view file, std::string_view language, std::string_view content,
                IndexedFile& out) {
    const auto lines = read_lines(content, false, true);
    ContainerStack containers;

    static const std::regex fn_re(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:async\s+)?(?:unsafe\s+)?(?:extern\s+"[^"]*"\s+)?fn\s+([A-Za-z_]\w*))");
    static const std::regex type_re(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(struct|enum|trait|union)\s+([A-Za-z_]\w*))");
    static const std::regex impl_re(R"(^\s*impl(?:\s*<[^>]*>)?\s+(?:([A-Za-z_]\w*)\s+for\s+)?([A-Za-z_]\w*))");
    static const std::regex mod_re(R"(^\s*(?:pub\s+)?mod\s+([A-Za-z_]\w*))");
    static const std::regex use_re(R"(^\s*use\s+([\w:]+))");
    static const std::regex alias_re(R"(^\s*(?:pub\s+)?type\s+([A-Za-z_]\w*))");
    static const std::regex const_re(R"(^\s*(?:pub\s+)?(?:const|static)\s+(?:mut\s+)?([A-Za-z_]\w*))");

    for (const auto& line : lines) {
        containers.close_to(line.depth);
        if (line.text.empty()) continue;

        // Every pattern below needs one of these keywords, and almost no line
        // has any of them. Without this each line was run through seven
        // std::regex searches: a full reindex of ripgrep took twice as long as
        // one of this repository despite having fewer files and a third of the
        // symbols. The include readers have always guarded their regexes this
        // way; this one was not.
        const bool maybe_use = has_word(line.text, "use");
        const bool maybe_impl = has_word(line.text, "impl");
        const bool maybe_type = has_word(line.text, "struct") || has_word(line.text, "enum") ||
                                has_word(line.text, "trait") || has_word(line.text, "union");
        const bool maybe_mod = has_word(line.text, "mod");
        const bool maybe_fn = has_word(line.text, "fn");
        const bool maybe_alias = has_word(line.text, "type");
        const bool maybe_const = has_word(line.text, "const") || has_word(line.text, "static");
        if (!maybe_use && !maybe_impl && !maybe_type && !maybe_mod && !maybe_fn && !maybe_alias &&
            !maybe_const) {
            continue;
        }

        const std::string owned{line.text};
        std::smatch match;

        if (maybe_use && std::regex_search(owned, match, use_re)) {
            RepoReference reference;
            reference.from_file = std::string{file};
            reference.to_file = match[1].str();
            reference.kind = ReferenceKind::Include;
            reference.line = line.number;
            out.references.push_back(std::move(reference));
            continue;
        }
        if (maybe_impl && std::regex_search(owned, match, impl_re)) {
            const auto target = match[2].str();
            containers.open(target, line.depth);
            continue;
        }
        if (maybe_type && std::regex_search(owned, match, type_re)) {
            const auto keyword = match[1].str();
            const auto kind = keyword == "trait"  ? SymbolKind::Interface
                              : keyword == "enum" ? SymbolKind::Enum
                                                  : SymbolKind::Struct;
            add(out, match[2].str(), kind, line, file, language, containers.current());
            if (owned.find('{') != std::string::npos) containers.open(match[2].str(), line.depth);
            continue;
        }
        if (maybe_mod && std::regex_search(owned, match, mod_re)) {
            add(out, match[1].str(), SymbolKind::Namespace, line, file, language,
                containers.current());
            if (owned.find('{') != std::string::npos) {
                containers.open(match[1].str(), line.depth, /*is_type=*/false);
            }
            continue;
        }
        if (maybe_fn && std::regex_search(owned, match, fn_re)) {
            const auto container = containers.current();
            add(out, match[1].str(),
                containers.inside_type() ? SymbolKind::Method : SymbolKind::Function, line, file,
                language, container);
            continue;
        }
        if (maybe_alias && std::regex_search(owned, match, alias_re)) {
            add(out, match[1].str(), SymbolKind::TypeAlias, line, file, language,
                containers.current());
            continue;
        }
        if (maybe_const && std::regex_search(owned, match, const_re)) {
            add(out, match[1].str(), SymbolKind::Variable, line, file, language,
                containers.current());
        }
    }
}

// --- JavaScript, TypeScript, C#, Java, Kotlin, Go ---------------------------

void index_curly(std::string_view file, std::string_view language, std::string_view content,
                 IndexedFile& out) {
    const auto lines = read_lines(content, false, true);
    ContainerStack containers;

    static const std::regex import_re(
        R"(^\s*(?:import\s+.*?from\s*['"]([^'"]+)['"]|import\s*['"]([^'"]+)['"]|(?:const|let|var)\s+.*=\s*require\s*\(\s*['"]([^'"]+)['"]|using\s+([\w.]+)\s*;|package\s+([\w.]+)\s*;))");
    static const std::regex type_re(
        R"(^\s*(?:export\s+)?(?:public\s+|private\s+|internal\s+|protected\s+|abstract\s+|sealed\s+|static\s+|final\s+|partial\s+|data\s+)*(class|interface|struct|enum|record|trait|object)\s+([A-Za-z_]\w*))");
    static const std::regex func_re(
        R"(^\s*(?:export\s+)?(?:public\s+|private\s+|internal\s+|protected\s+|static\s+|async\s+|override\s+|virtual\s+|final\s+|suspend\s+)*(?:function|fun|func)\s+([A-Za-z_]\w*))");
    static const std::regex arrow_re(
        R"(^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)\s*=\s*(?:async\s*)?(?:\([^)]*\)|[A-Za-z_]\w*)\s*=>)");
    static const std::regex go_method_re(R"(^\s*func\s*\(\s*\w+\s+\*?([A-Za-z_]\w*)\s*\)\s*([A-Za-z_]\w*))");
    // Go puts the name before the keyword: `type Server struct {`.
    static const std::regex go_type_re(R"(^\s*type\s+([A-Za-z_]\w*)\s+(struct|interface)\b)");
    static const std::regex namespace_re(R"(^\s*namespace\s+([\w.]+))");
    static const std::regex const_re(
        R"(^\s*(?:export\s+)?(?:public\s+|private\s+|internal\s+|static\s+|readonly\s+|const\s+|final\s+|val\s+)+([A-Za-z_]\w*)\s*[:=])");

    for (const auto& line : lines) {
        containers.close_to(line.depth);
        if (line.text.empty()) continue;

        // Same guard as the import match above, extended to the rest: one scan
        // of the line instead of up to seven std::regex searches on a line that
        // declares nothing.
        const bool maybe_import = has_word(line.text, "import") ||
                                  has_word(line.text, "require") ||
                                  has_word(line.text, "using") || has_word(line.text, "package");
        const bool maybe_namespace = has_word(line.text, "namespace");
        const bool maybe_go = has_word(line.text, "func") || has_word(line.text, "type");
        const bool maybe_type = has_word(line.text, "class") || has_word(line.text, "interface") ||
                                has_word(line.text, "struct") || has_word(line.text, "enum") ||
                                has_word(line.text, "record") || has_word(line.text, "trait") ||
                                has_word(line.text, "object");
        const bool maybe_func = has_word(line.text, "function") || has_word(line.text, "fun") ||
                                has_word(line.text, "func");
        const bool maybe_binding = has_word(line.text, "const") || has_word(line.text, "let") ||
                                   has_word(line.text, "var") || has_word(line.text, "val") ||
                                   has_word(line.text, "readonly") ||
                                   has_word(line.text, "static") || has_word(line.text, "final") ||
                                   has_word(line.text, "public") ||
                                   has_word(line.text, "private") ||
                                   has_word(line.text, "internal");
        if (!maybe_import && !maybe_namespace && !maybe_go && !maybe_type && !maybe_func &&
            !maybe_binding) {
            continue;
        }

        const std::string owned{line.text};
        std::smatch match;

        // Quoted module paths are blanked in `owned`; match the raw line,
        // but only when the stripped line still shows the keyword, so a
        // commented-out import stays ignored.
        if (maybe_import && std::regex_search(line.raw, match, import_re)) {
            std::string target;
            for (std::size_t i = 1; i < match.size(); ++i) {
                if (match[i].matched) { target = match[i].str(); break; }
            }
            if (!target.empty()) {
                RepoReference reference;
                reference.from_file = std::string{file};
                reference.to_file = target;
                reference.kind = ReferenceKind::Include;
                reference.line = line.number;
                out.references.push_back(std::move(reference));
            }
            continue;
        }
        if (maybe_namespace && std::regex_search(owned, match, namespace_re)) {
            add(out, match[1].str(), SymbolKind::Namespace, line, file, language,
                containers.current());
            if (owned.find('{') != std::string::npos) {
                containers.open(match[1].str(), line.depth, /*is_type=*/false);
            }
            continue;
        }
        if (maybe_go && std::regex_search(owned, match, go_type_re)) {
            const auto kind =
                match[2].str() == "interface" ? SymbolKind::Interface : SymbolKind::Struct;
            add(out, match[1].str(), kind, line, file, language, containers.current());
            if (owned.find('{') != std::string::npos) containers.open(match[1].str(), line.depth);
            continue;
        }
        if (maybe_go && std::regex_search(owned, match, go_method_re)) {
            add(out, match[2].str(), SymbolKind::Method, line, file, language, match[1].str());
            continue;
        }
        if (maybe_type && std::regex_search(owned, match, type_re)) {
            const auto keyword = match[1].str();
            const auto kind = keyword == "interface" || keyword == "trait" ? SymbolKind::Interface
                              : keyword == "enum"                          ? SymbolKind::Enum
                              : keyword == "struct"                        ? SymbolKind::Struct
                                                                           : SymbolKind::Class;
            add(out, match[2].str(), kind, line, file, language, containers.current());
            if (owned.find('{') != std::string::npos) containers.open(match[2].str(), line.depth);
            continue;
        }
        if (maybe_func && std::regex_search(owned, match, func_re)) {
            const auto container = containers.current();
            add(out, match[1].str(),
                containers.inside_type() ? SymbolKind::Method : SymbolKind::Function, line, file,
                language, container);
            continue;
        }
        if (maybe_binding && std::regex_search(owned, match, arrow_re)) {
            add(out, match[1].str(), SymbolKind::Function, line, file, language,
                containers.current());
            continue;
        }
        if (maybe_binding && std::regex_search(owned, match, const_re)) {
            add(out, match[1].str(), SymbolKind::Variable, line, file, language,
                containers.current());
        }
    }
}

// --- CMake -----------------------------------------------------------------
// Targets are declared, not inferred, so these are structural.

void index_cmake(std::string_view file, std::string_view language, std::string_view content,
                 IndexedFile& out) {
    const auto lines = read_lines(content, true, false);
    static const std::regex target_re(
        R"(^\s*(add_executable|add_library|qt_add_executable|add_custom_target)\s*\(\s*([A-Za-z_][\w.-]*))",
        std::regex::icase);
    static const std::regex include_re(R"(^\s*(?:include|add_subdirectory)\s*\(\s*([^\s\)]+))",
                                       std::regex::icase);
    static const std::regex function_re(R"(^\s*(?:function|macro)\s*\(\s*([A-Za-z_][\w]*))",
                                        std::regex::icase);

    for (const auto& line : lines) {
        const std::string owned{line.text};
        std::smatch match;
        if (std::regex_search(owned, match, target_re)) {
            add(out, match[2].str(), SymbolKind::Target, line, file, language, {},
                Evidence::Structural);
            continue;
        }
        if (std::regex_search(owned, match, function_re)) {
            add(out, match[1].str(), SymbolKind::Function, line, file, language, {},
                Evidence::Structural);
            continue;
        }
        if (std::regex_search(owned, match, include_re)) {
            RepoReference reference;
            reference.from_file = std::string{file};
            reference.to_file = match[1].str();
            reference.kind = ReferenceKind::Include;
            reference.line = line.number;
            reference.evidence = Evidence::Structural;
            out.references.push_back(std::move(reference));
        }
    }
}

// --- Assembly --------------------------------------------------------------

void index_assembly(std::string_view file, std::string_view language, std::string_view content,
                    IndexedFile& out) {
    const auto lines = read_lines(content, false, false);
    static const std::regex label_re(R"(^([A-Za-z_][\w.@$]*)\s*:)");
    static const std::regex proc_re(R"(^\s*(?:proc\s+)?([A-Za-z_][\w.@$]*)\s+proc\b)",
                                    std::regex::icase);
    static const std::regex global_re(R"(^\s*(?:\.globl|\.global|public)\s+([A-Za-z_][\w.@$]*))",
                                      std::regex::icase);

    for (const auto& line : lines) {
        // Assembly comments are ';' for MASM/FASM and '#' for GAS; both are
        // stripped here rather than in read_lines, which is shared.
        auto owned = std::string{line.text};
        const auto semicolon = owned.find(';');
        if (semicolon != std::string::npos) owned = owned.substr(0, semicolon);
        const auto hash = owned.find('#');
        if (hash != std::string::npos) owned = owned.substr(0, hash);

        std::smatch match;
        if (std::regex_search(owned, match, proc_re) ||
            std::regex_search(owned, match, global_re)) {
            add(out, match[1].str(), SymbolKind::Function, line, file, language, {});
            continue;
        }
        if (std::regex_search(owned, match, label_re)) {
            add(out, match[1].str(), SymbolKind::Function, line, file, language, {},
                Evidence::Heuristic);
        }
    }
}

}  // namespace

bool has_symbol_support(std::string_view language) {
    static const std::unordered_set<std::string> set = {
        "c", "c++", "c/c++ header", "rust", "python", "javascript", "typescript",
        "c#", "java", "kotlin", "go", "cmake", "assembly",
    };
    return set.count(std::string{language}) != 0;
}

IndexedFile index_source(std::string_view relative_path, std::string_view language,
                         std::string_view content) {
    IndexedFile out;
    out.line_count = static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n'));
    if (!content.empty() && content.back() != '\n') ++out.line_count;

    if (language == "c" || language == "c++" || language == "c/c++ header") {
        index_cpp(relative_path, language, content, out);
    } else if (language == "rust") {
        index_rust(relative_path, language, content, out);
    } else if (language == "python") {
        index_python(relative_path, language, content, out);
    } else if (language == "javascript" || language == "typescript" || language == "c#" ||
               language == "java" || language == "kotlin" || language == "go") {
        index_curly(relative_path, language, content, out);
    } else if (language == "cmake") {
        index_cmake(relative_path, language, content, out);
    } else if (language == "assembly") {
        index_assembly(relative_path, language, content, out);
    }

    return out;
}

}  // namespace localcode::repo
