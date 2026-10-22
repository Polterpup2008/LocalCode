// ---------------------------------------------------------------------------
// Writing files: whole-file writes, targeted replacements, and unified patches.
//
// Every write goes through fs::write_text_atomic, preserves the file's original
// encoding and line endings, and is recorded against the workspace so the diff
// panel can show exactly what the agent touched.
//
// The guard that matters: if a file already had uncommitted changes when the
// agent run started, these tools refuse to overwrite it unless the caller says
// so explicitly. Losing someone's uncommitted morning's work to a confident
// model is the failure mode worth engineering against.
// ---------------------------------------------------------------------------
#include <algorithm>

#include "../git/GitService.h"
#include "../platform/Environment.h"
#include "../platform/Process.h"
#include "../util/Log.h"
#include "CodeIntegrity.h"
#include "ForeignToolchain.h"
#include "SourceControlDiagnostics.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

constexpr std::size_t kMaxEditFileBytes = 8u * 1024 * 1024;

// Inspection only: this never selects a match or changes an edit argument.
void annotate_exact_find_failure(ToolResult& result, const std::string& decoded_find,
                                 const std::string& needle, const fs::TextFile& file) {
    const auto hidden = [](unsigned char byte) {
        return (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') || byte == 0x7f;
    };
    if (!std::any_of(decoded_find.begin(), decoded_find.end(), hidden) &&
        !std::any_of(file.content.begin(), file.content.end(), hidden))
        return;

    constexpr std::size_t kMaxFindJsonBytes = 1024;
    constexpr std::size_t kMaxFindControlDetails = 16;
    Json controls = Json::array();
    std::size_t count = 0, line = 1, column = 1;
    for (const unsigned char byte : decoded_find) {
        if (hidden(byte)) {
            ++count;
            if (controls.size() < kMaxFindControlDetails) {
                char codepoint[7];
                std::snprintf(codepoint, sizeof codepoint, "U+%04X", static_cast<unsigned>(byte));
                controls.push_back(
                    {{"line", line}, {"utf8_byte_column", column}, {"codepoint", codepoint}});
            }
        }
        if (byte == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    Json diagnostic{{"decoded_find_utf8_bytes", decoded_find.size()},
                    {"normalized_find_utf8_bytes", needle.size()},
                    {"CRLF_normalized_for_matching", decoded_find != needle},
                    {"find_control_count", count},
                    {"find_controls", controls},
                    {"json_omitted", true},
                    {"json_max_bytes", kMaxFindJsonBytes},
                    {"scope", "received JSON-decoded find argument; no second escape decoding"}};
    std::string encoded;
    if (decoded_find.size() + 2 <= kMaxFindJsonBytes) {
        encoded = Json(decoded_find).dump();
        encoded = strings::replace_all(encoded, std::string(1, '\x7f'), "\\u007f");
        if (encoded.size() <= kMaxFindJsonBytes) {
            diagnostic["json_string"] = encoded;
            diagnostic["json_omitted"] = false;
        } else {
            encoded.clear();
        }
    }
    result.structured["exact_find_diagnostic"] = std::move(diagnostic);
    result.content +=
        "\nDecoded find inspection: " + std::to_string(decoded_find.size()) + " UTF-8 bytes; " +
        std::to_string(count) + " nonprinting ASCII controls (first " +
        std::to_string(controls.size()) + " locations in structured result). Matching uses " +
        std::to_string(needle.size()) + " bytes after existing CRLF-to-LF normalization.\n";
    for (const auto& entry : controls)
        result.content += "find line " + std::to_string(entry.at("line").get<std::size_t>()) +
                          ", UTF-8 byte column " +
                          std::to_string(entry.at("utf8_byte_column").get<std::size_t>()) + ": " +
                          entry.at("codepoint").get<std::string>() + "\n";
    if (!encoded.empty())
        result.content += "Decoded find JSON (already parsed once):\nfind: " + encoded + "\n";
    else
        result.content +=
            "Decoded find JSON omitted: complete literal exceeds 1024 encoded bytes; no clipped "
            "JSON is shown.\n";
    result.content +=
        "Printable backslash spellings in the decoded argument remain printable bytes. "
        "The tool does not decode escapes again, guess intended text, or propose a replacement. "
        "Source controls below are inspection facts, not selected edit candidates. "
        "Decode an exact source-line JSON literal once before using its text in an exact edit.\n";

    // Reuse the read-tool receipt on the exact already-loaded normalized text.
    // A bounded internal prefix must never be described as a complete last line.
    fs::SourceRead source;
    source.file = file;
    source.start_line = 1;
    source.end_line = file.line_count;
    source.last_line_partial =
        file.truncated && !file.content.empty() && file.content.back() != '\n';
    annotate_source_controls(result, source);
    result.structured["source_control_scope"] = "already loaded normalized UTF-8 edit input";
    result.structured["source_control_input_truncated"] = file.truncated;
}

// Refuses to clobber pre-existing uncommitted work unless explicitly allowed.
std::optional<ToolResult> guard_uncommitted(ToolContext& context, const std::filesystem::path& path,
                                            bool allowed) {
    if (allowed) return std::nullopt;
    const auto relative = relative_to_workspace(context, path);
    const auto dirty = context.workspace->baseline_dirty_files();
    if (dirty.count(relative) == 0) return std::nullopt;

    return ToolResult::failure(
        "refused: " + relative + " has uncommitted changes",
        relative +
            " already had uncommitted changes before this task started, so overwriting it could "
            "destroy work that was not done by you.\n"
            "Read the file first and make a targeted edit with replace_in_file, or pass "
            "overwrite_uncommitted=true if replacing the whole file really is intended.");
}

// Keeps a file's original encoding and line endings so an edit does not show up
// in the diff as "every line changed".
struct FileStyle {
    fs::TextEncoding encoding = fs::TextEncoding::Utf8;
    fs::LineEnding line_ending = fs::LineEnding::Lf;
    bool existed = false;
};

FileStyle detect_style(const std::filesystem::path& path) {
    FileStyle style;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
#if defined(_WIN32)
        // A new file in a Windows project defaults to CRLF only when the rest of
        // the project does; without evidence, LF is the safer modern default.
        style.line_ending = fs::LineEnding::Lf;
#endif
        return style;
    }
    style.existed = true;
    auto text = fs::read_text(path, kMaxEditFileBytes);
    if (text) {
        style.encoding = text->encoding;
        style.line_ending =
            text->line_ending == fs::LineEnding::Mixed ? fs::LineEnding::Lf : text->line_ending;
        if (style.line_ending == fs::LineEnding::None) style.line_ending = fs::LineEnding::Lf;
    }
    return style;
}

// Says what was noticed about the text that was just written: unfinished work
// left behind, and edits to a test file that weaken it. Reported to the model
// at the moment of the write, when it is still cheap to fix, and attached to
// the structured result so the run's report can list it.
//
// Nothing is refused on the strength of these. A stub is sometimes the right
// thing to write, and a deleted assertion is sometimes a test that was wrong.
// Code written against a toolchain that is not on this machine.
//
// Asked for a game, a model wrote `#include "CoreMinimal.h"` and announced it
// would "create the level in Unreal", on a machine with no Unreal anywhere.
// The system prompt already said to check first. It chose first and never
// checked - which is why this says so here, at the write, rather than in a
// paragraph read twenty turns ago.
//
// Nothing is refused. The user may own the engine and want scaffolding for a
// different machine. What is refused is letting it pass silently, because a
// stack that cannot be built here can never reach the build stage, and so can
// never reach behaviour either: the work becomes unfinishable by construction.
void note_toolchain(ToolResult& result, const std::string& relative, std::string_view before,
                    std::string_view after) {
    auto signals = detect_foreign_toolchains(relative, after);
    if (signals.empty()) return;

    // Only what this edit introduced. A project that already builds against Qt
    // does not need to be told about Qt on every save.
    if (!before.empty()) {
        const auto existing = detect_foreign_toolchains(relative, before);
        std::erase_if(signals, [&](const ToolchainSignal& signal) {
            return std::any_of(existing.begin(), existing.end(), [&](const ToolchainSignal& seen) {
                return seen.toolchain == signal.toolchain;
            });
        });
    }
    if (signals.empty()) return;

    // Where one program settles it, ask the machine rather than the model.
    for (auto& signal : signals) {
        if (signal.probe.empty()) continue;
        signal.probe_failed = !resolve_executable(signal.probe).has_value();
    }

    std::string notice;
    Json json = Json::array();
    for (const auto& signal : signals) {
        json.push_back(Json{{"toolchain", signal.toolchain},
                            {"line", signal.line},
                            {"matched", signal.matched},
                            {"probe", signal.probe},
                            {"probe_failed", signal.probe_failed}});
        notice += "  " + signal.describe() + "\n";
        if (signal.probe_failed) {
            notice += "      LocalCode looked for '" + signal.probe +
                      "' and did not find it on this machine.\n";
        }
    }

    result.content +=
        "\n\nThis file depends on a toolchain that is not part of the project:\n" + notice +
        "\nBefore writing more of it, establish that it is actually here - find_executable, "
        "environment_info, or look for its files. If it is not, nothing you write against it "
        "can be configured, built, run or checked, so every verification stage stays 'not "
        "established' and the task cannot be finished at all. Say so to the user and agree on "
        "something that does build here, rather than producing code nobody can compile.\n";
    if (!result.structured.is_object()) result.structured = Json::object();
    if (!result.structured.contains("foreign_toolchains")) {
        result.structured["foreign_toolchains"] = Json::array();
    }
    for (auto& entry : json)
        result.structured["foreign_toolchains"].push_back(entry);
}

void note_integrity(ToolResult& result, const std::string& relative, std::string_view before,
                    std::string_view after) {
    note_toolchain(result, relative, before, after);
    auto stubs = scan_for_stubs(relative, after);
    // Only what this edit introduced. Reporting the file's existing TODOs back
    // to the model on every write buries the one that matters.
    if (!before.empty()) {
        const auto existing = scan_for_stubs(relative, before);
        std::erase_if(stubs, [&](const StubFinding& finding) {
            return std::any_of(existing.begin(), existing.end(), [&](const StubFinding& old) {
                return old.excerpt == finding.excerpt;
            });
        });
    }
    const auto tampering = scan_for_test_tampering(relative, before, after);
    if (stubs.empty() && tampering.empty()) return;

    Json stub_json = Json::array();
    std::string notice;
    for (const auto& finding : stubs) {
        if (finding.confidence == IntegrityConfidence::Low) continue;
        stub_json.push_back(Json{{"file", finding.file},
                                 {"line", finding.line},
                                 {"pattern", finding.pattern},
                                 {"confidence", std::string{to_string(finding.confidence)}},
                                 {"reason", finding.reason},
                                 {"in_test_code", finding.in_test_code}});
        notice += "  " + finding.describe() + "\n";
    }
    if (!notice.empty()) {
        result.content += "\n\nUnfinished work in what you just wrote:\n" + notice +
                          "This does not block anything. But the file now compiles while "
                          "not doing the job, so do not report the task as done on the "
                          "strength of a build that passes over it.\n";
        // Appended, not assigned: apply_patch calls this once per file it
        // wrote, and the last file's findings must not erase the others'.
        if (!result.structured.is_object()) result.structured = Json::object();
        if (!result.structured.contains("stub_findings")) {
            result.structured["stub_findings"] = Json::array();
        }
        for (auto& entry : stub_json)
            result.structured["stub_findings"].push_back(entry);
    }

    if (!tampering.empty()) {
        Json tamper_json = Json::array();
        std::string warning;
        for (const auto& finding : tampering) {
            tamper_json.push_back(Json{{"file", finding.file},
                                       {"line", finding.line},
                                       {"kind", std::string{to_string(finding.kind)}},
                                       {"confidence", std::string{to_string(finding.confidence)}},
                                       {"detail", finding.detail}});
            warning += "  " + finding.describe() + "\n";
        }
        result.content +=
            "\n\nThis edit weakened the tests:\n" + warning +
            "If the test was wrong, say so explicitly and explain why. If it was right, "
            "a passing suite after this change proves less than it did before, and this "
            "is recorded.\n";
        if (!result.structured.is_object()) result.structured = Json::object();
        if (!result.structured.contains("test_tampering")) {
            result.structured["test_tampering"] = Json::array();
        }
        for (auto& entry : tamper_json)
            result.structured["test_tampering"].push_back(entry);
    }
}

// --- write_file ------------------------------------------------------------
class WriteFileTool final : public ITool {
public:
    std::string name() const override { return "write_file"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Writes a complete file, creating it and any missing parent directories, or "
               "replacing it entirely if it exists. Use this for new files. To change part of an "
               "existing file, prefer replace_in_file or apply_patch - they are safer and produce "
               "a smaller diff. The file's existing encoding and line endings are preserved.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"path", schema::string_param("File to write, inside the workspace.")},
             {"content", schema::string_param("The complete new contents of the file.")},
             {"overwrite_uncommitted",
              schema::boolean_param(
                  "Allow replacing a file that already had uncommitted changes before this task "
                  "started. Defaults to false.",
                  false)}},
            {"path", "content"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto input = arg_string(arguments, "path");
        auto path = resolve(context, input, PathAccess::Create);
        if (!path) return ToolResult::failure("cannot write " + input, path.error().message);

        if (auto refusal = guard_uncommitted(context, *path,
                                             arg_bool(arguments, "overwrite_uncommitted", false))) {
            return *refusal;
        }

        // A missing "content" key means an empty file, which is legitimate, but
        // it is far more often a malformed call - so say what happened.
        if (!arguments.contains("content")) {
            return ToolResult::failure(
                "no content given",
                "write_file needs a 'content' parameter holding the complete file text. To "
                "create an empty file, pass content as an empty string.");
        }
        const auto content = arg_string(arguments, "content");
        std::optional<ToolResult> observation;
        if (context.whole_file_write_check) {
            observation = context.whole_file_write_check(*path);
            if (!observation->ok) return std::move(*observation);
        }
        const auto style = detect_style(*path);

        // Read before overwriting, so a rewrite that quietly drops a file's
        // tests can be compared with what was there.
        std::string previous;
        if (style.existed) {
            if (auto existing = fs::read_text(*path, kMaxEditFileBytes)) {
                previous = existing->content;
            }
        }

        // Recheck ordinary metadata after style/integrity reads. This narrows
        // stale observations; it is not an atomic compare-and-swap guarantee.
        if (context.whole_file_write_check) {
            observation = context.whole_file_write_check(*path);
            if (!observation->ok) return std::move(*observation);
        }
        auto written = fs::write_text_atomic(*path, content, style.encoding, style.line_ending);
        if (!written) {
            ToolResult result =
                ToolResult::failure("write failed: " + input, written.error().to_string());
            result.evidence.push_back(
                make_evidence(EvidenceKind::FileWritten, input, written.error().message, false));
            return result;
        }

        const auto relative = relative_to_workspace(context, *path);
        context.workspace->record_agent_change(
            *path, style.existed ? AgentChangeKind::Modified : AgentChangeKind::Created);

        const auto line_count = strings::split_lines(content).size();
        auto result = ToolResult::success(
            (style.existed ? "Rewrote " : "Created ") + relative + " (" +
                std::to_string(line_count) + " lines)",
            (style.existed ? "Rewrote " : "Created ") + relative + " with " +
                std::to_string(line_count) + " lines (" + strings::human_bytes(content.size()) +
                ", " + std::string{fs::to_string(style.line_ending)} + " line endings).");
        result.evidence.push_back(make_evidence(EvidenceKind::FileWritten, relative,
                                                std::to_string(line_count) + " lines", true));
        // Said first, because the file did not land where it was asked for.
        if (auto note = reinterpretation_note(context, input, PathAccess::Create); !note.empty()) {
            result.content = note + result.content;
        }
        if (observation) result.structured = std::move(observation->structured);
        note_integrity(result, relative, previous, content);
        return result;
    }
};

// --- replace_in_file -------------------------------------------------------
class ReplaceInFileTool final : public ITool {
public:
    std::string name() const override { return "replace_in_file"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Replaces an exact block of text in a file with new text. This is the preferred "
               "way to edit existing code: it touches only the lines you name and fails loudly "
               "rather than guessing. The 'find' text must appear exactly once unless "
               "replace_all is set - include enough surrounding lines to make it unique. Read the "
               "file first so the text you give matches the file byte for byte.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"path", schema::string_param("File to edit.")},
             {"find", schema::string_param(
                          "The exact existing text to replace, including indentation. Must match "
                          "the file exactly.")},
             {"replace", schema::string_param("The text to put in its place.")},
             {"replace_all",
              schema::boolean_param("Replace every occurrence instead of requiring exactly one. "
                                    "Defaults to false.",
                                    false)},
             {"overwrite_uncommitted",
              schema::boolean_param("Allow editing a file that already had uncommitted changes.",
                                    false)}},
            {"path", "find", "replace"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto input = arg_string(arguments, "path");
        auto path = resolve(context, input, PathAccess::Write);
        if (!path) return ToolResult::failure("cannot edit " + input, path.error().message);

        if (auto refusal = guard_uncommitted(context, *path,
                                             arg_bool(arguments, "overwrite_uncommitted", false))) {
            return *refusal;
        }

        const auto find = arg_string(arguments, "find");
        const auto replace = arg_string(arguments, "replace");
        const bool replace_all = arg_bool(arguments, "replace_all", false);

        if (find.empty()) {
            return ToolResult::failure("empty search text",
                                       "'find' must not be empty. Use write_file to replace a "
                                       "whole file.");
        }
        if (find == replace) {
            return ToolResult::failure("no change requested",
                                       "'find' and 'replace' are identical, so this edit would do "
                                       "nothing.");
        }

        auto file = fs::read_text(*path, kMaxEditFileBytes);
        if (!file) return ToolResult::failure("cannot read " + input, file.error().message);
        if (file->truncated) {
            return ToolResult::failure(
                "incomplete source: " + relative_to_workspace(context, *path),
                "The internal file read was truncated at the 8 MiB edit limit. "
                "A complete source file is required; no file was changed.");
        }

        // The file in memory is LF-normalised; normalise the search text too so
        // a model that emitted CRLF still matches.
        const auto needle = strings::replace_all(find, "\r\n", "\n");

        std::size_t occurrences = 0;
        for (std::size_t pos = file->content.find(needle); pos != std::string::npos;
             pos = file->content.find(needle, pos + needle.size())) {
            ++occurrences;
        }

        if (occurrences == 0) {
            // Being specific about *why* it failed saves the model a whole turn.
            std::string hint;
            const auto first_line = strings::trim(
                strings::split_lines(needle).empty() ? needle : strings::split_lines(needle)[0]);
            if (!first_line.empty() && file->content.find(first_line) != std::string::npos) {
                hint =
                    "\nThe first line of your 'find' text does appear in the file, so the "
                    "mismatch is probably in the indentation or in a later line. Read the exact "
                    "region with read_file_range and copy the text verbatim.";
            } else {
                hint =
                    "\nThe complete exact block was not found; parts may still be present. Read "
                    "the file to see its current contents before editing.";
            }
            auto result =
                ToolResult::failure("text not found in " + relative_to_workspace(context, *path),
                                    "The 'find' text does not appear in " +
                                        relative_to_workspace(context, *path) + "." + hint);
            annotate_exact_find_failure(result, find, needle, *file);
            return result;
        }
        if (occurrences > 1 && !replace_all) {
            return ToolResult::failure(
                "text is not unique in " + relative_to_workspace(context, *path),
                "The 'find' text appears " + std::to_string(occurrences) +
                    " times. Include more surrounding context to identify the one you mean, or "
                    "pass replace_all=true to change all of them.");
        }

        const auto replacement = strings::replace_all(replace, "\r\n", "\n");
        std::string updated;
        if (replace_all) {
            updated = strings::replace_all(file->content, needle, replacement);
        } else {
            const auto position = file->content.find(needle);
            updated = file->content.substr(0, position) + replacement +
                      file->content.substr(position + needle.size());
        }

        const auto line_ending =
            file->line_ending == fs::LineEnding::Mixed ? fs::LineEnding::Lf : file->line_ending;
        auto written = fs::write_text_atomic(*path, updated, file->encoding, line_ending);
        if (!written) {
            return ToolResult::failure("write failed", written.error().to_string());
        }

        const auto relative = relative_to_workspace(context, *path);
        context.workspace->record_agent_change(*path, AgentChangeKind::Modified);

        const auto before_lines = strings::split_lines(needle).size();
        const auto after_lines = strings::split_lines(replacement).size();
        auto result =
            ToolResult::success("Edited " + relative + " (" + std::to_string(occurrences) +
                                    " replacement" + (occurrences == 1 ? "" : "s") + ")",
                                "Replaced " + std::to_string(occurrences) + " occurrence(s) in " +
                                    relative + ": " + std::to_string(before_lines) +
                                    " line(s) became " + std::to_string(after_lines) + ".");
        result.evidence.push_back(make_evidence(EvidenceKind::FileWritten, relative,
                                                std::to_string(occurrences) + " replacement(s)",
                                                true));
        note_integrity(result, relative, file->content, updated);
        return result;
    }
};

// --- apply_patch -----------------------------------------------------------
// Applies a unified diff. Hunks are located by their context rather than
// trusting the line numbers, because a model's line numbers drift as soon as an
// earlier hunk changes the file length.
struct HunkApplication {
    bool applied = false;
    int offset = 0;   // how far from the stated position the hunk landed
    std::string failure;
};

std::vector<std::string> hunk_old_lines(const DiffHunk& hunk) {
    std::vector<std::string> lines;
    for (const auto& line : hunk.lines) {
        if (line.empty()) {
            lines.emplace_back();
            continue;
        }
        if (line[0] == ' ' || line[0] == '-') lines.push_back(line.substr(1));
    }
    return lines;
}

std::vector<std::string> hunk_new_lines(const DiffHunk& hunk) {
    std::vector<std::string> lines;
    for (const auto& line : hunk.lines) {
        if (line.empty()) {
            lines.emplace_back();
            continue;
        }
        if (line[0] == ' ' || line[0] == '+') lines.push_back(line.substr(1));
    }
    return lines;
}

// Inspection only. These facts never select a hunk, reinterpret escapes or
// authorize a write. One eligible failed hunk per tool call keeps the existing
// source receipt's 16-control / 4-line / 1024-encoded-byte bounds global.
bool patch_context_has_controls(const std::string& original, const DiffHunk& hunk) {
    const auto hidden = [](unsigned char byte) {
        return (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') || byte == 0x7f;
    };
    if (std::any_of(original.begin(), original.end(), hidden)) return true;
    return std::any_of(hunk.lines.begin(), hunk.lines.end(), [&](const std::string& line) {
        return !line.empty() && (line[0] == ' ' || line[0] == '-') &&
               std::any_of(line.begin() + 1, line.end(), hidden);
    });
}

ToolResult inspect_failed_patch_context(const std::string& target, const DiffHunk& hunk,
                                        const std::string& original, std::size_t original_lines) {
    ToolResult inspection;
    constexpr std::size_t kMaxPathJsonBytes = 256;
    constexpr std::size_t kMaxContextJsonBytes = 1024;
    constexpr std::size_t kMaxDiagnosticTextBytes = 8192;
    std::string encoded_path;
    if (target.size() + 2 <= kMaxPathJsonBytes) {
        encoded_path = Json(target).dump();
        if (encoded_path.size() > kMaxPathJsonBytes) encoded_path.clear();
    }
    inspection.structured = Json{
        {"hunk_old_start", hunk.old_start},
        {"hunk_old_count", hunk.old_count},
        {"path_label_omitted", encoded_path.empty()},
        {"path_json_max_bytes", kMaxPathJsonBytes},
        {"source_control_scope", "already loaded complete original normalized UTF-8 patch input"},
        {"source_control_input_truncated", false}};
    inspection.structured["path"] = target;   // Full authorized path remains structured data.
    inspection.content =
        "Failed patch context inspection for " +
        (encoded_path.empty() ? std::string{"[path omitted: exceeds 256 encoded bytes]"}
                              : encoded_path) +
        ", hunk old line " + std::to_string(hunk.old_start) + ":\n";
    fs::SourceRead source;
    source.file.content = original;
    source.file.line_count = original_lines;
    source.start_line = 1;
    source.end_line = original_lines;
    annotate_source_controls(inspection, source);

    // This is the parser's old context/removal text, joined with LF, rather
    // than an invented exact-find argument or a second decoding of the patch.
    const auto old_context = strings::join(hunk_old_lines(hunk), "\n");
    const auto control_count =
        std::count_if(old_context.begin(), old_context.end(), [](unsigned char byte) {
            return (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') || byte == 0x7f;
        });
    Json context{
        {"utf8_bytes", old_context.size()},
        {"control_count", control_count},
        {"json_omitted", true},
        {"json_max_bytes", kMaxContextJsonBytes},
        {"scope", "parsed old context/removal lines joined with LF; no second escape decoding"}};
    if (old_context.size() + 2 <= kMaxContextJsonBytes) {
        auto encoded = Json(old_context).dump();
        encoded = strings::replace_all(encoded, std::string(1, '\x7f'), "\\u007f");
        if (encoded.size() <= kMaxContextJsonBytes) {
            context["json_string"] = encoded;
            context["json_omitted"] = false;
            inspection.content +=
                "Parsed patch old context JSON (decode once):\nold_context: " + encoded + "\n";
        }
    }
    if (context.at("json_omitted").get<bool>())
        inspection.content +=
            "Parsed patch old context JSON omitted: exceeds 1024 encoded bytes; no clipped JSON is "
            "shown.\n";
    inspection.content +=
        "Inspection facts only. Printable backslash spellings remain printable bytes. "
        "Matching is unchanged; inspect the exact source and produce a new patch.\n";
    inspection.structured["parsed_old_context"] = std::move(context);

    // Generic context condensation retains only the beginning and end of a
    // tool result. Put one complete small JSON literal before the potentially
    // long failure list; this is a partial inspection view, never a source read.
    constexpr std::size_t kMaxCompactBytes = 520;
    constexpr std::size_t kMaxCompactJsonBytes = 256;
    const bool omit_compact_path = encoded_path.empty() || encoded_path.size() > 96;
    auto compact =
        "Failed patch context " +
        (omit_compact_path ? std::string{"[path omitted from compact view]"} : encoded_path) +
        "; hunk old line " + std::to_string(hunk.old_start) + ".\n";
    bool projected_source = false;
    if (inspection.structured.contains("source_control_lines")) {
        for (const auto& quoted : inspection.structured.at("source_control_lines")) {
            const auto encoded = quoted.at("json_string").get<std::string>();
            if (encoded.size() > kMaxCompactJsonBytes) continue;
            const auto line = "Exact source line JSON (inspection; decode once):\nline " +
                              std::to_string(quoted.at("line").get<std::size_t>()) + ": " +
                              encoded + "\n";
            if (compact.size() + line.size() <= kMaxCompactBytes) {
                compact += line;
                projected_source = true;
                break;
            }
        }
    }
    const bool omit_source =
        inspection.structured.at("source_control_count").get<std::size_t>() != 0 &&
        !projected_source;
    if (omit_source)
        compact +=
            "Source JSON omitted from compact view: no whole line fits 256 encoded bytes; read "
            "affected source.\n";
    const auto& received = inspection.structured.at("parsed_old_context");
    if (!projected_source && !received.at("json_omitted").get<bool>()) {
        const auto encoded = received.at("json_string").get<std::string>();
        const auto line =
            "Parsed old context JSON (inspection; decode once):\nold_context: " + encoded + "\n";
        if (encoded.size() <= kMaxCompactJsonBytes &&
            compact.size() + line.size() <= kMaxCompactBytes)
            compact += line;
    }
    const std::string partial = "Partial inspection view; more facts remain in the full result.\n";
    if (compact.size() + partial.size() <= kMaxCompactBytes) compact += partial;
    inspection.structured["compact_model_text"] = std::move(compact);
    inspection.structured["compact_model_text_max_bytes"] = kMaxCompactBytes;
    inspection.structured["compact_json_max_bytes"] = kMaxCompactJsonBytes;
    inspection.structured["compact_path_omitted"] = omit_compact_path;
    inspection.structured["compact_source_json_omitted"] = omit_source;
    inspection.structured["diagnostic_text_max_bytes"] = kMaxDiagnosticTextBytes;
    const bool omit_text = inspection.content.size() > kMaxDiagnosticTextBytes;
    inspection.structured["diagnostic_text_omitted"] = omit_text;
    if (omit_text)
        inspection.content =
            "Failed patch inspection text omitted: exceeds 8192 bytes; bounded structured facts "
            "remain. No clipped JSON is shown.\n";
    return inspection;
}

bool matches_at(const std::vector<std::string>& haystack, std::size_t position,
                const std::vector<std::string>& needle, bool ignore_trailing_whitespace) {
    if (position + needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i) {
        const auto& a = haystack[position + i];
        const auto& b = needle[i];
        if (a == b) continue;
        if (ignore_trailing_whitespace) {
            // Trailing-whitespace differences are the single most common reason a
            // model-produced patch fails to apply, and never meaningful in code.
            auto rstrip = [](const std::string& s) {
                const auto end = s.find_last_not_of(" \t");
                return end == std::string::npos ? std::string{} : s.substr(0, end + 1);
            };
            if (rstrip(a) == rstrip(b)) continue;
        }
        return false;
    }
    return true;
}

HunkApplication apply_hunk(std::vector<std::string>& lines, const DiffHunk& hunk) {
    HunkApplication outcome;
    const auto old_lines = hunk_old_lines(hunk);
    const auto new_lines = hunk_new_lines(hunk);

    if (old_lines.empty()) {
        // Pure insertion: place it at the stated line.
        const std::size_t at = std::min<std::size_t>(
            lines.size(), hunk.new_start > 0 ? static_cast<std::size_t>(hunk.new_start) - 1 : 0);
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), new_lines.begin(),
                     new_lines.end());
        outcome.applied = true;
        return outcome;
    }

    const std::size_t expected =
        hunk.old_start > 0 ? static_cast<std::size_t>(hunk.old_start) - 1 : 0;

    // The search has to be able to reach both ends of the file from wherever
    // the hunk claims to be - a hunk header naming line 50 of a 1-line file is
    // common when a model works from a stale reading.
    const std::size_t search_radius = std::max(expected, lines.size()) + 1;

    for (bool loose : {false, true}) {
        // Search outward from the expected position: exact first, then a widening
        // window, so a hunk that drifted a few lines still applies.
        for (std::size_t distance = 0; distance <= search_radius; ++distance) {
            const bool forward_ok = expected + distance + old_lines.size() <= lines.size();
            if (forward_ok && matches_at(lines, expected + distance, old_lines, loose)) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(expected + distance),
                            lines.begin() + static_cast<std::ptrdiff_t>(expected + distance +
                                                                        old_lines.size()));
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(expected + distance),
                             new_lines.begin(), new_lines.end());
                outcome.applied = true;
                outcome.offset = static_cast<int>(distance);
                return outcome;
            }
            if (distance > 0 && distance <= expected &&
                matches_at(lines, expected - distance, old_lines, loose)) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(expected - distance),
                            lines.begin() + static_cast<std::ptrdiff_t>(expected - distance +
                                                                        old_lines.size()));
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(expected - distance),
                             new_lines.begin(), new_lines.end());
                outcome.applied = true;
                outcome.offset = -static_cast<int>(distance);
                return outcome;
            }
            // Stop once the window has passed both ends of the file.
            if (distance > expected && !forward_ok) break;
        }
    }

    outcome.failure = "context does not match anywhere in the file";
    return outcome;
}

class ApplyPatchTool final : public ITool {
public:
    std::string name() const override { return "apply_patch"; }

    bool mutates() const override { return true; }

    std::string description() const override {
        return "Applies a unified diff to the workspace, the same format 'git diff' produces. "
               "Use this to change several places in a file, or several files, in one step. "
               "Hunks are located by their context lines, so exact line numbers do not have to be "
               "perfect, but the context lines themselves must match the current file. If a hunk "
               "cannot be placed the whole patch is rejected and nothing is written.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"patch",
              schema::string_param(
                  "The unified diff. Use Git-style 'diff --git a/path b/path' headers or "
                  "paired standard '--- a/path' / '+++ b/path' headers followed by "
                  "complete counted @@ hunks. Standard header timestamps may follow a tab.")},
             {"overwrite_uncommitted",
              schema::boolean_param("Allow patching files that already had uncommitted changes.",
                                    false)}},
            {"patch"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        const auto patch_text = arg_string(arguments, "patch");
        if (strings::trim(patch_text).empty()) {
            return ToolResult::failure("empty patch", "The 'patch' parameter is required.");
        }

        auto parsed = GitService::parse_unified_diff(patch_text);
        if (parsed.files.empty()) {
            return ToolResult::failure(
                "no files in patch",
                "No complete supported file changes were parsed. Use 'diff --git a/path b/path' "
                "or paired '--- a/path' and '+++ b/path' headers followed by complete counted "
                "@@ hunks. A malformed or incomplete standard unified block rejects the whole "
                "patch.");
        }

        const bool allow_dirty = arg_bool(arguments, "overwrite_uncommitted", false);

        // Two passes: validate and stage everything in memory, then write. A
        // patch that fails halfway must not leave the tree half-modified.
        struct Staged {
            std::filesystem::path path;
            std::string content;
            fs::TextEncoding encoding;
            fs::LineEnding line_ending;
            bool existed;
            AgentChangeKind kind;
            int additions;
            int deletions;
            std::string relative;
            std::string previous;   // for the integrity comparison
        };

        std::vector<Staged> staged;
        std::vector<std::string> problems;
        std::optional<ToolResult> patch_inspection;
        std::size_t patch_inspections_omitted = 0;

        for (const auto& file_diff : parsed.files) {
            if (file_diff.binary) {
                problems.push_back(file_diff.path + ": binary patches are not supported");
                continue;
            }
            const auto target =
                file_diff.change == GitChange::Deleted && !file_diff.old_path.empty()
                    ? file_diff.old_path
                    : file_diff.path;
            if (target.empty() || target == "/dev/null") continue;

            auto path = resolve(
                context, target,
                file_diff.change == GitChange::Added ? PathAccess::Create : PathAccess::Write);
            if (!path) {
                problems.push_back(target + ": " + path.error().message);
                continue;
            }

            if (auto refusal = guard_uncommitted(context, *path, allow_dirty)) {
                problems.push_back(target +
                                   ": has uncommitted changes; pass "
                                   "overwrite_uncommitted=true to patch it anyway");
                continue;
            }

            const auto style = detect_style(*path);
            std::vector<std::string> lines;
            std::string previous;
            std::size_t original_line_count = 0;
            if (style.existed) {
                auto file = fs::read_text(*path, kMaxEditFileBytes);
                if (!file) {
                    problems.push_back(target + ": " + file.error().message);
                    continue;
                }
                if (file->truncated) {
                    problems.push_back(target +
                                       ": internal file read was truncated at the "
                                       "8 MiB edit limit; complete source is required");
                    continue;
                }
                previous = file->content;
                original_line_count = file->line_count;
                lines = strings::split_lines(file->content);
            } else if (file_diff.change != GitChange::Added) {
                problems.push_back(target + ": file does not exist");
                continue;
            }

            bool file_ok = true;
            // Apply hunks bottom-up so earlier edits do not shift later ones.
            auto hunks = file_diff.hunks;
            std::sort(hunks.begin(), hunks.end(), [](const DiffHunk& a, const DiffHunk& b) {
                return a.old_start > b.old_start;
            });
            for (const auto& hunk : hunks) {
                if (hunk.lines.empty()) continue;
                auto outcome = apply_hunk(lines, hunk);
                if (!outcome.applied) {
                    problems.push_back(target + ": hunk at line " + std::to_string(hunk.old_start) +
                                       " could not be applied - " + outcome.failure);
                    if (patch_context_has_controls(previous, hunk)) {
                        if (patch_inspection) {
                            ++patch_inspections_omitted;
                        } else {
                            patch_inspection = inspect_failed_patch_context(target, hunk, previous,
                                                                            original_line_count);
                        }
                    }
                    file_ok = false;
                    break;
                }
            }
            if (!file_ok) continue;

            Staged entry;
            entry.path = *path;
            entry.content = strings::join(lines, "\n");
            if (!entry.content.empty()) entry.content.push_back('\n');
            entry.encoding = style.encoding;
            entry.line_ending = style.line_ending;
            entry.existed = style.existed;
            entry.kind = style.existed ? AgentChangeKind::Modified : AgentChangeKind::Created;
            entry.additions = file_diff.additions;
            entry.deletions = file_diff.deletions;
            entry.relative = relative_to_workspace(context, *path);
            entry.previous = std::move(previous);
            staged.push_back(std::move(entry));
        }

        if (!problems.empty()) {
            auto result = ToolResult::failure(
                "patch rejected (" + std::to_string(problems.size()) + " problem(s))",
                "The patch was not applied and no files were changed:\n  " +
                    strings::join(problems, "\n  ") +
                    "\n\nRead the current contents of the affected files and produce a patch whose "
                    "context lines match them exactly.");
            if (patch_inspection) {
                result.error =
                    patch_inspection->structured.at("compact_model_text").get<std::string>() +
                    "\n" + result.error;
                result.content = std::move(patch_inspection->content);
                result.structured["patch_context_diagnostic"] =
                    std::move(patch_inspection->structured);
                result.structured["patch_context_diagnostics_omitted"] = patch_inspections_omitted;
                if (patch_inspections_omitted != 0)
                    result.content += "Additional hidden-control patch inspections omitted: " +
                                      std::to_string(patch_inspections_omitted) +
                                      ". One bounded diagnostic per tool call.\n";
                if (result.content.size() > 8192) {
                    result.content =
                        "Failed patch inspection text omitted: exceeds 8192 bytes; bounded "
                        "structured facts remain. No clipped JSON is shown.\n";
                    result.structured["patch_context_diagnostic"]["diagnostic_text_omitted"] = true;
                }
            }
            return result;
        }

        std::vector<std::string> written_files;
        for (const auto& entry : staged) {
            auto written =
                fs::write_text_atomic(entry.path, entry.content, entry.encoding, entry.line_ending);
            if (!written) {
                return ToolResult::failure(
                    "patch partially applied",
                    "Failed while writing " + entry.relative + ": " + written.error().message +
                        "\nFiles already written: " + strings::join(written_files, ", "));
            }
            context.workspace->record_agent_change(entry.path, entry.kind);
            written_files.push_back(entry.relative);
        }

        std::string report =
            "Applied the patch to " + std::to_string(staged.size()) + " file(s):\n";
        for (const auto& entry : staged) {
            report += "  " + entry.relative + "  +" + std::to_string(entry.additions) + " -" +
                      std::to_string(entry.deletions) + "\n";
        }

        auto result = ToolResult::success(
            "Applied patch to " + std::to_string(staged.size()) + " file(s)", report);
        for (const auto& entry : staged) {
            result.evidence.push_back(make_evidence(
                EvidenceKind::FileWritten, entry.relative,
                "+" + std::to_string(entry.additions) + " -" + std::to_string(entry.deletions),
                true));
            note_integrity(result, entry.relative, entry.previous, entry.content);
        }
        return result;
    }
};

}   // namespace

std::vector<ToolPtr> make_edit_tools() {
    return {std::make_shared<WriteFileTool>(), std::make_shared<ReplaceInFileTool>(),
            std::make_shared<ApplyPatchTool>()};
}

}   // namespace localcode
