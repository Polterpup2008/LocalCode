#pragma once
// ---------------------------------------------------------------------------
// Context budgeting.
//
// A local model's context window is small and expensive, and the two things
// that fill it fastest are old tool output and a transcript nobody needs any
// more. This class decides what actually gets sent.
//
// The priority order, highest first:
//   1. The system prompt and the selected skills - the rules of the job.
//   2. The task state block - the mission, which must never be evicted.
//   3. The user's most recent message.
//   4. Recent conversation and tool results, newest first.
//   5. Older turns, condensed or dropped.
//
// The user's own prompt is never silently truncated. If it does not fit, that is
// reported as an error the user can see and act on.
// ---------------------------------------------------------------------------
#include <memory>
#include <string>
#include <vector>

#include "../inference/ChatTypes.h"

namespace localcode {

struct ContextBudget {
    int total = 8192;   // the model's context window
    int reserved_for_output = 2048;
    int system_prompt = 0;
    int skills = 0;
    int task_state = 0;
    int tool_definitions = 0;
    int conversation = 0;
    int used = 0;
    int available = 0;
    bool exact = false;   // true only for a fully tokenized serialized request

    [[nodiscard]] double fraction_used() const {
        return total > 0 ? static_cast<double>(used) / static_cast<double>(total) : 0.0;
    }
};

struct ContextBuildResult {
    std::vector<ChatMessage> messages;
    ContextBudget budget;
    int dropped_messages = 0;
    int condensed_messages = 0;
    int retained_source_excerpts = 0;
    bool prompt_does_not_fit = false;
    std::string note;   // shown in the UI when something was dropped
};

struct SourceAnchor {
    std::string path;
    std::size_t line = 0;
    std::size_t end_line = 0;
    // A source read call authenticates retained text from its current file version.
    // Empty ids remain lexical hints, never retained-source provenance.
    std::string tool_call_id = {};
};

class ContextManager {
public:
    ContextManager() = default;

    void set_context_size(int tokens);

    [[nodiscard]] int context_size() const { return context_size_; }

    void set_reserved_output(int tokens);

    [[nodiscard]] int reserved_output() const { return reserved_output_; }

    // Large text blocks use the runtime tokenizer when available. The complete
    // budget remains an estimate because message/tool-template overhead is not
    // tokenized as one serialized request.
    void set_backend(BackendPtr backend);

    [[nodiscard]] int count_tokens(std::string_view text) const;
    [[nodiscard]] int count_message(const ChatMessage& message) const;
    [[nodiscard]] int count_tools(const std::vector<ToolDefinition>& tools) const;

    // Assembles the request. `history` is the full conversation in order,
    // excluding the system message.
    [[nodiscard]] ContextBuildResult build(
        const std::string& system_prompt, const std::string& skills_block,
        const std::string& task_state_block, const std::vector<ChatMessage>& history,
        const std::vector<ToolDefinition>& tools,
        const std::vector<SourceAnchor>& source_anchors = {}) const;

    // Optional current-revision navigation data goes only on the copied latest
    // real request, after actual conversation/source evidence has its capacity.
    // It is omitted whole if it cannot fit; stored history is never changed.
    [[nodiscard]] ContextBuildResult build(const std::string& system_prompt,
                                           const std::string& skills_block,
                                           const std::string& task_state_block,
                                           const std::vector<ChatMessage>& history,
                                           const std::vector<ToolDefinition>& tools,
                                           const std::vector<SourceAnchor>& source_anchors,
                                           const std::string& current_request_source_context) const;

    // Closed reporting projection: complete genuine human context plus actual
    // ledger data and bounded current read receipts. No source IO or stored
    // history mutation; ambiguous/stale/partial source is explicitly omitted.
    [[nodiscard]] ContextBuildResult build_report(const std::string& report_record,
                                                  const std::vector<ChatMessage>& history,
                                                  const std::vector<SourceAnchor>& current_anchors,
                                                  CancellationToken cancel = {}) const;

    // Condenses an old tool result down to its first and last lines. Exposed for
    // testing, because getting this wrong quietly destroys information.
    [[nodiscard]] static std::string condense_tool_result(const std::string& content);

private:
    int context_size_ = 8192;
    int reserved_output_ = 2048;
    BackendPtr backend_;
    // Only a closed-reporting projection copy sets this. Ordinary discovery
    // keeps its prior default; report tokenization shares the run cancellation.
    CancellationToken counting_cancel_;
};

}   // namespace localcode
