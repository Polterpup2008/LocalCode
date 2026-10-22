#pragma once
// ---------------------------------------------------------------------------
// Filesystem access used by the tools.
//
// Everything here works on already-validated absolute paths: the security layer
// (PathPolicy) decides what may be touched, this layer decides how. It handles
// the things that actually break agents in the field - BOMs, CRLF, UTF-16 source
// files, binary blobs, files that vanish mid-read and paths beyond MAX_PATH.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../util/Result.h"

namespace localcode::fs {

enum class TextEncoding { Utf8, Utf8Bom, Utf16Le, Utf16Be, Unknown };

std::string_view to_string(TextEncoding encoding) noexcept;

enum class LineEnding { Lf, CrLf, Mixed, None };

std::string_view to_string(LineEnding ending) noexcept;

struct TextFile {
    std::string  content;                          // always UTF-8, LF-normalised
    TextEncoding encoding = TextEncoding::Utf8;
    LineEnding   line_ending = LineEnding::Lf;
    std::size_t  line_count = 0;
    std::uint64_t size_on_disk = 0;
    bool         truncated = false;                // hit the byte cap
};

// A cheap path-version hint for ordinary external edits, not an OS/handle
// identity guarantee. modified_time is the exact file_clock tick count.
struct SourceVersion {
    std::uint64_t size_bytes = 0;
    std::string modified_time;
    bool operator==(const SourceVersion&) const = default;
};

Result<SourceVersion> source_version(const std::filesystem::path& path);

// A bounded source window. Line coordinates describe returned text, never an
// unfulfilled requested range. EOF is observed from the same open stream.
struct SourceRead {
    TextFile file;
    std::size_t start_line = 0;
    std::size_t end_line = 0;
    std::size_t complete_end_line = 0;
    bool last_line_partial = false;
    bool eof = false;
    bool has_more = false;
    std::optional<SourceVersion> version;   // matching observations before/after read
};

struct FileInfo {
    std::filesystem::path path;
    bool          exists = false;
    bool          is_directory = false;
    bool          is_regular_file = false;
    bool          is_symlink = false;              // symlink or NTFS reparse point
    bool          is_binary = false;
    bool          readonly = false;
    std::uint64_t size = 0;
    std::int64_t  modified_unix_ms = 0;
    TextEncoding  encoding = TextEncoding::Unknown;
    std::size_t   line_count = 0;
};

struct DirEntry {
    std::string   name;
    bool          is_directory = false;
    bool          is_symlink = false;
    std::uint64_t size = 0;
    std::int64_t  modified_unix_ms = 0;
};

// Default byte cap for a single read. Bigger files must be read by range.
inline constexpr std::size_t kDefaultMaxReadBytes = std::size_t{2} * 1024 * 1024;

// --- Reading ---------------------------------------------------------------
// Includes NTFS junctions, which std::filesystem::is_symlink does not detect.
bool is_link_or_reparse_point(const std::filesystem::path& path);

Result<std::string> read_bytes(const std::filesystem::path& path,
                               std::size_t max_bytes = kDefaultMaxReadBytes);

// Decodes to UTF-8, normalises line endings and reports what it found. Fails
// with NotSupported for binary content so the model gets a clear reason rather
// than a screenful of NUL bytes.
Result<TextFile> read_text(const std::filesystem::path& path,
                           std::size_t max_bytes = kDefaultMaxReadBytes);

// 1-based inclusive line range. `end_line == 0` means "to end of file".
Result<TextFile> read_text_range(const std::filesystem::path& path,
                                 std::size_t start_line, std::size_t end_line,
                                 std::size_t max_bytes = kDefaultMaxReadBytes);

// After a bounded encoding probe, streams through the requested source window
// and at most one lookahead character. max_bytes bounds returned normalized
// UTF-8, including partial long lines. end_line == 0 means EOF; no scan is made
// merely to count the file.
Result<SourceRead> read_source(const std::filesystem::path& path, std::size_t start_line,
                               std::size_t end_line, std::size_t max_bytes = kDefaultMaxReadBytes);

// --- Writing ---------------------------------------------------------------
// Writes through a temporary file in the same directory and renames over the
// target, so an interrupted write can never leave a half-written source file.
Status write_text_atomic(const std::filesystem::path& path, std::string_view content,
                         TextEncoding encoding = TextEncoding::Utf8,
                         LineEnding line_ending = LineEnding::Lf);

Status append_text(const std::filesystem::path& path, std::string_view content);

// --- Inspection ------------------------------------------------------------
FileInfo stat_file(const std::filesystem::path& path);
bool     looks_binary(std::string_view sample);
TextEncoding detect_encoding(std::string_view sample);
LineEnding   detect_line_ending(std::string_view content);

Result<std::vector<DirEntry>> list_directory(const std::filesystem::path& path,
                                             bool include_hidden = false);

// --- Ignore rules ----------------------------------------------------------
// Directory names skipped by default when walking a workspace. Configurable;
// this is only the seed list.
const std::vector<std::string>& default_ignored_directories();

class IgnoreRules {
public:
    IgnoreRules();
    explicit IgnoreRules(std::vector<std::string> directories,
                         std::vector<std::string> file_globs = {});

    void add_directory(std::string name);
    void add_glob(std::string pattern);
    // Parses a .gitignore-style file. Only the subset LocalCode needs is
    // honoured (comments, directory entries, simple globs, negation with '!').
    void load_gitignore(const std::filesystem::path& file);

    [[nodiscard]] bool ignores_directory(std::string_view name) const;
    [[nodiscard]] bool ignores_file(std::string_view relative_path) const;

    [[nodiscard]] const std::vector<std::string>& directories() const { return directories_; }

private:
    std::vector<std::string> directories_;
    std::vector<std::string> globs_;
    std::vector<std::string> negations_;
};

struct WalkOptions {
    const IgnoreRules* ignore = nullptr;
    std::size_t max_depth = 64;
    std::size_t max_entries = 200000;      // hard stop on pathological trees
    bool        include_directories = false;
    bool        follow_symlinks = false;   // never true for workspace scans
};

// Iterative walk (no recursion, so deep trees cannot blow the stack). The
// callback receives paths relative to `root`; returning false stops the walk.
Status walk(const std::filesystem::path& root, const WalkOptions& options,
            const std::function<bool(const std::filesystem::path& relative,
                                     const DirEntry& entry)>& callback);

}  // namespace localcode::fs
