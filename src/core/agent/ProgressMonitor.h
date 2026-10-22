#pragma once
// ---------------------------------------------------------------------------
// No-progress detection.
//
// The characteristic failure of a small local model in an agent loop is not a
// wrong answer: it is running the same failing command twelve times, or
// alternating between two edits forever, entirely convinced it is making
// progress. Left alone that burns the user's evening and their context window.
//
// This class watches the stream of tool calls and results and answers one
// question: is anything actually changing? When the answer is no, the agent
// injects a short, factual note telling the model that its current strategy has
// already failed and it needs a different one. After repeated warnings the run
// is stopped and handed back to the user with what was learned.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "../util/Json.h"

namespace localcode {

enum class ProgressVerdict {
    Fine,
    RepeatedIdenticalCall,   // same tool, same arguments, same result
    RepeatedFailure,         // same command failing over and over
    Oscillating,             // A, B, A, B - undoing its own work
    Stalled,                 // many turns, no files changed and nothing verified
    Exhausted,               // warned repeatedly and still not moving
};

std::string_view to_string(ProgressVerdict verdict) noexcept;

struct ProgressObservation {
    std::string tool_name;
    std::string arguments_digest;   // normalised arguments
    std::string result_digest;      // normalised outcome
    bool succeeded = false;
    bool changed_files = false;
    std::int64_t at_unix_ms = 0;
};

struct ProgressAssessment {
    ProgressVerdict verdict = ProgressVerdict::Fine;
    // The note fed back to the model. Empty when the verdict is Fine.
    std::string guidance;
    // True when the agent should stop rather than continue.
    bool should_stop = false;
    int warning_count = 0;
};

class ProgressMonitor {
public:
    ProgressMonitor() = default;

    void reset();

    // Records one tool call and its outcome, and returns the assessment.
    //
    // `advanced_verification` is true when the call settled an acceptance
    // criterion. Verification changes no files by design, and counting only
    // file changes as progress made the monitor tell a model that was
    // correctly verifying its work to stop investigating and "make the
    // change" - then stop the run for repeating itself. Measured in two runs
    // that had already built and were three checks from done.
    ProgressAssessment observe(const std::string& tool_name, const Json& arguments,
                               const std::string& result_summary, bool succeeded,
                               bool changed_files, bool advanced_verification = false,
                               std::optional<bool> source_novelty = std::nullopt,
                               std::optional<std::string> failure_identity = std::nullopt);

    // failure_identity, when provided, is a bounded host-generated diagnostic
    // key. It preserves meaningful exit/status differences; model prose must
    // not supply it. Generic callers retain the legacy normalized summary.

    // Called once per agent iteration, so a model that talks without acting is
    // also caught.
    void note_iteration() { ++iterations_; }

    [[nodiscard]] int warnings_issued() const { return warnings_; }

    [[nodiscard]] const std::vector<std::string>& recent_failures() const { return failures_; }

    // Tuning knobs, exposed because the right values differ between a 3B model
    // and a 30B one.
    void set_identical_call_threshold(int value) { identical_threshold_ = value; }

    void set_failure_threshold(int value) { failure_threshold_ = value; }

    void set_max_warnings(int value) { max_warnings_ = value; }

    // Normalises arguments so that cosmetic differences - key order, whitespace
    // inside a command string - do not hide a genuine repeat.
    static std::string digest_arguments(const Json& arguments);
    static std::string digest_result(const std::string& summary);

private:
    std::deque<ProgressObservation> history_;
    std::vector<std::string> failures_;
    int iterations_ = 0;
    int warnings_ = 0;
    int identical_threshold_ = 3;
    int failure_threshold_ = 3;
    int max_warnings_ = 3;
    int last_change_iteration_ = 0;
};

}   // namespace localcode
