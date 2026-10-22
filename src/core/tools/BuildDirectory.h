#pragma once
// ---------------------------------------------------------------------------
// What a build directory already is.
//
// A CMake build tree remembers the generator it was configured with, and that
// memory outlives the attempt that created it. A configure that FAILS still
// writes CMakeCache.txt, so the failure quietly changes the meaning of the next
// command:
//
//   cmake -S . -B build -G Ninja                 fails: ninja is not on PATH
//   cmake -S . -B build -G "Visual Studio 17"    fails: "Does not match the
//                                                generator used previously"
//
// Measured over a whole run: the model alternated between those two commands
// eight times. Each Ninja attempt re-poisoned the tree, each Visual Studio
// attempt hit the mismatch, and neither error mentioned that the first command
// was the reason the second one failed. Both messages were true and neither was
// the point.
//
// LocalCode can read the cache. Knowing which generator a tree holds, whether
// the tree is entirely CMake's own output, and what the next command is about
// to ask for, turns that pair of unrelated-looking failures into one sentence.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <string>
#include <vector>

namespace localcode {

struct BuildDirectoryState {
    std::filesystem::path path;
    bool        exists = false;
    // A CMakeCache.txt is present, so this tree has been configured and its
    // generator is fixed until the cache goes.
    bool        configured = false;
    std::string generator;    // CMAKE_GENERATOR, e.g. "Ninja"
    std::string source_dir;   // CMAKE_HOME_DIRECTORY
    std::string make_program;   // CMAKE_MAKE_PROGRAM may already be an absolute path
    // Every top-level entry is something CMake or the build tool generated, so
    // removing the tree destroys nothing that was not machine-written.
    bool        generated_only = false;
    // Entries that are not recognised as generated output. Non-empty means the
    // directory holds something a person may have put there, and it must not
    // be described as disposable.
    std::vector<std::string> unrecognised;
};

// Reads the state of a build directory. Never throws; a directory that does
// not exist comes back with exists = false and everything else empty.
[[nodiscard]] BuildDirectoryState inspect_build_directory(const std::filesystem::path& path);

// The generator a cmake command line is asking for: the -G value, or empty
// when the command does not name one.
[[nodiscard]] std::string requested_generator(const std::vector<std::string>& argv);

// The build directory a cmake configure would write to: the -B value, or empty
// when the command is not a configure with an explicit binary directory.
[[nodiscard]] std::filesystem::path configure_binary_dir(
    const std::vector<std::string>& argv, const std::filesystem::path& working_directory);

// The build program a generator needs on PATH before it can be used at all:
// "ninja" for the Ninja generators, "make" for Unix Makefiles, and so on.
// Empty for generators that need nothing external - the Visual Studio ones
// locate their whole toolchain themselves.
[[nodiscard]] std::string generator_requires_program(std::string_view generator);

// True when the two names mean the same generator. CMake accepts "Visual Studio
// 17 2022" and records exactly that, but a caller may reasonably write it with
// different spacing or case.
[[nodiscard]] bool same_generator(std::string_view a, std::string_view b);

}  // namespace localcode
