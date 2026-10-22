#include "GitService.h"
#include "../platform/PathEncoding.h"

#include <algorithm>
#include <charconv>

#include "../platform/Process.h"
#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

GitChange change_from_code(char code) {
    switch (code) {
        case 'M': return GitChange::Modified;
        case 'A': return GitChange::Added;
        case 'D': return GitChange::Deleted;
        case 'R': return GitChange::Renamed;
        case 'C': return GitChange::Copied;
        case 'U': return GitChange::Conflicted;
        case '?': return GitChange::Untracked;
        case '!': return GitChange::Ignored;
        case '.':
        case ' ':
        default: return GitChange::Unmodified;
    }
}

int parse_int(std::string_view text) {
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

}   // namespace

std::string_view to_string(GitChange change) noexcept {
    switch (change) {
        case GitChange::Unmodified: return "unmodified";
        case GitChange::Modified: return "modified";
        case GitChange::Added: return "added";
        case GitChange::Deleted: return "deleted";
        case GitChange::Renamed: return "renamed";
        case GitChange::Copied: return "copied";
        case GitChange::Untracked: return "untracked";
        case GitChange::Ignored: return "ignored";
        case GitChange::Conflicted: return "conflicted";
    }
    return "unmodified";
}

std::string GitFileStatus::describe() const {
    if (conflicted) return "conflicted";
    if (untracked) return "untracked";
    std::string out;
    if (staged()) out += std::string{"staged "} + std::string{to_string(index_change)};
    if (worktree_change != GitChange::Unmodified) {
        if (!out.empty()) out += ", ";
        out += to_string(worktree_change);
    }
    return out.empty() ? "unmodified" : out;
}

const GitFileStatus* GitStatus::file(std::string_view path) const {
    for (const auto& entry : files) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

std::vector<std::string> GitStatus::dirty_paths() const {
    std::vector<std::string> paths;
    for (const auto& entry : files) {
        if (entry.dirty() || entry.staged()) paths.push_back(entry.path);
    }
    return paths;
}

// ---------------------------------------------------------------------------
bool GitService::available() {
    if (!looked_up_) {
        looked_up_ = true;
        if (auto found = find_executable("git")) git_ = *found;
    }
    return !git_.empty();
}

std::string GitService::version() {
    if (!available()) return {};
    if (!version_.empty()) return version_;

    ProcessRequest request;
    request.executable = git_;
    request.arguments = {"--version"};
    request.timeout = std::chrono::seconds(15);
    auto result = ProcessRunner::run(request);
    if (result && result->exit_code == 0) {
        version_ = strings::trim(result->stdout_text);
    }
    return version_;
}

Result<std::string> GitService::run(const std::filesystem::path& repository,
                                    std::vector<std::string> arguments, CancellationToken cancel,
                                    std::chrono::milliseconds timeout) {
    if (!available()) {
        return make_error(ErrorCode::NotFound, "git is not installed or not on PATH",
                          "Install Git to enable the repository panel and the git tools.");
    }

    ProcessRequest request;
    request.executable = git_;
    // Inspection must not launch a repository's fsmonitor hook or pager. These
    // subprocesses would bypass the command policy on otherwise read-only tools.
    request.arguments = {"--no-pager",     "-c", "core.fsmonitor=false", "-c",
                         "color.ui=false", "-c", "core.quotepath=false"};
    request.arguments.insert(request.arguments.end(), arguments.begin(), arguments.end());
    request.working_directory = repository;
    request.timeout = timeout;
    request.max_captured_bytes = 16u * 1024 * 1024;
    // Stop git from opening a credential prompt or a pager behind the GUI.
    request.environment = {{"GIT_TERMINAL_PROMPT", "0"},
                           {"GIT_PAGER", "cat"},
                           {"GIT_OPTIONAL_LOCKS", "0"},
                           {"LC_ALL", "C"}};

    auto result = ProcessRunner::run(request, cancel);
    if (!result) return result.error();
    if (result->cancelled) return make_error(ErrorCode::Cancelled, "git command cancelled");
    if (result->timed_out) {
        return make_error(ErrorCode::Timeout, "git command timed out",
                          ProcessRunner::display_command(request));
    }
    if (result->exit_code != 0) {
        return make_error(ErrorCode::BackendError,
                          "git exited with code " + std::to_string(result->exit_code),
                          strings::utf8_truncate(strings::trim(result->stderr_text), 600));
    }
    return result->stdout_text;
}

Result<std::filesystem::path> GitService::repository_root(const std::filesystem::path& path) {
    auto output = run(path, {"rev-parse", "--show-toplevel"}, {}, std::chrono::seconds(20));
    if (!output) return output.error();
    const auto trimmed = strings::trim(*output);
    if (trimmed.empty()) {
        return make_error(ErrorCode::NotFound,
                          "not inside a git repository: " + path_to_utf8(path));
    }
    return path_from_utf8(trimmed);
}

GitStatus GitService::parse_status_porcelain_v2(std::string_view text) {
    GitStatus status;
    status.is_repository = true;

    for (const auto& line : strings::split_lines(text)) {
        if (line.empty()) continue;

        // Header lines: "# branch.head main"
        if (line[0] == '#') {
            const auto parts = strings::split(line, ' ', false);
            if (parts.size() >= 3) {
                if (parts[1] == "branch.head") {
                    status.branch = parts[2];
                    status.detached_head = parts[2] == "(detached)";
                } else if (parts[1] == "branch.oid") {
                    status.head_commit = parts[2];
                } else if (parts[1] == "branch.upstream") {
                    status.upstream = parts[2];
                } else if (parts[1] == "branch.ab" && parts.size() >= 4) {
                    // "+3 -2"
                    status.ahead = parse_int(std::string_view{parts[2]}.substr(1));
                    status.behind = parse_int(std::string_view{parts[3]}.substr(1));
                }
            }
            continue;
        }

        GitFileStatus entry;
        switch (line[0]) {
            case '1': {
                // 1 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <path>
                const auto parts = strings::split(line, ' ', false);
                if (parts.size() < 9) continue;
                entry.index_change = change_from_code(parts[1][0]);
                entry.worktree_change = change_from_code(parts[1].size() > 1 ? parts[1][1] : '.');
                // The path is everything from field 9 onward (it may contain spaces).
                const auto position = line.find(parts[8]);
                entry.path = position == std::string::npos ? parts[8] : line.substr(position);
                break;
            }
            case '2': {
                // 2 <XY> <sub> <mH> <mI> <mW> <hH> <hI> <X><score> <path>\t<origPath>
                const auto parts = strings::split(line, ' ', false);
                if (parts.size() < 10) continue;
                entry.index_change = change_from_code(parts[1][0]);
                entry.worktree_change = change_from_code(parts[1].size() > 1 ? parts[1][1] : '.');
                const auto position = line.find(parts[9]);
                auto remainder = position == std::string::npos ? parts[9] : line.substr(position);
                const auto tab = remainder.find('\t');
                if (tab != std::string::npos) {
                    entry.path = remainder.substr(0, tab);
                    entry.original_path = remainder.substr(tab + 1);
                } else {
                    entry.path = remainder;
                }
                break;
            }
            case 'u': {
                const auto parts = strings::split(line, ' ', false);
                if (parts.size() < 11) continue;
                entry.conflicted = true;
                entry.index_change = GitChange::Conflicted;
                entry.worktree_change = GitChange::Conflicted;
                const auto position = line.find(parts[10]);
                entry.path = position == std::string::npos ? parts[10] : line.substr(position);
                break;
            }
            case '?': {
                entry.untracked = true;
                entry.worktree_change = GitChange::Untracked;
                entry.path = line.size() > 2 ? line.substr(2) : "";
                break;
            }
            case '!': continue;   // ignored files are not requested
            default: continue;
        }

        if (entry.path.empty()) continue;
        entry.path = strings::replace_all(entry.path, "\\", "/");
        entry.original_path = strings::replace_all(entry.original_path, "\\", "/");
        status.files.push_back(std::move(entry));
    }

    status.clean = status.files.empty();
    return status;
}

Result<GitStatus> GitService::status(const std::filesystem::path& repository,
                                     CancellationToken cancel) {
    auto root = repository_root(repository);
    if (!root) {
        GitStatus status;
        status.is_repository = false;
        return status;
    }

    auto output =
        run(*root, {"status", "--porcelain=v2", "--branch", "--untracked-files=all"}, cancel);
    if (!output) return output.error();

    auto status = parse_status_porcelain_v2(*output);
    status.root = *root;
    return status;
}

DiffResult GitService::parse_unified_diff(std::string_view text) {
    DiffResult result;
    result.raw = std::string{text};

    FileDiff current;
    bool have_file = false;
    bool bare_file = false;
    bool malformed_bare = false;
    int old_left = 0, new_left = 0;
    const auto invalid = [&] {
        DiffResult rejected;
        rejected.raw = result.raw;
        return rejected;
    };
    const auto header_path = [](const std::string& header, const char* prefix) {
        auto path = header.substr(4, header.find('\t', 4) - 4);
        if (strings::starts_with(path, prefix)) path.erase(0, 2);
        if (std::any_of(path.begin(), path.end(),
                        [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; }))
            return std::string{};
        return path;
    };
    const auto bare_hunk_header = [](const std::string& line, DiffHunk& hunk) {
        const auto range = [](std::string_view text, int& start, int& count) {
            const auto comma = text.find(',');
            const auto number = [](std::string_view value, int& output) {
                if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
                        return c >= '0' && c <= '9';
                    }))
                    return false;
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), output);
                return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
            };
            count = 1;
            return number(text.substr(0, comma), start) &&
                   (comma == std::string_view::npos || number(text.substr(comma + 1), count)) &&
                   (count == 0 || start > 0);
        };
        if (!strings::starts_with(line, "@@ -")) return false;
        const auto middle = line.find(' ', 4);
        if (middle == std::string::npos || line.compare(middle, 2, " +") != 0) return false;
        const auto close = line.find(' ', middle + 2);
        if (close == std::string::npos || line.compare(close, 3, " @@") != 0 ||
            (line.size() > close + 3 && line[close + 3] != ' '))
            return false;
        return range(std::string_view{line}.substr(4, middle - 4), hunk.old_start,
                     hunk.old_count) &&
               range(std::string_view{line}.substr(middle + 2, close - middle - 2), hunk.new_start,
                     hunk.new_count) &&
               (hunk.old_count != 0 || hunk.new_count != 0);
    };

    auto flush = [&] {
        if (bare_file && (old_left != 0 || new_left != 0 || current.hunks.empty()))
            malformed_bare = true;
        if (have_file && !current.path.empty()) {
            result.total_additions += current.additions;
            result.total_deletions += current.deletions;
            result.files.push_back(std::move(current));
        }
        current = FileDiff{};
        have_file = bare_file = false;
        old_left = new_left = 0;
    };

    const auto patch_lines = strings::split_lines(text);
    for (std::size_t index = 0; index < patch_lines.size(); ++index) {
        const auto& line = patch_lines[index];
        // Counted hunk data takes precedence over header/metadata spelling.
        // A removed source line beginning "-- " is itself written "--- ".
        if (have_file && !current.hunks.empty() && (old_left > 0 || new_left > 0)) {
            if (!line.empty() && (line[0] == ' ' || line[0] == '+' || line[0] == '-')) {
                const bool consumes_old = line[0] != '+';
                const bool consumes_new = line[0] != '-';
                if (bare_file &&
                    ((consumes_old && old_left == 0) || (consumes_new && new_left == 0)))
                    return invalid();
                old_left -= consumes_old;
                new_left -= consumes_new;
                if (line[0] == '+') ++current.additions;
                if (line[0] == '-') ++current.deletions;
                current.hunks.back().lines.push_back(line);
                continue;
            }
            if (strings::starts_with(line, "\\ No newline at end of file")) {
                current.hunks.back().lines.push_back(line);
                continue;
            }
            if (bare_file) return invalid();
        }
        if (strings::starts_with(line, "--- ") && index + 1 < patch_lines.size() &&
            strings::starts_with(patch_lines[index + 1], "+++ ")) {
            if (have_file && !bare_file && current.hunks.empty()) {
                // Git extended headers already supply paths and change kind.
                ++index;
                continue;
            }
            flush();
            if (malformed_bare) return invalid();
            auto old_path = header_path(line, "a/");
            auto new_path = header_path(patch_lines[++index], "b/");
            if (old_path.empty() || new_path.empty() ||
                (old_path == "/dev/null" && new_path == "/dev/null") ||
                (old_path != "/dev/null" && new_path != "/dev/null" && old_path != new_path))
                return invalid();
            have_file = bare_file = true;
            current.old_path = old_path;
            current.path = new_path == "/dev/null" ? old_path : new_path;
            if (old_path == "/dev/null") current.change = GitChange::Added;
            if (new_path == "/dev/null") current.change = GitChange::Deleted;
            continue;
        }
        if (strings::starts_with(line, "diff --git ")) {
            flush();
            if (malformed_bare) return invalid();
            have_file = true;
            // "diff --git a/path b/path" - take the b/ side, which is the new
            // name; renames are corrected by the rename headers below.
            const auto space = line.rfind(" b/");
            if (space != std::string::npos) current.path = line.substr(space + 3);
            const auto a_start = line.find(" a/");
            if (a_start != std::string::npos && space != std::string::npos) {
                current.old_path = line.substr(a_start + 3, space - a_start - 3);
            }
            continue;
        }
        if (!have_file) continue;

        // Extended Git metadata cannot override a standard header pair.
        if (bare_file && !line.empty() && !strings::starts_with(line, "@@") &&
            !strings::starts_with(line, "\\ No newline at end of file"))
            return invalid();

        if (strings::starts_with(line, "new file mode")) {
            current.change = GitChange::Added;
            continue;
        }
        if (strings::starts_with(line, "deleted file mode")) {
            current.change = GitChange::Deleted;
            continue;
        }
        if (strings::starts_with(line, "rename from ")) {
            current.old_path = line.substr(12);
            current.change = GitChange::Renamed;
            continue;
        }
        if (strings::starts_with(line, "rename to ")) {
            current.path = line.substr(10);
            current.change = GitChange::Renamed;
            continue;
        }
        if (strings::starts_with(line, "Binary files") ||
            strings::starts_with(line, "GIT binary patch")) {
            current.binary = true;
            continue;
        }
        if (strings::starts_with(line, "--- ") || strings::starts_with(line, "+++ ")) {
            if (!have_file || bare_file || !current.hunks.empty()) return invalid();
            continue;
        }
        if (strings::starts_with(line, "@@")) {
            DiffHunk hunk;
            hunk.header = line;
            if (bare_file && !bare_hunk_header(line, hunk)) return invalid();
            // @@ -oldStart,oldCount +newStart,newCount @@ optional context
            const auto minus = line.find('-');
            const auto plus = line.find('+');
            if (minus != std::string::npos && plus != std::string::npos) {
                const auto old_spec = line.substr(minus + 1, line.find(' ', minus) - minus - 1);
                const auto new_spec = line.substr(plus + 1, line.find(' ', plus) - plus - 1);
                const auto old_parts = strings::split(old_spec, ',', false);
                const auto new_parts = strings::split(new_spec, ',', false);
                if (!old_parts.empty()) hunk.old_start = parse_int(old_parts[0]);
                hunk.old_count = old_parts.size() > 1 ? parse_int(old_parts[1]) : 1;
                if (!new_parts.empty()) hunk.new_start = parse_int(new_parts[0]);
                hunk.new_count = new_parts.size() > 1 ? parse_int(new_parts[1]) : 1;
            }
            old_left = hunk.old_count;
            new_left = hunk.new_count;
            current.hunks.push_back(std::move(hunk));
            continue;
        }

        if (bare_file) {
            if (line.empty() || strings::starts_with(line, "\\ No newline at end of file"))
                continue;
            return invalid();
        }
        if (current.hunks.empty()) continue;
        if (!line.empty()) {
            if (line[0] == '+')
                ++current.additions;
            else if (line[0] == '-')
                ++current.deletions;
            else if (line[0] != ' ' && line[0] != '\\')
                continue;
        }
        current.hunks.back().lines.push_back(line);
    }
    flush();
    if (malformed_bare) return invalid();
    return result;
}

Result<DiffResult> GitService::diff(const std::filesystem::path& repository,
                                    const std::vector<std::string>& paths, bool staged,
                                    int context_lines, CancellationToken cancel) {
    auto root = repository_root(repository);
    if (!root) return root.error();

    std::vector<std::string> arguments = {
        "diff",          "--no-color",     "--no-ext-diff",
        "--no-textconv", "--find-renames", "--unified=" + std::to_string(context_lines)};
    if (staged) arguments.push_back("--cached");
    if (!paths.empty()) {
        arguments.push_back("--");
        arguments.insert(arguments.end(), paths.begin(), paths.end());
    }

    auto output = run(*root, arguments, cancel, std::chrono::seconds(120));
    if (!output) return output.error();

    auto result = parse_unified_diff(*output);

    // An untracked file has no diff at all, which reads as "nothing changed"
    // when the agent just created it. Synthesise an add-everything diff so new
    // files show up in the review panel like any other change.
    if (!staged) {
        auto current_status = status(*root, cancel);
        if (current_status) {
            for (const auto& entry : current_status->files) {
                if (!entry.untracked) continue;
                if (!paths.empty() &&
                    std::find(paths.begin(), paths.end(), entry.path) == paths.end()) {
                    continue;
                }
                FileDiff file;
                file.path = entry.path;
                file.change = GitChange::Added;
                file.hunks.push_back(DiffHunk{});
                file.hunks.back().header = "@@ untracked file @@";
                result.files.push_back(std::move(file));
            }
        }
    }
    return result;
}

Result<DiffResult> GitService::diff_against(const std::filesystem::path& repository,
                                            const std::string& revision,
                                            const std::vector<std::string>& paths,
                                            CancellationToken cancel) {
    auto root = repository_root(repository);
    if (!root) return root.error();

    std::vector<std::string> arguments = {"diff",          "--no-color",     "--no-ext-diff",
                                          "--no-textconv", "--find-renames", revision};
    if (!paths.empty()) {
        arguments.push_back("--");
        arguments.insert(arguments.end(), paths.begin(), paths.end());
    }
    auto output = run(*root, arguments, cancel, std::chrono::seconds(120));
    if (!output) return output.error();
    return parse_unified_diff(*output);
}

std::vector<GitCommit> GitService::parse_log(std::string_view text) {
    // Records are separated by \x1e and fields by \x1f, so subjects containing
    // newlines or tabs cannot corrupt the parse.
    std::vector<GitCommit> commits;
    for (const auto& record : strings::split(text, '\x1e', false)) {
        const auto trimmed = strings::trim(record);
        if (trimmed.empty()) continue;
        const auto fields = strings::split(trimmed, '\x1f', true);
        if (fields.size() < 6) continue;

        GitCommit commit;
        commit.hash = strings::trim(fields[0]);
        commit.short_hash = fields[1];
        commit.author_name = fields[2];
        commit.author_email = fields[3];
        commit.timestamp_unix = parse_int(fields[4]);
        commit.subject = fields[5];
        if (fields.size() > 6) commit.body = strings::trim(fields[6]);
        if (fields.size() > 7) commit.parents = strings::split(fields[7], ' ', false);
        commits.push_back(std::move(commit));
    }
    return commits;
}

Result<std::vector<GitCommit>> GitService::log(const std::filesystem::path& repository,
                                               int max_count, const std::string& path_filter,
                                               CancellationToken cancel) {
    auto root = repository_root(repository);
    if (!root) return root.error();

    std::vector<std::string> arguments = {
        "log", "--max-count=" + std::to_string(max_count),
        "--format=%H%x1f%h%x1f%an%x1f%ae%x1f%at%x1f%s%x1f%b%x1f%P%x1e"};
    if (!path_filter.empty()) {
        arguments.push_back("--");
        arguments.push_back(path_filter);
    }

    auto output = run(*root, arguments, cancel);
    if (!output) return output.error();
    return parse_log(*output);
}

Result<std::vector<GitBranch>> GitService::branches(const std::filesystem::path& repository,
                                                    bool include_remote, CancellationToken cancel) {
    auto root = repository_root(repository);
    if (!root) return root.error();

    std::vector<std::string> arguments = {
        "for-each-ref",
        "--format=%(refname:short)%x1f%(HEAD)%x1f%(upstream:short)%x1f%(objectname)", "refs/heads"};
    if (include_remote) arguments.push_back("refs/remotes");

    auto output = run(*root, arguments, cancel);
    if (!output) return output.error();

    std::vector<GitBranch> branches;
    for (const auto& line : strings::split_lines(*output)) {
        if (strings::trim(line).empty()) continue;
        const auto fields = strings::split(line, '\x1f', true);
        if (fields.empty()) continue;
        GitBranch branch;
        branch.name = fields[0];
        branch.current = fields.size() > 1 && strings::trim(fields[1]) == "*";
        if (fields.size() > 2) branch.upstream = fields[2];
        if (fields.size() > 3) branch.head_commit = fields[3];
        branch.remote = strings::starts_with(branch.name, "origin/") ||
                        branch.name.find('/') != std::string::npos;
        branches.push_back(std::move(branch));
    }
    return branches;
}

Result<std::string> GitService::show_file(const std::filesystem::path& repository,
                                          const std::string& revision, const std::string& path,
                                          CancellationToken cancel) {
    auto root = repository_root(repository);
    if (!root) return root.error();
    return run(*root, {"show", revision + ":" + path}, cancel);
}

Result<std::string> GitService::head_commit(const std::filesystem::path& repository,
                                            CancellationToken cancel) {
    auto root = repository_root(repository);
    if (!root) return root.error();
    auto output = run(*root, {"rev-parse", "HEAD"}, cancel, std::chrono::seconds(20));
    if (!output) return output.error();
    return strings::trim(*output);
}

}   // namespace localcode
