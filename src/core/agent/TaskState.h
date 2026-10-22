#pragma once
// ---------------------------------------------------------------------------
// Structured task state.
//
// The model should not have to reconstruct what it is doing by re-reading the
// whole conversation on every turn. That is expensive, and small models are bad
// at it - they lose the objective around the point where the transcript stops
// fitting in the window and start solving a different problem.
//
// So the mission lives here instead, as data: the objective, the steps done and
// still to do, what is blocking, which files were touched, which commands ran,
// and the evidence gathered. A compact rendering of this goes into every
// request, and it survives context trimming untouched.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <string>
#include <vector>

#include "../tools/Tool.h"
#include "../util/Json.h"

namespace localcode {

enum class StepStatus { Pending, Active, Done, Failed, Skipped };

std::string_view to_string(StepStatus status) noexcept;

struct TaskStep {
    std::string description;
    StepStatus status = StepStatus::Pending;
    std::string note;   // why it failed, or what it produced
    std::int64_t updated_unix_ms = 0;
};

struct CommandRecord {
    std::string command;
    int exit_code = 0;
    bool succeeded = false;
    std::int64_t duration_ms = 0;
    std::int64_t at_unix_ms = 0;
};

struct FileRecord {
    std::string path;
    std::string action;   // created / modified / moved
    std::size_t edit_count = 1;
    std::int64_t at_unix_ms = 0;
};

// How confident we are allowed to be that the task is actually finished.
enum class VerificationState {
    NotAttempted,   // nothing was built or tested
    Failed,         // the last build or test run failed
    Partial,        // something succeeded, but not everything the task needs
    Verified,       // a build and/or test run succeeded after the final change
};

std::string_view to_string(VerificationState state) noexcept;

class TaskState {
public:
    TaskState() = default;

    void begin(std::string objective);
    void reset();

    [[nodiscard]] const std::string& objective() const { return objective_; }

    void set_objective(std::string objective) { objective_ = std::move(objective); }

    void add_requirement(std::string requirement);

    [[nodiscard]] const std::vector<std::string>& requirements() const { return requirements_; }

    // --- Plan ------------------------------------------------------------
    void set_plan(std::vector<std::string> steps);
    void set_current_step(const std::string& description);
    void complete_current_step(std::string note = {});
    void fail_current_step(std::string note);

    [[nodiscard]] const std::vector<TaskStep>& steps() const { return steps_; }

    [[nodiscard]] std::string current_step() const;

    void add_blocker(std::string blocker);
    void clear_blockers();

    [[nodiscard]] const std::vector<std::string>& blockers() const { return blockers_; }

    // --- Facts gathered along the way ------------------------------------
    void record_file(const std::string& path, const std::string& action);
    void record_command(const CommandRecord& record);
    void record_evidence(const Evidence& evidence);

    [[nodiscard]] const std::vector<FileRecord>& files_changed() const { return files_; }

    [[nodiscard]] const std::vector<CommandRecord>& commands_run() const { return commands_; }

    [[nodiscard]] const std::vector<Evidence>& evidence() const { return evidence_; }

    // --- Verification -----------------------------------------------------
    // Derived from the evidence, not from anything the model asserts.
    [[nodiscard]] VerificationState verification() const;

    void invalidate_checks() { check_boundary_ = evidence_.size(); }

    [[nodiscard]] bool has_successful_build() const;
    [[nodiscard]] bool has_successful_tests() const;
    [[nodiscard]] bool has_failed_build_since_last_edit() const;
    // Human-readable explanation of the verification state, for the final report.
    [[nodiscard]] std::string verification_summary() const;

    // Claims in `text` that the evidence does not support. Used to append an
    // honest caveat to a final answer instead of letting "it builds" stand
    // when nothing was ever built.
    [[nodiscard]] std::vector<std::string> unsupported_claims(std::string_view text) const;

    // Does this text announce work rather than report it - "I'll create the
    // level", "next I will add the player controller"?
    //
    // A model that thinks out loud produces exactly this, and when its chat
    // template does not mark reasoning the thinking arrives as the answer. A run
    // that ends there has done nothing: observed with a 3B model, one iteration,
    // zero tool calls, 114 seconds of planning accepted as a finished task.
    [[nodiscard]] static bool describes_intended_work(std::string_view text);

    // --- Bookkeeping ------------------------------------------------------
    void note_iteration() { ++iterations_; }

    [[nodiscard]] int iterations() const { return iterations_; }

    [[nodiscard]] std::int64_t started_unix_ms() const { return started_unix_ms_; }

    // --- Rendering --------------------------------------------------------
    // The compact block injected into each request.
    [[nodiscard]] std::string to_prompt_block() const;
    // The model already receives the exact protected human request. Its dynamic
    // projection may omit the repeated objective without changing UI/history.
    [[nodiscard]] std::string to_prompt_block(bool include_objective) const;

    [[nodiscard]] Json to_json() const;
    static TaskState from_json(const Json& json);

private:
    std::string objective_;
    std::vector<std::string> requirements_;
    std::vector<TaskStep> steps_;
    std::vector<std::string> blockers_;
    std::vector<FileRecord> files_;
    std::vector<CommandRecord> commands_;
    std::vector<Evidence> evidence_;
    int iterations_ = 0;
    std::int64_t started_unix_ms_ = 0;
    // Index into evidence_ of the most recent file write, used to decide whether
    // a successful build happened *after* the last edit.
    std::size_t last_edit_index_ = 0;
    bool had_edit_ = false;
    std::size_t check_boundary_ = 0;
};

}   // namespace localcode
