#pragma once
// ---------------------------------------------------------------------------
// Command execution policy.
//
// The model never decides what is safe. It proposes a command; this class
// classifies it and the application enforces the classification.
//
// Three outcomes:
//   Allow   - run it, no prompt. Building, testing, compiling, reading git,
//             running the project's own binaries: the normal work of coding.
//   Confirm - run it only after a human says yes. Reaches outside the
//             workspace, installs software system-wide, rewrites history.
//   Block   - never run it, whatever the user or the model says. Disk
//             formatting, boot configuration, credential theft, disabling
//             security software, shutting the machine down.
//
// Note what is deliberately *not* a factor: where the executable lives.
// C:\Program Files\CMake\bin\cmake.exe driving a build in D:\Projects\Game is
// ordinary and allowed. Only the effect of a command matters here; where files
// may be written is PathPolicy's job.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <vector>

#include "PathPolicy.h"

namespace localcode {

struct ProcessRequest;

enum class CommandDecision { Allow, Confirm, Block };

std::string_view to_string(CommandDecision decision) noexcept;

// How much freedom the agent has for commands no rule recognises.
enum class AutonomyLevel {
    Restricted,   // unknown commands need confirmation
    Standard,     // unknown commands run; heuristically risky ones are confirmed
    Full,         // unknown commands run (Block rules still apply)
};

std::string_view to_string(AutonomyLevel level) noexcept;

struct CommandEvaluation {
    CommandDecision decision = CommandDecision::Confirm;
    std::string     rule_id;      // which rule fired, "" for the default
    std::string     reason;       // shown to the user, and to the model when refused
    std::string     matched_text; // the segment that triggered the rule
};

// A single classification rule. Rules are data, so users can extend the policy
// from settings without touching code.
struct CommandRule {
    std::string              id;
    CommandDecision          decision = CommandDecision::Block;
    std::string              reason;
    // Executable basenames this rule applies to; empty means any executable.
    std::vector<std::string> executables;
    // Case-insensitive globs matched against the normalised "exe arg arg" text.
    // A rule fires when any pattern matches (or when there are no patterns and
    // the executable matched).
    std::vector<std::string> patterns;
    // All of these must also be present for the rule to fire. Used to keep
    // "npm install" allowed while "npm install -g" is confirmed.
    std::vector<std::string> require_all;
    // Any of these suppresses the rule.
    std::vector<std::string> unless_any;
    bool                     enabled = true;
};

class CommandPolicy {
public:
    CommandPolicy();

    void set_autonomy(AutonomyLevel level) { autonomy_ = level; }
    [[nodiscard]] AutonomyLevel autonomy() const { return autonomy_; }

    void set_path_policy(const PathPolicy* policy) { paths_ = policy; }

    void add_rule(CommandRule rule);
    void set_rule_enabled(const std::string& id, bool enabled);
    [[nodiscard]] const std::vector<CommandRule>& rules() const { return rules_; }

    // Classifies a structured command.
    [[nodiscard]] CommandEvaluation evaluate(const ProcessRequest& request) const;

    // Classifies a shell one-liner. The string is split on &&, ||, |, ;, and
    // newlines and every segment is classified; the strictest outcome wins, so
    // "cmake --build . && format C:" is blocked as a whole.
    [[nodiscard]] CommandEvaluation evaluate_shell(std::string_view command_line) const;

    // Splits a shell command into its segments (exposed for tests and the UI).
    [[nodiscard]] static std::vector<std::string> split_shell_segments(std::string_view line);

    // Tokenises one segment into argv, honouring quotes. Not a full shell parser
    // - it exists so the policy can see the arguments, not to execute anything.
    [[nodiscard]] static std::vector<std::string> tokenize(std::string_view segment);

    // The built-in rule set. Exposed so the settings UI can show and document it.
    [[nodiscard]] static std::vector<CommandRule> builtin_rules();

private:
    [[nodiscard]] CommandEvaluation classify_tokens(const std::vector<std::string>& argv) const;
    [[nodiscard]] CommandEvaluation apply_heuristics(const std::vector<std::string>& argv,
                                                     std::string_view joined) const;

    std::vector<CommandRule> rules_;
    AutonomyLevel            autonomy_ = AutonomyLevel::Standard;
    const PathPolicy*        paths_ = nullptr;
};

}  // namespace localcode
