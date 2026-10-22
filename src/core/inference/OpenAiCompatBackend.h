#pragma once
// ---------------------------------------------------------------------------
// Backend that speaks the OpenAI-compatible HTTP API.
//
// Both llama.cpp's llama-server and ik_llama.cpp expose this API, which is why
// v1 talks to them over HTTP rather than linking libllama directly: it works
// with whatever build of either runtime the user already has, including ones
// compiled for their exact GPU, and a crashing model server cannot take the
// editor down with it.
//
// The two runtimes differ in their extensions, not their core, so the
// differences live in the small subclasses at the bottom of this file rather
// than being sprayed through the agent.
// ---------------------------------------------------------------------------
#include <list>
#include <memory>
#include <mutex>
#include <string>

#include "ChatTypes.h"
#include "HttpClient.h"

namespace localcode {

struct BackendEndpoint {
    std::string base_url = "http://127.0.0.1:8080";
    std::string api_key;   // llama-server's --api-key, when set
    std::chrono::milliseconds request_timeout{600000};
    // Set only for a managed llama.cpp binary that advertises the native budget.
    // External OpenAI-compatible providers keep their original payload shape.
    bool native_reasoning_budget = false;
};

class OpenAiCompatBackend : public IInferenceBackend {
public:
    // Turns a non-2xx chat response into an error that says what the server
    // said. A tool call the server could not parse is named as such, because
    // it is the model's doing and it is recoverable. Pure, and public so the
    // wording can be pinned down by tests.
    [[nodiscard]] static Error chat_http_error(int status_code, const std::string& body);

    OpenAiCompatBackend(BackendEndpoint endpoint, std::shared_ptr<HttpClient> http);

    [[nodiscard]] std::string id() const override { return "openai-compat"; }

    [[nodiscard]] std::string display_name() const override { return "OpenAI-compatible server"; }

    [[nodiscard]] BackendCapabilities capabilities() const override;

    Result<ChatResult> chat(const ChatRequest& request, const StreamHandlers& handlers,
                            CancellationToken cancel) override;
    Result<int> count_tokens(std::string_view text, CancellationToken cancel) override;
    Status health(CancellationToken cancel) override;
    Result<BackendInfo> info(CancellationToken cancel) override;

    void set_endpoint(BackendEndpoint endpoint);

    [[nodiscard]] const BackendEndpoint& endpoint() const { return endpoint_; }

    // Asked, when a request fails at the transport level, whether the process on
    // the other end is still alive - and if not, what it said on the way out.
    //
    // A dropped connection has two very different causes with identical
    // symptoms: the server died mid-generation, or the server is fine and the
    // fault is on this side. Both were reported here as
    // "the connection with the server was terminated abnormally", which sends
    // you looking in the wrong place. Whoever owns the process installs this so
    // the error can say which happened.
    using LivenessReporter = std::function<std::string()>;
    void set_liveness_reporter(LivenessReporter reporter);

protected:
    // Serialises a request. Subclasses override to add runtime-specific fields.
    [[nodiscard]] virtual Json build_payload(const ChatRequest& request) const;

    // Capability probing result, refreshed by info().
    mutable BackendCapabilities capabilities_;

    [[nodiscard]] std::string url_for(std::string_view path) const;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> default_headers() const;

    // Adds the server's liveness to a transport-level failure, when a reporter
    // is installed and has something to say.
    [[nodiscard]] Error annotate_transport_failure(Error error) const;

    BackendEndpoint endpoint_;
    std::shared_ptr<HttpClient> http_;
    LivenessReporter liveness_;

private:
    struct TokenCountEntry {
        std::string text;
        int tokens;
    };

    void clear_token_counts();
    std::mutex token_counts_mutex_;
    std::list<TokenCountEntry> token_counts_;
    std::size_t token_count_text_bytes_ = 0;
    std::uint64_t token_count_generation_ = 0;

    Result<ChatResult> chat_streaming(const ChatRequest& request, const Json& payload,
                                      const StreamHandlers& handlers, CancellationToken cancel);
    Result<ChatResult> chat_buffered(const ChatRequest& request, const Json& payload,
                                     const StreamHandlers& handlers, CancellationToken cancel);
};

// --- llama.cpp -------------------------------------------------------------
class LlamaCppBackend final : public OpenAiCompatBackend {
public:
    using OpenAiCompatBackend::OpenAiCompatBackend;

    [[nodiscard]] std::string id() const override { return "llama.cpp"; }

    [[nodiscard]] std::string display_name() const override { return "llama.cpp"; }
};

// --- ik_llama.cpp ----------------------------------------------------------
// A fork of llama.cpp with different quantization and attention kernels. Same
// API surface; kept as a distinct class so runtime-specific request fields and
// server arguments have somewhere to live as the fork diverges.
class IkLlamaBackend final : public OpenAiCompatBackend {
public:
    using OpenAiCompatBackend::OpenAiCompatBackend;

    [[nodiscard]] std::string id() const override { return "ik_llama.cpp"; }

    [[nodiscard]] std::string display_name() const override { return "ik_llama.cpp"; }
};

}   // namespace localcode
