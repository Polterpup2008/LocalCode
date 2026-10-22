#include "CommandPolicy.h"

#include <algorithm>

#include "../platform/Process.h"
#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

CommandDecision strictest(CommandDecision a, CommandDecision b) {
    auto rank = [](CommandDecision d) {
        switch (d) {
            case CommandDecision::Allow:   return 0;
            case CommandDecision::Confirm: return 1;
            case CommandDecision::Block:   return 2;
        }
        return 1;
    };
    return rank(a) >= rank(b) ? a : b;
}

std::string executable_key(std::string_view raw) {
    std::filesystem::path p{raw};
    auto name = p.filename().string();
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        const auto ext = strings::to_lower(name.substr(dot));
        if (ext == ".exe" || ext == ".com" || ext == ".cmd" || ext == ".bat") {
            name = name.substr(0, dot);
        }
    }
    return strings::to_lower(name);
}

CommandRule block(std::string id, std::string reason, std::vector<std::string> executables,
                  std::vector<std::string> patterns = {},
                  std::vector<std::string> require_all = {}) {
    CommandRule rule;
    rule.id = std::move(id);
    rule.decision = CommandDecision::Block;
    rule.reason = std::move(reason);
    rule.executables = std::move(executables);
    rule.patterns = std::move(patterns);
    rule.require_all = std::move(require_all);
    return rule;
}

CommandRule confirm(std::string id, std::string reason, std::vector<std::string> executables,
                    std::vector<std::string> patterns = {},
                    std::vector<std::string> require_all = {},
                    std::vector<std::string> unless_any = {}) {
    CommandRule rule;
    rule.id = std::move(id);
    rule.decision = CommandDecision::Confirm;
    rule.reason = std::move(reason);
    rule.executables = std::move(executables);
    rule.patterns = std::move(patterns);
    rule.require_all = std::move(require_all);
    rule.unless_any = std::move(unless_any);
    return rule;
}

CommandRule allow(std::string id, std::string reason, std::vector<std::string> executables,
                  std::vector<std::string> unless_any = {}) {
    CommandRule rule;
    rule.id = std::move(id);
    rule.decision = CommandDecision::Allow;
    rule.reason = std::move(reason);
    rule.executables = std::move(executables);
    rule.unless_any = std::move(unless_any);
    return rule;
}

}  // namespace

std::string_view to_string(CommandDecision decision) noexcept {
    switch (decision) {
        case CommandDecision::Allow:   return "allow";
        case CommandDecision::Confirm: return "confirm";
        case CommandDecision::Block:   return "block";
    }
    return "confirm";
}

std::string_view to_string(AutonomyLevel level) noexcept {
    switch (level) {
        case AutonomyLevel::Restricted: return "restricted";
        case AutonomyLevel::Standard:   return "standard";
        case AutonomyLevel::Full:       return "full";
    }
    return "standard";
}

std::vector<CommandRule> CommandPolicy::builtin_rules() {
    std::vector<CommandRule> rules;

    // --- Never, under any configuration -----------------------------------
    rules.push_back(block("disk.format", "formatting a volume destroys all data on it",
                          {"format", "diskpart"}));
    rules.push_back(block("disk.partition", "partition table edits can make the machine unbootable",
                          {"fdisk", "parted", "mkfs", "sgdisk"}));
    rules.push_back(block("boot.config", "boot configuration edits can make the machine unbootable",
                          {"bcdedit", "bootrec", "bootsect", "grub-install", "efibootmgr"}));
    rules.push_back(block("power.state", "shutting down or rebooting the machine is never part of a coding task",
                          {"shutdown", "reboot", "halt", "poweroff", "restart-computer",
                           "stop-computer"}));
    rules.push_back(block("backup.destroy", "deleting shadow copies destroys the user's ability to recover files",
                          {"vssadmin", "wbadmin"}, {"*delete*"}));
    rules.push_back(block("wmic.shadowcopy", "deleting shadow copies destroys recovery points",
                          {"wmic"}, {"*shadowcopy*delete*"}));
    rules.push_back(block("security.defender", "disabling anti-malware protection is out of scope for a coding agent",
                          {"set-mppreference", "add-mppreference"}));
    rules.push_back(block("security.firewall", "disabling the firewall is out of scope for a coding agent",
                          {"netsh"}, {"*advfirewall*off*", "*firewall*set*disable*"}));
    rules.push_back(block("security.service", "stopping security services is out of scope for a coding agent",
                          {"sc", "net"}, {"*stop*windefend*", "*stop*sense*", "*stop*wuauserv*"}));
    rules.push_back(block("creds.registry-hive", "dumping registry hives is credential theft",
                          {"reg", "reg.exe"}, {"*save*hklm\\sam*", "*save*hklm\\system*",
                                               "*save*hklm\\security*"}));
    rules.push_back(block("creds.lsass", "dumping LSASS memory is credential theft",
                          {"procdump", "procdump64", "rundll32", "taskmgr"},
                          {"*lsass*"}));
    rules.push_back(block("creds.tools", "credential extraction tooling is out of scope",
                          {"mimikatz", "lazagne", "secretsdump", "hashdump"}));
    rules.push_back(block("registry.destroy", "deleting registry hives breaks the operating system",
                          {"reg", "reg.exe"}, {"*delete*hklm\\software\\microsoft\\windows*",
                                               "*delete*hklm\\system\\currentcontrolset*"}));
    rules.push_back(block("ps.encoded", "base64-encoded PowerShell hides what is actually being run",
                          {"powershell", "pwsh"},
                          {"*-encodedcommand*", "*-enc *", "*frombase64string*"}));
    rules.push_back(block("shell.pipe-to-interpreter",
                          "piping a downloaded script straight into a shell executes unreviewed remote code",
                          {}, {"*curl*|*sh*", "*curl*|*bash*", "*wget*|*sh*", "*wget*|*bash*",
                               "*invoke-webrequest*|*iex*", "*invoke-expression*downloadstring*",
                               "*iwr*|*iex*"}));
    rules.push_back(block("fs.wipe-root",
                          "recursive deletion of a filesystem root would destroy the machine",
                          {}, {"rm -rf /", "rm -rf /*", "rm -fr /", "rm -rf ~", "rm -rf ~/*",
                               "*del /s /q c:\\*", "*rd /s /q c:\\*",
                               "*remove-item*-recurse*c:\\ *", "*remove-item*c:\\**"}));
    rules.push_back(block("fs.cipher-wipe", "secure-wiping free space destroys deleted-file recovery",
                          {"cipher"}, {"*/w*"}));
    rules.push_back(block("account.admin", "creating accounts or granting administrator rights is out of scope",
                          {"net", "net1"}, {"*user*/add*", "*localgroup*administrators*/add*"}));
    rules.push_back(block("schedule.persistence", "installing scheduled tasks or services is out of scope",
                          {"schtasks", "at"}, {"*/create*"}));

    // --- Needs a human ----------------------------------------------------
    rules.push_back(confirm("git.reset-hard",
                            "git reset --hard discards uncommitted work, including changes you made yourself",
                            {"git"}, {"*reset*--hard*"}));
    rules.push_back(confirm("git.clean",
                            "git clean deletes untracked files that are not recoverable from git",
                            {"git"}, {"*clean*-*d*", "*clean*-*f*"}));
    rules.push_back(confirm("git.force-push", "force-pushing rewrites history other people may have pulled",
                            {"git"}, {"*push*--force*", "*push*-f *", "*push*+*:*"}));
    rules.push_back(confirm("git.checkout-discard",
                            "checking out over modified files discards those modifications",
                            {"git"}, {"*checkout*--*.*", "*restore*--worktree*"}));
    rules.push_back(confirm("git.publish", "pushing publishes work outside this machine", {"git"},
                            {"*push*"}, {}, {"--dry-run"}));
    rules.push_back(confirm("pkg.system-install",
                            "installing software system-wide affects the whole machine",
                            {"choco", "scoop", "winget", "apt", "apt-get", "yum", "dnf", "pacman",
                             "brew", "msiexec"},
                            {"*install*", "*upgrade*", "*add*"}));
    rules.push_back(confirm("pkg.global-install",
                            "a global package install affects projects outside this workspace",
                            {"npm", "pnpm", "yarn", "pip", "pip3", "cargo", "dotnet", "gem"},
                            {"*install*-g*", "*install*--global*", "*add*--global*",
                             "*install*--user*"}));
    rules.push_back(confirm("net.remote-exec", "connecting to another machine to run commands is out of scope",
                            {"ssh", "scp", "psexec", "winrs", "invoke-command", "enter-pssession"}));
    rules.push_back(confirm("perm.take-ownership", "changing ownership or ACLs affects the whole system",
                            {"takeown", "icacls", "cacls", "chown", "chmod"},
                            {"*/r*", "*-r*", "*/t*"}));
    rules.push_back(confirm("env.persist", "persisting environment variables affects other applications",
                            {"setx"}));
    rules.push_back(confirm("registry.write", "registry edits affect the whole machine",
                            {"reg", "regedit"}, {"*add*", "*import*", "*delete*"}));

    // --- Explicitly allowed: the everyday work of writing code -------------
    rules.push_back(allow("build.tools", "build and compile tooling",
                          {"cmake", "ctest", "cpack", "ninja", "make", "gmake", "nmake", "msbuild",
                           "meson", "bazel", "buck", "xmake", "premake5", "scons"}));
    rules.push_back(allow("build.compilers", "compilers and linkers",
                          {"cl", "clang", "clang++", "clang-cl", "gcc", "g++", "c++", "cc", "link",
                           "lib", "ld", "ar", "ranlib", "rc", "midl", "nvcc", "dxc", "glslc",
                           "glslangvalidator", "spirv-val", "fxc"}));
    rules.push_back(allow("build.analysis", "static analysis and formatting",
                          {"clang-tidy", "clang-format", "cppcheck", "iwyu", "include-what-you-use",
                           "dumpbin", "objdump", "nm", "readelf", "strings", "addr2line"}));
    rules.push_back(allow("lang.runtimes", "language toolchains operating on the current project",
                          {"python", "python3", "py", "node", "deno", "bun", "ruby", "perl", "lua",
                           "java", "javac", "go", "gofmt", "rustc", "rustup", "dotnet", "php"},
                          {}));
    rules.push_back(allow("lang.packagemanagers", "project-local dependency resolution",
                          {"cargo", "npm", "pnpm", "yarn", "pip", "pip3", "poetry", "uv", "conan",
                           "vcpkg", "nuget", "gradle", "mvn", "composer", "bundler"}));
    rules.push_back(allow("test.runners", "test runners",
                          {"pytest", "gtest", "catch2", "jest", "vitest", "mocha", "tox", "nose2",
                           "unittest", "phpunit", "rspec"}));
    rules.push_back(allow("git.read", "read-only git inspection", {"git"}));
    rules.push_back(allow("shell.inspect", "read-only inspection utilities",
                          {"where", "which", "type", "file", "stat", "ls", "dir", "cat", "head",
                           "tail", "wc", "find", "findstr", "grep", "rg", "fd", "tree", "du",
                           "echo", "printf", "sort", "uniq", "diff", "cmp", "md5sum", "sha256sum",
                           "certutil", "vswhere", "systeminfo", "ver", "uname", "hostname",
                           "nvidia-smi", "wmic"}));

    return rules;
}

CommandPolicy::CommandPolicy() : rules_(builtin_rules()) {}

void CommandPolicy::add_rule(CommandRule rule) {
    // User rules are evaluated before built-ins so they can tighten, but the
    // Block rules above are re-checked first in evaluate(), so nothing a user
    // adds can un-block a blocked command.
    rules_.insert(rules_.begin(), std::move(rule));
}

void CommandPolicy::set_rule_enabled(const std::string& id, bool enabled) {
    for (auto& rule : rules_) {
        if (rule.id == id) rule.enabled = enabled;
    }
}

std::vector<std::string> CommandPolicy::tokenize(std::string_view segment) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single = false, in_double = false;
    bool has_token = false;

    for (std::size_t i = 0; i < segment.size(); ++i) {
        const char c = segment[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            has_token = true;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
            has_token = true;
        } else if (!in_single && !in_double && (c == ' ' || c == '\t')) {
            if (has_token) {
                tokens.push_back(current);
                current.clear();
                has_token = false;
            }
        } else {
            current.push_back(c);
            has_token = true;
        }
    }
    if (has_token) tokens.push_back(current);
    return tokens;
}

std::vector<std::string> CommandPolicy::split_shell_segments(std::string_view line) {
    std::vector<std::string> segments;
    std::string current;
    bool in_single = false, in_double = false;

    auto flush = [&] {
        auto trimmed = strings::trim(current);
        if (!trimmed.empty()) segments.push_back(trimmed);
        current.clear();
    };

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !in_double) { in_single = !in_single; current.push_back(c); continue; }
        if (c == '"' && !in_single) { in_double = !in_double; current.push_back(c); continue; }
        if (in_single || in_double) { current.push_back(c); continue; }

        if (c == '\n' || c == ';') { flush(); continue; }
        if ((c == '&' || c == '|') && i + 1 < line.size() && line[i + 1] == c) {
            flush();
            ++i;
            continue;
        }
        if (c == '|' || c == '&') { flush(); continue; }
        // Backtick and $( ) substitution: treat the inner text as its own
        // segment so "echo $(rm -rf /)" is still classified.
        if (c == '`') { flush(); continue; }
        if (c == '$' && i + 1 < line.size() && line[i + 1] == '(') { flush(); ++i; continue; }
        if (c == ')') { flush(); continue; }
        current.push_back(c);
    }
    flush();
    return segments;
}

CommandEvaluation CommandPolicy::classify_tokens(const std::vector<std::string>& argv) const {
    CommandEvaluation result;
    if (argv.empty()) {
        result.decision = CommandDecision::Block;
        result.reason = "empty command";
        return result;
    }

    const std::string exe = executable_key(argv[0]);
    std::string joined = exe;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        joined.push_back(' ');
        joined += argv[i];
    }
    const std::string joined_lower = strings::to_lower(joined);

    auto rule_matches = [&](const CommandRule& rule) {
        if (!rule.enabled) return false;
        if (!rule.executables.empty()) {
            bool hit = false;
            for (const auto& candidate : rule.executables) {
                if (strings::iequals(candidate, exe)) { hit = true; break; }
            }
            if (!hit) return false;
        }
        for (const auto& needle : rule.require_all) {
            if (!strings::icontains(joined_lower, needle)) return false;
        }
        for (const auto& needle : rule.unless_any) {
            if (strings::icontains(joined_lower, needle)) return false;
        }
        if (rule.patterns.empty()) return true;
        for (const auto& pattern : rule.patterns) {
            const bool wild = pattern.find('*') != std::string::npos ||
                              pattern.find('?') != std::string::npos;
            if (wild ? strings::glob_match(pattern, joined_lower)
                     : strings::icontains(joined_lower, pattern)) {
                return true;
            }
        }
        return false;
    };

    // Blocks win outright, and are checked before anything a user configured.
    for (const auto& rule : rules_) {
        if (rule.decision == CommandDecision::Block && rule_matches(rule)) {
            result.decision = CommandDecision::Block;
            result.rule_id = rule.id;
            result.reason = rule.reason;
            result.matched_text = joined;
            return result;
        }
    }
    for (const auto& rule : rules_) {
        if (rule.decision == CommandDecision::Confirm && rule_matches(rule)) {
            result.decision = CommandDecision::Confirm;
            result.rule_id = rule.id;
            result.reason = rule.reason;
            result.matched_text = joined;
            return result;
        }
    }
    for (const auto& rule : rules_) {
        if (rule.decision == CommandDecision::Allow && rule_matches(rule)) {
            result.decision = CommandDecision::Allow;
            result.rule_id = rule.id;
            result.reason = rule.reason;
            return result;
        }
    }

    return apply_heuristics(argv, joined_lower);
}

CommandEvaluation CommandPolicy::apply_heuristics(const std::vector<std::string>& argv,
                                                  std::string_view joined) const {
    CommandEvaluation result;
    const std::string exe = executable_key(argv[0]);

    // Recursive deletion is the one unrecognised shape worth inspecting closely:
    // inside the workspace it is ordinary cleanup, outside it is destruction.
    static const std::vector<std::string> kDeleters = {"rm", "rmdir", "rd", "del", "erase",
                                                       "remove-item", "ri", "unlink", "shred"};
    const bool is_delete =
        std::find(kDeleters.begin(), kDeleters.end(), exe) != kDeleters.end();
    const bool is_recursive =
        strings::icontains(joined, " -r") || strings::icontains(joined, " /s") ||
        strings::icontains(joined, "-recurse") || strings::icontains(joined, " -fr");

    if (is_delete) {
        for (std::size_t i = 1; i < argv.size(); ++i) {
            const auto& arg = argv[i];
            if (arg.empty() || arg[0] == '-' || arg[0] == '/') continue;
            if (!paths_) break;
            const auto decision = paths_->evaluate(arg, PathAccess::Delete);
            if (!decision.allowed) {
                result.decision = CommandDecision::Block;
                result.rule_id = "fs.delete-outside-workspace";
                result.reason = "recursive deletion outside the workspace: " + decision.reason;
                result.matched_text = arg;
                return result;
            }
        }
        if (is_recursive) {
            result.decision = CommandDecision::Allow;
            result.rule_id = "fs.delete-inside-workspace";
            result.reason = "recursive delete confined to the workspace";
            return result;
        }
    }

    // Anything that reaches here has no rule. What happens next is the user's
    // configured autonomy level, not the model's opinion.
    switch (autonomy_) {
        case AutonomyLevel::Restricted:
            result.decision = CommandDecision::Confirm;
            result.reason = "unrecognised command, and autonomy is set to restricted";
            break;
        case AutonomyLevel::Standard:
            result.decision = CommandDecision::Allow;
            result.reason = "unrecognised command allowed inside the selected workspace";
            break;
        case AutonomyLevel::Full:
            result.decision = CommandDecision::Allow;
            result.reason = "autonomy is set to full";
            break;
    }
    result.rule_id = "default";
    return result;
}

CommandEvaluation CommandPolicy::evaluate(const ProcessRequest& request) const {
    std::vector<std::string> argv;
    argv.push_back(request.executable.string());
    argv.insert(argv.end(), request.arguments.begin(), request.arguments.end());

    // Running an interpreter with an inline script means the real command is in
    // the argument, so classify that text too.
    const auto exe = executable_key(argv[0]);
    const bool is_shell = exe == "cmd" || exe == "powershell" || exe == "pwsh" ||
                          exe == "bash" || exe == "sh" || exe == "zsh";
    auto result = classify_tokens(argv);
    if (is_shell && result.decision != CommandDecision::Block) {
        for (std::size_t i = 1; i < argv.size(); ++i) {
            const auto lower = strings::to_lower(argv[i]);
            if (lower == "-c" || lower == "/c" || lower == "/k" || lower == "-command" ||
                lower == "-noprofile" || lower == "-nologo") {
                continue;
            }
            if (argv[i].size() > 2 && argv[i][0] != '-' && argv[i][0] != '/') {
                auto inner = evaluate_shell(argv[i]);
                if (strictest(result.decision, inner.decision) == inner.decision &&
                    inner.decision != result.decision) {
                    result = inner;
                }
            }
        }
    }

    log_debug(LogChannel::Security, "command classified",
              {{"exe", exe},
               {"decision", std::string{to_string(result.decision)}},
               {"rule", result.rule_id}});
    return result;
}

CommandEvaluation CommandPolicy::evaluate_shell(std::string_view command_line) const {
    CommandEvaluation worst;
    worst.decision = CommandDecision::Allow;
    worst.rule_id = "default";
    worst.reason = "no segment matched a restricting rule";

    // Some shapes are only visible in the whole line: piping a download into a
    // shell is dangerous precisely *because* of the pipe, and splitting on the
    // pipe first would hide it. Check those patterns before segmenting.
    {
        const auto lowered = strings::to_lower(command_line);
        for (const auto& rule : rules_) {
            if (!rule.enabled || rule.decision != CommandDecision::Block) continue;
            if (!rule.executables.empty()) continue;   // executable rules are per-segment
            for (const auto& pattern : rule.patterns) {
                if (pattern.find('|') == std::string::npos) continue;
                if (strings::glob_match(pattern, lowered)) {
                    CommandEvaluation blocked;
                    blocked.decision = CommandDecision::Block;
                    blocked.rule_id = rule.id;
                    blocked.reason = rule.reason;
                    blocked.matched_text = std::string{command_line};
                    return blocked;
                }
            }
        }
    }

    const auto segments = split_shell_segments(command_line);
    if (segments.empty()) {
        worst.decision = CommandDecision::Block;
        worst.reason = "empty command";
        return worst;
    }

    for (const auto& segment : segments) {
        const auto argv = tokenize(segment);
        if (argv.empty()) continue;
        auto evaluation = classify_tokens(argv);
        if (strictest(worst.decision, evaluation.decision) == evaluation.decision &&
            evaluation.decision != worst.decision) {
            worst = evaluation;
            worst.matched_text = segment;
        }
        if (worst.decision == CommandDecision::Block) break;
    }
    return worst;
}

}  // namespace localcode
