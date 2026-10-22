#pragma once
// The command contract: one authoritative representation of "run this".
//
// run_command grew three ways to say the same thing - a structured argv list, a
// shell string, and (now) an explicit executable plus arguments - and for a
// while it silently preferred one and discarded the others. A measured run sent
// both of these in a single call:
//
//   "command":       ["build", "RelWithDebInfo", "fibonacci.exe"]
//   "shell_command": "cd /workspace && build/RelWithDebInfo/fibonacci.exe"
//
// The first is a path that has been shattered into three argv elements and
// names a program called "build". The second says what was actually meant.
// LocalCode ran the first and never mentioned the second, and the run died
// looking for a program it had already built.
//
// Two representations of one operation, with no authoritative one, is a defect
// in the application and not in the model. This header defines which form wins,
// what happens when they disagree, and what a caller is told when the shape
// makes no sense. Nothing here guesses: a request that cannot be read one way
// is refused with both readings shown.
#include <string>
#include <vector>

#include "../util/Json.h"

namespace localcode {

// Which of the accepted shapes a request arrived in.
enum class CommandForm {
    None,        // nothing runnable was given
    Canonical,   // executable + arguments  (the form to use)
    LegacyArgv,  // command: [...]
    Shell,       // shell_command: "..."
};

std::string_view to_string(CommandForm form) noexcept;

struct ParsedCommand {
    bool        valid = false;
    CommandForm form = CommandForm::None;

    // What to run. For Shell, `shell_text` carries the line and argv is the
    // tokenisation of it that the security policy inspects.
    std::string              executable;
    std::vector<std::string> arguments;
    std::string              shell_text;
    bool                     use_shell = false;

    std::string working_directory;
    std::vector<std::pair<std::string, std::string>> environment;

    // Set when !valid. Summary is one line; detail explains the shape and shows
    // the reading LocalCode would have had to guess between.
    std::string error_summary;
    std::string error_detail;

    // Set when the request was runnable but the shape is not the preferred one.
    // Advisory, never fatal.
    std::string note;

    // The whole command as one line, for logs and messages.
    [[nodiscard]] std::string display() const;
    // executable followed by arguments, which is what the runner consumes.
    [[nodiscard]] std::vector<std::string> argv() const;
};

// True when a line needs a shell to mean what it says: pipes, redirects, &&,
// ||, ;, command substitution, or variable expansion. A line without any of
// these has exactly one reading and does not need one.
[[nodiscard]] bool needs_shell_semantics(std::string_view line);

// Reads a run_command argument object into the canonical form.
//
// Precedence, in order, and deliberately not negotiable:
//   1. `executable` is authoritative. Giving `command` or `shell_command` as
//      well is a conflict unless they say exactly the same thing - and three
//      ways of saying the same thing count as agreement, because they are
//      identities rather than resemblances: the second copy repeating the
//      whole argv, the second copy carrying the arguments after the program
//      `executable` named, and one path shattered across argv elements that
//      rejoins to exactly the `executable` string.
//   2. `command` and `shell_command` together must agree once the shell line
//      is tokenised. When they disagree, neither is run.
//   3. `shell_command` alone runs through a shell only if it needs one.
//
// `command` is accepted but no longer advertised in the tool schema. Offering
// two structured spellings of one thing was the defect, not the fix for it: a
// small model copies the shape of its own previous call, and a stale
// `executable` carried forward beside a fresh `command` produced a conflict it
// could not climb out of - measured at five identical retries before the run
// was stopped. One documented form means there is no second field to carry.
[[nodiscard]] ParsedCommand parse_command_request(const Json& arguments);

}  // namespace localcode
