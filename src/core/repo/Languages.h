#pragma once
// ---------------------------------------------------------------------------
// What a file is, from its name.
//
// Extension-based, which is unglamorous and right far more often than anything
// cleverer. Where it cannot tell - a bare `Makefile`, a `.h` that might be C or
// C++ - it says so rather than picking. Partial support that admits its gaps
// beats confident support that is wrong about a tenth of a tree.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <vector>

#include "RepoTypes.h"

namespace localcode::repo {

// "c++", "python", "rust", ... or "" when the extension says nothing useful.
[[nodiscard]] std::string language_of(std::string_view relative_path);

// True for files worth reading: source, manifests, documentation, config.
// False for images, archives, binaries and model weights.
[[nodiscard]] bool is_indexable_text(std::string_view relative_path);

// Where the file sits in the project, from its path and name alone.
[[nodiscard]] FileRole role_of(std::string_view relative_path, std::string_view language);

// Build systems and package managers a file is evidence of - "CMake",
// "Cargo", "npm". Empty when it is not that kind of file.
[[nodiscard]] std::string build_system_of(std::string_view relative_path);
[[nodiscard]] std::string package_manager_of(std::string_view relative_path);

// True when the file names a project rather than describing code: a manifest,
// a solution, a lockfile-adjacent descriptor.
[[nodiscard]] bool is_manifest(std::string_view relative_path);

// Directory names that are build output or dependency trees, and must not be
// indexed as if a person had written them.
[[nodiscard]] bool is_generated_directory(std::string_view name);
[[nodiscard]] bool is_vendored_directory(std::string_view name);

// The file name only, lowercased, with no directory part.
[[nodiscard]] std::string file_name_of(std::string_view relative_path);
// The extension including the dot, lowercased. Empty when there is none.
[[nodiscard]] std::string extension_of(std::string_view relative_path);

}  // namespace localcode::repo
