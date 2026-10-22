#include "Verification.h"

#include <algorithm>
#include <regex>

#include "../util/Json.h"
#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

constexpr std::size_t kStageCount = 7;

std::size_t index_of(Stage stage) { return static_cast<std::size_t>(stage); }

// Paths whose change invalidates the build even though they are not source in
// the narrow sense.
bool is_build_system_file(const std::string& path) {
    const auto lowered = strings::to_lower(path);
    static const char* kNames[] = {"cmakelists.txt", "cmakepresets.json", "makefile",
                                   "meson.build",    "build.ninja",       "cargo.toml",
                                   "package.json",   "pyproject.toml",    "build.gradle",
                                   "vcxproj",        ".sln",              "conanfile"};
    for (const char* name : kNames) {
        if (lowered.find(name) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

std::string_view to_string(Stage stage) noexcept {
    switch (stage) {
        case Stage::Source:    return "source";
        case Stage::Configure: return "configure";
        case Stage::Build:     return "build";
        case Stage::Test:      return "tests";
        case Stage::Execution: return "execution";
        case Stage::Behaviour: return "behaviour";
        case Stage::Objective: return "objective";
    }
    return "source";
}

std::vector<Stage> prerequisites_of(Stage stage) {
    switch (stage) {
        case Stage::Source:    return {};
        case Stage::Configure: return {};
        case Stage::Build:     return {Stage::Configure};
        case Stage::Test:      return {Stage::Build};
        case Stage::Execution: return {Stage::Build};
        // Behaviour can be reached either by running the program and checking
        // what it did, or by the project's own tests. Neither is required
        // outright, because a task may legitimately have only one of them.
        case Stage::Behaviour: return {};
        case Stage::Objective: return {};
    }
    return {};
}

std::string_view to_string(StageStatus status) noexcept {
    switch (status) {
        case StageStatus::Unknown:       return "not established";
        case StageStatus::Pass:          return "pass";
        case StageStatus::Fail:          return "fail";
        case StageStatus::Stale:         return "stale";
        case StageStatus::NotApplicable: return "not applicable";
    }
    return "not established";
}

std::string_view to_string(CriterionSource source) noexcept {
    switch (source) {
        case CriterionSource::UserExplicit:      return "stated by the user";
        case CriterionSource::UserDerived:       return "derived from the request";
        case CriterionSource::RepositoryDerived: return "read from the project";
        case CriterionSource::TestDerived:       return "asserted by the project's tests";
        case CriterionSource::ReferenceDerived:  return "checked against a reference";
        case CriterionSource::ModelInferred:     return "proposed by the model";
        case CriterionSource::OptionalRecommendation:
            return "an optional improvement nobody asked for";
    }
    return "proposed by the model";
}

std::string_view to_string(CriterionTrust trust) noexcept {
    switch (trust) {
        case CriterionTrust::Trusted:       return "trusted";
        case CriterionTrust::Derived:       return "derived";
        case CriterionTrust::ModelProposed: return "model-proposed";
        case CriterionTrust::Conflicted:    return "conflicted";
        case CriterionTrust::Unverified:    return "unverified";
    }
    return "model-proposed";
}

CriterionTrust AcceptanceCheck::trust() const {
    // A disagreement outranks everything else. It does not matter where a
    // criterion came from if something independent says it is wrong.
    if (!conflict.empty()) return CriterionTrust::Conflicted;

    switch (source) {
        case CriterionSource::UserExplicit:
        case CriterionSource::UserDerived:
            return CriterionTrust::Trusted;
        case CriterionSource::RepositoryDerived:
        case CriterionSource::TestDerived:
        case CriterionSource::ReferenceDerived:
            return CriterionTrust::Derived;
        case CriterionSource::ModelInferred:
            return CriterionTrust::ModelProposed;
        case CriterionSource::OptionalRecommendation:
            // Not the model's idea of what "done" means - the model's idea of
            // what would be nice. It carries no weight over the objective at
            // all, so it is weaker than ModelProposed rather than equal to it.
            return CriterionTrust::Unverified;
    }
    return CriterionTrust::ModelProposed;
}

std::string EvidenceRecord::describe() const {
    std::string out = id + " " + std::string{to_string(kind)};
    if (!subject.empty()) out += " " + subject;
    if (!detail.empty()) out += " - " + strings::utf8_truncate(detail, 200);
    out += positive ? "  [ok]" : "  [failed]";
    out += "  @r" + std::to_string(revision);
    return out;
}

void VerificationLedger::reset() {
    revision_ = 0;
    changed_.clear();
    evidence_.clear();
    stages_.clear();
    plan_.clear();
    plan_gap_.clear();
    next_evidence_ = 1;
}

StageState& VerificationLedger::slot(Stage stage) {
    if (stages_.size() < kStageCount) stages_.resize(kStageCount);
    return stages_[index_of(stage)];
}

const StageState& VerificationLedger::slot(Stage stage) const {
    static const StageState kEmpty;
    if (stages_.size() < kStageCount) return kEmpty;
    return stages_[index_of(stage)];
}

void VerificationLedger::note_source_changed(const std::string& path) {
    ++revision_;
    if (std::find(changed_.begin(), changed_.end(), path) == changed_.end()) {
        changed_.push_back(path);
    }
    log_debug(LogChannel::Agent, "source revision advanced",
              {{"revision", std::to_string(revision_)}, {"path", path}});

    // Nothing is cleared. A stage keeps the revision it was established against,
    // and status() compares that with the current one - which means the history
    // stays inspectable ("the build passed at r3") instead of being erased.
    // Acceptance checks also refer to that revision. Keep their evidence ids
    // for history, but require fresh checks after edits rather than reviving
    // them when an unrelated build subsequently passes.
    for (auto& check : plan_)
        check.settled = false;
    (void)is_build_system_file(path);
}

std::string VerificationLedger::add_evidence(EvidenceKind kind, std::string subject,
                                             std::string detail, bool positive) {
    EvidenceRecord record;
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "E%03d", next_evidence_++);
    record.id = buffer;
    record.kind = kind;
    record.subject = std::move(subject);
    record.detail = std::move(detail);
    record.positive = positive;
    record.revision = revision_;
    record.at_unix_ms = now_unix_ms();

    evidence_.push_back(std::move(record));
    // Bounded; the full history lives in storage.
    if (evidence_.size() > 400) evidence_.erase(evidence_.begin());
    return evidence_.back().id;
}

const EvidenceRecord* VerificationLedger::find_evidence(std::string_view id) const {
    for (const auto& record : evidence_) {
        if (record.id == id) return &record;
    }
    return nullptr;
}

void VerificationLedger::record(Stage stage, StageStatus status, std::string evidence_id,
                                std::string detail) {
    auto& state = slot(stage);
    state.status = status;
    state.established_at_revision = revision_;
    state.evidence_id = std::move(evidence_id);
    state.detail = std::move(detail);
    state.at_unix_ms = now_unix_ms();

    log_debug(LogChannel::Agent, "verification stage recorded",
              {{"stage", std::string{to_string(stage)}},
               {"status", std::string{to_string(status)}},
               {"revision", std::to_string(revision_)}});
    if (stage != Stage::Objective) reassess_objective();
}

void VerificationLedger::mark_not_applicable(Stage stage, std::string reason) {
    auto& state = slot(stage);
    state.status = StageStatus::NotApplicable;
    state.established_at_revision = revision_;
    state.detail = std::move(reason);
    state.at_unix_ms = now_unix_ms();
    if (stage != Stage::Objective) reassess_objective();
}

StageState VerificationLedger::raw(Stage stage) const { return slot(stage); }

StageStatus VerificationLedger::status(Stage stage) const {
    const auto& state = slot(stage);
    if (state.status == StageStatus::Unknown) return StageStatus::Unknown;
    if (state.status == StageStatus::NotApplicable) return StageStatus::NotApplicable;

    // A pass established before the current revision no longer says anything
    // about the current source. A failure does not go stale in the same way -
    // it stays a failure until something re-establishes it - but it is still
    // reported against its revision.
    if (state.status == StageStatus::Pass && state.established_at_revision < revision_) {
        return StageStatus::Stale;
    }
    return state.status;
}

bool VerificationLedger::is_stale(Stage stage) const {
    return status(stage) == StageStatus::Stale;
}

void VerificationLedger::set_acceptance_plan(std::vector<AcceptanceCheck> checks) {
    plan_ = std::move(checks);
    reassess_objective();
}

void VerificationLedger::settle_check(std::size_t index, bool satisfied,
                                      std::string evidence_id) {
    if (index >= plan_.size()) return;
    plan_[index].settled = true;
    plan_[index].satisfied = satisfied;
    plan_[index].evidence_id = std::move(evidence_id);
    reassess_objective();
}

void VerificationLedger::ground_check(std::size_t index, CriterionSource source,
                                      std::string reason, std::string derived_from) {
    if (index >= plan_.size()) return;
    plan_[index].source = source;
    plan_[index].reason = std::move(reason);
    plan_[index].derived_from = std::move(derived_from);
    plan_[index].conflict.clear();
    reassess_objective();
}

void VerificationLedger::dispute_check(std::size_t index, std::string conflict) {
    if (index >= plan_.size()) return;
    plan_[index].conflict = std::move(conflict);
    log_warn(LogChannel::Agent, "a criterion is disputed",
             {{"criterion", plan_[index].description}, {"conflict", plan_[index].conflict}});
    reassess_objective();
}

bool VerificationLedger::plan_is_complete() const {
    if (plan_.empty()) return false;
    return std::all_of(plan_.begin(), plan_.end(),
                       [](const AcceptanceCheck& c) { return c.settled && c.satisfied; });
}

bool VerificationLedger::plan_has_failure() const {
    return std::any_of(plan_.begin(), plan_.end(),
                       [](const AcceptanceCheck& c) { return c.settled && !c.satisfied; });
}

void VerificationLedger::record_plan_gap(std::string reason) {
    plan_gap_ = std::move(reason);
    reassess_objective();
}

void VerificationLedger::clear_acceptance_plan() {
    plan_.clear();
    plan_gap_.clear();
    auto& objective = slot(Stage::Objective);
    objective = StageState{};
}

void VerificationLedger::reassess_objective() {
    auto& objective = slot(Stage::Objective);

    // Objective is derived, never asserted. It holds only when there is a plan,
    // every check in it is satisfied, and nothing it depends on has gone stale.
    if (plan_has_failure()) {
        objective.status = StageStatus::Fail;
        objective.established_at_revision = revision_;
        objective.detail = "an acceptance check failed";
        return;
    }
    if (!plan_is_complete()) {
        const bool previously_stale = status(Stage::Objective) == StageStatus::Stale;
        objective.status = previously_stale ? StageStatus::Stale : StageStatus::Unknown;
        objective.established_at_revision = revision_;
        objective.detail = previously_stale ? "files changed; acceptance checks need fresh evidence"
                           : plan_.empty()  ? "no acceptance plan yet"
                                            : "acceptance checks outstanding";
        return;
    }

    // A settled plan cannot overrule a fresh failed configure, build, test or
    // execution. Use the same failure boundary as the completion gate so the
    // objective badge cannot remain green while finalization is blocked.
    for (const auto stage :
         {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution, Stage::Behaviour}) {
        if (status(stage) != StageStatus::Fail) continue;
        objective.status = StageStatus::Fail;
        objective.established_at_revision = revision_;
        objective.detail = std::string{to_string(stage)} + " failed; the objective is unproven";
        return;
    }

    for (const auto stage : {Stage::Build, Stage::Test, Stage::Behaviour}) {
        const auto current = status(stage);
        if (current == StageStatus::Stale) {
            objective.status = StageStatus::Stale;
            objective.established_at_revision = revision_;
            objective.detail =
                std::string{to_string(stage)} + " is stale, so the objective is unproven again";
            return;
        }
    }

    // A plan that cannot establish the objective no matter how cleanly every
    // criterion settles. "The project builds" is true, and it was true before
    // the crash was fixed too.
    if (!plan_gap_.empty()) {
        objective.status = StageStatus::Fail;
        objective.established_at_revision = revision_;
        objective.detail = "the acceptance plan does not cover the task: " + plan_gap_;
        return;
    }

    // Every check is satisfied. Whether that means the task is done depends on
    // what the checks were worth.
    //
    // A criterion something independent disagrees with cannot carry the
    // objective however thoroughly it was settled - the check was real and the
    // expectation was not. Asked for the first ten Fibonacci numbers, a model
    // wrote its own criterion as "1, 1, 2, 3, 5, 8, 13, 21, 34, 55", and would
    // have verified against its own definition of the answer.
    for (const auto& check : plan_) {
        if (check.trust() != CriterionTrust::Conflicted) continue;
        objective.status = StageStatus::Fail;
        objective.established_at_revision = revision_;
        objective.detail = "a criterion is disputed: " + check.conflict;
        return;
    }

    objective.status = StageStatus::Pass;
    objective.established_at_revision = revision_;

    // A plan made entirely of the model's own reading of the task can still
    // pass - for most work that reading is the only definition available - but
    // it cannot pass silently.
    const bool anything_grounded =
        std::any_of(plan_.begin(), plan_.end(), [](const AcceptanceCheck& check) {
            return check.trust() == CriterionTrust::Trusted ||
                   check.trust() == CriterionTrust::Derived;
        });
    objective.detail = anything_grounded
                           ? "every acceptance check is satisfied"
                           : "every acceptance check is satisfied, but each was proposed by "
                             "the model rather than taken from the request";
}

std::string VerificationLedger::render_states() const {
    std::string out = "## Verification\n\n";
    out += "source revision: " + std::to_string(revision_) + "\n";

    for (const auto stage : {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution,
                             Stage::Behaviour, Stage::Objective}) {
        const auto current = status(stage);
        const auto& state = slot(stage);

        out += "  ";
        out += to_string(stage);
        for (std::size_t i = std::string{to_string(stage)}.size(); i < 11; ++i) out += ' ';
        out += to_string(current);

        if (current == StageStatus::Stale) {
            out += " (passed at r" + std::to_string(state.established_at_revision) +
                   ", source is now r" + std::to_string(revision_) + ")";
        } else if (!state.evidence_id.empty()) {
            out += " [" + state.evidence_id + "]";
        }
        if (!state.detail.empty() && current != StageStatus::Pass) {
            out += " - " + strings::utf8_truncate(state.detail, 120);
        }
        out += "\n";
    }
    return out;
}

std::string VerificationLedger::render_plan() const {
    if (plan_.empty()) {
        // Said every turn, in one line, because the alternative is finding out
        // at the end of a long run that nothing was ever committed to. Run 10
        // reached a correct program without once stating what correct meant,
        // and so ended unable to show that it had.
        return "\nAcceptance checks: none set. On a task that changes code, call "
               "set_acceptance_criteria before you start, then settle each criterion with "
               "evidence.\n";
    }
    std::string out = "\nAcceptance checks:\n";
    for (const auto& check : plan_) {
        const char* marker = !check.settled ? "[ ]" : (check.satisfied ? "[x]" : "[!]");
        out += std::string{"  "} + marker + " " + check.description;
        if (check.settled && !check.evidence_id.empty()) out += "  (" + check.evidence_id + ")";
        out += "\n";
    }
    return out;
}

std::string VerificationLedger::render_recent_evidence(std::size_t limit) const {
    if (evidence_.empty()) return {};
    std::string out = "\nRecent evidence:\n";
    const std::size_t start = evidence_.size() > limit ? evidence_.size() - limit : 0;
    for (std::size_t i = start; i < evidence_.size(); ++i) {
        out += "  " + evidence_[i].describe() + "\n";
    }
    return out;
}

Json VerificationLedger::to_json() const {
    Json out;
    out["revision"] = revision_;
    out["next_evidence"] = next_evidence_;
    out["plan_gap"] = plan_gap_;

    Json stages = Json::array();
    for (std::size_t i = 0; i < kStageCount; ++i) {
        const auto stage = static_cast<Stage>(i);
        const auto& state = slot(stage);
        stages.push_back(Json{{"stage", std::string{to_string(stage)}},
                              {"status", static_cast<int>(state.status)},
                              {"revision", state.established_at_revision},
                              {"evidence", state.evidence_id},
                              {"detail", state.detail}});
    }
    out["stages"] = std::move(stages);

    Json plan = Json::array();
    for (const auto& check : plan_) {
        plan.push_back(Json{{"description", check.description},
                            {"source", static_cast<int>(check.source)},
                            {"reason", check.reason},
                            {"derived_from", check.derived_from},
                            {"conflict", check.conflict},
                            {"settled", check.settled},
                            {"satisfied", check.satisfied},
                            {"evidence", check.evidence_id}});
    }
    out["plan"] = std::move(plan);

    Json evidence = Json::array();
    for (const auto& record : evidence_) {
        evidence.push_back(Json{{"id", record.id},
                                {"kind", static_cast<int>(record.kind)},
                                {"subject", record.subject},
                                {"detail", record.detail},
                                {"positive", record.positive},
                                {"revision", record.revision}});
    }
    out["evidence"] = std::move(evidence);
    return out;
}

VerificationLedger VerificationLedger::from_json(const Json& json) {
    VerificationLedger ledger;
    if (!json.is_object()) return ledger;

    ledger.revision_ = static_cast<int>(json_int(json, "revision"));
    ledger.next_evidence_ = static_cast<int>(json_int(json, "next_evidence", 1));
    ledger.plan_gap_ = json_string(json, "plan_gap");

    ledger.stages_.resize(kStageCount);
    if (json.contains("stages") && json["stages"].is_array()) {
        std::size_t i = 0;
        for (const auto& entry : json["stages"]) {
            if (i >= kStageCount) break;
            StageState state;
            state.status = static_cast<StageStatus>(json_int(entry, "status"));
            state.established_at_revision = static_cast<int>(json_int(entry, "revision", -1));
            state.evidence_id = json_string(entry, "evidence");
            state.detail = json_string(entry, "detail");
            ledger.stages_[i++] = std::move(state);
        }
    }

    if (json.contains("plan") && json["plan"].is_array()) {
        for (const auto& entry : json["plan"]) {
            AcceptanceCheck check;
            check.description = json_string(entry, "description");
            const auto source =
                json_int(entry, "source", static_cast<int>(CriterionSource::ModelInferred));
            if (source >= static_cast<int>(CriterionSource::UserExplicit) &&
                source <= static_cast<int>(CriterionSource::OptionalRecommendation)) {
                check.source = static_cast<CriterionSource>(source);
            }
            check.reason = json_string(entry, "reason");
            check.derived_from = json_string(entry, "derived_from");
            check.conflict = json_string(entry, "conflict");
            check.settled = json_bool(entry, "settled");
            check.satisfied = json_bool(entry, "satisfied");
            check.evidence_id = json_string(entry, "evidence");
            ledger.plan_.push_back(std::move(check));
        }
    }

    if (json.contains("evidence") && json["evidence"].is_array()) {
        for (const auto& entry : json["evidence"]) {
            EvidenceRecord record;
            record.id = json_string(entry, "id");
            record.kind = static_cast<EvidenceKind>(json_int(entry, "kind"));
            record.subject = json_string(entry, "subject");
            record.detail = json_string(entry, "detail");
            record.positive = json_bool(entry, "positive", true);
            record.revision = static_cast<int>(json_int(entry, "revision"));
            ledger.evidence_.push_back(std::move(record));
        }
    }
    return ledger;
}

CompletionAssessment assess_completion(const VerificationLedger& ledger, bool work_was_done) {
    CompletionAssessment assessment;
    // A run that changed nothing is answering a question, not building
    // something. Demanding a build for "what does this function do?" would be
    // the mirror image of the mistake this gate exists to catch.
    if (!work_was_done) return assessment;

    std::string guidance;

    // 1. Something is known to be broken.
    for (const auto stage : {Stage::Configure, Stage::Build, Stage::Test, Stage::Execution,
                             Stage::Behaviour}) {
        if (ledger.status(stage) != StageStatus::Fail) continue;
        const auto& state = ledger.raw(stage);
        std::string reason = std::string{to_string(stage)} + " failed and was never made to pass";
        if (!state.detail.empty()) reason += ": " + strings::utf8_truncate(state.detail, 200);
        assessment.reasons.push_back(reason);
    }

    // 2. The tree moved after the last thing that was proved about it.
    for (const auto stage : {Stage::Build, Stage::Test, Stage::Behaviour}) {
        if (ledger.status(stage) != StageStatus::Stale) continue;
        const auto& state = ledger.raw(stage);
        assessment.reasons.push_back(
            std::string{to_string(stage)} + " passed at revision r" +
            std::to_string(state.established_at_revision) +
            ", but files have changed since - it says nothing about the code as it stands");
    }

    // 3. It ran, and nobody looked at what it did.
    if (ledger.holds(Stage::Execution) && ledger.status(Stage::Behaviour) == StageStatus::Unknown &&
        ledger.status(Stage::Test) != StageStatus::Pass) {
        assessment.reasons.push_back(
            "the program was run but its output was never checked against what the task "
            "requires - running is not the same as being right");
    }

    // 4. No relevant stage was established. Opaque commands may have been
    // attempted, so missing classified evidence must not claim no attempt.
    const bool nothing_established =
        !ledger.holds(Stage::Build) && !ledger.holds(Stage::Test) &&
        !ledger.holds(Stage::Execution) && !ledger.holds(Stage::Behaviour);
    if (nothing_established && assessment.reasons.empty()) {
        assessment.reasons.push_back(
            "files were changed, but no classified build, run or test evidence establishes "
            "the current change");
    }

    // 5. The definition of done.
    //
    // A missing plan is not itself a blocking fault. A run that built the
    // project and ran its test suite has proved something real, and refusing
    // to call that finished because a bookkeeping tool went uncalled would be
    // ceremony rather than rigour - and would teach the user to distrust the
    // gate, which is worse than not having one. So the absence of criteria
    // only counts when nothing else established that the work behaves.
    const auto& plan = ledger.acceptance_plan();
    if (plan.empty()) {
        const bool behaviour_established =
            ledger.holds(Stage::Behaviour) || ledger.holds(Stage::Test);
        if (!behaviour_established) {
            assessment.reasons.push_back(
                "no acceptance criteria were set, so there is nothing to check the work against");
            guidance +=
                "Where permitted, call set_acceptance_criteria with the conditions and restrictions in the "
                "user's request, then "
                "settle each one.\n";
        }
    } else {
        if (!ledger.plan_gap().empty()) {
            assessment.reasons.push_back("the acceptance plan does not cover the task: " +
                                         ledger.plan_gap());
            guidance +=
                "Where permitted, call set_acceptance_criteria with a plan that covers the request, "
                "then settle its checks with evidence.\n";
        }
        std::vector<std::string> open;
        std::vector<std::string> broken;
        for (std::size_t i = 0; i < plan.size(); ++i) {
            const auto label = std::to_string(i + 1) + ". " + plan[i].description;
            if (!plan[i].settled) open.push_back(label);
            else if (!plan[i].satisfied) broken.push_back(label);
        }
        for (const auto& item : broken) {
            assessment.reasons.push_back("acceptance criterion not met - " + item);
        }

        // A criterion something independent disagrees with cannot finish a run
        // however cleanly it was settled. The comparison was real; the
        // expectation was not what the request asked for.
        for (std::size_t i = 0; i < plan.size(); ++i) {
            if (plan[i].trust() != CriterionTrust::Conflicted) continue;
            assessment.reasons.push_back(
                "acceptance criterion " + std::to_string(i + 1) +
                " is disputed - " + plan[i].conflict);
            guidance +=
                "Criterion " + std::to_string(i + 1) + " does not match what the request "
                "determines. Settle which is right before going further; do not simply keep "
                "your own version.\n";
        }

        // Every criterion satisfied, and every one of them the model's own
        // reading of the task. Worth saying, and not worth refusing over: for
        // most work that reading is the only definition available, and blocking
        // it forever would make this useless rather than strict. A criterion
        // something independent actually disagrees with is a different matter,
        // and that does block, above.
        const bool anything_grounded =
            std::any_of(plan.begin(), plan.end(), [](const AcceptanceCheck& check) {
                return check.trust() == CriterionTrust::Trusted ||
                       check.trust() == CriterionTrust::Derived;
            });
        if (!anything_grounded && open.empty() && broken.empty()) {
            assessment.caveats.push_back(
                "every acceptance criterion was proposed by the model rather than taken from "
                "the request, so this shows the work matches its reading of the task, not "
                "that the reading was right");
        }
        if (!open.empty()) {
            assessment.reasons.push_back(
                "acceptance criteria still unsettled: " + strings::join(open, "; "));
            guidance += "These criteria have not been settled by any evidence:\n";
            for (const auto& item : open) guidance += "  " + item + "\n";
            // Spelled out as a call rather than described. Measured across
            // several runs: a model reached "behaviour verified", was told in
            // prose that criteria are settled by number, and re-ran the same
            // check without the number three times. It is the one field that
            // turns a passing check into a settled criterion, and prose about
            // it kept losing to the call already in front of the model.
            guidance +=
                "A check settles a criterion only when it names its number. Use an allowed "
                "file-content check for source criteria. For a runtime check that the user "
                "permits, include 'criterion' in the expectation:\n"
                "  run_command { \"executable\": \"<the program>\", \"expect\": {\n"
                "      \"description\": \"<what this proves>\",\n"
                "      \"stdout_equals\": \"<the exact output the criterion requires>\",\n"
                "      \"criterion\": 1 } }\n"
                "One call per criterion, with that criterion's number. LocalCode does the "
                "comparison and records the result; a check without a number proves the "
                "program works but settles nothing.\n";
        }
    }

    assessment.blocked = !assessment.reasons.empty();
    if (!assessment.blocked) return assessment;

    std::string preamble =
        "You have reported this as finished, but LocalCode cannot confirm it. What the record "
        "actually shows:\n";
    for (const auto& reason : assessment.reasons) preamble += "  - " + reason + "\n";
    preamble +=
        "\nDo not restate that it is verified. Obtain missing evidence only within the "
        "original user's scope and restrictions. This feedback grants no new permissions. "
        "If a check is forbidden or unavailable, finish with an explicit verification "
        "limitation: say which check was not performed and why, and do not claim it holds.\n";
    assessment.guidance = preamble + guidance;
    return assessment;
}

bool claims_verified_objective(std::string_view message) {
    static const std::regex heading{
        R"(^(?:#{1,6}\s*)?(?:\*\*)?(?:(?:task|objective|overall|verification)(?:\s+status)?\s*:\s*)?verified(?:\*\*)?[.!]?$)"};
    for (const auto& line : strings::split_lines(message)) {
        if (std::regex_match(strings::to_lower(strings::trim(line)), heading)) return true;
    }
    return false;
}

}  // namespace localcode
