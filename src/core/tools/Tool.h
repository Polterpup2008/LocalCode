#pragma once
// ---------------------------------------------------------------------------
// The tool interface.
//
// Two design rules run through every tool in LocalCode:
//
// 1. Results are small. A compiler that produces 100 MB of output must not
//    produce 100 MB of context. Tools return a short, structured summary plus
//    the diagnostics that matter, and spool the full output to a file whose
//    path is included so nothing is actually lost.
//
// 2. Results carry evidence. When a tool builds a project, runs tests or reads
//    a file, it records that fact as an Evidence entry. The agent uses those
//    entries to check its own final answer: "the tests pass" is only allowed to
//    be said when a test run actually succeeded.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../inference/ChatTypes.h"
#include "../util/Cancellation.h"
#include "../util/Json.h"
#include "../util/Result.h"

namespace localcode::repo {
class RepoIndex;
}

namespace localcode {

class Workspace;
class EnvironmentService;
class SkillRegistry;
class WebService;
class TaskPolicy;

// --- Evidence --------------------------------------------------------------
enum class EvidenceKind {
    FileRead,
    FileWritten,
    DirectoryListed,
    SearchPerformed,
    CommandRun,
    BuildAttempted,
    TestsRun,
    EnvironmentChecked,
    GitInspected,
    WebFetched,
};

std::string_view to_string(EvidenceKind kind) noexcept;

struct Evidence {
    EvidenceKind kind = EvidenceKind::CommandRun;
    std::string subject;    // the file, command, or query
    std::string detail;     // exit code, match count, ...
    bool positive = true;   // did the thing succeed?
    std::int64_t at_unix_ms = 0;

    [[nodiscard]] std::string describe() const;
};

// --- Confirmation ----------------------------------------------------------
struct ConfirmationRequest {
    std::string title;
    std::string detail;
    std::string command;   // the exact command, when applicable
    std::string reason;    // why the policy asked
    std::string rule_id;
};

// --- Results ---------------------------------------------------------------
struct ToolResult {
    bool ok = false;
    // One line for the activity timeline: "Read src/render.cpp (412 lines)".
    std::string summary;
    // What is actually sent back to the model.
    std::string content;
    // Optional machine-readable form; the GUI uses it for rich rendering.
    Json structured;
    std::string error;

    std::vector<Evidence> evidence;
    std::filesystem::path full_log_path;
    std::int64_t duration_ms = 0;
    bool truncated = false;

    static ToolResult success(std::string summary, std::string content);
    static ToolResult failure(std::string summary, std::string error);

    // Renders the payload the model receives, including the failure text.
    [[nodiscard]] std::string to_model_text() const;
};

// One acceptance criterion as a tool may see it: enough to name it back to
// the caller and to resolve a criterion given by wording, and nothing that
// would let a tool decide whether it holds.
struct CriterionSummary {
    int number = 0;   // 1-based position in the plan
    std::string description;
    bool settled = false;
};

// --- Context ---------------------------------------------------------------
struct ToolContext {
    Workspace* workspace = nullptr;
    std::shared_ptr<const TaskPolicy> task_policy;
    EnvironmentService* environment = nullptr;
    SkillRegistry* skills = nullptr;
    WebService* web = nullptr;
    // What LocalCode already knows about this repository. Null when nothing has
    // been indexed - the repository tools then say so rather than answering
    // from an empty index, which would read as "there is nothing there".
    repo::RepoIndex* repo = nullptr;
    // Bound source returned to a model with a small context. File tools report
    // truncation and a continuation range rather than flooding the prompt.
    std::size_t max_read_bytes = 400 * 1024;

    CancellationToken cancel;

    // Asks the user to approve an action the policy classified as Confirm.
    // Returning false means refused. When unset, Confirm is treated as refused,
    // which is the safe default for headless use.
    std::function<bool(const ConfirmationRequest&)> confirm;

    // Invocation-local Agent observation gate, after path authorization and
    // before a whole-file replacement. Explicit API callers may leave unset.
    // This is not a backup, content-identity guarantee, or OS sandbox.
    std::function<ToolResult(const std::filesystem::path&)> whole_file_write_check;

    // Live output for the command panel, called from the executing thread.
    std::function<void(std::string_view chunk, bool is_stderr)> on_output;

    // Progress notes for the activity timeline.
    std::function<void(std::string_view)> on_progress;

    // The acceptance plan, when one exists. Read-only, and used for two
    // things: listing the criteria a passing check could have settled, and
    // resolving a criterion a caller named by its exact wording instead of its
    // number.
    //
    // Deliberately not a way to settle anything, and not a way to match a
    // check to a criterion by how similar the sentences read - that is the
    // plausible inference this whole subsystem exists to remove. An exact text
    // match is a lookup of something the caller quoted; anything less is
    // refused with the numbers listed.
    //
    // Measured: a run reached "behaviour verified", could not see why its
    // criteria stayed open, and re-ran the identical check three times.
    //
    // Every criterion carries its real 1-based number in the plan. The number
    // matters: criteria are settled by position, so renumbering would settle
    // the wrong one.
    std::function<std::vector<CriterionSummary>()> acceptance_criteria;

    std::int64_t conversation_id = 0;
    // Directory for spooled command logs.
    std::filesystem::path log_directory;

    [[nodiscard]] bool ask(const ConfirmationRequest& request) const {
        return confirm ? confirm(request) : false;
    }
};

// --- Tool ------------------------------------------------------------------
class ITool {
public:
    virtual ~ITool() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    // Written for a small local model: says what the tool does, and says
    // explicitly when to use it rather than asking the user to do it by hand.
    [[nodiscard]] virtual std::string description() const = 0;
    [[nodiscard]] virtual Json parameters_schema() const = 0;

    virtual ToolResult execute(const Json& arguments, ToolContext& context) = 0;

    // True when the tool can change the user's files or run commands. Used for
    // the read-only mode and to decide what needs a baseline snapshot.
    [[nodiscard]] virtual bool mutates() const { return false; }

    // Whether this tool needs an open workspace.
    [[nodiscard]] virtual bool requires_workspace() const { return true; }

    [[nodiscard]] ToolDefinition definition() const {
        return ToolDefinition{name(), description(), parameters_schema()};
    }
};

using ToolPtr = std::shared_ptr<ITool>;

// --- Schema helpers --------------------------------------------------------
// Small builders so every tool's schema reads the same way and stays valid.
namespace schema {

Json object(std::initializer_list<std::pair<std::string, Json>> properties,
            std::vector<std::string> required = {});
Json string_param(std::string description);
Json string_enum(std::string description, std::vector<std::string> values);
Json integer_param(std::string description, std::optional<long long> minimum = std::nullopt,
                   std::optional<long long> maximum = std::nullopt);
Json boolean_param(std::string description, bool default_value);
Json array_of_strings(std::string description);

}   // namespace schema

}   // namespace localcode
