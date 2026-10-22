#include "CommandRequest.h"

#include <algorithm>

#include "../security/CommandPolicy.h"
#include "../util/Strings.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

// Quotes a token for display only, so a line printed back to the caller can be
// read as the shell line it corresponds to.
std::string quote_for_display(const std::string& token) {
    if (token.empty()) return "\"\"";
    if (token.find_first_of(" \t\"") == std::string::npos) return token;
    std::string quoted = "\"";
    for (const char c : token) {
        if (c == '"') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

std::string join_for_display(const std::vector<std::string>& argv) {
    std::vector<std::string> parts;
    parts.reserve(argv.size());
    for (const auto& token : argv) parts.push_back(quote_for_display(token));
    return strings::join(parts, " ");
}

// Two argv lists mean the same command when they are element-for-element
// equal after normalising the separators inside each token. build\Debug\a.exe
// and build/Debug/a.exe are the same program on Windows, and a model that
// writes one in the array and the other in the shell line has not contradicted
// itself.
bool same_command(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::string left = a[i];
        std::string right = b[i];
        std::replace(left.begin(), left.end(), '\\', '/');
        std::replace(right.begin(), right.end(), '\\', '/');
        if (left != right) return false;
    }
    return true;
}

// A refusal a small model can act on is one it can copy. "Send one of them,
// not both" is advice; a ready call is a thing to do. Measured: a model
// answered the abstract version by resending the identical call, because the
// only concrete text in front of it was the call it had just made.
std::string ready_to_send(const std::vector<std::string>& argv) {
    std::string call = "  {\"executable\": \"" + argv.front() + "\", \"arguments\": [";
    for (std::size_t i = 1; i < argv.size(); ++i) {
        if (i > 1) call += ", ";
        call += "\"" + argv[i] + "\"";
    }
    call += "]}";
    return call;
}

const char* kCanonicalShape =
    "The form that always works:\n"
    "  {\"executable\": \"cmake\",\n"
    "   \"arguments\": [\"--build\", \"build\", \"--config\", \"RelWithDebInfo\"],\n"
    "   \"working_directory\": \".\"}\n"
    "One program, one list of arguments, no quoting and nothing to parse. Give "
    "either that or a shell_command, never both.";

}  // namespace

std::string_view to_string(CommandForm form) noexcept {
    switch (form) {
        case CommandForm::None:       return "none";
        case CommandForm::Canonical:  return "executable + arguments";
        case CommandForm::LegacyArgv: return "command";
        case CommandForm::Shell:      return "shell_command";
    }
    return "none";
}

std::string ParsedCommand::display() const {
    if (use_shell) return shell_text;
    return join_for_display(argv());
}

std::vector<std::string> ParsedCommand::argv() const {
    if (use_shell) return CommandPolicy::tokenize(shell_text);
    std::vector<std::string> all;
    all.reserve(arguments.size() + 1);
    if (!executable.empty()) all.push_back(executable);
    all.insert(all.end(), arguments.begin(), arguments.end());
    return all;
}

bool needs_shell_semantics(std::string_view line) {
    bool in_single = false;
    bool in_double = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !in_double) { in_single = !in_single; continue; }
        if (c == '"' && !in_single) { in_double = !in_double; continue; }
        if (in_single || in_double) continue;

        switch (c) {
            case '|': case '&': case ';': case '<': case '>': case '`':
                return true;
            case '$':
                return true;   // variable or command substitution
            case '%':
                // Windows %VAR% expansion needs a closing % on the same line.
                if (line.find('%', i + 1) != std::string_view::npos) return true;
                break;
            default:
                break;
        }
    }
    return false;
}

ParsedCommand parse_command_request(const Json& arguments) {
    ParsedCommand parsed;

    const auto executable = json_string(arguments, "executable");
    auto legacy_argv = tool_support::arg_strings(arguments, "command");
    const auto shell_text = strings::trim(json_string(arguments, "shell_command"));

    parsed.working_directory = json_string(arguments, "working_directory");
    if (parsed.working_directory.empty()) parsed.working_directory = json_string(arguments, "cwd");

    if (const auto it = arguments.find("environment");
        it != arguments.end() && it->is_object()) {
        for (auto entry = it->begin(); entry != it->end(); ++entry) {
            parsed.environment.emplace_back(entry.key(),
                                            entry->is_string() ? entry->get<std::string>()
                                                               : entry->dump());
        }
    }

    const bool has_canonical = !executable.empty();
    const bool has_legacy = !legacy_argv.empty();
    const bool has_shell = !shell_text.empty();

    if (!has_canonical && !has_legacy && !has_shell) {
        parsed.error_summary = "no command given";
        parsed.error_detail = std::string{kCanonicalShape} +
                              "\n\nNothing runnable was in this call at all.";
        return parsed;
    }

    // The shell line, read as a command, so the two forms can be compared and
    // so the security policy has tokens to inspect either way.
    const auto shell_argv = has_shell ? CommandPolicy::tokenize(shell_text)
                                      : std::vector<std::string>{};

    // --- 1. executable is authoritative -----------------------------------
    if (has_canonical) {
        parsed.form = CommandForm::Canonical;
        parsed.executable = executable;
        parsed.arguments = tool_support::arg_strings(arguments, "arguments");
        const bool arguments_given = arguments.contains("arguments");

        // `executable` naming the program and `command` carrying the whole
        // argv is not a contradiction - it is the same command written twice,
        // and the second copy is the one with the arguments in it. Reading it
        // as a conflict deadlocked a whole run: the model sent
        //
        //   "executable": "cmake",
        //   "command":    ["cmake", "-S", ".", "-B", "build", ...]
        //
        // four times, was told its two forms disagreed, and had nowhere to go.
        // They agree. There is exactly one reading and no guess involved.
        if (has_legacy && !arguments_given && same_command({executable}, {legacy_argv.front()})) {
            parsed.arguments.assign(legacy_argv.begin() + 1, legacy_argv.end());
            parsed.note =
                "note: 'executable' and 'command' both named this command. Either alone is "
                "enough - 'executable' with 'arguments' is the clearest.\n";
            if (has_shell && !same_command(parsed.argv(), shell_argv)) {
                parsed.error_summary =
                    "conflicting command: 'shell_command' describes something else";
                parsed.error_detail =
                    "This call describes two different commands and LocalCode will not choose "
                    "between them.\n\n"
                    "  executable + command:  " + join_for_display(parsed.argv()) + "\n"
                    "  shell_command:         " + shell_text + "\n\n"
                    "Send one of them, not both.";
                parsed.note.clear();
                return parsed;
            }
            parsed.valid = true;
            return parsed;
        }

        // One path, written twice: correctly in `executable`, and shattered
        // across argv elements in `command`. Joining the pieces with a slash
        // gives back exactly the executable, character for character - so this
        // is an identity, not a resemblance, and there is nothing to choose
        // between.
        //
        // Measured: {"command": ["build", "RelWithDebInfo", "fibonacci.exe"],
        // "executable": "build/RelWithDebInfo/fibonacci.exe"} sent five times
        // in a row against a "these disagree" refusal. They did not disagree.
        // The correct spelling was sitting in the same call.
        if (has_legacy && !arguments_given && legacy_argv.size() > 1 &&
            same_command({strings::join(legacy_argv, "/")}, {executable})) {
            parsed.valid = true;
            parsed.note =
                "note: 'command' held this same path split across several elements. A path is "
                "one string - 'executable' had it right.\n";
            if (has_shell && !same_command(parsed.argv(), shell_argv)) {
                parsed.valid = false;
                parsed.note.clear();
                parsed.error_summary =
                    "conflicting command: 'shell_command' describes something else";
                parsed.error_detail =
                    "  executable:     " + join_for_display(parsed.argv()) + "\n"
                    "  shell_command:  " + shell_text + "\n\nSend one of them, not both.";
            }
            return parsed;
        }

        // The same thing again, with the arguments in `shell_command` instead
        // of `command`. Treating one as agreement and the other as a conflict
        // was an inconsistency in this contract, not a distinction: both are
        // "the program is named twice and the arguments came with the second
        // copy".
        //
        // Measured: {"executable": "cmake", "shell_command": "cmake -S . -B
        // build -G Ninja ..."} refused five times as two different commands.
        // It is one command, written once carelessly and once in full.
        if (has_shell && !has_legacy && !arguments_given && shell_argv.size() > 1 &&
            same_command({executable}, {shell_argv.front()})) {
            parsed.arguments.assign(shell_argv.begin() + 1, shell_argv.end());
            parsed.use_shell = needs_shell_semantics(shell_text);
            if (parsed.use_shell) {
                parsed.form = CommandForm::Shell;
                parsed.shell_text = shell_text;
            }
            parsed.valid = true;
            parsed.note =
                "note: 'executable' named the program and 'shell_command' carried the whole "
                "line. Either alone is enough - 'executable' with 'arguments' is the "
                "clearest.\n";
            return parsed;
        }

        const auto canonical_argv = parsed.argv();

        if (has_legacy && !same_command(canonical_argv, legacy_argv)) {
            parsed.error_summary = "conflicting command: 'executable' and 'command' disagree";
            parsed.error_detail =
                "This call names two different programs and LocalCode will not choose between "
                "them.\n\n"
                "  executable:  " + join_for_display(canonical_argv) + "\n"
                "  command:     " + join_for_display(legacy_argv) + "\n\n"
                "Send exactly one of these, with no other command fields:\n\n" +
                ready_to_send(legacy_argv) + "\n" + ready_to_send(canonical_argv) + "\n\n"
                "A leftover 'executable' from an earlier call is the usual cause. Whichever "
                "you want, send it on its own.";
            return parsed;
        }
        if (has_shell && !same_command(canonical_argv, shell_argv)) {
            // The shell line is the same command with the exit code echoed
            // after it. Measured: `... -DCMAKE_BUILD_TYPE=RelWithDebInfo; echo
            // "EXIT_CODE=$?"`. That is not a second command so much as a
            // workaround for something LocalCode already does, and saying only
            // "these disagree" leaves the caller no reason to drop it.
            //
            // Compared segment by segment rather than token by token: the
            // tokeniser splits on whitespace only, so "build;" arrives with
            // its semicolon attached and never matches "build".
            const auto segments = CommandPolicy::split_shell_segments(shell_text);
            const bool echoes_exit_code =
                segments.size() == 2 &&
                same_command(CommandPolicy::tokenize(segments.front()), canonical_argv) &&
                strings::starts_with(strings::to_lower(strings::trim(segments.back())), "echo") &&
                (shell_text.find("$?") != std::string::npos ||
                 strings::icontains(shell_text, "errorlevel"));
            if (echoes_exit_code) {
                parsed.error_summary =
                    "the shell line only adds an exit-code echo, which is already reported";
                parsed.error_detail =
                    "'shell_command' is this same command with the exit code echoed after "
                    "it. Every command's exit code is reported already - it is the \"exit:\" "
                    "line in the result - so the echo adds nothing and makes the call two "
                    "commands instead of one.\n\nSend just:\n\n" +
                    ready_to_send(canonical_argv) +
                    "\n\nTo check the exit code, put it in the expectation rather than in the "
                    "output:  \"expect\": { ..., \"exit_code\": 0 }";
                return parsed;
            }

            parsed.error_summary =
                "conflicting command: 'executable' and 'shell_command' disagree";
            parsed.error_detail =
                "This call describes two different commands and LocalCode will not choose "
                "between them.\n\n"
                "  executable + arguments:  " + join_for_display(canonical_argv) + "\n"
                "  shell_command:           " + shell_text + "\n\n"
                "Send one of them, not both. Use shell_command only when the line genuinely "
                "needs a shell - a pipe, a redirect, && or ||. Otherwise use the executable "
                "form on its own.";
            return parsed;
        }
        parsed.valid = true;
        return parsed;
    }

    // --- 2. command and shell_command must agree --------------------------
    if (has_legacy && has_shell) {
        if (!same_command(legacy_argv, shell_argv)) {
            parsed.error_summary = "conflicting command: 'command' and 'shell_command' disagree";
            parsed.error_detail =
                "Both were given and they are not the same command, so neither was run.\n\n"
                "  command:        " + join_for_display(legacy_argv) + "\n"
                "  shell_command:  " + shell_text + "\n\n"
                "Send exactly one, with no other command fields. To run the first:\n\n" +
                ready_to_send(legacy_argv) +
                "\n\nUse shell_command only when the line genuinely needs a shell - a pipe, a "
                "redirect, && or ||.";
            return parsed;
        }
        // They agree, so the structured form is used and the duplication is
        // pointed out rather than silently dropped.
        parsed.form = CommandForm::LegacyArgv;
        parsed.executable = legacy_argv.front();
        parsed.arguments.assign(legacy_argv.begin() + 1, legacy_argv.end());
        parsed.valid = true;
        parsed.note =
            "note: 'command' and 'shell_command' both described this same command. Only one "
            "is needed - send 'executable' and 'arguments' on their own.\n";
        return parsed;
    }

    // --- 3. one form on its own -------------------------------------------
    if (has_legacy) {
        parsed.form = CommandForm::LegacyArgv;
        parsed.executable = legacy_argv.front();
        parsed.arguments.assign(legacy_argv.begin() + 1, legacy_argv.end());
        parsed.valid = true;
        return parsed;
    }

    parsed.form = CommandForm::Shell;
    parsed.shell_text = shell_text;
    parsed.valid = true;

    if (needs_shell_semantics(shell_text)) {
        parsed.use_shell = true;
        return parsed;
    }

    // A shell line with nothing shell-like in it has exactly one reading, so
    // it is run directly. Going through a shell would add quoting rules for no
    // benefit, and every layer that reparses a Windows path is a chance to
    // lose a backslash.
    if (shell_argv.empty()) {
        parsed.valid = false;
        parsed.error_summary = "no command given";
        parsed.error_detail = "shell_command was present but contained no command.";
        return parsed;
    }
    parsed.executable = shell_argv.front();
    parsed.arguments.assign(shell_argv.begin() + 1, shell_argv.end());
    parsed.note =
        "note: this line needs no shell - it has no pipe, redirect, && or || - so it was run "
        "directly. Prefer 'executable' and 'arguments' for commands like this.\n";
    return parsed;
}

}  // namespace localcode
