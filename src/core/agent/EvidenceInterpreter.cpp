#include "EvidenceInterpreter.h"

#include <algorithm>
#include <filesystem>

#include "../util/Json.h"
#include "../util/Strings.h"
#include "CriterionStrength.h"
#include "ReferenceCheck.h"

namespace localcode {
namespace {

// Wrappers that run whatever follows them, so the interesting program is
// further along the line.
bool is_wrapper(const std::string& stem) {
    static const char* kWrappers[] = {"cmd", "sh",   "bash", "zsh",   "powershell", "pwsh",
                                      "env", "nice", "time", "xargs", "npx",        "uv"};
    return std::any_of(std::begin(kWrappers), std::end(kWrappers),
                       [&](const char* w) { return stem == w; });
}

// vcvars64.bat and friends set variables and exit, ignoring the rest of the
// line. Reading their exit 0 as "the build succeeded" is how a run recorded
// four builds that never happened.
bool is_environment_script(const std::string& stem) {
    return stem == "vcvars64" || stem == "vcvars32" || stem == "vcvarsall" || stem == "vsdevcmd";
}

std::string stem_of(const std::string& argument) {
    // Command records can contain Windows paths even when inspected on POSIX.
    // The host filesystem would treat their backslashes as filename characters.
    const char* separators = "/\\";
#if !defined(_WIN32)
    const bool drive_path = argument.size() >= 3 && argument[1] == ':' &&
                            ((argument[0] >= 'A' && argument[0] <= 'Z') ||
                             (argument[0] >= 'a' && argument[0] <= 'z')) &&
                            (argument[2] == '/' || argument[2] == '\\');
    // Backslashes in a native POSIX filename are literal characters. Only
    // interpret them as separators for explicit Windows drive or UNC paths.
    if (!drive_path && !argument.starts_with("\\\\")) separators = "/";
#endif
    const auto separator = argument.find_last_of(separators);
    const auto filename = argument.substr(separator == std::string::npos ? 0 : separator + 1);
    return strings::to_lower(std::filesystem::path{filename}.stem().string());
}

// A PowerShell host can execute a script whose purpose is opaque to us. Do
// not walk into its filename: `ctest.ps1` is not the known ctest runner, and
// `vcvars64.ps1 cmake ...` does not establish that CMake ran.
CommandIntent powershell_file_intent(const std::vector<std::string>& args, std::size_t host) {
    const auto extension =
        strings::to_lower(std::filesystem::path{args[host]}.extension().string());
    if (!extension.empty() && extension != ".exe") return CommandIntent::Unknown;

    static const char* kFlags[] = {"-noprofile", "-nologo", "-noninteractive",
                                   "-noexit",    "-sta",    "-mta"};
    static const char* kValueFlags[] = {
        "-executionpolicy",  "-inputformat",       "-outputformat", "-windowstyle",
        "-workingdirectory", "-configurationname", "-version"};
    for (std::size_t i = host + 1; i < args.size(); ++i) {
        const auto option = strings::to_lower(args[i]);
        if (option == "-file" || option == "-f") {
            if (i + 1 >= args.size() || args[i + 1].empty() ||
                strings::starts_with(args[i + 1], "-")) {
                return CommandIntent::Unknown;
            }
            const auto script_extension =
                strings::to_lower(std::filesystem::path{args[i + 1]}.extension().string());
            return script_extension == ".ps1" ? CommandIntent::Execution : CommandIntent::Unknown;
        }
        if (std::any_of(std::begin(kFlags), std::end(kFlags),
                        [&](const char* flag) { return option == flag; })) {
            continue;
        }
        if (std::any_of(std::begin(kValueFlags), std::end(kValueFlags),
                        [&](const char* flag) { return option == flag; })) {
            if (i + 1 >= args.size() || args[i + 1].empty() ||
                strings::starts_with(args[i + 1], "-")) {
                return CommandIntent::Unknown;
            }
            ++i;
            continue;
        }
        // Command/encoded-command forms, unknown options and implicit script
        // forms need shell interpretation; never infer a nested build/test.
        return CommandIntent::Unknown;
    }
    return CommandIntent::Unknown;
}

bool contains_word(const std::vector<std::string>& args, std::size_t from, std::string_view word) {
    for (std::size_t i = from; i < args.size(); ++i) {
        if (strings::to_lower(args[i]) == word) return true;
    }
    return false;
}

bool any_arg_starts_with(const std::vector<std::string>& args, std::size_t from,
                         std::string_view prefix) {
    for (std::size_t i = from; i < args.size(); ++i) {
        if (strings::starts_with(strings::to_lower(args[i]), prefix)) return true;
    }
    return false;
}

// Asking a program what version it is tells you nothing about the project.
bool is_pure_query(const std::vector<std::string>& args, std::size_t from) {
    bool saw_flag = false;
    for (std::size_t i = from; i < args.size(); ++i) {
        const auto lowered = strings::to_lower(args[i]);
        if (lowered == "--version" || lowered == "-version" || lowered == "--help" ||
            lowered == "-h" || lowered == "/?" || lowered == "-v") {
            saw_flag = true;
            continue;
        }
        return false;
    }
    return saw_flag;
}

bool is_inspection_program(const std::string& stem) {
    static const char* kNames[] = {"ls",    "dir",    "tree",     "cat",     "type", "more",
                                   "head",  "tail",   "echo",     "pwd",     "cd",   "where",
                                   "which", "whoami", "hostname", "findstr", "grep", "rg",
                                   "find",  "wc",     "du",       "df",      "ver",  "systeminfo",
                                   "date",  "sort",   "printenv", "set",     "env"};
    return std::any_of(std::begin(kNames), std::end(kNames),
                       [&](const char* n) { return stem == n; });
}

// git subcommands that only look.
bool is_git_inspection(const std::vector<std::string>& args, std::size_t from) {
    static const char* kNames[] = {"status", "log",      "diff",      "show",     "branch",
                                   "remote", "config",   "rev-parse", "ls-files", "describe",
                                   "blame",  "shortlog", "tag"};
    for (std::size_t i = from; i < args.size(); ++i) {
        if (strings::starts_with(args[i], "-")) continue;
        const auto lowered = strings::to_lower(args[i]);
        return std::any_of(std::begin(kNames), std::end(kNames),
                           [&](const char* n) { return lowered == n; });
    }
    return false;
}

// Interpreters and produced binaries: running one of these is an execution of
// the thing under construction, not a step in producing it.
bool is_interpreter(const std::string& stem) {
    static const char* kNames[] = {"python", "python3", "py",  "node", "deno",  "bun",
                                   "ruby",   "perl",    "lua", "java", "dotnet"};
    return std::any_of(std::begin(kNames), std::end(kNames),
                       [&](const char* n) { return stem == n; });
}

}   // namespace

std::string_view to_string(CommandIntent intent) noexcept {
    switch (intent) {
        case CommandIntent::Inspection: return "inspection";
        case CommandIntent::Configure: return "configure";
        case CommandIntent::Build: return "build";
        case CommandIntent::Test: return "test";
        case CommandIntent::Execution: return "execution";
        case CommandIntent::Unknown: return "unknown";
    }
    return "unknown";
}

std::vector<std::string> split_command_line(std::string_view line) {
    std::vector<std::string> args;
    std::string current;
    char quote = 0;
    bool have_current = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char c = line[index];
#if !defined(_WIN32)
        // ProcessRunner renders POSIX arguments with single quotes and uses
        // '\'' for an embedded apostrophe. Decode that display format without
        // evaluating the command or consuming backslashes in Windows paths.
        if (quote == 0 && c == '\\' && index + 1 < line.size() && line[index + 1] == '\'') {
            current.push_back(line[++index]);
            have_current = true;
            continue;
        }
        if (c == '\'' && (quote == 0 || quote == '\'')) {
            quote = quote == 0 ? '\'' : 0;
            have_current = true;
            continue;
        }
#endif
        if (c == '"' && (quote == 0 || quote == '"')) {
            quote = quote == 0 ? '"' : 0;
            have_current = true;
            continue;
        }
        if (quote == 0 && (c == ' ' || c == '\t')) {
            if (have_current) args.push_back(current);
            current.clear();
            have_current = false;
            continue;
        }
        current.push_back(c);
        have_current = true;
    }
    if (have_current) args.push_back(current);
    return args;
}

CommandIntent classify_intent(std::string_view command_line) {
    const auto args = split_command_line(command_line);
    if (args.empty()) return CommandIntent::Unknown;

    // Walk past wrappers to the program that actually does something.
    std::size_t index = 0;
    while (index + 1 < args.size() && is_wrapper(stem_of(args[index]))) {
        const auto wrapper = stem_of(args[index]);
        if (wrapper == "powershell" || wrapper == "pwsh")
            return powershell_file_intent(args, index);
        ++index;
        // `cmd /c ...`, `bash -c ...`: skip the switch too.
        while (index < args.size() && strings::starts_with(args[index], "/"))
            ++index;
        while (index + 1 < args.size() && (strings::to_lower(args[index]) == "-command" ||
                                           strings::to_lower(args[index]) == "-c" ||
                                           strings::to_lower(args[index]) == "-noprofile")) {
            ++index;
        }
    }
    if (index >= args.size()) return CommandIntent::Unknown;

    const auto program = stem_of(args[index]);
    const std::size_t rest = index + 1;

    // An environment script consumes the line and ignores it. Whatever tool
    // name appears afterwards, nothing of the kind ran.
    if (is_environment_script(program)) return CommandIntent::Unknown;

    if (is_pure_query(args, rest)) return CommandIntent::Inspection;
    if (is_inspection_program(program)) return CommandIntent::Inspection;
    if (program == "git") {
        return is_git_inspection(args, rest) ? CommandIntent::Inspection : CommandIntent::Unknown;
    }

    if (program == "cmake") {
        if (contains_word(args, rest, "--build")) return CommandIntent::Build;
        if (contains_word(args, rest, "--install")) return CommandIntent::Build;
        if (contains_word(args, rest, "-e") || contains_word(args, rest, "--version")) {
            return CommandIntent::Inspection;
        }
        return CommandIntent::Configure;
    }
    if (program == "configure" || program == "autoreconf" || program == "qmake") {
        return CommandIntent::Configure;
    }
    if (program == "meson") {
        return contains_word(args, rest, "test") ? CommandIntent::Test : CommandIntent::Configure;
    }

    if (program == "ctest" || program == "pytest" || program == "gtest" || program == "jest" ||
        program == "vitest" || program == "catch2") {
        return CommandIntent::Test;
    }

    static const char* kBuilders[] = {"ninja", "make",  "gmake", "nmake",   "msbuild",
                                      "gcc",   "g++",   "clang", "clang++", "cl",
                                      "link",  "rustc", "tsc",   "javac",   "devenv"};
    if (std::any_of(std::begin(kBuilders), std::end(kBuilders),
                    [&](const char* n) { return program == n; })) {
        return CommandIntent::Build;
    }

    // Subcommand-driven drivers.
    static const char* kDrivers[] = {"cargo", "go",    "npm",    "yarn",
                                     "pnpm",  "swift", "gradle", "mvn"};
    if (std::any_of(std::begin(kDrivers), std::end(kDrivers),
                    [&](const char* n) { return program == n; })) {
        if (contains_word(args, rest, "test")) return CommandIntent::Test;
        if (contains_word(args, rest, "build") || contains_word(args, rest, "compile") ||
            contains_word(args, rest, "install") || contains_word(args, rest, "ci")) {
            return CommandIntent::Build;
        }
        if (contains_word(args, rest, "run") || contains_word(args, rest, "start") ||
            contains_word(args, rest, "exec")) {
            return CommandIntent::Execution;
        }
        return CommandIntent::Unknown;
    }
    if (program == "dotnet") {
        if (contains_word(args, rest, "test")) return CommandIntent::Test;
        if (contains_word(args, rest, "build") || contains_word(args, rest, "publish")) {
            return CommandIntent::Build;
        }
        if (contains_word(args, rest, "run")) return CommandIntent::Execution;
        return CommandIntent::Execution;   // `dotnet app.dll`
    }

    if (is_interpreter(program)) {
        // `python -m pytest` is a test run wearing an interpreter's name.
        if (any_arg_starts_with(args, rest, "pytest") || contains_word(args, rest, "pytest") ||
            contains_word(args, rest, "unittest")) {
            return CommandIntent::Test;
        }
        return CommandIntent::Execution;
    }

    // Anything else that names a path is a program being run. A bare word that
    // is not a tool we know is left unclassified rather than guessed at.
    const auto& raw = args[index];
    const bool looks_like_a_path = raw.find('/') != std::string::npos ||
                                   raw.find('\\') != std::string::npos ||
                                   strings::ends_with(strings::to_lower(raw), ".exe") ||
                                   strings::ends_with(strings::to_lower(raw), ".bat") ||
                                   strings::ends_with(strings::to_lower(raw), ".sh");
    if (looks_like_a_path) {
        // A produced test binary is still a test run.
        if (program.find("test") != std::string::npos) return CommandIntent::Test;
        return CommandIntent::Execution;
    }
    return CommandIntent::Unknown;
}

std::optional<Stage> stage_for(CommandIntent intent) {
    switch (intent) {
        case CommandIntent::Configure: return Stage::Configure;
        case CommandIntent::Build: return Stage::Build;
        case CommandIntent::Test: return Stage::Test;
        case CommandIntent::Execution: return Stage::Execution;
        case CommandIntent::Inspection:
        case CommandIntent::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

bool reports_no_tests_ran(std::string_view output) {
    static const char* kPhrases[] = {
        "no tests were found",   "no tests found", "no tests to run",    "total tests: 0",
        "collected 0 items",     "ran 0 tests",    "0 tests ran",        "no test files",
        "0 tests, 0 assertions", "no test suites", "0 passed, 0 failed", "no tests ran"};
    return std::any_of(std::begin(kPhrases), std::end(kPhrases),
                       [&](const char* phrase) { return strings::icontains(output, phrase); });
}

void interpret_tool_result(const ToolCall& call, const ToolResult& result,
                           VerificationLedger& ledger, std::string_view objective,
                           ToolInterpretation* interpretation) {
    if (interpretation) *interpretation = {};
    // Anything written to disk moves the tree forward, which is what makes
    // earlier passes stale.
    for (const auto& evidence : result.evidence) {
        if (evidence.kind == EvidenceKind::FileWritten && evidence.positive) {
            ledger.note_source_changed(evidence.subject);
        }
    }

    if (!result.structured.is_object()) return;

    // Unfinished work and weakened tests are recorded as negative evidence
    // rather than as stage failures. They do not make the build fail - that is
    // exactly the problem with them - but they belong in the record, because a
    // green run over a stub proves less than a green run over real code.
    if (result.structured.contains("stub_findings") &&
        result.structured.at("stub_findings").is_array()) {
        for (const auto& finding : result.structured.at("stub_findings")) {
            const auto where =
                json_string(finding, "file") + ":" + std::to_string(json_int(finding, "line"));
            ledger.add_evidence(EvidenceKind::FileWritten, "unfinished work at " + where,
                                json_string(finding, "pattern") + " - " +
                                    json_string(finding, "reason") + " (" +
                                    json_string(finding, "confidence") + " confidence)",
                                false);
        }
    }
    if (result.structured.contains("test_tampering") &&
        result.structured.at("test_tampering").is_array()) {
        for (const auto& finding : result.structured.at("test_tampering")) {
            ledger.add_evidence(
                EvidenceKind::TestsRun,
                "the tests in " + json_string(finding, "file") + " were weakened",
                json_string(finding, "kind") + " - " + json_string(finding, "detail"), false);
        }
    }

    // The definition of done, committed to before the work.
    if (call.name == "set_acceptance_criteria" && result.ok &&
        result.structured.contains("acceptance_criteria")) {
        std::vector<AcceptanceCheck> plan;
        for (const auto& item : result.structured.at("acceptance_criteria")) {
            if (!item.is_string()) continue;
            AcceptanceCheck check;
            check.description = item.get<std::string>();

            // Where the request determines an answer, work it out here rather
            // than taking the model's word for it. A model asked for the first
            // ten Fibonacci numbers wrote its criterion as
            // "1, 1, 2, 3, 5, 8, 13, 21, 34, 55" and would have verified
            // against its own definition of the answer.
            const auto verdict = check_against_reference(objective, check.description);
            if (verdict.applicable && verdict.agrees) {
                check.source = CriterionSource::UserDerived;
                check.reason = verdict.derivation;
                check.derived_from = "the request";
            } else if (verdict.applicable) {
                check.conflict = verdict.conflict;
            }
            plan.push_back(std::move(check));
        }
        // Replacing a plan whose checks were already settled would let a run
        // rewrite its own definition of done once the results were in.
        const auto& existing = ledger.acceptance_plan();
        const bool anything_settled =
            std::any_of(existing.begin(), existing.end(),
                        [](const AcceptanceCheck& check) { return check.settled; });
        // Does this plan test what was asked, or only what is easy to satisfy?
        // A criterion can be perfectly true and beside the point - "the project
        // builds" for a task about a crash - and an agent writing its own
        // definition of done drifts towards the definition it can meet.
        //
        // Recorded on the plan rather than refused: the criteria are not wrong,
        // they are incomplete, and the objective must not rest on them alone.
        std::vector<std::string> descriptions;
        descriptions.reserve(plan.size());
        for (const auto& check : plan)
            descriptions.push_back(check.description);
        const auto strength = assess_plan_strength(objective, descriptions);

        if (!plan.empty() && !anything_settled) {
            ledger.set_acceptance_plan(std::move(plan));
            if (strength.insufficient) {
                ledger.record_plan_gap(strength.reason + "\n" + strength.suggestion);
            }
        }
        return;
    }

    // File assertions settle only the named content criterion. They establish
    // no build, test, execution or runtime-behaviour stage.
    if (call.name == "read_file" && json_bool(result.structured, "file_check_checked")) {
        const bool passed = json_bool(result.structured, "file_check_passed");
        const auto path = json_string(result.structured, "path");
        std::string detail = json_string(result.structured, "file_check_description");
        const auto snippets = result.structured.find("file_check_contains");
        if (snippets != result.structured.end() && snippets->is_array()) {
            for (const auto& snippet : *snippets)
                if (snippet.is_string())
                    detail += "\nrequires literal: " + snippet.get<std::string>();
        }
        const auto id = ledger.add_evidence(EvidenceKind::FileRead, "file content check: " + path,
                                            detail, passed);
        const auto criterion = json_int(result.structured, "file_check_criterion", 0);
        if (criterion >= 1 &&
            static_cast<std::size_t>(criterion) <= ledger.acceptance_plan().size())
            ledger.settle_check(static_cast<std::size_t>(criterion - 1), passed, id);
        return;
    }

    // run_artifact runs a command through exactly the same path and produces
    // exactly the same record; only the way the program was named differs.
    // Excluding it made every behaviour check and every criterion settled
    // through it invisible to the ledger - so a run could verify its work
    // perfectly and be told none of its criteria had been settled. Measured in
    // four runs, each repeating a correct, passing check until it was stopped.
    if (call.name != "run_command" && call.name != "run_artifact") return;

    const auto command = json_string(result.structured, "command");
    if (strings::trim(command).empty()) return;

    // A cancelled command proves nothing in either direction.
    if (json_bool(result.structured, "cancelled")) return;

    // --- Behaviour -------------------------------------------------------
    // The only route to the behaviour stage. It exists because the model
    // stated an expectation before the command ran and run_command compared
    // the real output against it; nothing infers behaviour from an exit code.
    if (json_bool(result.structured, "expectation_checked")) {
        const bool satisfied = json_bool(result.structured, "expectation_satisfied");
        const auto description = json_string(result.structured, "expectation_description");
        // Keep the observed comparison separate from the caller's account of
        // what it proves. An exit-only check may be associated with any named
        // criterion; that association does not classify a build or test.
        std::string detail = "Caller-associated comparison.";
        bool has_comparison = false;
        if (result.structured.contains("expectation_checks") &&
            result.structured.at("expectation_checks").is_array()) {
            for (const auto& entry : result.structured.at("expectation_checks")) {
                if (!entry.is_string()) continue;
                detail += "\nCompared: " + entry.get<std::string>();
                has_comparison = true;
            }
        }
        if (!has_comparison) detail += "\nCompared checks were not recorded.";
        detail += "\nCaller description: " + description;
        if (!satisfied && result.structured.contains("expectation_failures") &&
            result.structured.at("expectation_failures").is_array()) {
            for (const auto& failure : result.structured.at("expectation_failures")) {
                if (failure.is_string()) detail += "\n  " + failure.get<std::string>();
            }
        }
        const auto id = ledger.add_evidence(
            EvidenceKind::TestsRun, "behaviour check while running: " + command, detail, satisfied);
        ledger.record(Stage::Behaviour, satisfied ? StageStatus::Pass : StageStatus::Fail, id,
                      description);

        // The criterion is named by number, not matched by how similar the
        // wording is. An out-of-range number settles nothing rather than
        // settling the nearest thing to it.
        // One check can settle several criteria when the caller says so.
        // Running the program once legitimately establishes "prints the first
        // ten Fibonacci numbers" and "runs and produces the correct output" -
        // the same evidence, named twice in the plan.
        std::vector<int> criteria;
        if (result.structured.contains("expectation_criteria") &&
            result.structured.at("expectation_criteria").is_array()) {
            for (const auto& entry : result.structured.at("expectation_criteria")) {
                if (entry.is_number()) criteria.push_back(static_cast<int>(entry.get<long long>()));
            }
        }
        if (criteria.empty()) {
            const auto single =
                static_cast<int>(json_int(result.structured, "expectation_criterion", 0));
            if (single > 0) criteria.push_back(single);
        }

        for (const int criterion : criteria) {
            if (criterion >= 1 &&
                static_cast<std::size_t>(criterion) <= ledger.acceptance_plan().size()) {
                // An expectation the request itself determines is not the model's
                // opinion any more, so the criterion it settles stops being one.
                // Where the reference disagrees, settling it does not make the
                // objective pass, however cleanly the comparison ran.
                std::string expected;
                if (result.structured.contains("expectation_checks") &&
                    result.structured.at("expectation_checks").is_array()) {
                    for (const auto& entry : result.structured.at("expectation_checks")) {
                        if (entry.is_string()) expected += entry.get<std::string>() + " ";
                    }
                }
                const auto verdict = check_against_reference(objective, expected);
                if (verdict.applicable && verdict.agrees) {
                    ledger.ground_check(static_cast<std::size_t>(criterion - 1),
                                        CriterionSource::ReferenceDerived, verdict.derivation, {});
                } else if (verdict.applicable) {
                    ledger.dispute_check(static_cast<std::size_t>(criterion - 1), verdict.conflict);
                }
                ledger.settle_check(static_cast<std::size_t>(criterion - 1), satisfied, id);
            }
        }
    }

    const auto intent = classify_intent(command);
    const auto stage = stage_for(intent);
    if (!stage) return;

    const auto exit_code = static_cast<int>(json_int(result.structured, "exit_code"));
    const bool timed_out = json_bool(result.structured, "timed_out");
    const auto error_count = json_int(result.structured, "error_count");
    const bool succeeded = exit_code == 0 && !timed_out;

    std::string detail = "exit " + std::to_string(exit_code);
    if (timed_out) detail += " (killed on timeout)";
    if (error_count > 0) detail += ", " + std::to_string(error_count) + " diagnostic(s)";

    const auto kind = *stage == Stage::Build  ? EvidenceKind::BuildAttempted
                      : *stage == Stage::Test ? EvidenceKind::TestsRun
                                              : EvidenceKind::CommandRun;

    // `ctest` in a directory with no tests exits 0. Recording that as a passing
    // test stage would be the same lie in a different place.
    if (*stage == Stage::Test && succeeded && reports_no_tests_ran(result.content)) {
        if (interpretation) interpretation->no_tests_found = true;
        ledger.add_evidence(kind, command, "exit 0, but the run found no tests", false);
        ledger.mark_not_applicable(
            Stage::Test, "no tests were found by: " + strings::utf8_truncate(command, 120));
        return;
    }

    if (*stage == Stage::Execution && succeeded) {
        // Keep enough of the output that a later comparison has something to
        // compare. It is recorded as what the program printed - never as
        // evidence that what it printed was right.
        const auto excerpt = strings::utf8_truncate(strings::trim(result.content), 600);
        if (!excerpt.empty()) detail += "\noutput: " + excerpt;
    }

    const auto id = ledger.add_evidence(kind, command, detail, succeeded);
    ledger.record(*stage, succeeded ? StageStatus::Pass : StageStatus::Fail, id,
                  succeeded ? "" : detail);
}

}   // namespace localcode
