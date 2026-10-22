#pragma once
// ---------------------------------------------------------------------------
// Finding the definitions in a file.
//
// This is lexical. There is no compiler here and no language server, and
// pretending otherwise would put a fabricated call graph in front of the agent.
// What it does instead is recognise the shapes that declare something, one
// language at a time, and mark every result as lexical so nothing downstream
// can mistake it for a fact the compiler agreed to.
//
// Two things it does take care over, because getting them wrong produces
// confident nonsense rather than a missing entry:
//
//   - comments and strings are skipped, so a function named in a comment does
//     not become a definition;
//   - the enclosing class or namespace is tracked by brace depth, so a method
//     is attributed to its class rather than floating free.
//
// Where structure is genuinely available - a CMake target, a Cargo manifest -
// that is read as structural and marked accordingly.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <vector>

#include "RepoTypes.h"

namespace localcode::repo {

struct IndexedFile {
    std::vector<RepoSymbol>    symbols;
    std::vector<RepoReference> references;
    std::size_t                line_count = 0;
};

// Extracts what can be seen in `content`, which is the whole file.
// `relative_path` decides which language's shapes are looked for.
[[nodiscard]] IndexedFile index_source(std::string_view relative_path, std::string_view language,
                                       std::string_view content);

// True when this language has a dedicated reader. Everything else still gets
// line counts and include-style references where they are obvious, but no
// symbols - which is reported as "no structural support" rather than as a file
// with nothing in it.
[[nodiscard]] bool has_symbol_support(std::string_view language);

}  // namespace localcode::repo
