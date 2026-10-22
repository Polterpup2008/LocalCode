// ---------------------------------------------------------------------------
// Running commands.
//
// This is the tool that makes LocalCode an agent rather than a chat window: it
// builds, it tests, it runs the program. Three things keep it usable:
//
//   * Commands are argv vectors, not shell strings, so nothing is mangled by
//     quoting and nothing is injected by a model-authored filename.
//   * The result is small. A build that emits 40 MB of output comes back as an
//     exit code, a timing, the extracted diagnostics, and a path to the full log.
//   * The policy decides what may run. The model's opinion is not consulted.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <regex>

#include "../platform/Environment.h"
#include "../platform/Paths.h"
#include "../platform/Process.h"
#include "../util/Log.h"
#include "BuildDirectory.h"
#include "CommandInspection.h"
#include "CommandRequest.h"
#include "Diagnostics.h"
#include "OutputExpectation.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

// Recognises what a command was trying to do, so the evidence records "a build
// happened" rather than just "a process ran".
// Is this a wrapper that runs whatever follows it, so the real command is
// further along the argument list?
bool is_command_wrapper(const std::string& stem) {
    static const char* kWrappers[] = {"cmd", "sh",   "bash", "zsh",   "powershell", "pwsh",
                                      "env", "nice", "time", "xargs", "uv",         "npx"};
    for (const char* wrapper : kWrappers) {
        if (stem == wrapper) return true;
    }
    return false;
}

// What a command was actually doing, so the evidence says "a build happened"
// rather than "a process ran".
//
// Classification keys on the program being run, not on the whole command line.
// Scanning the joined line recorded this as a successful build:
//
//     vcvars64.bat cmake -B build     (exit 0)
//
// vcvars64.bat ignores arguments it does not recognise, so CMake never ran, the
// script exited 0, and the run was marked partially verified on a build that
// never happened. Fake progress of exactly the kind the evidence system exists
// to prevent - and the model repeated the command four times because it kept
// being told it had worked.
EvidenceKind classify_command(const std::vector<std::string>& argv) {
    if (argv.empty()) return EvidenceKind::CommandRun;

    static const char* kTestPrograms[] = {"ctest", "pytest", "gtest"};
    static const char* kBuildPrograms[] = {"cmake",   "ninja", "make", "gmake", "nmake",
                                           "msbuild", "gcc",   "g++",  "clang", "clang++",
                                           "cl",      "rustc", "tsc",  "javac"};
    // Programs whose meaning depends on the subcommand.
    static const char* kSubcommandDriven[] = {"cargo", "go",    "dotnet", "npm", "yarn",
                                              "pnpm",  "swift", "gradle", "mvn"};

    // Walk past any wrappers to the program that actually does something.
    std::size_t index = 0;
    for (; index + 1 < argv.size(); ++index) {
        auto stem = strings::to_lower(std::filesystem::path{argv[index]}.stem().string());
        if (!is_command_wrapper(stem)) break;
    }

    const auto program = strings::to_lower(std::filesystem::path{argv[index]}.stem().string());
    const auto rest = strings::to_lower(strings::join(
        {argv.begin() + static_cast<std::ptrdiff_t>(index) + (argv.size() > index), argv.end()},
        " "));

    for (const char* candidate : kTestPrograms) {
        if (program == candidate) return EvidenceKind::TestsRun;
    }
    for (const char* candidate : kSubcommandDriven) {
        if (program != candidate) continue;
        if (rest.find("test") != std::string::npos) return EvidenceKind::TestsRun;
        if (rest.find("build") != std::string::npos || rest.find("compile") != std::string::npos) {
            return EvidenceKind::BuildAttempted;
        }
        return EvidenceKind::CommandRun;
    }
    for (const char* candidate : kBuildPrograms) {
        if (program == candidate) return EvidenceKind::BuildAttempted;
    }

    // Anything else - including an environment script that happens to have a
    // build tool's name among its arguments - is just a command that ran.
    return EvidenceKind::CommandRun;
}

// Which build tree a build command is about to drive, so its CMake cache can
// be consulted. Empty when the command is not one of these.
//
//   cmake --build <dir>     the argument after --build
//   ninja / ninja -C <dir>  the working directory, or the -C argument
std::filesystem::path build_tree_of(const std::vector<std::string>& argv,
                                    const std::filesystem::path& working_directory) {
    if (argv.empty()) return {};
    const auto program = strings::to_lower(std::filesystem::path{argv[0]}.stem().string());

    const auto resolve = [&](const std::string& value) {
        std::filesystem::path candidate{value};
        return candidate.is_absolute() ? candidate : working_directory / candidate;
    };

    if (program == "cmake") {
        for (std::size_t i = 1; i + 1 < argv.size(); ++i) {
            if (argv[i] == "--build") return resolve(argv[i + 1]);
        }
        return {};
    }
    if (program == "ninja" || program == "make" || program == "gmake") {
        for (std::size_t i = 1; i + 1 < argv.size(); ++i) {
            if (argv[i] == "-C") return resolve(argv[i + 1]);
        }
        return working_directory;
    }
    return {};
}

// The source directory a configure was pointed at: the -S argument, or the
// first bare path, or the working directory. Empty for anything that is not a
// cmake configure.
std::filesystem::path configure_source_of(const std::vector<std::string>& argv,
                                          const std::filesystem::path& working_directory) {
    if (argv.empty()) return {};
    if (strings::to_lower(std::filesystem::path{argv[0]}.stem().string()) != "cmake") return {};

    const auto resolve = [&](const std::string& value) {
        std::filesystem::path candidate{value};
        return candidate.is_absolute() ? candidate : working_directory / candidate;
    };

    for (std::size_t i = 1; i < argv.size(); ++i) {
        if (argv[i] == "--build" || argv[i] == "--install") return {};   // not a configure
        if (argv[i] == "-S" || argv[i] == "--source") {
            if (i + 1 < argv.size()) return resolve(argv[i + 1]);
        }
        if (argv[i].rfind("-S", 0) == 0 && argv[i].size() > 2) return resolve(argv[i].substr(2));
    }
    // `cmake .` and `cmake ..` - the first argument that is not an option.
    for (std::size_t i = 1; i < argv.size(); ++i) {
        if (argv[i].rfind("-", 0) != 0) return resolve(argv[i]);
    }
    return working_directory;
}

// The C and C++ sources sitting directly in a directory, so a "there is no
// CMakeLists.txt here" message can name what a CMakeLists.txt would build.
std::vector<std::string> sources_in(const std::filesystem::path& directory) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto extension = strings::to_lower(entry.path().extension().string());
        if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c") {
            found.push_back(entry.path().filename().string());
        }
        if (found.size() >= 12) break;
    }
    std::sort(found.begin(), found.end());
    return found;
}

// Scripts that set up an environment and then exit, ignoring anything else on
// the command line. Running one and reading exit 0 as success is a trap.
bool is_environment_script_misuse(const std::vector<std::string>& argv) {
    if (argv.size() < 2) return false;
    const auto stem = strings::to_lower(std::filesystem::path{argv[0]}.stem().string());
    return stem == "vcvars64" || stem == "vcvars32" || stem == "vcvarsall" || stem == "vsdevcmd";
}

// The parameters every command-running tool shares: where to run, how long to
// wait, what to feed it, and what its output has to be. Defined once so
// run_command and run_artifact cannot drift apart - a behaviour check that
// worked on one and not the other would be a trap.
Json command_schema_common() {
    return schema::object(
        {{"working_directory",
          schema::string_param("Directory to run in, relative to the workspace root. "
                               "Defaults to the workspace root.")},
         {"environment",
          Json{{"type", "object"},
               {"description",
                "Extra environment variables for this command only, as name/value pairs. "
                "The MSVC developer environment is supplied automatically when it is "
                "needed, so it does not belong here."}}},
         {"timeout_seconds",
          schema::integer_param("Kill the command after this long. Defaults to 300.", 1, 3600)},
         {"stdin", schema::string_param("Text to feed to the command's standard input.")},
         {"expect",
          Json{{"type", "object"},
               {"description",
                "What the output has to be for the task to be satisfied. Pass this whenever "
                "you run the program to check what it does. LocalCode performs the "
                "comparison itself against the real output and records the result; without "
                "it, running the program only proves that it ran, not that it is right. "
                "Example: {\"description\": \"prints the first ten Fibonacci numbers\", "
                "\"stdout_equals\": \"0 1 1 2 3 5 8 13 21 34\"}"},
               {"properties",
                Json{{"description",
                      schema::string_param("What this comparison proves, in the terms of the "
                                           "task. Required when 'expect' is given.")},
                     {"exit_code", schema::integer_param("The exit code the command must "
                                                         "produce.")},
                     {"stdout_contains",
                      schema::array_of_strings("Text that must appear in the output.")},
                     {"stdout_absent",
                      schema::array_of_strings("Text that must not appear in the output.")},
                     {"stdout_equals",
                      schema::string_param("The complete expected output. Compared after "
                                           "collapsing runs of whitespace, so line breaks "
                                           "and padding do not decide the result.")},
                     {"stdout_matches",
                      schema::string_param("A regular expression the output must match.")},
                     {"criterion",
                      Json{{"description",
                            "Acceptance number, or an array such as [1,2] when the same "
                            "actual comparison proves each named criterion. Numbers start "
                            "at 1. Name only criteria this evidence actually establishes."},
                           {"oneOf",
                            Json::array({schema::integer_param("One criterion number.", 1, 32),
                                         Json{{"type", "array"},
                                              {"minItems", 1},
                                              {"maxItems", 32},
                                              {"items", schema::integer_param("Criterion number.",
                                                                              1, 32)}}})}}},
                     {"check_stderr",
                      schema::boolean_param("Check stderr instead of stdout.", false)}}}}}});
}

// --- run_command -----------------------------------------------------------
class RunCommandTool final : public ITool {
public:
    std::string name() const override { return "run_command"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        // The first sentence survives even the smallest advertised schema.
        // A generic "shell" leaves Windows models guessing Bash syntax.
        return
#if defined(_WIN32)
            "Runs commands; shell syntax is Windows cmd.exe. "
#else
            "Runs commands; shell syntax is POSIX sh. "
#endif
            "Direct executable/arguments bypass the shell. The working directory defaults "
            "to the workspace. Returns exit code, timing and output. Use this to configure and "
            "build the "
            "project, run its tests, run the compiler, and run the program itself. Do this "
            "rather than telling the user to run something by hand - if the command can be "
            "run here, run it. The executable does not have to live inside the workspace: "
            "system tools such as cmake, git and compilers are found on PATH. Output is "
            "shortened for you, with compiler and test diagnostics extracted; the complete log "
            "is written to a file whose path is returned. On Windows, remaining background "
            "descendants are terminated when the main command exits; this tool cannot leave "
            "a detached service running.";
    }

    Json parameters_schema() const override {
        // The shared parameters, plus the ways of naming a command.
        auto schema = command_schema_common();
        auto forms = schema::object(
            {{"executable",
              schema::string_param(
                  "The program to run, and nothing else: \"cmake\", \"ctest\", or a path to a "
                  "built program such as \"build/Debug/app.exe\". This is the form to use.\n\n"
                  "One path goes in one string. If the program is at "
                  "build/Debug/app.exe, that whole thing is the executable - do not split it "
                  "across several arguments.\n\n"
                  "Write paths with forward slashes. Windows accepts them everywhere, and a "
                  "backslash inside a JSON string is an escape: \\f becomes a form feed and \\U "
                  "is not valid at all, so the path arrives damaged and the program looks "
                  "missing when it is not.")},
             {"arguments", Json{{"type", "array"},
                                {"description",
                                 "The arguments to the program, each as its own element: "
                                 "[\"--build\", \"build\", \"--config\", \"Debug\"]. Nothing is "
                                 "quoted or parsed, so an argument containing spaces is simply "
                                 "one element. Omit for a program that takes none."},
                                {"items", Json{{"type", "string"}}}}},
             {"shell_command",
              schema::string_param(
#if defined(_WIN32)
                  "Uses Windows cmd.exe syntax (/d /s /c). "
#else
                  "Uses POSIX sh syntax (-c). "
#endif
                  "For a line that genuinely needs a shell: a pipe, a redirect, && or ||, or "
                  "variable expansion. Nothing else - a plain command sent this way is run "
                  "directly anyway, and every layer that reparses a Windows path is another "
                  "chance to lose a backslash.\n\n"
                  "Never send this alongside 'executable' or 'command' describing something "
                  "different: the call is refused, not guessed at.")}});
        for (auto entry = forms["properties"].begin(); entry != forms["properties"].end();
             ++entry) {
            schema["properties"][entry.key()] = entry.value();
        }
        return schema;
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        // One reading of the request, or none. Nothing below this point has to
        // decide between competing representations of the same command,
        // because a request that carries two of them never gets here.
        const auto shape = parse_command_request(arguments);
        if (!shape.valid) {
            auto refusal = ToolResult::failure(shape.error_summary, shape.error_detail);
            refusal.evidence.push_back(make_evidence(EvidenceKind::CommandRun,
                                                     strings::visible(shape.error_summary),
                                                     "invalid command shape", false));
            return refusal;
        }
        auto argv = shape.argv();
        const auto shell_command = shape.use_shell ? shape.shell_text : std::string{};

        // Parsed before anything runs: a mistyped expectation should be
        // reported rather than discovered after the program has already
        // executed and the moment to check it has passed.
        OutputExpectation expectation;
        if (arguments.is_object() && arguments.contains("expect")) {
            auto parsed = OutputExpectation::from_json(arguments.at("expect"));
            if (!parsed) {
                return ToolResult::failure(parsed.error().message, parsed.error().to_string());
            }
            expectation = *parsed;
        }

        // A check whose description is a criterion's wording, word for word,
        // and which names no number. Quoting the criterion as what this check
        // proves is as explicit as giving its position - the caller did not
        // resemble the criterion, it reproduced it - so the number is filled
        // in and the result says so.
        //
        // This is the same exact-string observation as the mismatch below,
        // read the other way. It is not sentence similarity: prose that merely
        // sounds like a criterion is left alone, because deciding that would
        // be the inference this subsystem exists to remove.
        //
        // Measured across four runs: a complete, correct expectation - the
        // right output, the right description, a passing comparison - settling
        // nothing, three or four times in a row, until the run was stopped.
        std::string adopted_criterion;
        if (expectation.criterion == 0 && expectation.criterion_text.empty() &&
            !expectation.description.empty() && !expectation.description_derived &&
            context.acceptance_criteria) {
            const auto wanted = strings::to_lower(normalise_whitespace(expectation.description));
            int matches = 0;
            int found = 0;
            for (const auto& entry : context.acceptance_criteria()) {
                if (strings::to_lower(normalise_whitespace(entry.description)) != wanted) continue;
                ++matches;
                found = entry.number;
            }
            // Exactly one, or it is ambiguous and nothing is assumed.
            if (matches == 1) {
                expectation.criterion = found;
                expectation.criteria = {found};
                adopted_criterion =
                    "note:     this check names acceptance criterion " + std::to_string(found) +
                    " word for word, so it was read as settling that one. Give 'criterion' "
                    "explicitly to be sure of which.\n";
            }
        }

        // The description is one criterion's wording and the number is another
        // criterion's. Both are exact strings, so noticing that they point at
        // different criteria is an observation, not a similarity judgement -
        // and it is not acted on, because which one was meant is the caller's
        // to say.
        //
        // Measured: a run settled criterion 1 five times while passing
        // criterion 2's exact text as the description, and criterion 2 stayed
        // open until the run was stopped.
        if (expectation.criterion > 0 && !expectation.description.empty() &&
            context.acceptance_criteria) {
            const auto wanted = strings::to_lower(normalise_whitespace(expectation.description));
            for (const auto& entry : context.acceptance_criteria()) {
                if (entry.number == expectation.criterion) continue;
                if (strings::to_lower(normalise_whitespace(entry.description)) != wanted) continue;
                return ToolResult::failure(
                    "the description and the criterion number disagree",
                    "The description in this expectation is criterion " +
                        std::to_string(entry.number) + "'s wording, word for word:\n  " +
                        entry.description + "\n\nBut 'criterion' says " +
                        std::to_string(expectation.criterion) +
                        ". One of the two is wrong and LocalCode will not pick.\n\n"
                        "If this check settles criterion " +
                        std::to_string(entry.number) +
                        ", send \"criterion\": " + std::to_string(entry.number) +
                        ". If it settles criterion " + std::to_string(expectation.criterion) +
                        ", write that criterion's own wording as the description.");
            }
        }

        // A criterion named by its wording rather than its number. Quoting a
        // criterion back identifies it exactly, so this is a lookup - but only
        // an exact one. Anything looser would be matching a check to a
        // criterion by how similar the sentences read, which is the inference
        // this subsystem exists to remove.
        //
        // Measured: a run sent "criterion": "Program runs and outputs the
        // correct sequence: ..." and the number silently became zero. The
        // check ran, passed, settled nothing, and said nothing about why.
        if (!expectation.criterion_text.empty() && context.acceptance_criteria) {
            const auto plan = context.acceptance_criteria();
            const auto wanted = strings::to_lower(normalise_whitespace(expectation.criterion_text));
            for (const auto& entry : plan) {
                if (strings::to_lower(normalise_whitespace(entry.description)) != wanted) continue;
                expectation.criterion = entry.number;
                expectation.criteria = {entry.number};
                break;
            }
            if (expectation.criterion == 0) {
                std::string message =
                    "'criterion' says which acceptance criterion a check settles, by its number "
                    "in the plan. \"" +
                    strings::utf8_truncate(expectation.criterion_text, 120) +
                    "\" does not match any of them exactly.\n";
                if (plan.empty()) {
                    message +=
                        "\nNo acceptance criteria have been set in this run, so there is nothing "
                        "for a check to settle. Call set_acceptance_criteria first.";
                } else {
                    message += "\nThe criteria are:\n";
                    for (const auto& entry : plan) {
                        message += "  " + std::to_string(entry.number) + ". " + entry.description +
                                   (entry.settled ? "   (already settled)\n" : "\n");
                    }
                    message +=
                        "\nUse the number of the criterion this check actually proves; "
                        "do not default to the first one.\n";
                }
                return ToolResult::failure("'criterion' must be a number", message);
            }
        }

        auto& workspace = *context.workspace;
        const auto& policy =
            context.task_policy ? context.task_policy->commands() : workspace.commands();

        // --- Working directory -------------------------------------------
        std::filesystem::path working_directory = paths(context).workspace_root();
        const auto& requested_directory = shape.working_directory;
        if (!requested_directory.empty()) {
            auto resolved = resolve(context, requested_directory, PathAccess::Create);
            if (!resolved) {
                return ToolResult::failure("bad working directory", resolved.error().message);
            }
            std::error_code ec;
            if (!std::filesystem::is_directory(*resolved, ec)) {
                return ToolResult::failure("working directory does not exist",
                                           requested_directory +
                                               " is not a directory. Create it first if that is "
                                               "intended.");
            }
            working_directory = *resolved;
        }

        // --- Build the request -------------------------------------------
        ProcessRequest request;
        CommandEvaluation evaluation;
        std::string display;
        bool msvc_environment_applied = false;

        // Structured unless the line genuinely needs a shell. `argv` is
        // populated either way - the policy inspects tokens in both cases - so
        // the decision is the parsed form, not whether argv happens to be
        // filled in.
        // The virtual /workspace prefix applies to every token of a command,
        // not only to the program. A model that writes /workspace/app.exe also
        // writes `cmake --build /workspace/build`, and rewriting the first
        // while passing the second through unchanged is worse than not
        // rewriting at all: the same prefix then works in one position and
        // fails in another, for no reason the caller can see. Measured -
        // `--build /workspace/build` failed twice before the model gave up and
        // wrote the absolute path by hand.
        //
        // Only the one literal prefix, and only into this workspace; anything
        // else is left exactly as written.
        std::vector<std::string> rewritten_arguments;
        for (auto& token : argv) {
            const auto decision = paths(context).evaluate(token, PathAccess::Read);
            if (!decision.allowed || decision.reinterpreted_from.empty()) continue;
            rewritten_arguments.push_back(token);
            token = decision.canonical.generic_string();
        }

        if (!shape.use_shell) {
            // resolve_executable, not find_executable: a build tool that ships
            // inside Visual Studio is on nobody's PATH and must still be runnable.
            auto executable = resolve_executable(argv[0], working_directory);
            if (!executable) {
                // A missing tool is a normal situation worth answering usefully
                // rather than just failing.
                // A shell builtin is not an executable and never will be, so
                // "not found" sends the model looking for something that does
                // not exist. Observed: a 3B model spent three turns retrying
                // `dir` before giving up on the directory listing entirely.
                static const std::vector<std::pair<const char*, const char*>> kBuiltins = {
                    {"dir", "list_directory"},
                    {"ls", "list_directory"},
                    {"cd", "the working_directory parameter of this tool"},
                    {"type", "read_file"},
                    {"cat", "read_file"},
                    {"echo", "write_file, if you meant to create a file"},
                    {"copy", "copy_file"},
                    {"move", "move_file"},
                    {"del", "delete_path"},
                    {"rm", "delete_path (recursive=true for a directory)"},
                    {"rmdir", "delete_path (recursive=true for a directory)"},
                    {"rd", "delete_path (recursive=true for a directory)"},
                    {"mkdir", "create_directory"},
                    {"md", "create_directory"},
                    {"findstr", "search_text"},
                    {"grep", "search_text"},
                    {"set", "environment_info"},
                    {"cls", "nothing - there is no terminal to clear"},
                };
                const auto lowered = strings::to_lower(argv[0]);
                for (const auto& [builtin, alternative] : kBuiltins) {
                    if (lowered != builtin) continue;
                    auto hint = ToolResult::failure(
                        std::string{"'"} + builtin + "' is a shell builtin, not a program",
                        std::string{"Commands here are run directly rather than through a "
                                    "shell, so builtins like '"} +
                            builtin + "' do not exist as executables. Use " + alternative +
                            ". If you genuinely need the shell, use the shell_command "
                            "parameter instead of command.");
                    hint.evidence.push_back(
                        make_evidence(EvidenceKind::CommandRun, argv[0], "shell builtin", false));
                    return hint;
                }

                // A Windows path written into a JSON string without doubling
                // its backslashes comes out mangled, and the wreckage looks
                // like a filename. Measured: `build\RelWithDebInfo\fib.exe`
                // arrived as "RelWithDebInfoib.exe" - \f had been read as a
                // form feed - and LocalCode said the executable was not found,
                // which sent the model hunting for a file that was exactly
                // where it expected.
                // An artifact id, sent to the wrong tool. The build report
                // offers `run_artifact { "artifact": "build:fibonacci" }` and
                // a caller can take the id from it while keeping the tool it
                // was already using. The id is not a path, and diagnosing it
                // as a damaged one - the colon looks like an illegal filename
                // character - is a confidently wrong answer.
                //
                // Named rather than executed: run_command's `executable` means
                // a program path and nothing else, and giving it a second
                // namespace would be the ambiguity this contract removed.
                // Matched on the id or the name only. Resource lookup also
                // accepts a recorded path, which is right for run_artifact and
                // very wrong here: it made LocalCode tell a caller that
                // "build/RelWithDebInfo/app.exe" was an id and not a path,
                // five times, about a path. The message is for something that
                // is not a path at all, like "build:fibonacci".
                const auto named_resource = [&]() -> std::optional<ResolvedResource> {
                    for (const auto& entry : workspace.resolved_resources()) {
                        if (entry.id == argv[0] || strings::iequals(entry.name, argv[0])) {
                            return entry;
                        }
                    }
                    return std::nullopt;
                }();
                if (const auto& resource = named_resource; resource) {
                    auto redirect = ToolResult::failure(
                        "that is an artifact id, not a path: " + argv[0],
                        "\"" + resource->id +
                            "\" is the name of a program this workspace "
                            "built. run_command takes a path; run_artifact takes the name:\n\n"
                            "  run_artifact { \"artifact\": \"" +
                            resource->id +
                            "\" }\n\n"
                            "That is the whole call - LocalCode holds the path. If you would "
                            "rather use run_command, the path is " +
                            resource->path + ".");
                    redirect.evidence.push_back(make_evidence(
                        EvidenceKind::CommandRun, argv[0], "artifact id given as a path", false));
                    return redirect;
                }

                if (const auto damage = escape_damage(argv[0]); !damage.empty()) {
                    // The path first, then the diagnosis. This branch fires at
                    // the one moment LocalCode has *proof* the model cannot
                    // spell the path it wants - so withholding the path it
                    // already knows is the worst possible time to be tidy.
                    // Measured in run 9: the model built fibonacci.exe, was
                    // told the path was damaged twice, and never learned what
                    // the path was, because this branch returned before
                    // reaching the produced-binaries block below.
                    std::string advice;
                    if (const auto produced = workspace.produced_binaries(); !produced.empty()) {
                        advice = "The program you built is here:";
                        for (const auto& binary : produced)
                            advice += "\n  " + binary;
                        advice += "\nSend that exactly as written, forward slashes and all.\n\n";
                    }
                    const bool executable_pattern = damage == "a '*'" || damage == "a '?'";
                    if (executable_pattern) {
                        advice +=
                            "The executable path contains a wildcard. run_command needs one "
                            "exact executable path and does not expand executable wildcards. "
                            "Use list_directory to find the actual path, then send it as "
                            "executable with a separate arguments array. Patterns in arguments "
                            "are passed to the selected program, which may support them.";
                    } else {
                        advice +=
                            "The path you sent contains " + damage +
                            ", which cannot appear in a filename. A backslash in your JSON "
                            "arguments was read as an escape: \"build\\RelWithDebInfo\\fib.exe\" "
                            "becomes \"build\" + RelWithDebInfo + form-feed + \"ib.exe\".\n\n"
                            "Write Windows paths with forward slashes - build/RelWithDebInfo/"
                            "fib.exe works everywhere and needs no escaping - or double every "
                            "backslash. The file itself is almost certainly fine.";
                    }
                    auto mangled = ToolResult::failure(
                        (executable_pattern ? "executable wildcard needs an exact path: "
                                            : "the path arrived damaged: ") +
                            strings::visible(argv[0]),
                        std::move(advice));
                    mangled.evidence.push_back(
                        make_evidence(EvidenceKind::CommandRun, strings::visible(argv[0]),
                                      executable_pattern ? "executable wildcard was not expanded"
                                                         : "path damaged by JSON escaping",
                                      false));
                    return mangled;
                }

                // A path that has been shattered into argv elements. Measured:
                // ["build", "RelWithDebInfo", "fibonacci.exe"] for what should
                // have been build/RelWithDebInfo/fibonacci.exe, reported as a
                // missing program called "build".
                //
                // The pieces are NOT silently rejoined and run. Deciding that
                // three arguments were meant to be one path is a guess, and a
                // wrong guess here executes something nobody asked for. The
                // reading is shown and the caller sends it again.
                if (argv.size() > 1) {
                    const auto joined = strings::join(argv, "/");
                    std::error_code ec;
                    const auto candidate = working_directory / joined;
                    if (std::filesystem::is_regular_file(candidate, ec)) {
                        auto path_error = ToolResult::failure(
                            "invalid command shape: a path split across arguments",
                            "There is no program called '" + argv[0] +
                                "'. Joined with slashes these arguments name a file that does "
                                "exist:\n\n  " +
                                joined +
                                "\n\nThat is one path, so it belongs in one string. Run it as:\n"
                                "  {\"executable\": \"" +
                                joined +
                                "\", \"arguments\": []}\n\n"
                                "LocalCode will not join them for you: deciding that three "
                                "arguments were meant to be one path is a guess, and running "
                                "the wrong guess is worse than saying so.");
                        path_error.evidence.push_back(make_evidence(
                            EvidenceKind::CommandRun, strings::visible(strings::join(argv, " ")),
                            "path split across argv elements", false));
                        return path_error;
                    }
                }

                std::string message;

                // An absolute path to a program, assembled rather than
                // observed. Measured: a run spent its last turns on
                // "...\VC\Tools\Microsoft MSVC\14.44.35207\..." - the real
                // directory is "MSVC" - and LocalCode, which had resolved
                // cl.exe minutes earlier, answered "not found" and let it keep
                // guessing at the spelling.
                //
                // The program is named by its filename, so if that filename is
                // one LocalCode can locate, the real location is the answer.
                // Not a rewrite: the path is not corrected and nothing is run.
                const auto wanted_leaf = std::filesystem::path{argv[0]}.filename().string();
                if (!wanted_leaf.empty() && wanted_leaf != argv[0]) {
                    if (const auto real = resolve_executable(wanted_leaf, working_directory)) {
                        message += "There is no " + argv[0] + " on this machine. " + wanted_leaf +
                                   " is at:\n  " + real->generic_string() +
                                   "\nUse that path, or just \"" +
                                   std::filesystem::path{wanted_leaf}.stem().string() +
                                   "\" - LocalCode resolves it, including copies that are not "
                                   "on PATH.\n\n";
                    }
                }

                // The path a build recorded, with nothing there now. Telling
                // the caller to "use the path exactly as written" is what the
                // block below would say, and they did: the spelling is right
                // and the file is gone.
                if (const auto recorded = workspace.resolved_resource(argv[0]);
                    recorded && recorded->path == argv[0]) {
                    auto missing = ToolResult::failure(
                        "the build recorded this, but it is not there now: " + argv[0],
                        "The " + recorded->source + " named " + recorded->path +
                            ", so the path is right - there is simply no file there any more. "
                            "Something removed it, or the build was into a different "
                            "configuration directory.\n\nBuild again, then run it by name:\n"
                            "  run_artifact { \"artifact\": \"" +
                            recorded->id + "\" }");
                    missing.evidence.push_back(make_evidence(EvidenceKind::CommandRun, argv[0],
                                                             "recorded artifact is gone", false));
                    return missing;
                }

                message += "'" + argv[0] + "' was not found on PATH or in the workspace.";

                // The thing LocalCode already knows. A model that has just
                // built something and cannot name the result does not need a
                // list of installed development tools; it needs the path the
                // build printed a moment ago.
                if (const auto produced = workspace.produced_binaries(); !produced.empty()) {
                    message += "\nThe last build in this workspace produced:";
                    for (const auto& binary : produced)
                        message += "\n  " + binary;
                    message +=
                        "\nIf you meant one of those, use the path exactly as written - "
                        "forward slashes and all.";
                }
                if (context.environment) {
                    const auto& report = context.environment->report();
                    std::vector<std::string> available;
                    for (const auto& tool : report.tools) {
                        if (tool.found) available.push_back(tool.id);
                    }
                    if (!available.empty()) {
                        message += "\nDevelopment tools that are available here: " +
                                   strings::join(available, ", ") +
                                   ".\nUse environment_info for their exact paths and versions.";
                    }
                }
                auto result = ToolResult::failure("executable not found: " + argv[0], message);
                result.evidence.push_back(
                    make_evidence(EvidenceKind::CommandRun, argv[0], "not found", false));
                return result;
            }
            request.executable = *executable;
            request.arguments.assign(argv.begin() + 1, argv.end());

            // The MSVC compiler needs INCLUDE, LIB and LIBPATH, which come from
            // a batch file that sets them in a shell and exits. A model cannot
            // carry that across two separate commands, and one spent four turns
            // trying - appending the build to vcvars64.bat, which ignored it.
            // Supplying the variables here means `cl main.cpp` simply works.
            //
            // The same applies one level up. `cmake --build build` on a
            // Ninja tree starts cl.exe, which fails on every standard header
            // with "no include path set" - and the message says nothing about
            // the environment, so a model reads it as a missing #include and
            // spends its run adding <vector> to files that already have it.
            // Measured: eight steps, four edits, none of them the problem.
            //
            // What makes this safe rather than blind is that the build tree's
            // own cache is read first. cl.exe is already the chosen compiler,
            // written down at configure time; supplying its variables cannot
            // change that choice, only let it work. A Visual Studio generator
            // arranges its own environment and is left alone.
            //
            // And when the command line itself says `-DCMAKE_CXX_COMPILER=
            // ...\cl.exe`, the choice is the model's, stated outright. Giving
            // that compiler its environment cannot change a decision written
            // on the command line - and without it the configure gets as far
            // as linking and dies at "RC Pass 1 ... no such file or
            // directory", because rc.exe and mt.exe live in the Windows SDK
            // and are not on PATH either. A run followed exactly this advice
            // and failed for exactly that reason.
            const bool needs_environment =
                needs_msvc_environment(request.executable) ||
                build_tree_drives_msvc_directly(build_tree_of(argv, working_directory)) ||
                build_tree_drives_msvc_directly(configure_binary_dir(argv, working_directory)) ||
                command_selects_msvc_compiler(argv);
            if (needs_environment) {
                const auto& developer = msvc_environment();
                if (!developer.empty()) {
                    for (const auto& [name, value] : developer) {
                        request.environment.push_back({name, value});
                    }
                    msvc_environment_applied = true;
                }
            }

            for (const auto& [name, value] : shape.environment) {
                request.environment.push_back({name, value});
            }

            evaluation = is_windows_cmd_script(request)
                             ? policy.evaluate_shell(request.arguments.back())
                             : policy.evaluate(request);
            display = ProcessRunner::display_command(request);
        } else {
            evaluation = policy.evaluate_shell(shell_command);
            // The shell itself is resolved through PATH like anything else.
#if defined(_WIN32)
            auto shell = find_executable("cmd");
            if (!shell) return ToolResult::failure("no shell", "cmd.exe was not found.");
            request.executable = *shell;
            request.arguments = {"/d", "/s", "/c", shell_command};
#else
            auto shell = find_executable("sh");
            if (!shell) return ToolResult::failure("no shell", "sh was not found.");
            request.executable = *shell;
            request.arguments = {"-c", shell_command};
#endif
            display = shell_command;
            argv = CommandPolicy::tokenize(shell_command);

            // The same reasoning as above, for the same command written the
            // other way. `shell_command: "cmake --build build"` starts the same
            // cl.exe and fails on the same headers; leaving the hole open here
            // would mean the fix worked or not depending on which parameter the
            // model happened to choose.
            if (build_tree_drives_msvc_directly(build_tree_of(argv, working_directory)) ||
                build_tree_drives_msvc_directly(configure_binary_dir(argv, working_directory))) {
                for (const auto& [name, value] : msvc_environment()) {
                    request.environment.push_back({name, value});
                    msvc_environment_applied = true;
                }
            }
        }

        // --- Policy -------------------------------------------------------
        if (evaluation.decision == CommandDecision::Block) {
            log_warn(LogChannel::Security, "command blocked",
                     {{"command", display}, {"rule", evaluation.rule_id}});
            auto result = ToolResult::failure(
                "blocked: " + display,
                "This command is blocked by LocalCode's security policy and will not be run.\n"
                "Reason: " +
                    evaluation.reason + " (rule: " + evaluation.rule_id +
                    ")\n"
                    "This is not something you can override. Find another way to accomplish the "
                    "task, "
                    "or explain to the user what they would need to do themselves.");
            result.evidence.push_back(
                make_evidence(EvidenceKind::CommandRun, display, "blocked by policy", false));
            return result;
        }

        if (evaluation.decision == CommandDecision::Confirm) {
            ConfirmationRequest confirmation;
            confirmation.title = "Run this command?";
            confirmation.command = display;
            confirmation.detail = "Working directory: " + working_directory.string();
            confirmation.reason = evaluation.reason;
            confirmation.rule_id = evaluation.rule_id;

            if (!context.ask(confirmation)) {
                log_info(LogChannel::Security, "command refused by user",
                         {{"command", display}, {"rule", evaluation.rule_id}});
                auto result = ToolResult::failure(
                    "not approved: " + display,
                    "The user did not approve this command.\nReason it needed approval: " +
                        evaluation.reason +
                        "\nContinue without it, or choose a different approach.");
                result.evidence.push_back(
                    make_evidence(EvidenceKind::CommandRun, display, "refused by user", false));
                return result;
            }
        }

        // --- Run ----------------------------------------------------------
        request.working_directory = working_directory;
        request.timeout = std::chrono::seconds(arg_int(arguments, "timeout_seconds", 300));
        request.stdin_data = arg_string(arguments, "stdin");
        request.max_captured_bytes = 512 * 1024;
        request.on_output = context.on_output;

        const auto log_directory =
            context.log_directory.empty() ? app_paths().command_log_dir() : context.log_directory;
        request.full_log_path = log_directory / ("cmd-" + std::to_string(now_unix_ms()) + ".log");

        // --- Is this configure about to fail for a reason already known? ---
        //
        // A build tree remembers its generator. Asking it for a different one
        // fails with "Does not match the generator used previously", which
        // says nothing about how it came to hold the first. Usually the answer
        // is that an earlier configure failed - a failed configure still
        // writes CMakeCache.txt - so the error the caller is reading is a
        // consequence of an error it already saw and moved past.
        //
        // Refused rather than run, because running it produces a failure with
        // a worse explanation than this one. Nothing is deleted: what to do
        // about the tree is the caller's decision, and both safe options are
        // named.
        if (const auto binary_dir = configure_binary_dir(argv, working_directory);
            !binary_dir.empty()) {
            const auto wanted = requested_generator(argv);
            const auto state = inspect_build_directory(binary_dir);

            // A generator whose build program is not here cannot succeed, and
            // failing is not the harmless part: a FAILED configure still
            // writes CMakeCache.txt, so it also makes the next configure fail
            // with an unrelated-looking generator mismatch. Three runs went
            // that way, and one spent eight steps alternating between the two
            // errors.
            //
            // LocalCode already resolves executables, so whether ninja exists
            // is a fact it holds before the command starts. Checking it here
            // costs nothing and leaves the tree untouched.
            // find_executable, not resolve_executable: the question is whether
            // CMake will find it, and CMake searches PATH. A ninja.exe sitting
            // inside Visual Studio is real and is still invisible to the
            // generator, which is exactly why this failure is confusing.
            const auto needed = generator_requires_program(wanted);
            const std::filesystem::path cached_program{state.make_program};
            std::error_code cached_error;
            const bool cached_program_available =
                state.configured && same_generator(state.generator, wanted) &&
                cached_program.is_absolute() &&
                strings::iequals(cached_program.stem().string(), needed) &&
                std::filesystem::is_regular_file(cached_program, cached_error) && !cached_error;
            if (!needed.empty() && !find_executable(needed) && !cached_program_available) {
                std::string message =
                    "The " + wanted + " generator needs " + needed +
                    " on PATH and it is not there, so this configure cannot succeed.\n\n"
                    "Refused rather than run, because a configure that fails still writes a "
                    "cache: the attempt would leave " +
                    workspace.to_relative(binary_dir) + " remembering " + wanted +
                    ", and every later configure with a different generator would then fail "
                    "with \"does not match the generator used previously\" - an error about "
                    "this one, which by then is off the screen.\n\n"
                    "Drop the -G and let CMake choose. On this machine that selects the Visual "
                    "Studio generator, which locates the compiler, the linker, rc.exe and "
                    "mt.exe by itself and needs nothing on PATH:\n"
                    "  cmake -S . -B " +
                    workspace.to_relative(binary_dir) + "\n";
                if (const auto ninja = resolve_executable(needed); ninja) {
                    message += "\n(" + needed + " does exist at " + ninja->string() +
                               ", but it is not on PATH, and the link step also needs rc.exe "
                               "and mt.exe from the Windows SDK which are not either. Naming "
                               "the compiler is not enough.)\n";
                }
                auto refusal = ToolResult::failure(
                    "generator unavailable: " + needed + " is not on PATH", message);
                // CommandRun, not BuildAttempted: nothing was built, because
                // nothing ran. Recording a failed build for a command that was
                // refused before it started leaves the task permanently
                // "failing" - measured on a run whose every criterion was
                // settled and whose work was correct.
                refusal.evidence.push_back(make_evidence(EvidenceKind::CommandRun, wanted,
                                                         needed + " not on PATH", false));
                return refusal;
            }
            if (!wanted.empty() && state.configured && !state.generator.empty() &&
                !same_generator(state.generator, wanted)) {
                const auto where = workspace.to_relative(binary_dir);

                std::string message =
                    where + " was configured with " + state.generator +
                    ", and this command asks "
                    "for " +
                    wanted +
                    ". A build tree remembers its generator, so this configure would fail no "
                    "matter what else is on the command line - the cache decides, not the "
                    "arguments.\n\n";

                if (state.generated_only) {
                    message += "Everything in " + where +
                               " was generated by CMake, so there are two "
                               "safe ways forward:\n"
                               "  1. Use a different directory and keep both:\n"
                               "       -B " +
                               where + "-" + strings::to_lower(wanted.substr(0, wanted.find(' '))) +
                               "\n"
                               "  2. Remove the tree and configure again:\n"
                               "       delete_path { \"path\": \"" +
                               where + "\", \"recursive\": true }\n";
                } else {
                    message += "This directory also holds files that CMake did not generate (" +
                               strings::join(state.unrecognised, ", ") +
                               "), so it must not simply be deleted. Configure into a different "
                               "directory instead:\n"
                               "       -B " +
                               where + "-" + strings::to_lower(wanted.substr(0, wanted.find(' '))) +
                               "\n";
                }
                message +=
                    "\nBefore choosing: the generator already there works. On this machine the "
                    "Visual Studio generator finds the whole toolchain by itself, while Ninja "
                    "needs cl.exe, rc.exe and mt.exe on PATH and they are not. If " +
                    state.generator + " is that generator, keep it and drop the -G.";

                auto conflict = ToolResult::failure(
                    "generator conflict: " + where + " holds " + state.generator, message);
                // Refused before running, so no build was attempted.
                conflict.evidence.push_back(make_evidence(
                    EvidenceKind::CommandRun, where,
                    "configured with " + state.generator + ", asked for " + wanted, false));
                return conflict;
            }
        }

        if (context.on_progress) context.on_progress("running: " + display);

        auto run_result = ProcessRunner::run(request, context.cancel);
        if (!run_result) {
            auto result =
                ToolResult::failure("could not start: " + display, run_result.error().to_string());
            result.evidence.push_back(
                make_evidence(EvidenceKind::CommandRun, display, "failed to start", false));
            return result;
        }

        const auto& process = *run_result;
        const auto kind = classify_command(argv);
        const bool succeeded = process.success();

        // --- Compose a compact result -------------------------------------
        std::string report;
        if (is_environment_script_misuse(argv)) {
            // Exit 0 from this means the script ran, not that the rest of the
            // line did. Observed four times in one run, each time read as a
            // successful configure.
            report += "WARNING: " + std::filesystem::path{argv[0]}.filename().string() +
                      " sets up a shell environment and then exits. It ignores the other "
                      "arguments, so nothing after it ran and this exit code says nothing "
                      "about them.\nTo build with MSVC, either use a generator that does not "
                      "need the environment (cmake -G Ninja works once the compiler is on "
                      "PATH), or run the script and the build together in one shell with "
                      "shell_command, for example:\n"
                      "  \"" +
                      argv[0] + "\" && cmake --build build\n\n";
        }
        if (msvc_environment_applied) {
            report +=
                "note:     the MSVC developer environment was applied automatically, so "
                "INCLUDE, LIB and PATH are set for this command\n";
        }
        if (!shape.note.empty()) report += shape.note;
        if (!adopted_criterion.empty()) report += adopted_criterion;
        // Said, not done quietly. The command that ran is not the command that
        // was written, and the caller has to be able to see the difference.
        if (!rewritten_arguments.empty()) {
            report += "note:     " + strings::join(rewritten_arguments, ", ") +
                      " resolved to this workspace (" + workspace.root().generic_string() +
                      "). There is no /workspace on this machine.\n";
        }
        report += "command:  " + display + "\n";
        report += "cwd:      " + working_directory.string() + "\n";
        report += "exit:     " + std::to_string(process.exit_code);
        if (process.timed_out) report += "  (TIMED OUT)";
        if (process.cancelled) report += "  (CANCELLED)";
        report += "\n";
        report += "duration: " + strings::human_duration_ms(process.duration_ms) + "\n";

        const auto combined = process.stdout_text + "\n" + process.stderr_text;
        const auto diagnostics = parse_diagnostics(combined);

        if (!diagnostics.empty()) report += "\n" + diagnostics.summarise();

        std::vector<std::string> produced;
        if (succeeded) {
            const auto tree = build_tree_of(argv, working_directory);
            auto resources = resources_produced(combined, workspace.root(),
                                                tree.empty() ? working_directory : tree);
            for (const auto& resource : resources)
                produced.push_back(resource.path);
            // Remembered, so that a later command which cannot find the program
            // can be told where it is instead of being left to guess. A run
            // said "the path is build/RelWithDebInfo/fibonacci.exe" in prose
            // and then called the build command three more times.
            //
            // Recorded by id as well as by path, so run_artifact can start it
            // without the path being retyped at all.
            workspace.record_resolved_resources(resources);
            if (!produced.empty()) {
                report += "\nThe build produced:\n";
                for (const auto& resource : resources) {
                    report += "  " + resource.path + "   (run_artifact: \"" + resource.id + "\")\n";
                }
                report +=
                    "Run one of those paths exactly as written. They use forward slashes "
                    "deliberately: a Windows path with backslashes inside a JSON string loses "
                    "them to escapes, and two runs have died looking for a program that was "
                    "sitting where they left it.\n";
            }
        }

        // A build tree remembers its generator, so a failed `-G Ninja` leaves a
        // cache that makes every later configure fail with a mismatch. The
        // model diagnosed this correctly and then had nowhere to go: `rm` on a
        // drive-less path looked like a filesystem-root wipe and was blocked,
        // and there was no delete tool. It looped until the run was stopped.
        //
        // LocalCode knows which directory holds the cache. Saying so, and
        // naming the tool that can remove it, is the whole recovery.
        if (!succeeded && strings::icontains(combined, "does not match the generator used")) {
            const auto tree = build_tree_of(argv, working_directory);
            std::string where = "the build directory";
            if (!tree.empty()) {
                auto relative = tree.string();
                std::error_code relative_ec;
                const auto from_root =
                    std::filesystem::relative(tree, workspace.root(), relative_ec);
                if (!relative_ec && !from_root.empty()) relative = from_root.generic_string();
                where = relative;
            }
            report +=
                "\nnote:     a build tree remembers the generator it was configured with, and "
                "this one was configured with a different one. Nothing you pass to cmake will "
                "change that while the old cache is there.\n"
                "          Delete " +
                where +
                " and configure again:\n"
                "            delete_path { \"path\": \"" +
                where +
                "\", \"recursive\": true }\n"
                "          Deleting the whole directory is safer than deleting CMakeCache.txt "
                "alone, which leaves the rest of the tree describing a generator that is no "
                "longer in use.\n";
        }

        // "does not appear to contain CMakeLists.txt. Specify --help for usage"
        // reads as "you have used cmake wrongly", and the recovery it suggests
        // is the help text. Measured in run 10: the model had just written
        // fibonacci.cpp, ran configure, got this in 33ms, and concluded "this
        // is a standalone C++ project, not a CMake-based project - let me
        // compile it directly with g++". g++ is not installed here, so it then
        // spent ten steps hunting for a compiler and the run died.
        //
        // The project was one four-line file short of being a CMake project.
        // Saying so, and naming the sources that file would list, is the whole
        // recovery.
        if (!succeeded && strings::icontains(combined, "does not appear to contain CMakeLists")) {
            const auto source = configure_source_of(argv, working_directory);
            const auto sources = source.empty() ? std::vector<std::string>{} : sources_in(source);
            report +=
                "\nnote:     this does not mean CMake is the wrong tool or that the project is "
                "not a CMake project. It means the file that would make it one has not been "
                "written yet. Write CMakeLists.txt and configure again - do not switch to "
                "invoking a compiler directly, which needs a toolchain on PATH that is usually "
                "not there on Windows.\n";
            if (!sources.empty()) {
                report +=
                    "          The sources next to it are: " + strings::join(sources, ", ") + "\n";
                report +=
                    "          A complete CMakeLists.txt for that is four lines:\n"
                    "            cmake_minimum_required(VERSION 3.16)\n"
                    "            project(app CXX)\n"
                    "            add_executable(app " +
                    strings::join(sources, " ") + ")\n";
            } else {
                report +=
                    "          There are no C or C++ sources in that directory either, so check "
                    "that the path passed to -S is the one the code is actually in.\n";
            }
        }

        // A tool invoked with no arguments at all, which printed its usage.
        // Nothing was asked of it, so nothing happened - and a behaviour check
        // attached to it is measuring the help text.
        //
        // Measured: a model settled "Program builds successfully with CMake"
        // against `cmake` on its own, five times. Bare cmake prints usage and
        // does not build, so the check failed and the criterion was recorded
        // as NOT met while the real build had succeeded minutes earlier.
        // LocalCode said only "exit 1".
        const bool printed_usage_only =
            argv.size() == 1 && (strings::icontains(process.stdout_text, "usage") ||
                                 strings::icontains(process.stderr_text, "usage"));
        if (printed_usage_only) {
            const auto program = std::filesystem::path{argv[0]}.stem().string();
            report += "\nnote:     " + program +
                      " was run with no arguments, so it printed its usage and did no work. "
                      "This output says nothing about your project.\n";
            if (strings::iequals(program, "cmake")) {
                report +=
                    "          To configure:  cmake -S . -B build\n"
                    "          To build:      cmake --build build\n";
            }
            // With an expectation attached this is refused, not noted. Bare
            // cmake exits 0, so the summary line reads "OK cmake (exit 0)" and
            // a note in the body loses to it - three runs sent this same call
            // five times each, reading success at the top and never getting to
            // the paragraph explaining that nothing had happened.
            //
            // A refusal is short enough to be read whole, and its first line
            // says the thing that matters.
            if (expectation.present) {
                std::string detail =
                    program +
                    " with no arguments prints its usage and exits. Nothing was configured, "
                    "built or run, so there is nothing here for a check to be about - and a "
                    "check against a help message would settle a criterion on the strength of "
                    "documentation.\n\n";
                if (strings::iequals(program, "cmake")) {
                    detail +=
                        "Attach the check to the command that does the work:\n"
                        "  {\"executable\": \"cmake\", \"arguments\": [\"--build\", \"build\"],\n"
                        "   \"expect\": {\"description\": \"" +
                        expectation.description + "\", \"exit_code\": 0";
                    if (expectation.criterion > 0) {
                        detail += ", \"criterion\": " + std::to_string(expectation.criterion);
                    }
                    detail += "}}\n";
                } else {
                    detail += "Give " + program +
                              " the arguments that do the work, and attach the check to that.\n";
                }
                auto refusal = ToolResult::failure(
                    program + " with no arguments did nothing, so the check has no subject",
                    std::move(detail));
                refusal.evidence.push_back(
                    make_evidence(EvidenceKind::CommandRun, display, "usage message only", false));
                return refusal;
            }
        }

        // A configure that fails still writes CMakeCache.txt, and the cache
        // records the generator that failed. So the next configure with a
        // different generator fails too, with a completely different message
        // that never mentions this one. Measured: eight alternating attempts
        // in a single run, each error true and each beside the point.
        //
        // Said here, at the moment it becomes true, while the caller is still
        // looking at the failure that caused it.
        if (!succeeded) {
            if (const auto binary_dir = configure_binary_dir(argv, working_directory);
                !binary_dir.empty()) {
                const auto state = inspect_build_directory(binary_dir);
                if (state.configured) {
                    const auto where = workspace.to_relative(binary_dir);
                    report +=
                        "\nnote:     this configure failed but still wrote a cache into " + where +
                        ", and that cache now records " +
                        (state.generator.empty() ? std::string{"this generator"}
                                                 : state.generator) +
                        ".\n"
                        "          Configuring the same directory with a different generator "
                        "will now fail with \"does not match the generator used previously\" - a "
                        "different error, caused by this one. Either fix what failed here and "
                        "configure again with the same generator, or start from a clean "
                        "directory:\n"
                        "            delete_path { \"path\": \"" +
                        where + "\", \"recursive\": true }\n";
                }
            }
        }

        // A fresh `cmake -G Ninja` on Windows fails twice over, and neither
        // message mentions the reason: ninja and cl.exe both live inside Visual
        // Studio and neither is on PATH. LocalCode already knows exactly where
        // they are - it resolved both a few turns earlier in the run where this
        // was measured - so the paths go in the answer rather than the model
        // being left to rediscover them.
        //
        // Deliberately advice and not an injection. Configure is where the
        // toolchain gets chosen, and quietly supplying an environment there
        // would decide that choice on the model's behalf. Once the choice is
        // written into the cache, supplying it is safe; before then it is not.
        const bool configure_toolchain_missing =
            !succeeded &&
            (strings::icontains(combined, "unable to find a build program corresponding to") ||
             strings::icontains(combined, "CMAKE_CXX_COMPILER not set") ||
             strings::icontains(combined, "No CMAKE_CXX_COMPILER could be found") ||
             strings::icontains(combined, "No CMAKE_C_COMPILER could be found"));
        if (configure_toolchain_missing) {
            report +=
                "\nnote:     this is a PATH problem, not a missing installation. On Windows the "
                "compiler and ninja usually ship inside Visual Studio and neither is on PATH, "
                "so a bare `cmake -G Ninja` cannot find them.\n";
            const auto ninja = resolve_executable("ninja");
            const auto compiler = resolve_executable("cl");
            if (ninja) {
                report += "          ninja is at " + ninja->string() + "\n";
            }
            if (compiler) {
                report += "          cl.exe is at " + compiler->string() + "\n";
            }
            report +=
                "          Two ways out:\n"
                "            1. Drop -G Ninja. The Visual Studio generator finds the whole "
                "toolchain by itself and needs nothing on PATH. On this machine that is the "
                "one that works, and it is the first thing to try.\n"
                "            2. Keep Ninja, but give it the developer environment, not just "
                "paths. Naming the compiler is not enough: the link step also runs rc.exe and "
                "mt.exe from the Windows SDK, which are not on PATH either, and the configure "
                "fails at \"RC Pass 1 ... no such file or directory\" after appearing to get "
                "further. Run vcvars and the configure together with shell_command:\n"
                "                 \"...\\vcvars64.bat\" && cmake -S . -B build -G Ninja\n"
                "          LocalCode does not set that environment for you here, because "
                "configure is where the compiler is chosen and choosing it for you would be "
                "making the decision on your behalf. Once a build tree exists and its cache "
                "names cl.exe, later builds do get the environment automatically.\n";
        }

        // C1034 says "no include path set", which reads as a missing #include
        // and is not one. It means cl.exe ran without INCLUDE. Saying so here
        // costs one line and saves a run: a model that reads it literally goes
        // and adds <vector> to files that already include it.
        if (!succeeded && (strings::icontains(combined, "no include path set") ||
                           strings::icontains(combined, "C1034"))) {
            report +=
                "\nnote:     \"no include path set\" is not a missing #include. It means the "
                "MSVC compiler ran without INCLUDE, so it cannot find any standard header. "
                "Adding #include lines will not fix it - the environment is what is "
                "missing.\n";
            if (!msvc_environment_applied) {
                report +=
                    "          LocalCode supplies that environment for cl, link and for a "
                    "build tree whose CMake cache names cl.exe as the compiler. It did not "
                    "here, which means either Visual Studio was not found or this tree was "
                    "configured with a different toolchain than the one being run. Check "
                    "with environment_info, and reconfigure from scratch if the cache is "
                    "stale.\n";
            }
        }

        // Show the tail on failure - the last lines are where the summary is -
        // and the head on success, where configure output tends to matter.
        const auto stdout_excerpt = succeeded ? strings::excerpt(process.stdout_text, 40, 20, 8000)
                                              : strings::excerpt(process.stdout_text, 15, 60, 8000);
        const auto stderr_excerpt = strings::excerpt(process.stderr_text, 10, 50, 6000);

        if (!strings::trim(stdout_excerpt.text).empty()) {
            report += "\nstdout";
            if (stdout_excerpt.truncated) {
                report += " (" + std::to_string(stdout_excerpt.original_lines) +
                          " lines total, abridged)";
            }
            report += ":\n" + stdout_excerpt.text + "\n";
        }
        if (!strings::trim(stderr_excerpt.text).empty()) {
            report += "\nstderr";
            if (stderr_excerpt.truncated) {
                report += " (" + std::to_string(stderr_excerpt.original_lines) +
                          " lines total, abridged)";
            }
            report += ":\n" + stderr_excerpt.text + "\n";
        }
        if (process.stdout_total_bytes == 0 && process.stderr_total_bytes == 0) {
            report += "\n(the command produced no output)\n";
        }

        if (process.timed_out) {
            report += "\nThe command was killed after " +
                      strings::human_duration_ms(request.timeout.count()) +
                      ". Its whole process tree was terminated. If it needs longer, raise "
                      "timeout_seconds; if it is hanging, investigate why.\n";
        }

        // Said again, last, because of what sits in between. An MSBuild log is
        // mostly absolute paths with backslashes - the link line itself reads
        // "fibonacci.vcxproj -> C:\...\build\Debug\fibonacci.exe", and MSB8029
        // repeats a full path once per project. In run 9 the correct spelling
        // was given above the output and the model still copied the absolute
        // one from the log, twice, and lost it to JSON escaping both times.
        // Whatever comes last is what gets copied, so the last word here is
        // the path that works.
        if (!produced.empty()) {
            const auto resources = workspace.resolved_resources();
            report += "\nTo run what was just built:\n";
            for (const auto& resource : resources) {
                report += "  run_artifact { \"artifact\": \"" + resource.id + "\" }\n";
            }
            report +=
                "That is the whole call. LocalCode holds the path, the configuration "
                "directory and the extension, so none of them can be mistyped - and the build "
                "log above is full of absolute paths whose backslashes do not survive a JSON "
                "string.\n";
            if (resources.size() == 1) {
                report +=
                    "(The path, if you need it for something else: " + resources.front().path +
                    ")\n";
            }
        }

        // --- The comparison the model asked for ---------------------------
        //
        // Done here, on the captured output, by LocalCode. The model states
        // what it expects; it does not get to say whether the expectation was
        // met.
        // A check against a usage message settles nothing, in either
        // direction. Letting it record a failure was the worse half of this
        // bug: "Program builds successfully with CMake" went down as NOT met
        // on the strength of cmake's help text, while the real build had
        // succeeded minutes earlier.
        const auto outcome = printed_usage_only
                                 ? ExpectationOutcome{}
                                 : evaluate_expectation(expectation, process.stdout_text,
                                                        process.stderr_text, process.exit_code);
        // An expectation about what the program prints, attached to the command
        // that builds it. A build's stdout is a build log, so the comparison is
        // between a compiler's output and a program's, and it can only fail.
        //
        // Measured in the adversarial benchmark: a run fixed all three defects
        // correctly - the median, the missing semicolon, the stub - derived the
        // right answer unprompted, and then checked "median=2.5\nmean=3.5"
        // against `cmake --build`. LocalCode recorded correct work as a failed
        // criterion and the run died believing its own fix was wrong.
        //
        // Refused rather than evaluated. Settling a criterion either way on
        // this comparison says nothing true about the program.
        if (outcome.evaluated && !outcome.satisfied && (kind == EvidenceKind::BuildAttempted) &&
            (expectation.stdout_equals || expectation.stdout_matches)) {
            std::string detail =
                "This check says what the PROGRAM prints, and it is attached to the command "
                "that BUILDS it. The output compared was the build log, so the comparison "
                "could only fail - it says nothing about whether the program is right.\n\n";
            const auto resources = workspace.resolved_resources();
            if (!resources.empty()) {
                detail += "Run the program and check it there:\n";
                for (const auto& resource : resources) {
                    detail += "  run_artifact { \"artifact\": \"" + resource.id +
                              "\", "
                              "\"expect\": { ... } }\n";
                }
            } else {
                detail +=
                    "Build first, then run the program the build produced and attach the "
                    "check to that.\n";
            }
            detail +=
                "\nTo check that the build itself succeeded, check its exit code instead:\n"
                "  \"expect\": { \"description\": \"the project builds\", \"exit_code\": 0 }";

            auto misattached = ToolResult::failure(
                "this check is about the program's output, but it was attached to the build",
                std::move(detail));
            misattached.evidence.push_back(make_evidence(
                EvidenceKind::CommandRun, display, "output check attached to a build", false));
            return misattached;
        }

        if (outcome.evaluated) {
            report += "\nBehaviour check: " + expectation.description + "\n";
            if (expectation.description_derived) {
                report +=
                    "  (no description was given, so this one was written from the comparison "
                    "itself. Give 'description' to say what the check proves in the terms of "
                    "the task.)\n";
            }
            for (const auto& line : outcome.passed)
                report += "  ok    " + line + "\n";
            for (const auto& line : outcome.failures)
                report += "  FAIL  " + line + "\n";
            report += outcome.satisfied
                          ? "  -> the output is what the task requires.\n"
                          : "  -> the output is NOT what the task requires. This is a real "
                            "failure: fix it rather than restating the expectation.\n";

            // "stdout_equals": "" says the command must print nothing at all,
            // which is a real check but almost never the one intended. What it
            // usually means is "I do not care what it prints, only that it
            // worked" - and there is a parameter for that.
            //
            // Measured: a build that exited 0 was checked with an empty
            // stdout_equals, failed, and settled "Program builds successfully"
            // as NOT met. The build had succeeded. Saying so here is the
            // difference between a run that recovers and one that records a
            // false negative about its own work.
            if (!outcome.satisfied && expectation.stdout_equals &&
                expectation.stdout_equals->empty() && process.stdout_total_bytes > 0) {
                report +=
                    "\n  Note: \"stdout_equals\": \"\" asks for this command to print nothing, "
                    "and it printed " +
                    strings::human_bytes(process.stdout_total_bytes) +
                    ". If you meant \"it only has to succeed\", check the exit code instead:\n"
                    "    \"expect\": { \"description\": \"" +
                    expectation.description + "\", \"exit_code\": 0";
                if (expectation.criterion > 0) {
                    report += ", \"criterion\": " + std::to_string(expectation.criterion);
                }
                report +=
                    " }\n"
                    "  This command exited " +
                    std::to_string(process.exit_code) +
                    ". Re-running the check with the right comparison replaces this result.\n";
            }

            // A passing check that names no criterion settles nothing, and
            // from the outside that looks like the check being ignored. A run
            // got here, reported the criteria as still open, and re-ran the
            // identical check three times without ever learning that a number
            // was missing from it.
            if (outcome.satisfied && expectation.criterion == 0 && context.acceptance_criteria) {
                std::vector<CriterionSummary> open;
                for (const auto& entry : context.acceptance_criteria()) {
                    if (!entry.settled) open.push_back(entry);
                }
                if (!open.empty()) {
                    report +=
                        "\nThis check settled no acceptance criterion, because it did not name "
                        "one. Criteria are settled by number, not by wording. Still open:\n";
                    for (const auto& entry : open) {
                        report +=
                            "  " + std::to_string(entry.number) + ". " + entry.description + "\n";
                    }
                    report +=
                        "If this check settles one of them, run it again with that number in "
                        "the expectation. Choose the corresponding criterion; this command "
                        "does not prove every open item. For file contents, use read_file "
                        "with expect.contains_all and that content criterion's number.\n";
                }
            }
        }

        // The other half of the same gap. A build or a test run is the whole
        // evidence for a criterion like "it builds" or "the tests pass", and
        // succeeding at one settles nothing on its own. Measured: a model ran
        // the same successful build three times trying to settle "Program
        // builds successfully with CMake", because nothing it did carried an
        // expectation and so nothing ever pointed at the criterion.
        //
        // Only for builds and test runs, which are the commands that settle
        // criteria. Saying this after every successful command would be noise,
        // and noise is what buries the line that matters.
        // Running the program the workspace built is the other half, and the
        // more important one: it is the moment where "it ran" is most easily
        // mistaken for "it is right". Measured across three runs - the program
        // was run with no expectation, nothing was settled, and each run then
        // rebuilt the project repeatedly because the build was the last thing
        // that had worked.
        // Matched on the program's name, not its exact path. A workspace
        // builds fibonacci.exe into Debug and RelWithDebInfo, and running the
        // one that was not the last recorded is still running the program this
        // workspace built - which is the only thing this decides.
        const bool ran_own_artifact = [&] {
            if (argv.empty() || request.executable.empty()) return false;
            if (!workspace.to_relative(request.executable).starts_with("..") &&
                request.executable.has_stem()) {
                const auto stem = strings::to_lower(request.executable.stem().string());
                for (const auto& resource : workspace.resolved_resources()) {
                    if (strings::iequals(resource.name, stem)) return true;
                }
            }
            return false;
        }();

        if (succeeded && !expectation.present && context.acceptance_criteria &&
            (ran_own_artifact || kind == EvidenceKind::BuildAttempted ||
             kind == EvidenceKind::TestsRun)) {
            std::vector<CriterionSummary> open;
            for (const auto& entry : context.acceptance_criteria()) {
                if (!entry.settled) open.push_back(entry);
            }
            if (!open.empty()) {
                report += ran_own_artifact
                              ? "\nThis program ran, which is not the same as it being right: "
                                "nothing compared its output to anything, so no acceptance "
                                "criterion was settled. Still open:\n"
                              : std::string{"\nThis "} +
                                    (kind == EvidenceKind::TestsRun ? "test run" : "build") +
                                    " succeeded but settled no acceptance criterion, because it "
                                    "carried no expectation. Still open:\n";
                for (const auto& entry : open) {
                    report += "  " + std::to_string(entry.number) + ". " + entry.description + "\n";
                }
                // The example matches what was just run. Offering exit_code
                // for a program whose output is the whole point is how a run
                // settles "it prints the right thing" with "it exited 0".
                if (ran_own_artifact) {
                    report +=
                        "Run it again with the output the criterion requires, and that "
                        "criterion's number. Set expect.stdout_equals or stdout_contains "
                        "from the task's expected result, and expect.criterion to the matching "
                        "runtime criterion. Do not use this output to settle an unrelated "
                        "source-content criterion; use read_file with expect.contains_all "
                        "and that content criterion's number instead.\n";
                } else {
                    report +=
                        "If one of them is about this succeeding, run it again and say so - for "
                        "something that only has to exit cleanly, the exit code is the whole "
                        "check. Set expect.exit_code to 0 and expect.criterion to the number "
                        "of that build/test criterion. Do not bind the result to the first "
                        "open item merely because it is first. File contents need read_file "
                        "with expect.contains_all and their own criterion number.\n";
                }
            }
        }

        std::string summary = (succeeded ? "OK " : "FAILED ") + display + "  (exit " +
                              std::to_string(process.exit_code) + ", " +
                              strings::human_duration_ms(process.duration_ms) + ")";
        if (outcome.evaluated) {
            summary += outcome.satisfied ? "  [behaviour verified]" : "  [behaviour WRONG]";
        }

        ToolResult result;
        result.ok = true;   // the tool worked, even when the command failed
        result.summary = summary;
        result.content = report;
        result.full_log_path = process.full_log_path;
        result.truncated = stdout_excerpt.truncated || stderr_excerpt.truncated;
        // The whole record of what ran, so a failed run can be diagnosed from
        // the trace instead of by reasoning about what the model probably
        // meant. Run 9 could not be: the trace held the tool name and nothing
        // else, and the cause had to be reconstructed from a later run that
        // happened to repeat it.
        result.structured = {{"command", display},
                             {"requested_command", shape.display()},
                             {"form", std::string{to_string(shape.form)}},
                             {"shell", shape.use_shell},
                             {"executable", request.executable.generic_string()},
                             {"arguments", request.arguments},
                             {"cwd", working_directory.generic_string()},
                             {"msvc_environment_applied", msvc_environment_applied},
                             {"exit_code", process.exit_code},
                             {"duration_ms", process.duration_ms},
                             {"timed_out", process.timed_out},
                             {"cancelled", process.cancelled},
                             {"error_count", diagnostics.error_count()},
                             {"warning_count", diagnostics.warning_count()},
                             {"stdout_bytes", process.stdout_total_bytes},
                             {"stderr_bytes", process.stderr_total_bytes},
                             {"full_log", process.full_log_path.generic_string()}};

        // Progress diagnostics come from actual output, never from the rendered
        // command/cwd prefix. These bounded tails are data, not instructions.
        const auto diagnostic_tail = [](std::string_view text) {
            constexpr std::size_t limit = 512;
            auto start = text.size() > limit ? text.size() - limit : 0;
            while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                ++start;
            return std::string{text.substr(start)};
        };
        result.structured["progress_stdout_tail"] = diagnostic_tail(process.stdout_text);
        result.structured["progress_stderr_tail"] = diagnostic_tail(process.stderr_text);

        std::string detail = "exit " + std::to_string(process.exit_code);
        if (diagnostics.error_count() > 0) {
            detail += ", " + std::to_string(diagnostics.error_count()) + " diagnostic(s)";
        }
        result.evidence.push_back(make_evidence(kind, display, detail, succeeded));

        if (outcome.evaluated) {
            result.structured["expectation_checked"] = true;
            result.structured["expectation_satisfied"] = outcome.satisfied;
            result.structured["expectation_description"] = expectation.description;
            result.structured["expectation_checks"] = expectation.describe_checks();
            result.structured["expectation_failures"] = outcome.failures;
            result.structured["expectation_criterion"] = expectation.criterion;
            result.structured["expectation_criteria"] = expectation.criteria;
            result.evidence.push_back(make_evidence(
                EvidenceKind::TestsRun, "behaviour check: " + expectation.description,
                outcome.satisfied
                    ? "the output matched what was expected"
                    : strings::utf8_truncate(strings::join(outcome.failures, "; "), 400),
                outcome.satisfied));
        }
        return result;
    }
};

// --- find_executable -------------------------------------------------------
class FindExecutableTool final : public ITool {
public:
    std::string name() const override { return "find_executable"; }

    bool requires_workspace() const override { return false; }

    std::string description() const override {
        return "Finds the full path of a program on PATH, and reports every copy of it that is "
               "installed. Use this to check whether a tool exists before trying to run it, and "
               "to find out which copy would actually be used when several are installed.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"name", schema::string_param("Program name, with or without an extension, "
                                           "e.g. 'cmake' or 'python'.")}},
            {"name"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto name = arg_string(arguments, "name");
        if (name.empty()) {
            return ToolResult::failure("no name given", "The 'name' parameter is required.");
        }

        // The answer this gives ends tasks: told that CMake is not installed, a
        // model reasonably concludes the job cannot be done and says so. It must
        // therefore look everywhere the runner would, not just on PATH.
        auto primary = resolve_executable(name);
        const auto all = find_all_executables(name);

        if (!primary) {
            // "not installed" is a claim about the machine; "not found" is a
            // report about a search. Only the second is true, and the
            // difference matters: told that msbuild and vcvars64 were "not
            // installed" - on a machine with Visual Studio - a 3B model
            // abandoned C++ and wrote a Cargo.toml instead.
            auto result = ToolResult::success(
                name + " was not found",
                "'" + name +
                    "' is not on PATH and not in the install locations LocalCode knows about. "
                    "That is the result of a search, not proof that it is absent: it may be "
                    "installed somewhere unusual, be a shell builtin, or be part of a larger "
                    "package. Check environment_info, or look for its directory, before "
                    "concluding it is missing or choosing a different technology.");
            result.evidence.push_back(
                make_evidence(EvidenceKind::EnvironmentChecked, name, "not found", false));
            return result;
        }

        std::string report = name + " resolves to:\n  " + primary->string() + "\n";
        if (all.size() > 1) {
            report += "\nOther copies on PATH (the first one above wins):\n";
            for (const auto& path : all) {
                if (path != *primary) report += "  " + path.string() + "\n";
            }
        }

        auto result = ToolResult::success(name + " -> " + primary->string(), report);
        result.evidence.push_back(
            make_evidence(EvidenceKind::EnvironmentChecked, name, primary->string(), true));
        return result;
    }
};

// --- run_artifact ----------------------------------------------------------
//
// Running what was just built, without naming where it is.
//
// This is not a convenience wrapper. Four measured runs produced a correct
// program, built it successfully, and then failed to start it - every time
// because the path had to be rebuilt from prose and did not survive the trip:
// \f eaten as a form feed, \U rejected outright, the wrong config directory,
// the path split across three argv elements. The path was never in doubt.
// MSBuild had printed it, LocalCode had parsed it, and the model was still
// asked to type it back.
//
// So it is not asked to. The build records the program under an id, and this
// starts it by id. Nothing about Windows path syntax is on the critical path
// between "it compiled" and "here is what it prints".
class RunArtifactTool final : public ITool {
public:
    std::string name() const override { return "run_artifact"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Runs a program that a build in this workspace produced, by name, without you "
               "having to know or spell out where it is. After a successful build the report "
               "lists what was produced along with the name to use here - pass that as "
               "'artifact'. LocalCode already knows the path, the configuration directory and "
               "the extension, so none of them can be got wrong. Use this rather than "
               "run_command for anything this workspace built. Takes the same 'expect' as "
               "run_command, and you should give it: running a program proves it runs, not that "
               "it is right.";
    }

    Json parameters_schema() const override {
        auto schema = command_schema_common();
        schema["properties"]["artifact"] = schema::string_param(
            "Which built program to run, as named in the build report - for example "
            "\"build:fibonacci\", or just \"fibonacci\". Call the build first if nothing has "
            "been built yet.");
        schema["required"] = Json::array({"artifact"});
        return schema;
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (!context.workspace) {
            return ToolResult::failure("no workspace is open", "Open a workspace first.");
        }
        const auto requested = arg_string(arguments, "artifact");
        if (requested.empty()) {
            return ToolResult::failure(
                "no artifact named",
                "Give 'artifact' as the name from the build report, for example "
                "\"build:fibonacci\".");
        }

        auto& workspace = *context.workspace;
        const auto resource = workspace.resolved_resource(requested);
        if (!resource) {
            const auto available = workspace.resolved_resources();
            std::string message;
            if (available.empty()) {
                message =
                    "No build in this workspace has produced a program yet, so there is "
                    "nothing to run by name. Build first with run_command, and the report will "
                    "list what it produced and what to call it here.";
            } else {
                message = "This workspace has:\n";
                for (const auto& entry : available) {
                    message += "  " + entry.id + "   -> " + entry.path + "\n";
                }
                message += "\nNothing here is called \"" + requested + "\".";
            }
            auto missing = ToolResult::failure("no such artifact: " + requested, message);
            missing.evidence.push_back(
                make_evidence(EvidenceKind::CommandRun, requested, "artifact not resolved", false));
            return missing;
        }

        // The recorded path is checked before it is used. A build tree can be
        // deleted between the build and the run, and reporting that plainly is
        // better than starting a process that fails for a reason nobody sees.
        std::error_code ec;
        const auto absolute = workspace.root() / resource->path;
        if (!std::filesystem::is_regular_file(absolute, ec)) {
            return ToolResult::failure(
                "artifact no longer exists: " + resource->path,
                "The " + resource->source + " named " + resource->path +
                    ", but there is no file there now. Something removed it since the build - "
                    "build again before running it.");
        }

        // Delegated, so behaviour checks, timeouts, output shortening,
        // diagnostics and evidence are identical to any other command. There
        // is exactly one execution path.
        Json forwarded = arguments.is_object() ? arguments : Json::object();
        forwarded.erase("artifact");
        // The absolute path, not the recorded relative one. A relative
        // executable resolves against the working directory, and the caller
        // controls that separately - so passing the relative form makes the
        // meaning of an id depend on a parameter that has nothing to do with
        // it. Measured: run_artifact "build:app" with working_directory
        // "/workspace/build" looked for build/build/RelWithDebInfo/app.exe.
        //
        // The whole point of an id is that the path stops being anybody's
        // problem, so it is resolved here, once, and handed over settled.
        forwarded["executable"] = absolute.generic_string();
        if (!forwarded.contains("arguments")) forwarded["arguments"] = Json::array();

        auto result = runner_.execute(forwarded, context);
        result.content = "artifact: " + resource->id + " -> " + resource->path + "  (" +
                         resource->source + ")\n" + result.content;
        return result;
    }

private:
    RunCommandTool runner_;
};

}   // namespace

std::vector<ToolPtr> make_process_tools() {
    return {std::make_shared<RunCommandTool>(), std::make_shared<RunArtifactTool>(),
            std::make_shared<FindExecutableTool>()};
}

}   // namespace localcode
