#pragma once
// ---------------------------------------------------------------------------
// Where a path came from.
//
// Not all paths deserve the same treatment when they fail. A path the user
// typed, or one a tool printed a moment ago, is very likely real and the
// failure is worth investigating. A path that is absolute, does not exist,
// lies outside the workspace, and matches nothing LocalCode has ever produced
// was almost certainly assembled by the model out of nothing - and telling it
// "no such file" invites it to try a neighbouring guess.
//
// Measured: a run wrote to /scratchpad/fib60/main.cpp, then /scratchpad/fib60,
// four times, and was stopped. Another spent its remaining turns on
// "C:\Program Files\...\VC\Tools\Microsoft MSVC\14.44.35207\..." - the real
// directory is "MSVC", not "Microsoft MSVC". Neither path had been seen
// anywhere; both were plausible and invented.
//
// A deliberate limit: this classifies what LocalCode can actually establish
// from the path and the workspace, not a full chain of custody. Distinguishing
// "the user typed this" from "a tool printed it" would need every tool to
// record every path it emits, and the distinction does not change what the
// recovery message should say. What matters is the one class that is reliably
// detectable, and it is the one that was costing runs.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <string>
#include <string_view>

namespace localcode {

class Workspace;

enum class PathOrigin {
    // Relative, so it means something definite: the workspace root plus this.
    WorkspaceRelative,
    // Absolute and inside the workspace. Ordinary, if verbose.
    InsideWorkspace,
    // Absolute, outside the workspace, and really there. Reading it may still
    // be refused, but the path itself is a fact about this machine.
    ExistsOutside,
    // A resource LocalCode itself resolved and recorded - a built program, a
    // located tool.
    ResourceResolved,
    // Absolute, outside the workspace, not on disk, and matching nothing
    // LocalCode has produced. Nothing supports it existing.
    Unsupported,
};

[[nodiscard]] std::string_view to_string(PathOrigin origin) noexcept;

// Classifies a model-supplied path against what the workspace knows.
[[nodiscard]] PathOrigin classify_path_origin(const Workspace& workspace,
                                              const std::string& input);

// The message for an Unsupported path: what was asked for, that nothing
// supports it, and the forms that do work. Empty for every other origin, so a
// caller can append it unconditionally.
//
// Deliberately does not rewrite the path. Guessing that /scratchpad/x meant
// <root>/x would hide a real mistake and make one prefix mean two things.
[[nodiscard]] std::string unsupported_path_note(const Workspace& workspace,
                                                const std::string& input);

}  // namespace localcode
