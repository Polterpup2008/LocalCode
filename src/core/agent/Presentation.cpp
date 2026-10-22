#include "Presentation.h"

#include "../util/Strings.h"
#include "TaskState.h"
#include "ToolCallParser.h"
#include "Verification.h"

namespace localcode {

std::string_view to_string(PresentationState state) noexcept {
    switch (state) {
        case PresentationState::None: return "none";
        case PresentationState::Pending: return "pending";
        case PresentationState::ReasoningOnly: return "reasoning only";
        case PresentationState::Ready: return "ready";
        case PresentationState::Emitted: return "emitted";
        case PresentationState::Failed: return "finalization failed";
    }
    return "none";
}

bool is_presentation_failure(PresentationState state) noexcept {
    return state == PresentationState::Failed || state == PresentationState::ReasoningOnly;
}

bool EvidenceDigest::has_anything() const {
    return !settled.empty() || !unsettled.empty() || !files_changed.empty() || !evidence.empty() ||
           !stages.empty() || !unexecuted_actions.empty();
}

bool is_usable_final_answer(std::string_view content) {
    const std::string trimmed = strings::trim(std::string{content});
    if (trimmed.empty()) return false;
    if (ToolCallParser::contains_unquoted_tool_intent(trimmed)) return false;
    if (ToolCallParser::looks_like_tool_call(trimmed)) {
        const auto parsed = ToolCallParser::parse(trimmed);
        if ((!parsed.calls.empty() || !parsed.malformed.empty()) &&
            strings::trim(parsed.remaining_text).empty())
            return false;
    }
    // A reply of two or three characters is not a summary of anything. Measured
    // cases include "." and "OK" arriving after the model spent its budget
    // thinking, which read as success to an emptiness check and told the user
    // nothing at all.
    return trimmed.size() >= 12;
}

EvidenceDigest build_evidence_digest(std::string objective, const VerificationLedger& ledger,
                                     const TaskState& task) {
    EvidenceDigest digest;
    digest.objective = std::move(objective);
    digest.objective_verified = ledger.holds(Stage::Objective);

    // --- Criteria, split by whether anything settled them -----------------
    //
    // Both halves are given to the finalizer. A model told only what passed
    // will describe a partial result as a complete one, which is the failure
    // the whole verification subsystem exists to prevent - it must not be
    // reintroduced at the last step by handing over a flattering summary.
    const auto& plan = ledger.acceptance_plan();
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const auto& check = plan[i];
        std::string line = std::to_string(i + 1) + ". " + check.description;
        if (check.settled) {
            line += check.satisfied ? "  [associated check passed" : "  [associated check failed";
            if (!check.evidence_id.empty()) line += ", by " + check.evidence_id;
            line += "]";
            if (!check.conflict.empty()) {
                line += "  (disputed: " + check.conflict + ")";
            }
            digest.settled.push_back(line);
        } else {
            digest.unsettled.push_back(line + "  [never checked]");
        }
    }

    if (!ledger.plan_gap().empty()) {
        digest.warnings.push_back("The criteria do not cover the request on their own: " +
                                  ledger.plan_gap());
    }
    for (const auto& check : plan) {
        if (!check.conflict.empty() && check.settled && check.satisfied) {
            digest.warnings.push_back(
                "A criterion was settled but is disputed by an "
                "independent check: " +
                check.description);
        }
    }

    // --- Stages -----------------------------------------------------------
    for (const Stage stage : {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution,
                              Stage::Behaviour, Stage::Objective}) {
        const StageStatus status = ledger.status(stage);
        // A changed tree can have passing caller-associated comparisons while
        // no build was classified. State that absence without making a build
        // a prerequisite for completion or changing any ledger outcome.
        if (status == StageStatus::Unknown &&
            !(stage == Stage::Build && !task.files_changed().empty()))
            continue;
        std::string line = std::string{to_string(stage)} + ": " + std::string{to_string(status)};
        digest.stages.push_back(line);
    }

    // --- Files ------------------------------------------------------------
    for (const auto& file : task.files_changed()) {
        digest.files_changed.push_back(file.action + ": " + file.path);
    }

    // --- Evidence ---------------------------------------------------------
    //
    // Bounded. The point is to give the finalizer the observations it may cite,
    // not to replay the run; a long tail of early exploratory calls adds tokens
    // and invites the model to narrate the process instead of the result.
    constexpr std::size_t kMaxEvidence = 12;
    const auto& records = ledger.evidence();
    const std::size_t first = records.size() > kMaxEvidence ? records.size() - kMaxEvidence : 0;
    for (std::size_t i = first; i < records.size(); ++i) {
        digest.evidence.push_back(records[i].describe());
    }

    return digest;
}

namespace {

void append_section(std::string& out, std::string_view title,
                    const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    out += "\n";
    out += title;
    out += "\n";
    for (const auto& line : lines) {
        out += "  " + line + "\n";
    }
}

}   // namespace

std::string finalization_request(const EvidenceDigest& digest, int attempt) {
    std::string out;

    if (attempt <= 1) {
        out =
            "The action phase is closed. Report the recorded results and unfinished "
            "work without starting any new actions.\n\n"
            "Write the final answer for the user now in the requested format.\n";
    } else {
        // The strict retry. The first ask produced thinking and no answer, so
        // the missing ingredient is not more instruction - it is fewer moving
        // parts and an unmistakable demand for visible text.
        out =
            "Your previous reply contained only reasoning or otherwise no usable "
            "visible answer. The record remains unchanged.\n\n"
            "Write the answer itself this time. Not a plan, not analysis - the "
            "text the user reads.\n";
    }

    out +=
        "\nRules for this message:\n"
        "  - Use only the facts listed below. They are the whole record.\n"
        "  - Do not call tools. Actions are closed; report any unfinished work honestly.\n"
        "  - Do not claim any build, test, run or check that is not listed.\n"
        "  - State plainly anything that was not established.\n"
        "  - Criterion associations are supplied by the caller. A passing associated check "
        "establishes its recorded comparisons, not every implication of its description. "
        "Keep those observations separate from the classified stage results.\n";

    if (attempt > 1) {
        out +=
            "  - Do not restate your reasoning. Give the conclusion.\n"
            "  - Keep the answer concise within the requested format.\n";
    }

    if (!digest.objective.empty()) {
        out += "\nObjective shorthand (the human conversation contains the full request):\n  " +
               digest.objective + "\n";
    }

    append_section(out, "Criteria that were checked:", digest.settled);
    append_section(out, "Criteria that were NOT checked:", digest.unsettled);
    append_section(out, "Files changed:", digest.files_changed);
    if (digest.files_changed.empty()) out += "\nNo file changes were recorded for this task.\n";
    append_section(out, "Actions not executed:", digest.unexecuted_actions);
    append_section(out, "What was established:", digest.stages);
    append_section(out, "Evidence:", digest.evidence);
    append_section(out, "Worth stating:", digest.warnings);

    if (!digest.unsettled.empty()) {
        out +=
            "\nSome criteria were never checked. Say so. Describing the task as "
            "fully done would be false.\n";
    }

    return out;
}

}   // namespace localcode
