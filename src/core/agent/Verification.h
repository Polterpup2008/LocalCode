#pragma once
// ---------------------------------------------------------------------------
// Knowing the difference between doing something and proving it was done right.
//
// A run built a program, ran it to exit code 0, and reported that the first ten
// Fibonacci numbers were "printed correctly". The program had printed
// 1 3 6 12 24 48 96 192 384 768. Every piece of evidence LocalCode held was
// true - it configured, it built, it ran - and the conclusion drawn from them
// was false, because none of them was about the answer.
//
// So verification is modelled as stages with a direction of dependency:
//
//     SOURCE -> CONFIGURE -> BUILD -> EXECUTION -> BEHAVIOUR -> OBJECTIVE
//                              \-> TEST ----------^
//
// and two rules that the rest of the system leans on:
//
//   * A stage is only as current as the source it was established against. Edit
//     a file and every downstream stage becomes STALE, not PASS.
//   * A lower stage never implies a higher one. Building is not running,
//     running is not behaving, behaving is not the objective.
//
// Nothing here decides whether a program is correct. It records what was
// checked, against which revision, and by which piece of evidence, so that a
// claim can be tested against something other than the model's confidence.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../tools/Tool.h"

namespace localcode {

enum class Stage {
    Source,      // the tree itself; its "status" is its revision
    Configure,   // a build system exists and accepted the project
    Build,       // the project compiled and linked
    Test,        // the project's own tests ran and passed
    Execution,   // the produced program ran
    Behaviour,   // it did what was asked of it
    Objective,   // the user's request is satisfied
};

[[nodiscard]] std::string_view to_string(Stage stage) noexcept;

// The stages that must hold before this one means anything.
[[nodiscard]] std::vector<Stage> prerequisites_of(Stage stage);

enum class StageStatus {
    Unknown,        // never established
    Pass,
    Fail,
    Stale,          // was established, but against an older revision
    NotApplicable,  // does not apply to this task - a compile-only request has
                    // no behaviour to check, and saying "unknown" would be a lie
};

[[nodiscard]] std::string_view to_string(StageStatus status) noexcept;

// One thing that was actually observed, with an identity so a claim can point
// at it.
struct EvidenceRecord {
    std::string  id;          // E001, E002, ...
    EvidenceKind kind = EvidenceKind::CommandRun;
    std::string  subject;     // the command, file, or check
    std::string  detail;      // exit code, output excerpt, comparison
    bool         positive = true;
    int          revision = 0;   // source revision when this was captured
    std::int64_t at_unix_ms = 0;

    [[nodiscard]] std::string describe() const;
};

struct StageState {
    StageStatus  status = StageStatus::Unknown;
    // The source revision this was established against. Compared with the
    // ledger's current revision to decide staleness.
    int          established_at_revision = -1;
    std::string  evidence_id;
    std::string  detail;
    std::int64_t at_unix_ms = 0;
};

// Where a criterion came from, which decides how much it is worth.
//
// Writing a criterion down does not make it true. Asked for the first ten
// Fibonacci numbers, a model wrote its own criterion as
// "1, 1, 2, 3, 5, 8, 13, 21, 34, 55" - a defensible convention, and not the
// one the user asked for - and would then have verified against its own
// definition of the answer and reported success. The check would have been
// real. The expectation would have been wrong.
//
// So a criterion carries its origin, and the origin is what the verifier
// weighs. Ordered from strongest to weakest.
enum class CriterionSource {
    UserExplicit,        // stated in the request, in the user's own words
    UserDerived,         // computed from the request by a rule, not by opinion
    RepositoryDerived,   // read out of the project - a README, a spec, a schema
    TestDerived,         // the project's own tests already assert it
    ReferenceDerived,    // an independent implementation or known-good answer
    ModelInferred,       // the model decided this was needed. Plausible, unproven
    // Something worth doing that the task did not ask for - a nicety the model
    // added. It may be true and still must not decide whether the objective is
    // met, because nobody asked for it.
    OptionalRecommendation,
};

[[nodiscard]] std::string_view to_string(CriterionSource source) noexcept;

// How far a criterion can be trusted once its origin and any independent check
// are taken together.
enum class CriterionTrust {
    Trusted,        // from the user, or derived from the user's words by rule
    Derived,        // computed or read from something outside the model
    ModelProposed,  // the model's own idea of what "done" means
    Conflicted,     // an independent check disagrees with it - see `conflict`
    // Nothing has been established about this criterion yet: no evidence has
    // settled it and no independent check has looked at it. Distinct from
    // ModelProposed, which is a claim about where it came from.
    Unverified,
};

[[nodiscard]] std::string_view to_string(CriterionTrust trust) noexcept;

// One thing that has to be true for the task to be done, written down before it
// is attempted so that "done" is not decided after the fact.
struct AcceptanceCheck {
    std::string     description;
    CriterionSource source = CriterionSource::ModelInferred;
    // Why it has that origin, in a phrase - "stated in the request",
    // "computed from the sequence the user named".
    std::string     reason;
    // What it was derived from, when it was derived: the criterion it follows
    // from, or the file it was read out of.
    std::string     derived_from;

    // Set when something independent of the model disagrees with it. A
    // conflicted criterion can still be settled, and settling it never makes
    // the objective pass.
    std::string     conflict;

    // Filled in when the check is settled.
    bool            settled = false;
    bool            satisfied = false;
    std::string     evidence_id;

    [[nodiscard]] CriterionTrust trust() const;
};

class VerificationLedger {
public:
    void reset();

    // --- Source ----------------------------------------------------------
    [[nodiscard]] int revision() const { return revision_; }

    // A file changed. Bumps the revision, which makes every stage established
    // against an earlier one stale. Build-system and test files are recorded
    // too, because they invalidate the same things.
    void note_source_changed(const std::string& path);

    // Stored proof is history until checked against a resumed workspace.
    void invalidate_for_resume() { ++revision_; }

    [[nodiscard]] const std::vector<std::string>& changed_paths() const { return changed_; }

    // --- Evidence --------------------------------------------------------
    std::string add_evidence(EvidenceKind kind, std::string subject, std::string detail,
                             bool positive);
    [[nodiscard]] const std::vector<EvidenceRecord>& evidence() const { return evidence_; }
    [[nodiscard]] const EvidenceRecord* find_evidence(std::string_view id) const;

    // --- Stages ----------------------------------------------------------
    void record(Stage stage, StageStatus status, std::string evidence_id, std::string detail);
    void mark_not_applicable(Stage stage, std::string reason);

    // The stored state, unresolved.
    [[nodiscard]] StageState raw(Stage stage) const;
    // The state as it stands now: a pass established against an older revision
    // reads as STALE.
    [[nodiscard]] StageStatus status(Stage stage) const;
    [[nodiscard]] bool is_stale(Stage stage) const;

    // True only when the stage is a current pass.
    [[nodiscard]] bool holds(Stage stage) const { return status(stage) == StageStatus::Pass; }

    // --- Acceptance plan --------------------------------------------------
    void set_acceptance_plan(std::vector<AcceptanceCheck> checks);
    // A follow-up request is a different question about the same tree. The
    // stages keep what they proved; the definition of done does not carry over.
    void clear_acceptance_plan();
    [[nodiscard]] const std::vector<AcceptanceCheck>& acceptance_plan() const { return plan_; }

    // Why this plan cannot establish the objective on its own, even if every
    // criterion in it settles cleanly - a plan that only checks the build for
    // a task about a crash, say. Empty when the plan covers what was asked.
    void record_plan_gap(std::string reason);
    [[nodiscard]] const std::string& plan_gap() const { return plan_gap_; }
    void settle_check(std::size_t index, bool satisfied, std::string evidence_id);
    // Records that a criterion turned out to rest on something other than the
    // model's own judgement, and why.
    void ground_check(std::size_t index, CriterionSource source, std::string reason,
                      std::string derived_from);
    // Records that something independent disagrees with a criterion. A disputed
    // criterion can still be settled; settling it never makes the objective pass.
    void dispute_check(std::size_t index, std::string conflict);
    [[nodiscard]] bool plan_is_complete() const;
    [[nodiscard]] bool plan_has_failure() const;

    // Recomputes Objective from the plan and the stages. Objective is never set
    // directly by anything the model says.
    void reassess_objective();

    // --- Rendering --------------------------------------------------------
    // The compact block the model sees, and the same facts for the interface.
    [[nodiscard]] std::string render_states() const;
    [[nodiscard]] std::string render_plan() const;
    [[nodiscard]] std::string render_recent_evidence(std::size_t limit = 8) const;

    [[nodiscard]] Json to_json() const;
    static VerificationLedger from_json(const Json& json);

private:
    int                          revision_ = 0;
    std::vector<std::string>     changed_;
    std::vector<EvidenceRecord>  evidence_;
    std::vector<StageState>      stages_;
    std::vector<AcceptanceCheck> plan_;
    std::string                  plan_gap_;
    int                          next_evidence_ = 1;

    [[nodiscard]] StageState& slot(Stage stage);
    [[nodiscard]] const StageState& slot(Stage stage) const;
};

// --- The completion gate ---------------------------------------------------
// Whether a run may be reported as finished, and if not, what is missing.
//
// This is the point of the whole subsystem. The model says it is done; the
// ledger says what was established. Where they disagree, the ledger wins,
// because it is made of things that were observed and the sentence is not.
struct CompletionAssessment {
    bool                     blocked = false;
    // Short statements of what is missing, for the report and the interface.
    std::vector<std::string> reasons;
    // True things the answer should say that are not grounds to refuse it.
    //
    // "Every criterion was your own idea of what the task meant" is worth
    // saying and is not worth blocking on: for most tasks the model's reading
    // is the only definition available, and refusing those forever would make
    // the gate useless rather than strict. A criterion something independent
    // actually disagrees with is different, and that does block.
    std::vector<std::string> caveats;
    // The instruction handed back to the model, phrased so it can act on it.
    std::string              guidance;
};

// `work_was_done` is false for a run that only answered a question - nothing
// was written, built or run - and such a run is never gated. Verifying an
// answer that changed nothing would mean demanding a build for "what does this
// function do?".
[[nodiscard]] CompletionAssessment assess_completion(const VerificationLedger& ledger,
                                                     bool work_was_done);

// Explicit answer/status headings claiming the entire objective was verified.
// Individual checks and acknowledgements of missing verification do not qualify.
[[nodiscard]] bool claims_verified_objective(std::string_view message);

}  // namespace localcode
