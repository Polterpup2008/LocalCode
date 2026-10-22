#include "PathPolicy.h"
#include "../platform/PathEncoding.h"

#include <algorithm>
#include <cstdlib>
#include <system_error>

#include "../util/Log.h"
#include "../util/Strings.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace localcode {
namespace {

bool component_equal(const std::filesystem::path& a, const std::filesystem::path& b) {
#if defined(_WIN32)
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}

// Folds "." and ".." without touching the filesystem. Used before the OS call so
// a traversal attempt can be reported even when the target does not exist.
std::filesystem::path lexical_normalize(const std::filesystem::path& input) {
    std::filesystem::path result;
    if (input.has_root_name()) result = input.root_name();
    if (input.has_root_directory()) result /= input.root_directory();

    std::vector<std::filesystem::path> parts;
    for (const auto& part : input.relative_path()) {
        if (part == "." || part.empty()) continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!input.has_root_directory()) {
                parts.push_back(part);   // keep a leading ".." on relative paths
            }
            // A ".." at the root of an absolute path is absorbed, matching the OS.
            continue;
        }
        parts.push_back(part);
    }
    for (const auto& part : parts) result /= part;
    return result;
}

#if defined(_WIN32)
// Resolves reparse points (symlinks, junctions), 8.3 short names and drive
// substitutions for a path that exists.
std::filesystem::path final_path_of_existing(const std::filesystem::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};

    std::wstring buffer(1024, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(),
                                             static_cast<DWORD>(buffer.size()), VOLUME_NAME_DOS);
    if (length > buffer.size()) {
        buffer.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(),
                                           static_cast<DWORD>(buffer.size()), VOLUME_NAME_DOS);
    }
    CloseHandle(handle);
    if (length == 0) return {};
    buffer.resize(length);

    // GetFinalPathNameByHandleW returns the \\?\ form; strip it so paths stay
    // comparable with everything else in the application.
    std::wstring_view view{buffer};
    if (view.rfind(LR"(\\?\UNC\)", 0) == 0) {
        return std::filesystem::path{std::wstring{LR"(\\)"} + std::wstring{view.substr(8)}};
    }
    if (view.rfind(LR"(\\?\)", 0) == 0) view.remove_prefix(4);
    return std::filesystem::path{std::wstring{view}};
}

bool is_reserved_device_name(std::string_view component) {
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                      "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                      "LPT7", "LPT8", "LPT9"};
    // A device name is reserved with or without an extension: "NUL.txt" is NUL.
    auto stem = component.substr(0, component.find('.'));
    for (const char* name : kReserved) {
        if (strings::iequals(stem, name)) return true;
    }
    return false;
}
#endif

std::filesystem::path env_path(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
    std::filesystem::path result{buffer};
    std::free(buffer);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::filesystem::path{value} : std::filesystem::path{};
#endif
}

// One virtual prefix, and only one: "/workspace".
//
// Models trained in containers learned that the project lives at /workspace,
// and they emit it even while reasoning correctly about the real root. This
// rewrites that one literal prefix and nothing else. It is deliberately not a
// family of guesses: /app, /repo, /code and the rest are ordinary directory
// names that a user could legitimately mean, and rewriting those would make an
// absolute path mean something other than what it says.
//
// Returns the part after the prefix, or empty when the path is not one. An
// empty return is also how "there is a real /workspace on this machine" is
// reported: then it is a real path and keeps its ordinary meaning.
std::string virtual_workspace_prefix(std::string_view input) {
    constexpr std::string_view kPrefix = "/workspace";

    // Backslashes normalised so \workspace\x is the same request, and case
    // folded because /Workspace is the same convention shouting.
    std::string text{input};
    std::replace(text.begin(), text.end(), '\\', '/');
    const auto lowered = strings::to_lower(text);

    if (lowered.size() < kPrefix.size()) return {};
    if (lowered.compare(0, kPrefix.size(), kPrefix) != 0) return {};
    // Exactly the prefix, or the prefix followed by a separator - so
    // /workspaces and /workspace-old are not matched.
    if (lowered.size() != kPrefix.size() && lowered[kPrefix.size()] != '/') return {};

    // On a machine that really has /workspace, it is a real directory.
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path{kPrefix}, ec) && !ec) return {};

    auto remainder = text.substr(kPrefix.size());
    while (!remainder.empty() && remainder.front() == '/') remainder.erase(0, 1);
    // The prefix alone names the workspace root.
    return remainder.empty() ? "." : remainder;
}

}  // namespace

std::string_view to_string(PathAccess access) noexcept {
    switch (access) {
        case PathAccess::Read:   return "read";
        case PathAccess::Write:  return "write";
        case PathAccess::Create: return "create";
        case PathAccess::Delete: return "delete";
    }
    return "read";
}

PathPolicy::PathPolicy() = default;

PathPolicy::PathPolicy(std::filesystem::path workspace_root) {
    set_workspace_root(std::move(workspace_root));
}

void PathPolicy::set_workspace_root(std::filesystem::path root) {
    workspace_ = root.empty() ? std::filesystem::path{} : canonicalize(root);
}

void PathPolicy::add_writable_root(std::filesystem::path root) {
    auto canonical = canonicalize(root);
    if (canonical.empty()) return;
    if (std::find(writable_roots_.begin(), writable_roots_.end(), canonical) ==
        writable_roots_.end()) {
        writable_roots_.push_back(std::move(canonical));
    }
}

void PathPolicy::clear_writable_roots() { writable_roots_.clear(); }

std::string PathPolicy::validate_syntax(std::string_view input) {
    if (input.empty()) return "path is empty";
    if (input.find('\0') != std::string_view::npos) return "path contains a NUL byte";
    if (!strings::is_valid_utf8(input)) return "path must be valid UTF-8";

#if defined(_WIN32)
    // Device namespace prefixes bypass normal path parsing entirely.
    if (strings::starts_with(input, R"(\\?\)") || strings::starts_with(input, R"(\\.\)")) {
        return "device-namespace paths (\\\\?\\ and \\\\.\\) are not accepted";
    }
    if (strings::starts_with(input, R"(\\)") || strings::starts_with(input, "//")) {
        return "UNC network paths are not accepted";
    }

    const std::string normalized = strings::replace_all(input, "\\", "/");
    const auto parts = strings::split(normalized, '/', false);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto& part = parts[i];
        if (part.empty()) continue;

        // A colon is legal only as the drive separator in the first component.
        const auto colon = part.find(':');
        if (colon != std::string::npos) {
            const bool is_drive_spec = (i == 0 && colon == 1 &&
                                        std::isalpha(static_cast<unsigned char>(part[0])) &&
                                        part.size() >= 2);
            if (!is_drive_spec) {
                return "NTFS alternate data streams are not accepted: " + part;
            }
        }
        if (part == "." || part == "..") continue;
        // Win32 silently strips trailing dots and spaces, so "workspace/.. ."
        // would resolve outside. Refuse them instead.
        if (part.back() == ' ' || part.back() == '.') {
            return "path component has a trailing dot or space: " + part;
        }
        if (is_reserved_device_name(part)) {
            return "reserved Windows device name: " + part;
        }
        if (part.find_first_of("<>|\"*?") != std::string::npos) {
            return "path contains characters that are invalid on Windows: " + part;
        }
    }
#endif
    return {};
}

std::filesystem::path PathPolicy::canonicalize(const std::filesystem::path& input) {
    if (input.empty()) return {};

    std::error_code ec;
    std::filesystem::path absolute =
        input.is_absolute() ? input : std::filesystem::absolute(input, ec);
    if (ec) absolute = input;

    absolute = lexical_normalize(absolute);

#if defined(_WIN32)
    // Resolve the deepest existing ancestor through the OS, then re-append the
    // components that do not exist yet. This defeats junctions planted anywhere
    // along the path while still allowing "create this new file" to be checked.
    std::vector<std::filesystem::path> tail;
    std::filesystem::path probe = absolute;
    while (!probe.empty()) {
        auto resolved = final_path_of_existing(probe);
        if (!resolved.empty()) {
            for (auto it = tail.rbegin(); it != tail.rend(); ++it) resolved /= *it;
            return lexical_normalize(resolved);
        }
        if (!probe.has_relative_path() || probe == probe.parent_path()) break;
        tail.push_back(probe.filename());
        probe = probe.parent_path();
    }
    return absolute;
#else
    auto resolved = std::filesystem::weakly_canonical(absolute, ec);
    return ec ? absolute : lexical_normalize(resolved);
#endif
}

bool PathPolicy::is_within(const std::filesystem::path& root,
                           const std::filesystem::path& candidate) {
    if (root.empty()) return false;

    auto root_it = root.begin();
    auto cand_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++cand_it) {
        // Trailing empty component produced by a directory separator.
        if (root_it->empty() && std::next(root_it) == root.end()) break;
        if (cand_it == candidate.end()) return false;
        if (!component_equal(*root_it, *cand_it)) return false;
    }
    return true;
}

const std::vector<std::filesystem::path>& PathPolicy::protected_roots() {
    static const std::vector<std::filesystem::path> kRoots = [] {
        std::vector<std::filesystem::path> roots;
        auto push = [&roots](std::filesystem::path p) {
            if (p.empty()) return;
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) roots.push_back(canonicalize(p));
        };
#if defined(_WIN32)
        push(env_path("SystemRoot"));
        push(env_path("ProgramFiles"));
        push(env_path("ProgramFiles(x86)"));
        push(env_path("ProgramData"));
        const auto profile = env_path("USERPROFILE");
        if (!profile.empty()) {
            push(profile / ".ssh");
            push(profile / ".aws");
            push(profile / ".gnupg");
            push(profile / "AppData" / "Roaming" / "Microsoft" / "Windows" / "Start Menu");
        }
#else
        push("/etc");
        push("/bin");
        push("/sbin");
        push("/usr/bin");
        push("/usr/sbin");
        push("/boot");
        push("/sys");
        push("/proc");
        const auto home = env_path("HOME");
        if (!home.empty()) {
            push(home / ".ssh");
            push(home / ".aws");
            push(home / ".gnupg");
        }
#endif
        return roots;
    }();
    return kRoots;
}

bool PathPolicy::is_writable_location(const std::filesystem::path& canonical) const {
    for (const auto& root : protected_roots()) {
        if (is_within(root, canonical)) return false;
    }
    if (!workspace_.empty() && is_within(workspace_, canonical)) return true;
    for (const auto& root : writable_roots_) {
        if (is_within(root, canonical)) return true;
    }
    return false;
}

PathDecision PathPolicy::evaluate(std::string_view input, PathAccess access) const {
    PathDecision decision;

    if (auto problem = validate_syntax(input); !problem.empty()) {
        decision.reason = problem;
        return decision;
    }

    auto requested = path_from_utf8(input);

    // Checked before is_absolute, which on Windows is false for "/workspace/x"
    // - it has a root directory but no drive - so the placeholder would
    // otherwise be appended to the workspace and land at C:/workspace/x.
    const auto virtual_root = workspace_.empty() ? std::string{}
                                                : virtual_workspace_prefix(input);

    if (!virtual_root.empty()) {
        // Models name the project root /workspace - the mount point of the
        // containers their training data was produced in. It is a naming
        // convention, not an attempt to leave the sandbox, and it is the
        // single most persistent wrong prior measured here: three runs died
        // on it.
        //
        // Refusing did not work, even with the real root as the first line of
        // the refusal. Run 11 is the proof: the model wrote "The workspace is
        // a temp directory, not /workspace. Let me write the file there" and
        // then emitted /workspace/fibonacci.cpp again - twice. Its prose was
        // right every time and only the generated argument was wrong, and
        // each refusal left another copy of the wrong path in the context for
        // the next one to copy.
        //
        // So this is resolved rather than refused, and the result says it was.
        // Only when nothing of that name exists: a real /workspace on a Linux
        // machine is a real directory and keeps its ordinary meaning.
        decision.reinterpreted_from = std::string{input};
        requested = workspace_ / path_from_utf8(virtual_root);
    } else if (!requested.is_absolute()) {
        if (workspace_.empty()) {
            decision.reason =
                "relative path given but no workspace is selected: " + std::string{input};
            return decision;
        }
        requested = workspace_ / requested;
    }

    decision.canonical = canonicalize(requested);
    if (decision.canonical.empty()) {
        decision.reason = "path could not be resolved: " + std::string{input};
        return decision;
    }

    if (!workspace_.empty() && is_within(workspace_, decision.canonical)) {
        decision.inside_workspace = true;
        std::error_code ec;
        decision.relative = std::filesystem::relative(decision.canonical, workspace_, ec);
        if (ec) decision.relative.clear();
    }

    if (access == PathAccess::Read) {
        if (decision.inside_workspace || allow_read_outside_) {
            decision.allowed = true;
        } else {
            bool granted = false;
            for (const auto& root : writable_roots_) {
                if (is_within(root, decision.canonical)) { granted = true; break; }
            }
            decision.allowed = granted;
            if (!granted) {
                decision.reason = "reads are confined to the workspace: " +
                                  path_to_utf8(decision.canonical);
            }
        }
        return decision;
    }

    if (workspace_.empty()) {
        decision.reason = "no workspace is selected, so nothing may be modified";
        return decision;
    }

    if (is_writable_location(decision.canonical)) {
        decision.allowed = true;
        return decision;
    }

    decision.reason = "writes are confined to the workspace (" + path_to_utf8(workspace_) +
                      "); refused " + std::string{to_string(access)} + " of " +
                      path_to_utf8(decision.canonical);
    return decision;
}

Result<std::filesystem::path> PathPolicy::resolve(std::string_view input,
                                                  PathAccess access) const {
    auto decision = evaluate(input, access);
    if (!decision.allowed) {
        log_warn(LogChannel::Security, "path refused",
                 {{"path", std::string{input}},
                  {"access", std::string{to_string(access)}},
                  {"reason", decision.reason}});
        return make_error(ErrorCode::PermissionDenied, decision.reason);
    }
    return decision.canonical;
}

}  // namespace localcode
