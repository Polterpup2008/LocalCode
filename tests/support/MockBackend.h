#pragma once
// ---------------------------------------------------------------------------
// A scripted inference backend.
//
// The agent loop is the part of LocalCode most worth testing and the hardest to
// test against a real model, because a real model is slow and non-deterministic.
// This backend plays a fixed script of turns, which makes the whole loop -
// tool dispatch, evidence collection, progress detection, the final evidence
// check - exercisable in milliseconds and identical on every run.
// ---------------------------------------------------------------------------
#include <functional>
#include <string>
#include <vector>

#include "inference/ChatTypes.h"

namespace localcode::test {

struct ScriptedTurn {
    std::string content;
    std::string reasoning;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason = "stop";
};

class MockBackend final : public IInferenceBackend {
public:
    explicit MockBackend(std::vector<ScriptedTurn> script = {});

    // Adds a turn that asks for one tool call.
    MockBackend& then_call(std::string tool_name, Json arguments, std::string preamble = {});
    // Adds a turn that ends the run with a final answer.
    MockBackend& then_say(std::string content);

    // When set, the next turn is chosen by this function instead of the script.
    // Used to test reactions to tool results.
    void set_responder(std::function<ScriptedTurn(const ChatRequest&, int turn)> responder);

    // Simulates a blocking transport while exercising the run's actual token.
    void set_before_reply(std::function<void(CancellationToken)> hook) {
        before_reply_ = std::move(hook);
    }

    [[nodiscard]] std::string id() const override { return "mock"; }

    [[nodiscard]] std::string display_name() const override { return "Mock backend"; }

    [[nodiscard]] BackendCapabilities capabilities() const override;

    Result<ChatResult> chat(const ChatRequest& request, const StreamHandlers& handlers,
                            CancellationToken cancel) override;
    Result<int> count_tokens(std::string_view text, CancellationToken cancel) override;
    Status health(CancellationToken cancel) override;
    Result<BackendInfo> info(CancellationToken cancel) override;

    // --- Inspection -------------------------------------------------------
    [[nodiscard]] int turns_taken() const { return turn_; }

    [[nodiscard]] const std::vector<ChatRequest>& requests() const { return requests_; }

    [[nodiscard]] const ChatRequest& last_request() const { return requests_.back(); }

    // Fails the next call with this error instead of answering.
    void fail_next(Error error) { pending_error_ = std::move(error); }

private:
    std::vector<ScriptedTurn> script_;
    std::vector<ChatRequest> requests_;
    std::function<ScriptedTurn(const ChatRequest&, int)> responder_;
    std::function<void(CancellationToken)> before_reply_;
    std::optional<Error> pending_error_;
    int turn_ = 0;
};

}   // namespace localcode::test
