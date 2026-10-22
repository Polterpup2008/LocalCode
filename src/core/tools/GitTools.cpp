// ---------------------------------------------------------------------------
// Read-only git tools.
//
// There is deliberately no commit, reset, clean or push tool here. Those change
// history or destroy work, and the decision to run them belongs to the user, not
// to a model. They remain available through run_command, where CommandPolicy
// requires explicit confirmation for each one.
// ---------------------------------------------------------------------------
#include "../git/GitService.h"
#include "../util/Log.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

std::optional<ToolResult> require_repository(ToolContext& context) {
    if (!context.workspace->git().available()) {
        return ToolResult::failure(
            "git is not installed",
            "git was not found on PATH, so repository information is unavailable. This is not a "
            "problem with the project - do not report it as one.");
    }
    if (!context.workspace->is_git_repository()) {
        return ToolResult::failure(
            "not a git repository",
            "The workspace is not inside a git repository, so there is no version history to "
            "inspect. Use file_info and list_directory instead.");
    }
    return std::nullopt;
}

// --- git_status ------------------------------------------------------------
class GitStatusTool final : public ITool {
public:
    std::string name() const override { return "git_status"; }

    std::string description() const override {
        return "Shows the current branch and which files are modified, staged or untracked. Use "
               "this at the start of a task to see what was already changed before you began, and "
               "at the end to see exactly what you changed. It also marks which of the changes "
               "were made by you during this session.";
    }

    Json parameters_schema() const override { return schema::object({}); }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (auto refusal = require_repository(context)) return *refusal;

        auto status = context.workspace->git_status(context.cancel);
        if (!status) return ToolResult::failure("git status failed", status.error().to_string());

        const auto agent_changes = context.workspace->agent_changes();
        auto changed_by_agent = [&agent_changes](const std::string& path) {
            for (const auto& change : agent_changes) {
                if (change.relative_path == path) return true;
            }
            return false;
        };
        const auto baseline = context.workspace->baseline_dirty_files();

        std::string report;
        report += "branch:   " + (status->branch.empty() ? "(unknown)" : status->branch);
        if (status->detached_head) report += "  (detached HEAD)";
        report += "\n";
        if (!status->upstream.empty()) {
            report += "upstream: " + status->upstream;
            if (status->ahead || status->behind) {
                report += "  (ahead " + std::to_string(status->ahead) + ", behind " +
                          std::to_string(status->behind) + ")";
            }
            report += "\n";
        }
        if (!status->head_commit.empty()) {
            report += "HEAD:     " + status->head_commit.substr(0, 12) + "\n";
        }

        if (status->files.empty()) {
            report += "\nThe working tree is clean - nothing is modified.\n";
        } else {
            report += "\n" + std::to_string(status->files.size()) + " changed file(s):\n";
            for (const auto& file : status->files) {
                report += "  " + file.describe() + ": " + file.path;
                if (!file.original_path.empty()) report += "  (was " + file.original_path + ")";
                if (changed_by_agent(file.path)) report += "   [changed by you this session]";
                else if (baseline.count(file.path)) report += "   [was already modified before]";
                report += "\n";
            }
        }

        const auto conflicts = context.workspace->conflicting_changes();
        if (!conflicts.empty()) {
            report += "\nWarning: you edited " + std::to_string(conflicts.size()) +
                      " file(s) that already had uncommitted changes before this task started:\n";
            for (const auto& change : conflicts) report += "  " + change.relative_path + "\n";
        }

        auto result = ToolResult::success(
            status->files.empty() ? "Working tree is clean"
                                  : std::to_string(status->files.size()) + " file(s) changed",
            report);
        result.evidence.push_back(make_evidence(EvidenceKind::GitInspected, "git status",
                                                std::to_string(status->files.size()) + " changed",
                                                true));
        return result;
    }
};

// --- git_diff --------------------------------------------------------------
class GitDiffTool final : public ITool {
public:
    std::string name() const override { return "git_diff"; }

    std::string description() const override {
        return "Shows the actual changes in the working tree as a unified diff. Use this to "
               "review your own edits before reporting that a task is done - reading the diff is "
               "how you verify you changed what you meant to change and nothing else.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"paths", schema::array_of_strings(
                           "Limit the diff to these paths. Omit for every change.")},
             {"staged", schema::boolean_param("Show staged changes instead of unstaged ones.",
                                              false)},
             {"since_task_start",
              schema::boolean_param(
                  "Diff against the commit that was checked out when this task started, which "
                  "includes changes that have since been staged. Defaults to false.",
                  false)},
             {"context_lines", schema::integer_param("Context lines per hunk. Defaults to 3.", 0,
                                                     20)},
             {"max_bytes", schema::integer_param(
                               "Truncate the diff at this size. Defaults to 60000.", 1000,
                               400000)}});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (auto refusal = require_repository(context)) return *refusal;

        const auto paths = arg_strings(arguments, "paths");
        const bool staged = arg_bool(arguments, "staged", false);
        const bool since_start = arg_bool(arguments, "since_task_start", false);
        const auto context_lines = static_cast<int>(arg_int(arguments, "context_lines", 3));
        const auto max_bytes = static_cast<std::size_t>(arg_int(arguments, "max_bytes", 60000));

        const auto root = context.workspace->root();
        auto& git = context.workspace->git();

        Result<DiffResult> diff = make_error(ErrorCode::Internal, "not run");
        if (since_start) {
            const auto baseline = context.workspace->baseline_commit();
            if (baseline.empty()) {
                diff = git.diff(root, paths, staged, context_lines, context.cancel);
            } else {
                diff = git.diff_against(root, baseline, paths, context.cancel);
            }
        } else {
            diff = git.diff(root, paths, staged, context_lines, context.cancel);
        }
        if (!diff) return ToolResult::failure("git diff failed", diff.error().to_string());

        if (diff->files.empty()) {
            return ToolResult::success(
                "No changes",
                std::string{"There are no changes in the working tree"} +
                    (paths.empty() ? "" : " for the given paths") + ".");
        }

        std::string summary_block = std::to_string(diff->files.size()) + " file(s) changed, +" +
                                    std::to_string(diff->total_additions) + " -" +
                                    std::to_string(diff->total_deletions) + "\n";
        for (const auto& file : diff->files) {
            summary_block += "  " + std::string{to_string(file.change)} + ": " + file.path + "  +" +
                             std::to_string(file.additions) + " -" +
                             std::to_string(file.deletions) + "\n";
        }

        const auto body = strings::excerpt(diff->raw, 400, 200, max_bytes);
        std::string content = summary_block + "\n" + body.text;
        if (body.truncated) {
            content +=
                "\n[the diff was abridged; use git_diff with a 'paths' filter to see a specific "
                "file in full]";
        }

        auto result = ToolResult::success(
            std::to_string(diff->files.size()) + " file(s) changed (+" +
                std::to_string(diff->total_additions) + " -" +
                std::to_string(diff->total_deletions) + ")",
            content);
        result.truncated = body.truncated;
        result.evidence.push_back(make_evidence(
            EvidenceKind::GitInspected, "git diff",
            std::to_string(diff->files.size()) + " files", true));
        return result;
    }
};

// --- git_log ---------------------------------------------------------------
class GitLogTool final : public ITool {
public:
    std::string name() const override { return "git_log"; }

    std::string description() const override {
        return "Shows recent commits, optionally for one file. Use this to understand how a piece "
               "of code came to be the way it is, or to find when a regression was introduced.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"max_count", schema::integer_param("How many commits. Defaults to 20.", 1, 200)},
             {"path", schema::string_param("Only commits touching this path.")}});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (auto refusal = require_repository(context)) return *refusal;

        const auto max_count = static_cast<int>(arg_int(arguments, "max_count", 20));
        const auto path = arg_string(arguments, "path");

        auto commits = context.workspace->git().log(context.workspace->root(), max_count, path,
                                                    context.cancel);
        if (!commits) return ToolResult::failure("git log failed", commits.error().to_string());

        if (commits->empty()) {
            return ToolResult::success("No commits found",
                                       path.empty() ? "The repository has no commits."
                                                    : "No commits touch " + path + ".");
        }

        std::string report;
        for (const auto& commit : *commits) {
            report += commit.short_hash + "  " + format_timestamp(commit.timestamp_unix * 1000) +
                      "  " + commit.author_name + "\n";
            report += "    " + commit.subject + "\n";
        }

        auto result = ToolResult::success(
            std::to_string(commits->size()) + " commit(s)",
            std::to_string(commits->size()) + " most recent commit(s)" +
                (path.empty() ? "" : " touching " + path) + ":\n" + report);
        result.evidence.push_back(make_evidence(EvidenceKind::GitInspected, "git log",
                                                std::to_string(commits->size()) + " commits",
                                                true));
        return result;
    }
};

}  // namespace

std::vector<ToolPtr> make_git_tools() {
    return {std::make_shared<GitStatusTool>(), std::make_shared<GitDiffTool>(),
            std::make_shared<GitLogTool>()};
}

}  // namespace localcode
