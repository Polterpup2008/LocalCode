// ---------------------------------------------------------------------------
// Reading and inspecting the filesystem.
// ---------------------------------------------------------------------------
#include "../repo/Hashing.h"
#include "../util/Log.h"
#include "SourceControlDiagnostics.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

constexpr std::size_t kMaxToolReadBytes = 400 * 1024;

struct FileExpectation {
    std::vector<std::string> contains;
    std::string description;
    int criterion = 0;
};

Result<FileExpectation> parse_file_expectation(const Json& json) {
    const auto invalid = [] {
        return make_error(
            ErrorCode::InvalidArgument, "invalid file expectation",
            "Use expect with contains_all (1-16 nonempty literal strings, each at most 512 bytes), "
            "an optional description, and an optional criterion number from 1 to 32. "
            "This checks file contents; program-output expectation fields are not supported.");
    };
    if (!json.is_object()) return invalid();
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() != "contains_all" && it.key() != "description" && it.key() != "criterion")
            return invalid();
    }
    if (!json.contains("contains_all") || !json.at("contains_all").is_array()) return invalid();
    const auto& entries = json.at("contains_all");
    if (entries.empty() || entries.size() > 16) return invalid();
    FileExpectation expectation;
    for (const auto& entry : entries) {
        if (!entry.is_string()) return invalid();
        const auto value = entry.get<std::string>();
        if (strings::trim(value).empty() || value.size() > 512) return invalid();
        expectation.contains.push_back(value);
    }
    if (json.contains("description")) {
        if (!json.at("description").is_string()) return invalid();
        expectation.description =
            strings::utf8_truncate(json.at("description").get<std::string>(), 512);
    }
    if (json.contains("criterion")) {
        if (!json.at("criterion").is_number_integer()) return invalid();
        const auto value = json.at("criterion").get<std::int64_t>();
        if (value < 1 || value > 32) return invalid();
        expectation.criterion = static_cast<int>(value);
    }
    return expectation;
}

struct SourceReadLimits {
    std::size_t effective;
    std::size_t context;
    std::size_t host;
    std::optional<std::size_t> requested;
};

Json source_read_metadata(const std::string& path, const fs::SourceRead& source,
                          std::size_t requested_start, std::size_t requested_end,
                          const SourceReadLimits& limits) {
    Json metadata{{"source_read", true},
                  {"path", path},
                  {"requested_start_line", requested_start},
                  {"requested_end_line", requested_end},
                  {"start_line", source.start_line},
                  {"end_line", source.end_line},
                  {"complete_end_line", source.complete_end_line},
                  {"line_count", source.file.line_count},
                  {"lines", source.file.line_count},
                  {"last_line_partial", source.last_line_partial},
                  {"eof", source.eof},
                  {"has_more", source.has_more},
                  {"truncated", source.file.truncated},
                  {"effective_max_bytes", limits.effective},
                  {"context_max_read_bytes", limits.context},
                  {"bytes", source.file.size_on_disk},
                  {"encoding", std::string{fs::to_string(source.file.encoding)}},
                  {"content_hash", repo::fnv1a_hex(source.file.content)},
                  {"hash_algorithm", "fnv1a64"},
                  {"hash_scope", "returned_utf8_source"}};
    if (limits.requested) metadata["requested_max_bytes"] = *limits.requested;
    metadata["source_version_stable"] = source.version.has_value();
    if (source.version) {
        metadata["source_size_bytes"] = source.version->size_bytes;
        metadata["source_modified_time"] = source.version->modified_time;
    }
    // This fingerprint identifies the bytes returned by this read. It is not
    // cryptographic proof of file identity or a hash of an unread whole file.
    if (source.has_more)
        metadata["next_line"] =
            source.complete_end_line == 0 ? requested_start : source.complete_end_line + 1;
    return metadata;
}

std::string source_read_header(const std::string& path, const fs::SourceRead& source) {
    if (source.file.line_count == 0) return path + "  no source lines returned";
    return path + "  lines " + std::to_string(source.start_line) + "-" +
           std::to_string(source.end_line) + " (" + std::to_string(source.file.line_count) +
           " returned" + (source.last_line_partial ? ", last line partial)" : ")");
}

std::string source_read_continuation(const fs::SourceRead& source, std::size_t requested_start,
                                     const SourceReadLimits& limits) {
    if (source.eof) return "\nEOF: no further source lines.\n";
    const auto next =
        source.complete_end_line == 0 ? requested_start : source.complete_end_line + 1;
    std::string ceiling;
    if (source.file.truncated) {
        ceiling = "\nTRUNCATED at an effective limit of " + std::to_string(limits.effective) +
                  " UTF-8 source bytes (context cap " + std::to_string(limits.context);
        if (limits.requested)
            ceiling += "; requested max_bytes " + std::to_string(*limits.requested);
        ceiling += "). ";
        if (limits.effective == limits.host) {
            ceiling += limits.host == limits.context ? "The model/context cap limits this read. "
                                                     : "The tool cap limits this read. ";
            ceiling += "Increasing max_bytes cannot exceed " + std::to_string(limits.host) +
                       " source bytes. ";
        } else {
            ceiling += limits.requested ? "The caller's max_bytes limits this read. "
                                        : "The default max_bytes limits this read. ";
            ceiling +=
                "The available host ceiling is " + std::to_string(limits.host) + " source bytes. ";
        }
    }
    if (source.last_line_partial || source.file.line_count == 0)
        return ceiling +
               "The last line may be partial. Use search_text/repo_symbol to locate "
               "the relevant source, or read_file_range starting at line " +
               std::to_string(next) +
               " to continue without skipping the partial line. No later source line has "
               "been returned.\n";
    return (source.file.truncated ? ceiling : "\nMore source exists. ") +
           "Use search_text/repo_symbol to follow the relevant code; request the next range "
           "starting at line " +
           std::to_string(next) + " only if needed.\n";
}

// --- read_file -------------------------------------------------------------
class ReadFileTool final : public ITool {
public:
    std::string name() const override { return "read_file"; }

    std::string description() const override {
        return "Reads the actual contents of a text or source file from disk and returns them "
               "with line numbers. Use this whenever answering a question or making a change "
               "requires knowing what is in a file. Never ask the user to paste a file that "
               "this tool can read, and never guess at a file's contents. Paths may be relative "
               "to the workspace or absolute. Very large files are truncated - use "
               "read_file_range for those. To verify documentation or source text, supply "
               "expect.contains_all with literal snippets and expect.criterion with its "
               "acceptance number. LocalCode checks the actual file without running a script. "
               "A content check proves those snippets exist, not that the program works.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"path", schema::string_param(
                          "Path to the file, relative to the workspace root or absolute.")},
             {"max_bytes",
              schema::integer_param(
                  "Maximum bytes to read, further bounded by the model context budget.", 1,
                  4000000)},
             {"expect",
              schema::object(
                  {{"contains_all",
                    schema::array_of_strings(
                        "Literal snippets that must all exist in the complete file.")},
                   {"description",
                    schema::string_param("What this file-content comparison proves.")},
                   {"criterion",
                    schema::integer_param("Acceptance criterion number this content check settles.",
                                          1, 32)}},
                  {"contains_all"})}},
            {"path"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        std::optional<FileExpectation> expectation;
        if (arguments.contains("expect")) {
            auto parsed = parse_file_expectation(arguments.at("expect"));
            if (!parsed)
                return ToolResult::failure(parsed.error().message, parsed.error().to_string());
            expectation = std::move(*parsed);
        }
        const auto input = arg_string(arguments, "path");
        auto path = resolve(context, input, PathAccess::Read);
        if (!path) return ToolResult::failure("cannot read " + input, path.error().message);

        const auto caller_limit = static_cast<std::size_t>(
            arg_int(arguments, "max_bytes", static_cast<long long>(kMaxToolReadBytes)));
        const auto host_limit = std::min<std::size_t>(context.max_read_bytes, 4u * 1024 * 1024);
        const SourceReadLimits limits{
            std::min(host_limit, caller_limit), context.max_read_bytes, host_limit,
            arguments.contains("max_bytes") ? std::optional{caller_limit} : std::nullopt};

        auto source = fs::read_source(*path, 1, 0, limits.effective);
        if (!source) {
            // Reading is allowed anywhere, so an invented absolute path gets
            // past the policy and fails at the filesystem with no context at
            // all. The same hint belongs here.
            ToolResult result =
                ToolResult::failure("cannot read " + input,
                                    path_recovery_hint(context, input) + source.error().message);
            result.evidence.push_back(
                make_evidence(EvidenceKind::FileRead, input, source.error().message, false));
            return result;
        }

        const auto* file = &source->file;
        const auto relative = relative_to_workspace(context, *path);

        // Line numbers matter: they are how the model refers to a location in a
        // later replace_in_file or in its explanation to the user.
        std::string numbered;
        numbered.reserve(file->content.size() + file->line_count * 8);
        std::size_t line_number = 1;
        for (const auto& line : strings::split_lines(file->content)) {
            char prefix[16];
            std::snprintf(prefix, sizeof prefix, "%6zu\t", line_number++);
            numbered += prefix;
            numbered += line;
            numbered.push_back('\n');
        }

        const auto header = source_read_header(relative, *source);
        auto result = ToolResult::success(
            "Read " + header, reinterpretation_note(context, input, PathAccess::Read) + header +
                                  "\n" + numbered + source_read_continuation(*source, 1, limits));
        result.truncated = file->truncated;
        result.structured = source_read_metadata(relative, *source, 1, 0, limits);
        annotate_source_controls(result, *source);
        result.evidence.push_back(make_evidence(EvidenceKind::FileRead, relative,
                                                std::to_string(file->line_count) + " lines", true));
        if (expectation) {
            std::vector<std::string> failures;
            if (file->truncated) {
                failures.push_back(
                    "file content check was not evaluated because the file is truncated");
            } else {
                for (const auto& wanted : expectation->contains) {
                    if (file->content.find(wanted) == std::string::npos)
                        failures.push_back("missing literal snippet: " + wanted);
                }
            }
            const bool passed = failures.empty();
            result.structured["file_check_checked"] = !file->truncated;
            result.structured["file_check_passed"] = passed;
            result.structured["file_check_criterion"] = expectation->criterion;
            result.structured["file_check_description"] = expectation->description;
            result.structured["file_check_contains"] = expectation->contains;
            result.content +=
                "\nFile content check: " + std::string{passed ? "PASS" : "NOT PROVEN"};
            for (const auto& failure : failures)
                result.content += "\n  " + failure;
            if (!passed) {
                result.ok = false;
                result.error = strings::join(failures, "; ");
                result.summary = "File content check failed: " + relative;
            }
        }
        return result;
    }
};

// --- read_file_range -------------------------------------------------------
class ReadFileRangeTool final : public ITool {
public:
    std::string name() const override { return "read_file_range"; }

    std::string description() const override {
        return "Reads a specific range of lines from a file. Use this for large files, for "
               "following up on a search_text hit, or when a compiler error names a line number "
               "and you need the surrounding code. Line numbers are 1-based and inclusive.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"path", schema::string_param("Path to the file.")},
             {"start_line", schema::integer_param("First line to read (1-based).", 1)},
             {"end_line",
              schema::integer_param(
                  "Last line, inclusive. Omit for a 120-line excerpt; use 0 for EOF.", 0)}},
            {"path", "start_line"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto input = arg_string(arguments, "path");
        auto path = resolve(context, input, PathAccess::Read);
        if (!path) return ToolResult::failure("cannot read " + input, path.error().message);

        const auto start =
            static_cast<std::size_t>(std::max<long long>(1, arg_int(arguments, "start_line", 1)));
        const bool default_excerpt = !arguments.contains("end_line");
        const auto end = default_excerpt ? start + 119
                                         : static_cast<std::size_t>(std::max<long long>(
                                               0, arg_int(arguments, "end_line", 0)));

        const auto limit = std::min(kMaxToolReadBytes, context.max_read_bytes);
        const SourceReadLimits limits{limit, context.max_read_bytes, limit, std::nullopt};
        auto source = fs::read_source(*path, start, end, limits.effective);
        if (!source) {
            ToolResult result = ToolResult::failure("cannot read " + input, source.error().message);
            result.evidence.push_back(
                make_evidence(EvidenceKind::FileRead, input, source.error().message, false));
            return result;
        }
        const auto relative = relative_to_workspace(context, *path);
        std::string numbered;
        std::size_t line_number = source->start_line;
        for (const auto& line : strings::split_lines(source->file.content)) {
            char prefix[32];
            std::snprintf(prefix, sizeof prefix, "%6zu\t", line_number++);
            numbered += prefix;
            numbered += line;
            numbered.push_back('\n');
        }
        const auto header = source_read_header(relative, *source);
        auto result = ToolResult::success(
            "Read " + header,
            header + "\n" + numbered + source_read_continuation(*source, start, limits));
        result.truncated = source->file.truncated;
        result.structured = source_read_metadata(relative, *source, start, end, limits);
        annotate_source_controls(result, *source);
        result.evidence.push_back(make_evidence(EvidenceKind::FileRead, relative, header,
                                                source->file.line_count != 0 || start == 1));
        return result;
    }
};

// --- list_directory --------------------------------------------------------
class ListDirectoryTool final : public ITool {
public:
    std::string name() const override { return "list_directory"; }

    std::string description() const override {
        return "Lists the files and subdirectories of a directory in the workspace. Use this to "
               "find out what a project contains before guessing at filenames. Heavy directories "
               "such as .git, node_modules and build output are skipped by default.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"path", schema::string_param(
                          "Directory to list, relative to the workspace root. Omit for the root.")},
             {"recursive",
              schema::boolean_param(
                  "List the whole subtree instead of one level. Defaults to false.", false)},
             {"max_entries",
              schema::integer_param("Maximum entries to return. Defaults to 400.", 1, 5000)}});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto input = arg_string(arguments, "path");
        const bool recursive = arg_bool(arguments, "recursive", false);
        const auto max_entries = static_cast<std::size_t>(arg_int(arguments, "max_entries", 400));

        if (!recursive) {
            auto entries = context.workspace->list(input);
            if (!entries) {
                // The root, again, because a model that guessed one absolute
                // path will otherwise guess another.
                return ToolResult::failure(
                    "cannot list " + (input.empty() ? "." : input),
                    entries.error().message + "\n\nThis workspace is " +
                        path_to_utf8(context.workspace->root()) +
                        ".\nList it with no path at all to see what is there.");
            }
            std::string listing;
            std::size_t shown = 0;
            for (const auto& entry : *entries) {
                if (shown++ >= max_entries) break;
                listing += entry.is_directory ? "  " + entry.relative_path + "/\n"
                                              : "  " + entry.relative_path + "  (" +
                                                    strings::human_bytes(entry.size) + ")\n";
            }
            // The path as LocalCode resolved it, not as the model typed it.
            //
            // Echoing the input taught a model its own mistake. It asked for
            // "/Users/priam/..." - an absolute path with the drive letter
            // dropped - LocalCode accepted it, answered "Listed
            // /Users/priam/...", and the model kept that form. Several turns
            // later it tried to `rm -rf` that path, which with no drive letter
            // looks exactly like a filesystem root, and the security policy
            // blocked it. The run never recovered.
            std::string label{"workspace root"};
            if (!input.empty()) {
                if (auto canonical = resolve(context, input, PathAccess::Read)) {
                    const auto resolved = relative_to_workspace(context, *canonical);
                    label = resolved.empty() || resolved == "." ? std::string{"workspace root"}
                                                                : resolved;
                } else {
                    label = input;
                }
            }
            auto result = ToolResult::success(
                "Listed " + label + " (" + std::to_string(entries->size()) + " entries)",
                reinterpretation_note(context, input, PathAccess::Read) + label + ":\n" +
                    (listing.empty() ? "  (empty)\n" : listing));
            result.evidence.push_back(make_evidence(EvidenceKind::DirectoryListed, label,
                                                    std::to_string(entries->size()) + " entries",
                                                    true));
            return result;
        }

        auto files = context.workspace->all_files(max_entries);
        if (!files) return ToolResult::failure("cannot walk the workspace", files.error().message);

        std::string listing;
        for (const auto& file : *files) {
            if (!input.empty() && !strings::starts_with(file.relative_path, input)) continue;
            listing += "  " + file.relative_path + "\n";
        }
        auto result = ToolResult::success(
            "Listed " + std::to_string(files->size()) + " files recursively",
            "Files under " + (input.empty() ? std::string{"the workspace root"} : input) + ":\n" +
                listing);
        result.evidence.push_back(make_evidence(EvidenceKind::DirectoryListed,
                                                input.empty() ? "." : input,
                                                std::to_string(files->size()) + " files", true));
        return result;
    }
};

// --- file_info -------------------------------------------------------------
class FileInfoTool final : public ITool {
public:
    std::string name() const override { return "file_info"; }

    std::string description() const override {
        return "Reports whether a path exists and what it is: file or directory, size, line "
               "count, text encoding, whether it is binary, and when it was last modified. Use "
               "this before reading something that might be huge or binary, and to check whether "
               "a file you intend to create already exists.";
    }

    Json parameters_schema() const override {
        return schema::object({{"path", schema::string_param("Path to inspect.")}}, {"path"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto input = arg_string(arguments, "path");
        auto path = resolve(context, input, PathAccess::Read);
        if (!path) return ToolResult::failure("cannot inspect " + input, path.error().message);

        const auto info = fs::stat_file(*path);
        const auto relative = relative_to_workspace(context, *path);

        if (!info.exists) {
            auto result =
                ToolResult::success("Does not exist: " + relative, relative + " does not exist.");
            result.structured = {{"path", relative}, {"exists", false}};
            return result;
        }

        std::string report = relative + "\n";
        report += "  type:      " + std::string{info.is_directory ? "directory" : "file"} + "\n";
        if (info.is_regular_file) {
            report += "  size:      " + strings::human_bytes(info.size) + "\n";
            report += "  binary:    " + std::string{info.is_binary ? "yes" : "no"} + "\n";
            report += "  encoding:  " + std::string{fs::to_string(info.encoding)} + "\n";
            if (info.line_count) report += "  lines:     " + std::to_string(info.line_count) + "\n";
        }
        report += "  read-only: " + std::string{info.readonly ? "yes" : "no"} + "\n";
        if (info.is_symlink) report += "  symlink/reparse point: yes\n";
        report += "  modified:  " + format_timestamp(info.modified_unix_ms) + "\n";

        auto result = ToolResult::success("Inspected " + relative, report);
        result.structured = {
            {"path", relative},  {"exists", true},           {"is_directory", info.is_directory},
            {"size", info.size}, {"binary", info.is_binary}, {"lines", info.line_count}};
        return result;
    }
};

// --- create_directory ------------------------------------------------------
class CreateDirectoryTool final : public ITool {
public:
    std::string name() const override { return "create_directory"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Creates a directory inside the workspace, including any missing parent "
               "directories. Succeeds quietly if it already exists.";
    }

    Json parameters_schema() const override {
        return schema::object({{"path", schema::string_param("Directory to create.")}}, {"path"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto input = arg_string(arguments, "path");
        auto path = resolve(context, input, PathAccess::Create);
        if (!path) return ToolResult::failure("cannot create " + input, path.error().message);

        std::error_code ec;
        const bool existed = std::filesystem::is_directory(*path, ec);
        std::filesystem::create_directories(*path, ec);
        if (ec && !std::filesystem::is_directory(*path)) {
            return ToolResult::failure("cannot create " + input, ec.message());
        }

        const auto relative = relative_to_workspace(context, *path);

        // A directory that was already there was not created, and recording it
        // as a change makes the run look like it did something it did not.
        // Observed: `create_directory .` on the workspace root, listed
        // afterwards as "created: ." among the files the run had changed.
        if (existed) {
            auto result = ToolResult::success(
                relative + " already exists",
                "The directory " + relative + " is already there; nothing was created.");
            result.evidence.push_back(
                make_evidence(EvidenceKind::DirectoryListed, relative, "already exists", true));
            return result;
        }

        context.workspace->record_agent_change(*path, AgentChangeKind::Created);

        auto result =
            ToolResult::success("Created directory " + relative, "Created directory " + relative);
        result.evidence.push_back(
            make_evidence(EvidenceKind::FileWritten, relative, "directory created", true));
        return result;
    }
};

// --- delete_path -----------------------------------------------------------
//
// There was no way to remove anything, and it ended a run. `cmake -G Ninja`
// failed because ninja is not on PATH, but it had already written a
// CMakeCache.txt naming Ninja, so every later configure failed with "Does not
// match the generator used previously". The model diagnosed that correctly and
// tried to delete the cache. `rm` failed, `rm -rf build` was blocked by the
// security policy - rightly, on a drive-less path that looked like a
// filesystem root - and there was nothing else to try. An agent that can
// create files but never remove one cannot maintain a build tree, which is one
// of the most ordinary things in C++ work.
//
// This does not widen the security boundary. It stays inside the workspace,
// refuses the workspace root itself, and asks a human before removing a
// directory. What it replaces is a hole, not a wall.
class DeletePathTool final : public ITool {
public:
    std::string name() const override { return "delete_path"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Deletes a file or directory inside the workspace. Use this to clear a stale "
               "build directory - a CMake cache that names the wrong generator cannot be "
               "reconfigured and has to be removed - or to remove a file you created and no "
               "longer want. Removing a directory needs 'recursive' and is confirmed by the "
               "user first. The workspace root itself can never be deleted, and neither can "
               "anything outside the workspace.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"path", schema::string_param(
                          "File or directory to delete, relative to the workspace root. Use "
                          "forward slashes: build/CMakeCache.txt")},
             {"recursive", schema::boolean_param(
                               "Required to delete a directory and everything inside it.", false)}},
            {"path"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto input = arg_string(arguments, "path");
        const bool recursive = arg_bool(arguments, "recursive", false);

        auto path = resolve(context, input, PathAccess::Delete);
        if (!path) return ToolResult::failure("cannot delete " + input, path.error().message);

        std::error_code ec;
        // The workspace root is the one path inside the workspace that must
        // never go. Compared canonically, so "." and "a/.." cannot reach it.
        const auto root = std::filesystem::weakly_canonical(context.workspace->root(), ec);
        const auto target = std::filesystem::weakly_canonical(*path, ec);
        if (!ec && target == root) {
            return ToolResult::failure(
                "refusing to delete the workspace root",
                "That path is the workspace itself: " + path_to_utf8(root) +
                    "\nDelete something inside it instead. If you meant to clear a build "
                    "directory, name that directory.");
        }

        const auto relative = relative_to_workspace(context, *path);
        if (!std::filesystem::exists(*path, ec)) {
            return ToolResult::failure(
                relative + " does not exist",
                "There is nothing at " + relative +
                    " to delete, so no change was made. If "
                    "you expected it to be there, list the directory to see what is.");
        }

        const bool is_directory = std::filesystem::is_directory(*path, ec);
        if (is_directory && !recursive) {
            return ToolResult::failure(
                relative + " is a directory",
                "Deleting a directory removes everything inside it, so it has to be asked for "
                "explicitly: call this again with recursive=true.");
        }

        if (is_directory) {
            ConfirmationRequest confirmation;
            confirmation.title = "Delete this directory and everything in it?";
            confirmation.command = relative;
            confirmation.detail = path->string();
            confirmation.reason =
                "Deleting a directory cannot be undone, and anything inside it goes with it.";
            confirmation.rule_id = "fs.delete-directory";
            if (!context.ask(confirmation)) {
                auto refused = ToolResult::failure("not approved: delete " + relative,
                                                   "The user did not approve deleting " + relative +
                                                       ".\nLeave it alone and find another way.");
                refused.evidence.push_back(make_evidence(EvidenceKind::FileWritten, relative,
                                                         "deletion refused by the user", false));
                return refused;
            }
        }

        std::uintmax_t removed = 0;
        if (is_directory) {
            removed = std::filesystem::remove_all(*path, ec);
        } else {
            removed = std::filesystem::remove(*path, ec) ? 1 : 0;
        }
        if (ec) {
            return ToolResult::failure("could not delete " + relative, ec.message());
        }

        context.workspace->record_agent_change(*path, AgentChangeKind::Deleted);
        log_info(LogChannel::Tool, "path deleted",
                 {{"path", relative}, {"entries", std::to_string(removed)}});

        const auto what = is_directory
                              ? relative + " and " + std::to_string(removed ? removed - 1 : 0) +
                                    " item(s) inside it"
                              : relative;
        auto result = ToolResult::success("Deleted " + relative, "Deleted " + what + ".");
        result.evidence.push_back(
            make_evidence(EvidenceKind::FileWritten, relative, "deleted", true));
        return result;
    }
};

// Validate every destination before a recursive copy can mutate anything. A
// checked top-level directory does not constrain its nested junctions or hard
// links. This is a host-tool check, not confinement of arbitrary child code.
Status validate_copy_tree(const std::filesystem::path& source,
                          const std::filesystem::path& destination, ToolContext& context) {
    std::error_code error;
    const bool directory = std::filesystem::is_directory(source, error);
    if (error) return make_error(ErrorCode::IoError, "cannot inspect copy source", error.message());
    if (directory && PathPolicy::is_within(source, destination))
        return make_error(ErrorCode::InvalidArgument, "cannot copy a directory into itself");

    auto check = [&](const std::filesystem::path& from, const std::filesystem::path& to) -> Status {
        if (context.cancel.cancelled()) return make_error(ErrorCode::Cancelled, "copy cancelled");
        if (fs::is_link_or_reparse_point(from))
            return make_error(ErrorCode::PermissionDenied,
                              "recursive copy does not follow links: " + path_to_utf8(from),
                              "Copy the intended link target explicitly instead.");
        auto readable = resolve(context, path_to_utf8(from), PathAccess::Read);
        if (!readable) return readable.error();
        auto writable = resolve(context, path_to_utf8(to), PathAccess::Create);
        if (!writable) return writable.error();
        std::error_code inspect_error;
        if (std::filesystem::is_regular_file(*writable, inspect_error)) {
            const auto links = std::filesystem::hard_link_count(*writable, inspect_error);
            if (inspect_error)
                return make_error(ErrorCode::IoError, "cannot inspect copy destination",
                                  inspect_error.message());
            if (links > 1)
                return make_error(
                    ErrorCode::PermissionDenied,
                    "copy would overwrite a file with hard-link aliases: " + path_to_utf8(to),
                    "Remove the destination link explicitly before copying a private file.");
        }
        return ok_status();
    };

    auto target = destination;
    if (!directory && std::filesystem::is_directory(destination, error))
        target /= source.filename();
    if (auto checked = check(source, target); !checked) return checked;
    if (!directory) return ok_status();

    std::filesystem::recursive_directory_iterator entry(source, error), end;
    if (error) return make_error(ErrorCode::IoError, "cannot inspect copy tree", error.message());
    std::size_t entries = 0;
    for (; entry != end; entry.increment(error)) {
        if (error)
            return make_error(ErrorCode::IoError, "cannot inspect copy tree", error.message());
        if (++entries > 200000)
            return make_error(ErrorCode::InvalidArgument, "copy tree exceeds 200000 entries");
        const auto relative = entry->path().lexically_relative(source);
        if (auto checked = check(entry->path(), destination / relative); !checked) return checked;
    }
    if (error) return make_error(ErrorCode::IoError, "cannot inspect copy tree", error.message());
    return ok_status();
}

// --- move_file -------------------------------------------------------------
class MoveFileTool final : public ITool {
public:
    std::string name() const override { return "move_file"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Moves or renames a file or directory. Both the source and the destination must "
               "be inside the workspace. Refuses to overwrite an existing destination unless "
               "overwrite is set to true.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"source", schema::string_param("Existing path to move.")},
             {"destination", schema::string_param("New path.")},
             {"overwrite", schema::boolean_param(
                               "Replace the destination if it exists. Defaults to false.", false)}},
            {"source", "destination"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto source_input = arg_string(arguments, "source");
        const auto destination_input = arg_string(arguments, "destination");

        auto source = resolve(context, source_input, PathAccess::Delete);
        if (!source)
            return ToolResult::failure("cannot move " + source_input, source.error().message);
        auto destination = resolve(context, destination_input, PathAccess::Create);
        if (!destination) {
            return ToolResult::failure("cannot move to " + destination_input,
                                       destination.error().message);
        }

        std::error_code ec;
        if (!std::filesystem::exists(*source, ec)) {
            return ToolResult::failure("source does not exist", source_input + " does not exist.");
        }
        const bool overwrite = arg_bool(arguments, "overwrite", false);
        if (std::filesystem::exists(*destination, ec) && !overwrite) {
            return ToolResult::failure(
                "destination exists",
                destination_input +
                    " already exists. Pass overwrite=true if replacing it is intended.");
        }

        std::filesystem::create_directories(destination->parent_path(), ec);
        std::filesystem::rename(*source, *destination, ec);
        if (ec) {
            // Renaming across volumes fails; fall back to copy + remove.
            const auto checked = validate_copy_tree(*source, *destination, context);
            if (!checked) return ToolResult::failure("move refused", checked.error().to_string());
            std::filesystem::copy(*source, *destination,
                                  std::filesystem::copy_options::overwrite_existing |
                                      std::filesystem::copy_options::recursive,
                                  ec);
            if (ec) return ToolResult::failure("move failed", ec.message());
            std::filesystem::remove_all(*source, ec);
            if (ec) {
                const auto from = relative_to_workspace(context, *source);
                const auto to = relative_to_workspace(context, *destination);
                context.workspace->record_agent_change(*destination, AgentChangeKind::Created);
                context.workspace->record_agent_change(*source, AgentChangeKind::Modified);
                auto incomplete =
                    ToolResult::failure("move incomplete: source cleanup failed",
                                        "Copied to " + to + ", but could not finish removing " +
                                            from + ": " + ec.message() +
                                            ". Inspect both paths before retrying; the source may "
                                            "have been partly removed.");
                incomplete.evidence.push_back(make_evidence(
                    EvidenceKind::FileWritten, to, "copied; move source cleanup failed", true));
                return incomplete;
            }
        }

        context.workspace->record_agent_change(*destination, AgentChangeKind::Moved, *source);

        const auto from = relative_to_workspace(context, *source);
        const auto to = relative_to_workspace(context, *destination);
        auto result =
            ToolResult::success("Moved " + from + " to " + to, "Moved " + from + " to " + to);
        result.evidence.push_back(
            make_evidence(EvidenceKind::FileWritten, to, "moved from " + from, true));
        return result;
    }
};

// --- copy_file -------------------------------------------------------------
class CopyFileTool final : public ITool {
public:
    std::string name() const override { return "copy_file"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Copies a file or directory. The source may be anywhere readable; the destination "
               "must be inside the workspace.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"source", schema::string_param("Path to copy from.")},
             {"destination", schema::string_param("Path to copy to, inside the workspace.")},
             {"overwrite", schema::boolean_param("Replace the destination if it exists.", false)}},
            {"source", "destination"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto source_input = arg_string(arguments, "source");
        const auto destination_input = arg_string(arguments, "destination");

        auto source = resolve(context, source_input, PathAccess::Read);
        if (!source)
            return ToolResult::failure("cannot read " + source_input, source.error().message);
        auto destination = resolve(context, destination_input, PathAccess::Create);
        if (!destination) {
            return ToolResult::failure("cannot write " + destination_input,
                                       destination.error().message);
        }

        std::error_code ec;
        if (!std::filesystem::exists(*source, ec)) {
            return ToolResult::failure("source does not exist", source_input + " does not exist.");
        }
        const bool overwrite = arg_bool(arguments, "overwrite", false);
        if (std::filesystem::exists(*destination, ec) && !overwrite) {
            return ToolResult::failure("destination exists",
                                       destination_input +
                                           " already exists. Pass overwrite=true "
                                           "if replacing it is intended.");
        }

        const auto checked = validate_copy_tree(*source, *destination, context);
        if (!checked) return ToolResult::failure("copy refused", checked.error().to_string());
        std::filesystem::create_directories(destination->parent_path(), ec);
        auto options = std::filesystem::copy_options::recursive;
        if (overwrite) options |= std::filesystem::copy_options::overwrite_existing;
        std::filesystem::copy(*source, *destination, options, ec);
        if (ec) return ToolResult::failure("copy failed", ec.message());

        context.workspace->record_agent_change(*destination, AgentChangeKind::Created);

        const auto to = relative_to_workspace(context, *destination);
        auto result =
            ToolResult::success("Copied to " + to, "Copied " + source_input + " to " + to);
        result.evidence.push_back(make_evidence(EvidenceKind::FileWritten, to, "copied", true));
        return result;
    }
};

}   // namespace

std::vector<ToolPtr> make_file_tools() {
    return {std::make_shared<ReadFileTool>(),        std::make_shared<ReadFileRangeTool>(),
            std::make_shared<ListDirectoryTool>(),   std::make_shared<FileInfoTool>(),
            std::make_shared<CreateDirectoryTool>(), std::make_shared<MoveFileTool>(),
            std::make_shared<CopyFileTool>(),        std::make_shared<DeletePathTool>()};
}

}   // namespace localcode
