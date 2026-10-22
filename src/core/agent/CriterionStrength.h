#pragma once
// ---------------------------------------------------------------------------
// Does the plan actually test what was asked?
//
// A criterion can be perfectly true and still prove nothing about the task. The
// dangerous shape is not a false criterion - the reference checker catches
// those - but a true one that is beside the point:
//
//   "Fix the crash when opening an empty file."
//     criterion: "the project builds"          - it built before the fix too
//
//   "Return JSON with count = 4."
//     criterion: "the JSON contains a count field"   - so does count = 0
//
// Both settle cleanly against real evidence, and neither says anything about
// the thing the user wanted. An agent that writes its own definition of done
// will drift towards the definition it can satisfy, and "it compiles" is always
// the easiest one available.
//
// The opposite case matters just as much and is the reason this is not simply
// strict: when the task IS "make it compile", "a fresh build succeeds" is the
// whole of it, and demanding a behavioural check there would be noise.
//
// So this compares two things it can read off the text: what the request
// demands, and what the plan covers. It reports; it does not rewrite anyone's
// criteria.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <vector>

namespace localcode {

// What the request is asking to be true when the work is done.
enum class ObjectiveDemand {
    // Nothing decisive in the wording. Most tasks land here, and nothing is
    // claimed about them.
    Unknown,
    // The task is about the code compiling, linking or configuring, and says
    // nothing about what it then does.
    BuildOnly,
    // The task is about what the program does when it runs: what it prints,
    // returns, or stops crashing at.
    RuntimeBehaviour,
};

[[nodiscard]] ObjectiveDemand demand_of(std::string_view objective);

struct StrengthFinding {
    // True when the plan cannot establish the objective no matter how cleanly
    // every criterion settles. False for a plan that is merely thin.
    bool        insufficient = false;
    std::string reason;    // what the plan is missing, in the task's own terms
    std::string suggestion;  // the criterion that would close the gap
};

// Compares a request with the criteria proposed for it. Returns nothing when
// the plan covers what the request demands, or when the request is not one this
// can read - silence is the normal answer.
[[nodiscard]] StrengthFinding assess_plan_strength(std::string_view objective,
                                                   const std::vector<std::string>& criteria);

}  // namespace localcode
