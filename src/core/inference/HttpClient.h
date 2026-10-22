#pragma once
// ---------------------------------------------------------------------------
// HTTP transport.
//
// Two very different consumers share this interface:
//   * the inference backends, which need streaming chunked reads with low
//     latency and immediate cancellation, and
//   * the web tools, which need ordinary request/response with a size cap.
//
// On Windows the implementation is WinHTTP: it ships with the OS, does TLS and
// proxy handling correctly, and adds no dependency to the build. Elsewhere
// libcurl is used when present. When neither is available the factory returns a
// client that fails every call with a clear message, rather than a stub that
// pretends to work.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../util/Cancellation.h"
#include "../util/Result.h"

namespace localcode {

enum class HttpMethod { Get, Post, Head };

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::chrono::milliseconds connect_timeout{15000};
    // Applies to the whole exchange for buffered requests. Streaming requests
    // use it as an inactivity timeout instead, because a long generation is not
    // a stalled connection.
    std::chrono::milliseconds timeout{300000};

    std::size_t max_response_bytes = 8u * 1024 * 1024;
    bool follow_redirects = true;
    std::size_t max_redirects = 5;
};

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;   // lower-cased keys
    bool truncated = false;
    std::int64_t duration_ms = 0;
    std::string final_url;

    [[nodiscard]] bool ok() const { return status_code >= 200 && status_code < 300; }

    [[nodiscard]] std::string header(std::string_view name) const;
};

// Called with each chunk as it arrives. Return false to abort the transfer.
using HttpStreamCallback = std::function<bool(std::string_view chunk)>;

class HttpClient {
public:
    virtual ~HttpClient() = default;

    virtual Result<HttpResponse> send(const HttpRequest& request,
                                      CancellationToken cancel = {}) = 0;

    // Streams the response body. The returned HttpResponse has an empty body;
    // status and headers are still filled in.
    virtual Result<HttpResponse> stream(const HttpRequest& request,
                                        const HttpStreamCallback& on_chunk,
                                        CancellationToken cancel = {}) = 0;

    // Downloads to a file, reporting progress. Kept separate from send() so a
    // large artifact never has to be held in memory.
    virtual Status download(
        const HttpRequest& request, const std::filesystem::path& destination,
        const std::function<void(std::uint64_t received, std::uint64_t total)>& on_progress,
        CancellationToken cancel = {}) = 0;

    [[nodiscard]] virtual std::string backend_name() const = 0;

    // Creates the platform's client.
    static std::shared_ptr<HttpClient> create();
};

// ---------------------------------------------------------------------------
// Server-Sent Events parsing.
//
// llama.cpp and ik_llama.cpp both stream chat completions as SSE. Chunks arrive
// split at arbitrary byte boundaries, so the parser has to buffer across calls
// - getting this wrong shows up as tokens silently vanishing under load.
// ---------------------------------------------------------------------------
class SseParser {
public:
    struct Event {
        std::string event;   // "event:" field, usually empty
        std::string data;    // concatenated "data:" lines
    };

    // Feeds bytes and invokes the callback for each complete event.
    void feed(std::string_view chunk, const std::function<void(const Event&)>& on_event);

    // Flushes a trailing event that was not terminated by a blank line.
    void finish(const std::function<void(const Event&)>& on_event);

    void reset();

private:
    void dispatch(const std::function<void(const Event&)>& on_event);

    std::string buffer_;
    std::string current_event_;
    std::string current_data_;
    bool has_data_ = false;
    // Keep the frequently accessed string members in their original layout.
    // Bytes in the unterminated line already checked for a newline. Transport
    // fragmentation must not make a long event quadratic to scan.
    std::size_t searched_bytes_ = 0;
};

}   // namespace localcode
