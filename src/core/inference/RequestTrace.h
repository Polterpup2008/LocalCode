#pragma once
// ---------------------------------------------------------------------------
// Recording what was actually sent to the model, so an interaction can be
// reproduced rather than reconstructed.
//
// This exists because of a specific failure to investigate one. Qwen3.5-9B
// reached a verified objective ten times out of ten and produced no final
// answer eight of those times, and there was no way to replay the exact request
// that had failed: llama-server does not log prompts at any verbosity, its
// /slots endpoint reports only slot bookkeeping, and LocalCode kept nothing. The
// diagnosis had to be rebuilt from synthetic fixtures, which cost a day and
// left the absolute numbers untrustworthy.
//
// Off unless asked for. Tracing writes whole prompts to disk, and a debugging
// aid that turns itself on is a way to leak a workspace.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <string>

#include "../util/Json.h"
#include "ChatTypes.h"

namespace localcode {

// One model interaction, request and response together.
struct TraceEntry {
    std::string phase;   // "agent", "finalization", "finalization-retry"
    std::string model;
    ChatRequest request;
    // Response side. Left empty when the call failed, with `error` saying why.
    std::string content;
    std::string reasoning;
    std::string finish_reason;
    std::string error;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    bool usage_from_server = false;
    std::int64_t duration_ms = 0;
    int tool_calls = 0;
};

// Writes one JSON object per interaction into a run file. Disabled unless a
// destination is configured, in which case every method is a cheap no-op.
class RequestTrace {
public:
    // Enabled by LOCALCODE_TRACE_DIR naming a writable directory. Returns false
    // when tracing is off or the directory cannot be used - a trace that cannot
    // be written must not take the run down with it.
    static bool enabled();

    // Where entries are going, empty when disabled.
    [[nodiscard]] static std::filesystem::path directory();

    // Starts a new trace file for one agent run. Safe to call when disabled.
    static void begin_run(const std::string& label);

    // Appends one interaction. Message and tool content is written in full,
    // because a truncated prompt cannot be replayed; secrets are redacted first.
    static void record(const TraceEntry& entry);

    // Optional stage metadata goes to a separate *.events.jsonl file. Existing
    // replay files remain one model request per line; no per-token event log.
    static void record_event(std::string_view stage, const Json& fields);

    static void end_run();

    // The file the current run is writing to, empty when disabled.
    [[nodiscard]] static std::filesystem::path current_file();

    // Testing seam: point tracing at a directory without touching the
    // environment, and turn it off again afterwards.
    static void set_directory_for_testing(std::filesystem::path directory);
    static void reset_for_testing();

    // Serialises one entry exactly as it would be written. Exposed so a test can
    // assert the shape without going near the filesystem.
    [[nodiscard]] static Json to_json(const TraceEntry& entry);
};

}   // namespace localcode
