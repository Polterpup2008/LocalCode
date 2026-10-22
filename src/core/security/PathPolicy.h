#pragma once
// ---------------------------------------------------------------------------
// Filesystem access policy.
//
// The distinction this class exists to enforce:
//
//   * READ is broad. An agent legitimately reads system headers, the Windows
//     SDK, a sibling checkout, its own logs. Blocking that makes it useless.
//   * WRITE is narrow. Only the selected workspace (plus explicitly granted
//     extra roots) may be modified. Everything else is refused.
//   * The EXECUTABLE a command runs is unrelated to either. cmake.exe living in
//     C:\Program Files while the workspace is D:\Projects\Game is the normal
//     case, not a violation. See CommandPolicy for that half.
//
// Containment is decided on *canonical* paths only. On Windows canonicalisation
// goes through GetFinalPathNameByHandleW, which resolves symlinks, NTFS
// junctions and 8.3 short names - a lexical check alone is escapable with a
// junction planted inside the workspace.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../util/Result.h"

namespace localcode {

enum class PathAccess {
    Read,      // open for reading
    Write,     // modify an existing file
    Create,    // create a new file or directory
    Delete,    // remove a file or directory
};

std::string_view to_string(PathAccess access) noexcept;

struct PathDecision {
    bool                  allowed = false;
    std::filesystem::path canonical;      // resolved absolute path
    std::filesystem::path relative;       // relative to the workspace, when inside
    bool                  inside_workspace = false;
    std::string           reason;         // why it was refused
    // Set when an absolute path naming a conventional project root - the
    // /workspace, /app, /repo of a training corpus - was read as workspace-
    // relative. Holds the path as it was written, so the tool can say what it
    // did rather than silently landing the write somewhere else.
    std::string           reinterpreted_from;
};

class PathPolicy {
public:
    PathPolicy();
    explicit PathPolicy(std::filesystem::path workspace_root);

    void set_workspace_root(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& workspace_root() const { return workspace_; }

    // Additional roots the agent may write to (build output outside the tree,
    // a sibling library the user explicitly granted). Must be set by a human.
    void add_writable_root(std::filesystem::path root);
    void clear_writable_roots();
    [[nodiscard]] const std::vector<std::filesystem::path>& writable_roots() const {
        return writable_roots_;
    }

    // When false, reads are also confined to the workspace and granted roots.
    void set_allow_read_outside_workspace(bool allow) { allow_read_outside_ = allow; }
    [[nodiscard]] bool allow_read_outside_workspace() const { return allow_read_outside_; }

    // Resolves a model-supplied path (absolute or workspace-relative) and
    // decides whether the requested access is permitted.
    [[nodiscard]] PathDecision evaluate(std::string_view input, PathAccess access) const;

    // Convenience wrapper: the canonical path, or a PermissionDenied error.
    [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view input,
                                                        PathAccess access) const;

    // True when `candidate` is `root` or lives beneath it. Both must already be
    // canonical; comparison is component-wise and case-insensitive on Windows.
    [[nodiscard]] static bool is_within(const std::filesystem::path& root,
                                        const std::filesystem::path& candidate);

    // Canonicalises a path that need not exist: the deepest existing ancestor is
    // resolved through the OS, the remaining components are appended lexically
    // after "." and ".." have been folded away.
    [[nodiscard]] static std::filesystem::path canonicalize(const std::filesystem::path& input);

    // Rejects syntactically dangerous inputs before they ever reach the OS:
    // embedded NUL, NTFS alternate data streams, Windows reserved device names,
    // components with trailing dots or spaces, and \\?\ / \\.\ device prefixes.
    [[nodiscard]] static std::string validate_syntax(std::string_view input);

    // Locations that are never writable regardless of configuration.
    [[nodiscard]] static const std::vector<std::filesystem::path>& protected_roots();

private:
    [[nodiscard]] bool is_writable_location(const std::filesystem::path& canonical) const;

    std::filesystem::path              workspace_;
    std::vector<std::filesystem::path> writable_roots_;
    bool                               allow_read_outside_ = true;
};

}  // namespace localcode
