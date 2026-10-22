#pragma once
// ---------------------------------------------------------------------------
// What was achieved and what was reported are not the same thing.
//
// Measured on Qwen3.5-9B over ten runs of the Fibonacci benchmark: every run
// reached ObjectiveState = VERIFIED with a clean ledger, and eight ended with
// the model emitting reasoning and no prose. All ten were recorded as
// incomplete, which is false about the engineering - the programs were written,
// built, run, and their output compared against the criteria.
//
// The failure is not in the agent loop. It reproduces with one HTTP request:
// hand the model a finished, verified task and ask for a summary, and it spends
// its whole budget thinking. Capping reasoning globally moved the clean rate
// from 20% to 40% and no further, which is why the remedy belongs here rather
// than in a generation setting.
//
// So presentation is tracked separately from the objective. A run may honestly be
//
//     ObjectiveState    = VERIFIED
//     PresentationState = FinalizationFailed
//
// and that pair must survive to the report, because "the work is not done" and
// "the work is done and could not be described" call for different responses
// from whoever reads it.
//
// Nothing here may change what was verified. The finalizer summarises the
// ledger. It never adds to it.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <vector>

namespace localcode {

class VerificationLedger;
class TaskState;

// How far the run got in telling the user what happened. Orthogonal to whether
// the work itself succeeded.
enum class PresentationState {
    None,            // no final response was called for
    Pending,         // the objective is established; the summary is not written
    ReasoningOnly,   // the model thought and said nothing the user can read
    Ready,           // prose exists but has not been checked against evidence
    Emitted,         // prose exists and the evidence check accepted it
    Failed,          // the bounded attempts were used up without prose
};

[[nodiscard]] std::string_view to_string(PresentationState state) noexcept;

// True for the states where engineering succeeded but the user got nothing
// readable, which is the case the report must not describe as failed work.
[[nodiscard]] bool is_presentation_failure(PresentationState state) noexcept;

// The facts a summary may rest on, pulled from the ledger rather than from the
// conversation. Compact on purpose: the finalizer is not re-deriving the work,
// it is describing what is already established, and a model handed the whole
// history tends to start solving the task again.
struct EvidenceDigest {
    std::string objective;
    // "criterion - settled by E003" / "criterion - NOT settled".
    std::vector<std::string> settled;
    std::vector<std::string> unsettled;
    std::vector<std::string> files_changed;
    // Host-recorded closures, not model claims or parsed tool-output text.
    std::vector<std::string> unexecuted_actions;
    // The observations themselves: what ran, and what it showed.
    std::vector<std::string> evidence;
    // Stage results worth stating, including the ones that do not apply.
    std::vector<std::string> stages;
    // Things true and unflattering: a conflicted criterion, a gap in the plan.
    std::vector<std::string> warnings;

    bool objective_verified = false;

    [[nodiscard]] bool has_anything() const;
};

// Builds the digest from state LocalCode owns. The objective is the user's own
// words; everything else comes from the ledger and the task record.
[[nodiscard]] EvidenceDigest build_evidence_digest(std::string objective,
                                                   const VerificationLedger& ledger,
                                                   const TaskState& task);

// The request sent to the model for a final user-facing answer.
//
// `attempt` is 1 for the ordinary ask and 2 for the strict retry. The retry is
// deliberately smaller and blunter: the first ask having produced only thinking
// is evidence that more instructions are not what is missing.
[[nodiscard]] std::string finalization_request(const EvidenceDigest& digest, int attempt);

// How many finalization attempts a run may make before the outcome is recorded
// as a presentation failure. One ordinary ask, one strict retry; a model that
// will not answer twice will not answer the third time either, and looping on it
// spends the user's evening to no purpose.
inline constexpr int kMaxFinalizationAttempts = 2;

// Whether a reply counts as a usable final answer. Reasoning with no content is
// not an answer; neither is whitespace, nor a fragment so short it cannot say
// anything about the work.
[[nodiscard]] bool is_usable_final_answer(std::string_view content);

}   // namespace localcode
