#include "TaskState.h"

#include <cstring>

#include <algorithm>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

// Phrases that assert a verifiable outcome. When one of these shows up in a
// final answer, the evidence had better contain a matching success.
struct ClaimPattern {
    const char* phrase;
    EvidenceKind requires_kind;
    const char* complaint;
};

const std::vector<ClaimPattern>& claim_patterns() {
    static const std::vector<ClaimPattern> kPatterns = {
        {"builds successfully", EvidenceKind::BuildAttempted,
         "you say the project builds, but no classified successful build evidence was established"},
        {"builds cleanly", EvidenceKind::BuildAttempted,
         "you say the project builds, but no classified successful build evidence was established"},
        {"compiles successfully", EvidenceKind::BuildAttempted,
         "you say the code compiles, but no classified successful compile evidence was "
         "established"},
        {"compiles cleanly", EvidenceKind::BuildAttempted,
         "you say the code compiles, but no classified successful compile evidence was "
         "established"},
        {"now builds", EvidenceKind::BuildAttempted,
         "you say the project now builds, but no classified successful build evidence was "
         "established"},
        {"the build succeeds", EvidenceKind::BuildAttempted,
         "you say the build succeeds, but no classified successful build evidence was established"},
        {"build passes", EvidenceKind::BuildAttempted,
         "you say the build passes, but no classified successful build evidence was established"},
        {"tests pass", EvidenceKind::TestsRun,
         "you say the tests pass, but no classified successful test evidence was established"},
        {"tests are passing", EvidenceKind::TestsRun,
         "you say the tests pass, but no classified successful test evidence was established"},
        {"all tests pass", EvidenceKind::TestsRun,
         "you say all tests pass, but no classified successful test evidence was established"},
        {"tests now pass", EvidenceKind::TestsRun,
         "you say the tests now pass, but no classified successful test evidence was established"},
        {"the test suite passes", EvidenceKind::TestsRun,
         "you say the test suite passes, but no classified successful test evidence was "
         "established"},

        // Claims that the *result* is right, as opposed to claims that
        // something ran. Running a program proves it ran; it proves nothing
        // about whether the output was correct, and nothing here can check
        // that for you.
        //
        // Measured: a program was built, executed with exit code 0, and printed
        // "1 3 6 12 24 48 96 192 384 768". The reply said "the first 10
        // Fibonacci numbers are indeed printed correctly" and "the task is
        // verified". Both false - it is a doubling sequence - and both passed
        // unchallenged because a build had genuinely succeeded.
        {"the task is verified", EvidenceKind::TestsRun,
         "you say the task is verified, but nothing compared the actual output against what "
         "was asked for - running a program is not checking its result"},
        {"output is as expected", EvidenceKind::TestsRun,
         "you say the output is as expected, but no check compared it against the expected "
         "values - say what the output actually was and let the user judge"},
        {"output is correct", EvidenceKind::TestsRun,
         "you say the output is correct, but nothing verified it against the requirement"},
        {"prints the expected", EvidenceKind::TestsRun,
         "you say the output is what was expected, but nothing checked it"},
        {"works correctly", EvidenceKind::TestsRun,
         "you say it works correctly, but no test confirmed the behaviour"},
        {"working correctly", EvidenceKind::TestsRun,
         "you say it works correctly, but no test confirmed the behaviour"},
        {"printed correctly", EvidenceKind::TestsRun,
         "you say the values were printed correctly, but nothing checked them against the "
         "expected values"},
        {"behaves correctly", EvidenceKind::TestsRun,
         "you say it behaves correctly, but no test confirmed the behaviour"},
        {"as expected", EvidenceKind::TestsRun,
         "you say the result was as expected, but nothing compared it against an expectation"},
    };
    return kPatterns;
}

}   // namespace

std::string_view to_string(StepStatus status) noexcept {
    switch (status) {
        case StepStatus::Pending: return "pending";
        case StepStatus::Active: return "in progress";
        case StepStatus::Done: return "done";
        case StepStatus::Failed: return "failed";
        case StepStatus::Skipped: return "skipped";
    }
    return "pending";
}

std::string_view to_string(VerificationState state) noexcept {
    switch (state) {
        case VerificationState::NotAttempted: return "not verified";
        case VerificationState::Failed: return "failing";
        case VerificationState::Partial: return "partially verified";
        case VerificationState::Verified: return "verified";
    }
    return "not verified";
}

void TaskState::begin(std::string objective) {
    reset();
    objective_ = std::move(objective);
    started_unix_ms_ = now_unix_ms();
}

void TaskState::reset() {
    objective_.clear();
    requirements_.clear();
    steps_.clear();
    blockers_.clear();
    files_.clear();
    commands_.clear();
    evidence_.clear();
    iterations_ = 0;
    started_unix_ms_ = 0;
    last_edit_index_ = 0;
    had_edit_ = false;
    check_boundary_ = 0;
}

void TaskState::add_requirement(std::string requirement) {
    auto trimmed = strings::trim(requirement);
    if (trimmed.empty()) return;
    if (std::find(requirements_.begin(), requirements_.end(), trimmed) == requirements_.end()) {
        requirements_.push_back(std::move(trimmed));
    }
}

void TaskState::set_plan(std::vector<std::string> steps) {
    steps_.clear();
    for (auto& description : steps) {
        auto trimmed = strings::trim(description);
        if (trimmed.empty()) continue;
        TaskStep step;
        step.description = std::move(trimmed);
        step.updated_unix_ms = now_unix_ms();
        steps_.push_back(std::move(step));
    }
}

void TaskState::set_current_step(const std::string& description) {
    const auto trimmed = strings::trim(description);
    if (trimmed.empty()) return;

    for (auto& step : steps_) {
        if (step.status == StepStatus::Active) step.status = StepStatus::Pending;
    }
    auto it = std::find_if(steps_.begin(), steps_.end(), [&trimmed](const TaskStep& step) {
        return strings::iequals(step.description, trimmed);
    });
    if (it == steps_.end()) {
        TaskStep step;
        step.description = trimmed;
        steps_.push_back(std::move(step));
        it = steps_.end() - 1;
    }
    it->status = StepStatus::Active;
    it->updated_unix_ms = now_unix_ms();
}

std::string TaskState::current_step() const {
    for (const auto& step : steps_) {
        if (step.status == StepStatus::Active) return step.description;
    }
    for (const auto& step : steps_) {
        if (step.status == StepStatus::Pending) return step.description;
    }
    return {};
}

void TaskState::complete_current_step(std::string note) {
    for (auto& step : steps_) {
        if (step.status == StepStatus::Active) {
            step.status = StepStatus::Done;
            step.note = std::move(note);
            step.updated_unix_ms = now_unix_ms();
            return;
        }
    }
}

void TaskState::fail_current_step(std::string note) {
    for (auto& step : steps_) {
        if (step.status == StepStatus::Active) {
            step.status = StepStatus::Failed;
            step.note = std::move(note);
            step.updated_unix_ms = now_unix_ms();
            return;
        }
    }
}

void TaskState::add_blocker(std::string blocker) {
    auto trimmed = strings::trim(blocker);
    if (trimmed.empty()) return;
    if (std::find(blockers_.begin(), blockers_.end(), trimmed) == blockers_.end()) {
        blockers_.push_back(std::move(trimmed));
    }
}

void TaskState::clear_blockers() {
    blockers_.clear();
}

void TaskState::record_file(const std::string& path, const std::string& action) {
    auto it = std::find_if(files_.begin(), files_.end(),
                           [&path](const FileRecord& record) { return record.path == path; });
    if (it != files_.end()) {
        ++it->edit_count;
        it->at_unix_ms = now_unix_ms();
        return;
    }
    FileRecord record;
    record.path = path;
    record.action = action;
    record.at_unix_ms = now_unix_ms();
    files_.push_back(std::move(record));
}

void TaskState::record_command(const CommandRecord& record) {
    commands_.push_back(record);
    // Keep the list bounded; the full history lives in the database.
    if (commands_.size() > 200) commands_.erase(commands_.begin());
}

void TaskState::record_evidence(const Evidence& evidence) {
    evidence_.push_back(evidence);
    if (evidence.kind == EvidenceKind::FileWritten && evidence.positive) {
        had_edit_ = true;
        last_edit_index_ = evidence_.size() - 1;
    }
    if (evidence_.size() > 400) {
        evidence_.erase(evidence_.begin());
        if (last_edit_index_ > 0) --last_edit_index_;
        if (check_boundary_ > 0) --check_boundary_;
    }
}

bool TaskState::has_successful_build() const {
    const auto start = std::max(check_boundary_, had_edit_ ? last_edit_index_ : 0);
    for (std::size_t i = evidence_.size(); i > start; --i)
        if (evidence_[i - 1].kind == EvidenceKind::BuildAttempted) return evidence_[i - 1].positive;
    return false;
}

bool TaskState::has_successful_tests() const {
    const auto start = std::max(check_boundary_, had_edit_ ? last_edit_index_ : 0);
    for (std::size_t i = evidence_.size(); i > start; --i)
        if (evidence_[i - 1].kind == EvidenceKind::TestsRun) return evidence_[i - 1].positive;
    return false;
}

bool TaskState::has_failed_build_since_last_edit() const {
    if (!had_edit_) return false;
    for (std::size_t i = last_edit_index_; i < evidence_.size(); ++i) {
        const auto& e = evidence_[i];
        if ((e.kind == EvidenceKind::BuildAttempted || e.kind == EvidenceKind::TestsRun) &&
            !e.positive) {
            return true;
        }
    }
    return false;
}

VerificationState TaskState::verification() const {
    // The question is not "did a build ever succeed" but "did one succeed after
    // the last change was made". Anything else is a stale green light.
    // The LAST attempt of each kind since the edit, not "was there ever a
    // failure". Fixing what a build complained about and building again is the
    // ordinary loop, and a superseded failure is not evidence about the code as
    // it stands now - only the most recent attempt is.
    //
    // Measured: a run whose every acceptance criterion was settled, whose
    // program printed exactly the right thing, and whose final answer was
    // accurate, reported "failing" - because one earlier command had failed and
    // no edit happened between it and the successful build that replaced it.
    std::optional<bool> last_build;
    std::optional<bool> last_tests;

    const std::size_t start = std::max(check_boundary_, had_edit_ ? last_edit_index_ : 0);
    for (std::size_t i = start; i < evidence_.size(); ++i) {
        const auto& e = evidence_[i];
        if (e.kind == EvidenceKind::BuildAttempted)
            last_build = e.positive;
        else if (e.kind == EvidenceKind::TestsRun)
            last_tests = e.positive;
    }

    const bool build_after_edit = last_build.value_or(false);
    const bool tests_after_edit = last_tests.value_or(false);
    const bool failure_after_edit = (last_build && !*last_build) || (last_tests && !*last_tests);

    if (failure_after_edit) return VerificationState::Failed;
    if (build_after_edit && tests_after_edit) return VerificationState::Verified;
    if (build_after_edit || tests_after_edit) return VerificationState::Partial;
    return VerificationState::NotAttempted;
}

std::string TaskState::verification_summary() const {
    switch (verification()) {
        case VerificationState::Verified:
            return "VERIFIED - the project built and the tests ran successfully after the final "
                   "change.";
        case VerificationState::Partial:
            return std::string{"PARTIALLY VERIFIED - "} +
                   (has_successful_build() ? "the build succeeded after the final change, but no "
                                             "test run confirmed the behaviour."
                                           : "tests ran, but no build was confirmed after the "
                                             "final change.");
        case VerificationState::Failed:
            return "NOT VERIFIED - the most recent build or test run after the final change "
                   "failed.";
        case VerificationState::NotAttempted:
        default:
            return "NOT VERIFIED - no classified build or test evidence confirms the current "
                   "source, so the change is unproven.";
    }
}

bool TaskState::describes_intended_work(std::string_view text) {
    // Two parts, because a fixed phrase list is too brittle: "I'll create the
    // level" was matched and "I'll now create the project files" was not, and
    // the difference is one adverb. A first-person future marker followed
    // closely by an action verb catches both without matching a report of
    // finished work ("I created the level and it builds").
    static const char* kFuture[] = {
        "i'll",      "i will",      "let me",  "let's", "let us",  "i'm going to", "i am going to",
        "i plan to", "i intend to", "i shall", "we'll", "we will", "i can now",    "i should now",
    };
    static const char* kAction[] = {
        "creat",   "add",    "implement", "writ",    "set up", "setup", "build",  "start",
        "begin",   "make",   "generat",   "proceed", "outlin", "draft", "produc", "construct",
        "develop", "define", "prepar",    "compil",  "modif",  "updat",
    };
    // A plan announcement with no verb of its own.
    static const char* kPlanning[] = {
        "here's the plan", "here is the plan", "the plan is",    "here are the steps",
        "steps i will",    "my plan",          "plan of action", "to summarize the steps",
    };

    const auto lowered = strings::to_lower(text);

    for (const char* phrase : kPlanning) {
        if (lowered.find(phrase) != std::string::npos) return true;
    }

    for (const char* marker : kFuture) {
        std::size_t at = lowered.find(marker);
        while (at != std::string::npos) {
            // Look a short way ahead - far enough for "I'll now create", not so
            // far that an unrelated later sentence counts.
            const auto window = lowered.substr(at, std::strlen(marker) + 40);
            for (const char* verb : kAction) {
                if (window.find(verb) != std::string::npos) return true;
            }
            at = lowered.find(marker, at + 1);
        }
    }
    return false;
}

std::vector<std::string> TaskState::unsupported_claims(std::string_view text) const {
    std::vector<std::string> problems;
    const auto lowered = strings::to_lower(text);

    for (const auto& pattern : claim_patterns()) {
        if (!strings::icontains(lowered, pattern.phrase)) continue;

        const bool supported = pattern.requires_kind == EvidenceKind::BuildAttempted
                                   ? has_successful_build()
                                   : has_successful_tests();
        if (!supported) {
            const std::string complaint{pattern.complaint};
            if (std::find(problems.begin(), problems.end(), complaint) == problems.end()) {
                problems.push_back(complaint);
            }
        }
    }
    return problems;
}

std::string TaskState::to_prompt_block() const {
    return to_prompt_block(true);
}

std::string TaskState::to_prompt_block(bool include_objective) const {
    if (objective_.empty()) return {};

    std::string out = "## Current task\n\n";
    if (include_objective) out += "Objective: " + objective_ + "\n";

    if (!requirements_.empty()) {
        out += "\nRequirements:\n";
        for (const auto& requirement : requirements_)
            out += "  - " + requirement + "\n";
    }

    if (!steps_.empty()) {
        out += "\nPlan:\n";
        for (const auto& step : steps_) {
            const char* marker = step.status == StepStatus::Done      ? "[x]"
                                 : step.status == StepStatus::Active  ? "[>]"
                                 : step.status == StepStatus::Failed  ? "[!]"
                                 : step.status == StepStatus::Skipped ? "[-]"
                                                                      : "[ ]";
            out += std::string{"  "} + marker + " " + step.description;
            if (!step.note.empty()) out += "  (" + strings::utf8_truncate(step.note, 160) + ")";
            out += "\n";
        }
    }

    if (!files_.empty()) {
        out += "\nFiles you have changed (" + std::to_string(files_.size()) + "):\n";
        const std::size_t shown = std::min<std::size_t>(files_.size(), 20);
        for (std::size_t i = files_.size() - shown; i < files_.size(); ++i) {
            out += "  " + files_[i].action + ": " + files_[i].path;
            if (files_[i].edit_count > 1) {
                out += "  (" + std::to_string(files_[i].edit_count) + " edits)";
            }
            out += "\n";
        }
    }

    if (!commands_.empty()) {
        out += "\nRecent commands:\n";
        const std::size_t shown = std::min<std::size_t>(commands_.size(), 8);
        for (std::size_t i = commands_.size() - shown; i < commands_.size(); ++i) {
            const auto& command = commands_[i];
            out += std::string{"  "} + (command.succeeded ? "ok   " : "FAIL ") +
                   strings::utf8_truncate(command.command, 160) + "  (exit " +
                   std::to_string(command.exit_code) + ")\n";
        }
    }

    if (!blockers_.empty()) {
        out += "\nBlockers:\n";
        for (const auto& blocker : blockers_)
            out += "  - " + blocker + "\n";
    }

    out += "\nVerification status: " + std::string{to_string(verification())} + "\n";
    if ((had_edit_ || check_boundary_ > 0) && verification() != VerificationState::Verified) {
        out +=
            "You may not claim build/test verification until a classified build or test "
            "succeeds after your last edit.\n";
    }
    return out;
}

Json TaskState::to_json() const {
    Json out;
    out["objective"] = objective_;
    out["requirements"] = requirements_;
    out["iterations"] = iterations_;
    out["started_unix_ms"] = started_unix_ms_;
    out["blockers"] = blockers_;

    Json steps = Json::array();
    for (const auto& step : steps_) {
        steps.push_back({{"description", step.description},
                         {"status", std::string{to_string(step.status)}},
                         {"note", step.note},
                         {"updated", step.updated_unix_ms}});
    }
    out["steps"] = std::move(steps);

    Json files = Json::array();
    for (const auto& file : files_) {
        files.push_back({{"path", file.path},
                         {"action", file.action},
                         {"edits", file.edit_count},
                         {"at", file.at_unix_ms}});
    }
    out["files"] = std::move(files);

    Json commands = Json::array();
    for (const auto& command : commands_) {
        commands.push_back({{"command", command.command},
                            {"exit_code", command.exit_code},
                            {"succeeded", command.succeeded},
                            {"duration_ms", command.duration_ms},
                            {"at", command.at_unix_ms}});
    }
    out["commands"] = std::move(commands);

    Json evidence = Json::array();
    for (const auto& item : evidence_) {
        evidence.push_back({{"kind", std::string{to_string(item.kind)}},
                            {"subject", item.subject},
                            {"detail", item.detail},
                            {"positive", item.positive},
                            {"at", item.at_unix_ms}});
    }
    out["evidence"] = std::move(evidence);
    out["verification"] = std::string{to_string(verification())};
    out["check_boundary"] = check_boundary_;
    return out;
}

TaskState TaskState::from_json(const Json& json) {
    TaskState state;
    if (!json.is_object()) return state;

    state.objective_ = json_string(json, "objective");
    state.iterations_ = static_cast<int>(json_int(json, "iterations"));
    state.started_unix_ms_ = json_int(json, "started_unix_ms");

    auto read_strings = [&json](const char* key, std::vector<std::string>& out) {
        if (json.contains(key) && json[key].is_array()) {
            for (const auto& item : json[key]) {
                if (item.is_string()) out.push_back(item.get<std::string>());
            }
        }
    };
    read_strings("requirements", state.requirements_);
    read_strings("blockers", state.blockers_);

    if (json.contains("steps") && json["steps"].is_array()) {
        for (const auto& entry : json["steps"]) {
            TaskStep step;
            step.description = json_string(entry, "description");
            const auto status = json_string(entry, "status");
            step.status = status == "done"          ? StepStatus::Done
                          : status == "in progress" ? StepStatus::Active
                          : status == "failed"      ? StepStatus::Failed
                          : status == "skipped"     ? StepStatus::Skipped
                                                    : StepStatus::Pending;
            step.note = json_string(entry, "note");
            step.updated_unix_ms = json_int(entry, "updated");
            state.steps_.push_back(std::move(step));
        }
    }

    if (json.contains("files") && json["files"].is_array()) {
        for (const auto& entry : json["files"]) {
            FileRecord record;
            record.path = json_string(entry, "path");
            record.action = json_string(entry, "action");
            record.edit_count = static_cast<std::size_t>(json_int(entry, "edits", 1));
            record.at_unix_ms = json_int(entry, "at");
            state.files_.push_back(std::move(record));
        }
    }

    if (json.contains("commands") && json["commands"].is_array()) {
        for (const auto& entry : json["commands"]) {
            CommandRecord record;
            record.command = json_string(entry, "command");
            record.exit_code = static_cast<int>(json_int(entry, "exit_code"));
            record.succeeded = json_bool(entry, "succeeded");
            record.duration_ms = json_int(entry, "duration_ms");
            record.at_unix_ms = json_int(entry, "at");
            state.commands_.push_back(std::move(record));
        }
    }

    if (json.contains("evidence") && json["evidence"].is_array()) {
        for (const auto& entry : json["evidence"]) {
            Evidence item;
            const auto kind = json_string(entry, "kind");
            item.kind = kind == "file_read"          ? EvidenceKind::FileRead
                        : kind == "file_written"     ? EvidenceKind::FileWritten
                        : kind == "directory_listed" ? EvidenceKind::DirectoryListed
                        : kind == "search"           ? EvidenceKind::SearchPerformed
                        : kind == "build"            ? EvidenceKind::BuildAttempted
                        : kind == "tests"            ? EvidenceKind::TestsRun
                        : kind == "environment"      ? EvidenceKind::EnvironmentChecked
                        : kind == "git"              ? EvidenceKind::GitInspected
                        : kind == "web"              ? EvidenceKind::WebFetched
                                                     : EvidenceKind::CommandRun;
            item.subject = json_string(entry, "subject");
            item.detail = json_string(entry, "detail");
            item.positive = json_bool(entry, "positive", true);
            item.at_unix_ms = json_int(entry, "at");
            state.record_evidence(item);
        }
    }
    state.check_boundary_ = std::min(
        state.evidence_.size(),
        static_cast<std::size_t>(std::max<std::int64_t>(0, json_int(json, "check_boundary"))));
    return state;
}

}   // namespace localcode
