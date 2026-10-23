#pragma once
// ---------------------------------------------------------------------------
// What a tool result proves.
//
// This is the only place allowed to answer that question. Everywhere else -
// the agent loop, the interface, the final answer - reads the ledger and does
// not reinterpret raw output for itself, because that is how "exit 0" quietly
// became "the task is done" in the first place.
//
// Two rules it exists to enforce:
//
//   * A command establishes at most the stage it was actually performing.
//     `cmake -B build` is a configure. `cmake --build build` is a build.
//     `build\fib.exe` is an execution. None of them is a behaviour check.
//   * Nothing here ever establishes BEHAVIOUR. A program's output being
//     produced is not the same as its output being right, and no exit code
//     carries that information. Behaviour is established only by an explicit
//     comparison against a stated expectation.
// ---------------------------------------------------------------------------
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../tools/Tool.h"
#include "Verification.h"

namespace localcode {

// What a command was trying to do. Keyed on the program being run, not on the
// text of the whole command line: `vcvars64.bat cmake -B build` runs vcvars64,
// which ignores the rest and exits 0.
enum class CommandIntent {
    Inspection,   // read-only - git status, where, dir. Establishes nothing.
    Configure,
    Build,
    Test,
    Execution,   // running a program that the project produced
    Unknown,
};

[[nodiscard]] std::string_view to_string(CommandIntent intent) noexcept;

// Splits a rendered process command into arguments, honouring double quotes
// and the POSIX single-quote format emitted by ProcessRunner. It is not a shell.
[[nodiscard]] std::vector<std::string> split_command_line(std::string_view line);

[[nodiscard]] CommandIntent classify_intent(std::string_view command_line);

// The stage a given intent can establish. Inspection and Unknown establish
// nothing, and no intent maps to Behaviour or Objective.
[[nodiscard]] std::optional<Stage> stage_for(CommandIntent intent);

// A run that exited 0 without doing the thing. `ctest` in a directory with no
// tests reports success; recording that as a passing test stage would be the
// same lie in a different place.
[[nodiscard]] bool reports_no_tests_ran(std::string_view output);

// Per-invocation facts already established by the evidence interpreter. This
// does not grant verification; the caller can use it for progress feedback.
struct ToolInterpretation {
    bool no_tests_found = false;
};

// Folds one executed tool call into the ledger: notes changed files, and
// records whichever stage the command genuinely established.
//
// `objective` is the user's own words. It is passed so that a criterion or an
// expectation can be checked against what the request itself determines,
// rather than only against what the model believes - the model marking its own
// homework is the failure this whole subsystem exists to catch. Passing the
// model's restatement of the request instead would defeat the point.
void interpret_tool_result(const ToolCall& call, const ToolResult& result,
                           VerificationLedger& ledger, std::string_view objective = {},
                           ToolInterpretation* interpretation = nullptr);

}   // namespace localcode
