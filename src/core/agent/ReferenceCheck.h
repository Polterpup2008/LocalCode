#pragma once
// ---------------------------------------------------------------------------
// Checking the model's expectation against something that is not the model.
//
// Asked for the first ten Fibonacci numbers, a model wrote its own acceptance
// criterion as "1, 1, 2, 3, 5, 8, 13, 21, 34, 55". That is a defensible
// convention and it is not the sequence the user asked for. Had the run
// continued, LocalCode would have compared the program's output against that
// expectation, found them equal, and reported success. Every check would have
// been real. The answer would still have been wrong, because the model was
// marking its own homework.
//
// So where a task names something with a determinate answer, LocalCode works
// that answer out for itself and compares. This is deliberately not a theorem
// prover and never will be: it recognises a short list of things it can
// compute exactly, says nothing at all about anything else, and when it does
// disagree it raises a conflict rather than substituting its own answer.
//
// Silence here means "no opinion", never "agreed".
// ---------------------------------------------------------------------------
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace localcode {

// What an independent check concluded about one expectation.
struct ReferenceVerdict {
    // False when nothing in the request was recognisable. The overwhelmingly
    // common case, and it must never read as approval.
    bool        applicable = false;
    bool        agrees = false;
    // What LocalCode worked out, in the same shape as the expectation.
    std::string reference;
    // How it was worked out, for the record: "the first ten Fibonacci numbers,
    // computed".
    std::string derivation;
    // Filled in only on disagreement, phrased for the model to act on.
    std::string conflict;
};

// The named sequences and quantities LocalCode can compute exactly. Kept
// small on purpose.
[[nodiscard]] std::optional<std::vector<long long>> reference_sequence(
    std::string_view request);

// Compares an expectation with what the request itself determines.
//
// `request` is the user's own words - not the model's restatement of them,
// which would defeat the point. `expectation` is the output the model says is
// correct.
[[nodiscard]] ReferenceVerdict check_against_reference(std::string_view request,
                                                       std::string_view expectation);

}  // namespace localcode
