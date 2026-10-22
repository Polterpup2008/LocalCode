#include "Outcome.h"

namespace localcode {

std::string_view to_string(OutcomeKind kind) noexcept {
    switch (kind) {
        case OutcomeKind::Verified:               return "verified";
        case OutcomeKind::VerifiedWithoutSummary: return "verified, no summary";
        case OutcomeKind::Answered:               return "answered";
        case OutcomeKind::NotVerified:            return "not verified";
        case OutcomeKind::Cancelled:              return "cancelled";
    }
    return "not verified";
}

OutcomeReport describe_outcome(const AgentRunSummary& summary) {
    OutcomeReport report;
    report.objective_verified = summary.ledger.holds(Stage::Objective);
    report.presentation = summary.presentation;
    report.finalization_attempts = summary.finalization_attempts;
    report.plan_complete = summary.ledger.plan_is_complete();
    report.plan_has_failure = summary.ledger.plan_has_failure();
    report.had_plan = !summary.ledger.acceptance_plan().empty();
    report.completed = summary.completed;

    for (const auto& check : summary.ledger.acceptance_plan()) {
        if (!check.settled) report.unsettled.push_back(check.description);
        if (!check.conflict.empty()) {
            report.disputed.push_back(check.description + " - " + check.conflict);
        }
    }

    // No attempt was made. Whatever else is true, the finalizer neither ran nor
    // failed, and the plan above says why not.
    report.finalization_gated_off =
        is_presentation_failure(summary.presentation) && summary.finalization_attempts == 0;

    if (summary.cancelled) {
        report.kind = OutcomeKind::Cancelled;
        report.headline = "Cancelled. Everything done so far is preserved.";
        return report;
    }

    // The model answered. That is an answer, not a failure, even when there
    // was no objective to establish - which is every ordinary question.
    if (summary.completed) {
        const bool coding_result = report.had_plan || !summary.ledger.changed_paths().empty();
        report.kind = report.objective_verified ? OutcomeKind::Verified
                      : coding_result           ? OutcomeKind::NotVerified
                                                : OutcomeKind::Answered;
        report.headline = report.objective_verified ? "The work is verified."
                          : coding_result ? "The work was not verified."
                                          : "Answered. Nothing was claimed to be verified.";
        return report;
    }

    if (!report.objective_verified) {
        report.kind = OutcomeKind::NotVerified;
        report.headline = summary.stop_reason.empty()
                              ? "The objective was not established."
                              : summary.stop_reason;
        return report;
    }

    if (!is_presentation_failure(summary.presentation)) {
        report.kind = OutcomeKind::Verified;
        report.headline = "The work is verified.";
        return report;
    }

    report.kind = OutcomeKind::VerifiedWithoutSummary;
    report.headline =
        report.finalization_gated_off
            // Withheld on purpose. Saying "failed" here would describe the guard
            // as the fault.
            ? "The work is verified, and no summary was attempted."
            : "The work is verified. The summary of it is not.";
    return report;
}

}  // namespace localcode
