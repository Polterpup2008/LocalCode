#pragma once
// ---------------------------------------------------------------------------
// What a finished run should be reported as.
//
// The engine already keeps the engineering verdict and the account of it apart.
// Turning that pair into the one sentence a user reads is a decision, and it
// belongs here rather than in a window: no test can reach a Qt slot, and the
// question "was this a failure?" is exactly the one that must not be answered
// differently by the CLI and the GUI.
//
// The rule it encodes, in full:
//
//   objective verified, summary produced   -> Verified
//   objective verified, summary missing    -> VerifiedWithoutSummary
//   objective not verified                 -> NotVerified
//   cancelled                              -> Cancelled
//
// The middle one is the whole reason this exists. It is not a failure, and a
// report that calls it one throws away work that was done and checked.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include "Agent.h"
#include "Presentation.h"

namespace localcode {

enum class OutcomeKind {
    Verified,                // the work stands and was described
    VerifiedWithoutSummary,  // the work stands; the description did not arrive
    // The model answered a request that set no acceptance criteria. Nothing was
    // verified because nothing was claimed - which is not the same as failing
    // to verify something, and must not borrow the word "verified".
    Answered,
    NotVerified,             // the objective was never established
    Cancelled,
};

[[nodiscard]] std::string_view to_string(OutcomeKind kind) noexcept;

struct OutcomeReport {
    OutcomeKind       kind = OutcomeKind::NotVerified;
    bool              objective_verified = false;
    PresentationState presentation = PresentationState::None;
    int               finalization_attempts = 0;

    // The finalizer never ran: the work was not established enough to describe,
    // so no summary was attempted. Distinct from a finalizer that ran and could
    // not produce one, and reporting the two the same way would blame the
    // feature for a guard doing its job.
    bool finalization_gated_off = false;
    bool plan_complete = false;
    bool plan_has_failure = false;
    // Whether the run set itself any acceptance criteria at all. A question
    // that asks for no work has none, and "not verified" is then a statement
    // about nothing rather than a problem to report.
    bool had_plan = false;
    // The model produced its answer. Coding work can still be NotVerified;
    // Answered is reserved for requests without a plan or recorded changes.
    bool completed = false;

    std::vector<std::string> unsettled;  // criteria never checked
    std::vector<std::string> disputed;   // criteria an independent check contests

    // One line, safe to show as-is. Never claims more than the ledger holds.
    std::string headline;
};

[[nodiscard]] OutcomeReport describe_outcome(const AgentRunSummary& summary);

}  // namespace localcode
