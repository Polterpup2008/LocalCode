#pragma once
// Shared helpers for the built-in tools. Internal to tools/.
#include <optional>
#include <string>

#include "../platform/FileSystem.h"
#include "../platform/PathEncoding.h"
#include "../security/TaskPolicy.h"
#include "../util/Log.h"
#include "../util/Strings.h"
#include "../workspace/Workspace.h"
#include "PathOrigin.h"
#include "Tool.h"

namespace localcode::tool_support {

inline const PathPolicy& paths(const ToolContext& context) {
    return context.task_policy ? context.task_policy->paths() : context.workspace->paths();
}

// Where the workspace is, and how the name that was asked for would be spelled
// inside it.
//
// A model that guesses an absolute path - /workspace, /app, /home/user/project
// - has a strong prior and does not abandon it easily. Measured twice: one run
// spent six steps on /workspace, then /workspace/scratchpad, then `ls /`, and
// never wrote the program it was asked for; another kept trying
// /workspace/fib8 through six refusals while the correct root sat further down
// the same message. The useful part cannot be underneath the "denied".
inline std::string path_recovery_hint(ToolContext& context, const std::string& input) {
    if (!context.workspace) return {};
    const auto root = context.workspace->root();
    if (root.empty()) return {};

    // A path nothing supports gets that said first: it is a different mistake
    // from naming a real place that is out of bounds, and the recovery is
    // different too.
    if (auto unsupported = unsupported_path_note(*context.workspace, input); !unsupported.empty()) {
        return unsupported + "\n";
    }

    std::string hint = "This workspace is " + path_to_utf8(root) + "\n";

    // The same name, spelled the way it would work here. Not a guess at intent
    // - a statement of the form. When a file of that name really is in the
    // workspace, its actual location is given instead.
    const auto asked = path_from_utf8(input);
    const auto leaf = path_to_utf8(asked.filename());
    if (!leaf.empty() && leaf != "." && leaf != "..") {
        std::error_code ec;
        if (std::filesystem::is_regular_file(root / asked.filename(), ec)) {
            hint += "A file called " + leaf + " does exist here, as: " + leaf + "\n";
        } else {
            hint += "Named from inside it, what you asked for would be: " + leaf + "\n";
        }
    }
    hint +=
        "Relative paths resolve against that root, so \"src/main.cpp\" is the usual and "
        "safest way to name a file. Do not guess at an absolute path - list_directory "
        "with no arguments shows what is actually there.\n\n";
    return hint;
}

// Resolves a model-supplied path through the workspace's path policy and turns
// a refusal into a tool error that explains the boundary rather than just
// saying "denied".
inline Result<std::filesystem::path> resolve(ToolContext& context, const std::string& input,
                                             PathAccess access) {
    if (!context.workspace) {
        return make_error(ErrorCode::NotFound, "no workspace is open");
    }
    auto decision = paths(context).evaluate(input, access);
    if (decision.allowed) return decision.canonical;

    // The recovery fact comes first, then the refusal.
    std::string message = path_recovery_hint(context, input);
    message += decision.reason;
    if (access != PathAccess::Read) {
        message +=
            "\nLocalCode may only modify files inside the selected workspace. Reading files "
            "elsewhere is allowed; writing outside is not.";
    }
    return make_error(ErrorCode::PermissionDenied, std::move(message));
}

// A line saying that an absolute path naming a conventional project root was
// read as workspace-relative, or empty when nothing was reinterpreted. Tools
// put this in front of their result: landing the write somewhere other than
// the name that was asked for is fine, but doing it silently is not.
inline std::string reinterpretation_note(ToolContext& context, const std::string& input,
                                         PathAccess access) {
    if (!context.workspace) return {};
    const auto decision = paths(context).evaluate(input, access);
    if (decision.reinterpreted_from.empty() || !decision.allowed) return {};

    // The usable form leads and the invalid one appears once, for diagnosis.
    // The ordering is the point: a refusal that quoted the bad path put
    // another copy of it in the context, and the next call copied it back.
    // Whatever this message repeats most is what gets written next time.
    return "Workspace-relative path:  " + context.workspace->to_relative(decision.canonical) +
           "\nCanonical destination:    " + path_to_utf8(decision.canonical) + "\n(" +
           decision.reinterpreted_from +
           " is a container convention and does not exist on this machine; it was read as the "
           "workspace root. Use the relative form above.)\n";
}

inline std::string relative_to_workspace(ToolContext& context, const std::filesystem::path& path) {
    return context.workspace ? context.workspace->to_relative(path) : path_to_utf8(path);
}

// Reads a required string parameter, tolerating a number where a string was
// expected because small models produce those regularly.
inline std::string arg_string(const Json& arguments, std::string_view key,
                              std::string fallback = {}) {
    return json_string(arguments, key, std::move(fallback));
}

inline long long arg_int(const Json& arguments, std::string_view key, long long fallback) {
    return json_int(arguments, key, fallback);
}

inline bool arg_bool(const Json& arguments, std::string_view key, bool fallback) {
    return json_bool(arguments, key, fallback);
}

inline std::vector<std::string> arg_strings(const Json& arguments, std::string_view key) {
    std::vector<std::string> values;
    const auto it = arguments.find(std::string{key});
    if (it == arguments.end()) return values;
    if (it->is_array()) {
        for (const auto& entry : *it) {
            if (entry.is_string())
                values.push_back(entry.get<std::string>());
            else if (!entry.is_null())
                values.push_back(entry.dump());
        }
    } else if (it->is_string()) {
        values.push_back(it->get<std::string>());
    }
    return values;
}

inline Evidence make_evidence(EvidenceKind kind, std::string subject, std::string detail,
                              bool positive) {
    Evidence evidence;
    evidence.kind = kind;
    evidence.subject = std::move(subject);
    evidence.detail = std::move(detail);
    evidence.positive = positive;
    evidence.at_unix_ms = now_unix_ms();
    return evidence;
}

}   // namespace localcode::tool_support
