#pragma once
// ---------------------------------------------------------------------------
// Code written against a toolchain that is not on this machine.
//
// Asked for a game, a model wrote `#include "CoreMinimal.h"`, a `UCLASS`-style
// input struct, and then announced it would "create the level in Unreal". No
// Unreal installation existed anywhere on the machine. The system prompt
// already said to check whether an engine is present before choosing it; the
// model chose first and never checked.
//
// The cost is not only a wasted run. A stack that cannot be built here can
// never reach the BUILD stage, so it can never reach BEHAVIOUR either - every
// row of the verification table stays "not established" for the rest of time,
// and the work is unfinishable by construction rather than by mistake.
//
// So the signal is raised where it is cheapest to act on: at the moment the
// file is written. This detects the dependency and reports it. It does not
// refuse the write, and it does not choose a stack - the user may well own an
// Unreal licence and want scaffolding for a machine that is not this one.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <vector>

namespace localcode {

struct ToolchainSignal {
    std::string toolchain;   // "Unreal Engine"
    std::string matched;     // the line that gave it away
    int         line = 0;
    // An executable that would exist if the toolchain were installed, or empty
    // when there is no single program that settles it.
    std::string probe;
    // Whether that probe was looked for and came back empty. False when there
    // is no probe to run, which is not the same as "it is here".
    bool        probe_failed = false;

    [[nodiscard]] std::string describe() const;
};

// Signature match only - no filesystem access, so this stays cheap and
// testable. `probe` is filled in; running it is the caller's job.
[[nodiscard]] std::vector<ToolchainSignal> detect_foreign_toolchains(std::string_view path,
                                                                     std::string_view content);

}  // namespace localcode
