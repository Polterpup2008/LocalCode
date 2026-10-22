#pragma once
// ---------------------------------------------------------------------------
// What LocalCode knows about a repository.
//
// The agent used to rediscover a project on every task: list the tree, grep for
// a name, open half a dozen files to find out where something lives, and do it
// again on the next request. This is the persistent alternative - built from
// things that can be checked rather than guessed, and carrying, for every fact,
// how it was arrived at.
//
// The distinction that matters most here is confidence. A symbol found by a
// parser that understands the language and a symbol found by a regular
// expression are both useful, and presenting them as the same thing is how a
// tool starts lying. Every entity below records which it is.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace localcode::repo {

// How a piece of information was obtained. Ordered: later is stronger.
enum class Evidence {
    // A guess from shape alone - a name that looks like a test, a directory
    // called "src". Useful for ranking, never for an assertion.
    Heuristic,
    // Matched by pattern against the file's text. Right far more often than
    // not, and wrong in the ways regular expressions are wrong: it cannot see
    // scope, and it cannot tell code from a comment or a string in every case.
    Lexical,
    // Read from a file whose format is defined - a manifest, a project file, a
    // build description. What it says is what the project says.
    Structural,
    // LocalCode ran it and watched it work.
    Verified,
};

[[nodiscard]] std::string_view to_string(Evidence evidence) noexcept;
[[nodiscard]] std::optional<Evidence> evidence_from_string(std::string_view text);

// --- Files -----------------------------------------------------------------

// What a file is for, as far as can be told from where it sits and what it is
// called. Deliberately coarse: the point is to rank and filter, not to classify
// perfectly.
enum class FileRole {
    Source,
    Test,
    Build,        // CMakeLists.txt, Cargo.toml, package.json, a .vcxproj
    Config,
    Documentation,
    Asset,
    Generated,    // build output, anything under a generated directory
    Vendored,     // third_party, vendor, node_modules
    Unknown,
};

[[nodiscard]] std::string_view to_string(FileRole role) noexcept;

struct RepoFile {
    std::string   path;            // workspace-relative, forward slashes
    std::string   language;        // "c++", "python", "" when unknown
    FileRole      role = FileRole::Unknown;
    std::uint64_t size_bytes = 0;
    std::int64_t  modified_unix_ms = 0;
    // Content hash. Size and mtime catch almost everything; the hash catches a
    // file written back with the same length, which an editor does often.
    std::string   content_hash;
    std::size_t   line_count = 0;
    // Set when the file was seen but deliberately not read: too large, or not
    // text. It still belongs in the map - its absence would be a lie about the
    // repository - but nothing was indexed from inside it.
    bool          skipped = false;
    std::string   skip_reason;
};

// --- Symbols ---------------------------------------------------------------

enum class SymbolKind {
    Function,
    Method,
    Class,
    Struct,
    Enum,
    Interface,    // trait, protocol, interface
    Namespace,    // module, package
    Variable,     // global, static, constant
    TypeAlias,
    Macro,
    Target,       // a build target: a CMake executable, a Cargo bin
    Unknown,
};

[[nodiscard]] std::string_view to_string(SymbolKind kind) noexcept;
[[nodiscard]] std::optional<SymbolKind> symbol_kind_from_string(std::string_view text);

struct RepoSymbol {
    std::string  name;
    SymbolKind   kind = SymbolKind::Unknown;
    std::string  file;          // workspace-relative
    std::size_t  line = 0;      // 1-based, where the definition starts
    std::size_t  end_line = 0;  // best effort; equal to `line` when unknown
    std::string  language;
    // The enclosing class, namespace or module, when it could be determined.
    std::string  container;
    // The declaration as written, trimmed - enough to tell two overloads apart.
    std::string  signature;
    Evidence     evidence = Evidence::Lexical;
};

// --- References ------------------------------------------------------------

enum class ReferenceKind {
    Include,      // #include, import, use, require
    Mention,      // the name appears in this file
    TestSubject,  // this test file appears to exercise that production file
    BuildInput,   // this source belongs to that build target
};

[[nodiscard]] std::string_view to_string(ReferenceKind kind) noexcept;

struct RepoReference {
    std::string   from_file;
    std::string   to_file;     // empty when the target is a symbol, not a file
    std::string   to_symbol;   // empty when the target is a file
    ReferenceKind kind = ReferenceKind::Mention;
    std::size_t   line = 0;
    Evidence      evidence = Evidence::Lexical;
};

// --- Build and test knowledge ----------------------------------------------

struct RepoCommand {
    std::string  purpose;       // "build", "test", "configure", "run", "lint"
    std::string  command;
    std::string  working_directory;  // workspace-relative, empty means the root
    // Where this came from, in words: "Cargo.toml", "package.json scripts.test",
    // "README", "an existing build/ directory".
    std::string  source;
    Evidence     evidence = Evidence::Structural;
    // Set once LocalCode has run it and seen it succeed.
    bool         verified = false;
    std::int64_t verified_at_unix_ms = 0;
    int          last_exit_code = -1;
};

// --- Facts -----------------------------------------------------------------

// Something LocalCode has established about the repository and can be asked
// about later. Never a model's opinion: a fact carries what it was derived
// from, and it goes stale when that changes.
struct RepoFact {
    std::string  key;          // "build.command", "entry_point", "output.binary"
    std::string  value;
    std::string  source;       // how it was established, in words
    Evidence     evidence = Evidence::Heuristic;
    std::int64_t recorded_at_unix_ms = 0;
    // Files this depends on. When any of them changes, the fact is stale.
    std::vector<std::string> depends_on;
    // The index generation this was recorded against.
    std::int64_t generation = 0;
};

// --- The map ---------------------------------------------------------------

struct LanguageSummary {
    std::string  language;
    std::size_t  files = 0;
    std::size_t  lines = 0;
};

struct RepoOverview {
    std::string  root;
    std::string  git_root;          // empty when not a repository, or the same
    bool         is_git_repository = false;

    std::vector<LanguageSummary> languages;   // most code first
    std::vector<std::string>     build_systems;
    std::vector<std::string>     package_managers;
    std::vector<std::string>     source_roots;
    std::vector<std::string>     test_roots;
    std::vector<std::string>     manifests;
    std::vector<std::string>     entry_points;
    std::vector<std::string>     documentation;
    std::vector<std::string>     generated_directories;
    std::vector<std::string>     vendored_directories;

    std::size_t   indexed_files = 0;
    std::size_t   skipped_files = 0;
    std::size_t   total_lines = 0;
    std::uint64_t indexed_bytes = 0;
    std::size_t   symbol_count = 0;

    // True when a limit stopped the walk, so the counts are a floor rather
    // than a total. Saying nothing here would make a partial scan read as a
    // complete one.
    bool          truncated = false;

    std::int64_t  indexed_at_unix_ms = 0;
    std::int64_t  generation = 0;
};

}  // namespace localcode::repo
