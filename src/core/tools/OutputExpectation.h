#pragma once
// ---------------------------------------------------------------------------
// Checking what a program actually printed.
//
// Running a program tells you it ran. It does not tell you the answer was
// right - a run that printed 1 3 6 12 24 48 96 192 384 768 exited 0 and was
// reported as having printed the first ten Fibonacci numbers correctly.
//
// So an expectation is stated at the moment the program is run, and LocalCode
// - not the model - performs the comparison against the captured output. The
// failure text carries both sides, expected and actual, so the discrepancy is
// visible rather than summarised.
//
// This does not make the model's expectation true. It makes the expectation
// explicit, checkable and recorded: a wrong expectation is now a stated claim
// in the transcript rather than an unexamined assumption.
// ---------------------------------------------------------------------------
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../util/Json.h"
#include "../util/Result.h"

namespace localcode {

struct OutputExpectation {
    bool        present = false;
    // What this comparison proves, in the terms of the task: "prints the first
    // ten Fibonacci numbers".
    std::string description;
    // True when the description was derived from the comparisons because none
    // was given. Reported rather than hidden: a verified behaviour must never
    // appear to carry a stated intent that nobody stated.
    bool        description_derived = false;

    std::optional<int>         exit_code;
    std::vector<std::string>   stdout_contains;
    std::vector<std::string>   stdout_absent;
    // Compared after collapsing runs of whitespace, so line breaks and column
    // padding do not decide correctness.
    std::optional<std::string> stdout_equals;
    std::optional<std::string> stdout_matches;   // ECMAScript regex, searched
    // Checks stderr rather than stdout for the phrases above.
    bool                       use_stderr = false;
    // Which acceptance criterion this comparison settles, 1-based, or 0 for
    // none. Named explicitly rather than guessed from the wording: matching a
    // check to a criterion by how similar the sentences look is exactly the
    // sort of plausible inference this subsystem exists to remove.
    int                        criterion = 0;
    // Every criterion this one check settles. A single run of the program can
    // legitimately settle "prints the first ten Fibonacci numbers" and "runs
    // and produces the correct output" at once - they are the same evidence,
    // and making the caller run the program twice to say so is bookkeeping for
    // its own sake. The caller names the list; nothing is inferred.
    //
    // `criterion` stays as the first entry so everything reading a single
    // number keeps working.
    std::vector<int>           criteria;
    // Set when `criterion` arrived as text rather than a number - a caller
    // naming the criterion by its wording instead of its position. Resolved
    // against the plan by the caller of this parser, which is the only place
    // the plan is visible. Never silently discarded: dropping it settled
    // nothing and said nothing, and a run spent its remaining turns re-running
    // a check that had already passed.
    std::string                criterion_text;

    // Returns an error when the object is present but malformed, so a
    // mistyped expectation is reported instead of silently checking nothing.
    static Result<OutputExpectation> from_json(const Json& json);

    [[nodiscard]] bool has_any_check() const;
    // The comparisons, spelled out, for the report and the transcript.
    [[nodiscard]] std::vector<std::string> describe_checks() const;
};

struct ExpectationOutcome {
    bool                     evaluated = false;
    bool                     satisfied = false;
    std::vector<std::string> passed;
    std::vector<std::string> failures;   // each states expected and actual
};

// Collapses runs of whitespace and trims, so "0 1 1" and "0\n1\n1\n" compare
// equal. Exposed for the tests that pin this behaviour down.
[[nodiscard]] std::string normalise_whitespace(std::string_view text);

[[nodiscard]] ExpectationOutcome evaluate_expectation(const OutputExpectation& expectation,
                                                      std::string_view stdout_text,
                                                      std::string_view stderr_text,
                                                      int exit_code);

}  // namespace localcode
