#include "../util/Strings.h"
#include "HttpClient.h"

#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>

namespace localcode {
namespace {
using Clock = std::chrono::steady_clock;

struct CurlGlobal {
    CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);

    ~CurlGlobal() {
        if (status == CURLE_OK) curl_global_cleanup();
    }
};

std::shared_ptr<CurlGlobal> curl_global() {
    static auto state = std::make_shared<CurlGlobal>();
    return state;
}

// Each lease belongs to one request at a time. The multi handle owns its
// connection cache, so neither concurrent threads nor clients share sockets.
struct Transport {
    CURL* easy = curl_easy_init();
    CURLM* multi = curl_multi_init();

    Transport() {
        if (multi) curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, 4L);
    }

    ~Transport() {
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
    }
};

struct Headers {
    curl_slist* value = nullptr;

    ~Headers() { curl_slist_free_all(value); }
};

struct Transfer {
    CURL* easy;
    const HttpRequest& request;
    const HttpStreamCallback& on_chunk;
    CancellationToken cancel;
    std::uint64_t* content_length = nullptr;
    HttpResponse response{};
    Clock::time_point activity = Clock::now();
    bool stopped = false;
    bool callback_failed = false;
    bool insecure_redirect = false;
    int status = 0;

    static std::size_t body(char* bytes, std::size_t size, std::size_t count,
                            void* userdata) noexcept {
        auto& self = *static_cast<Transfer*>(userdata);
        const auto length = size * count;
        self.activity = Clock::now();
        if (self.cancel.cancelled()) return 0;
        if (self.request.follow_redirects &&
            (self.status == 301 || self.status == 302 || self.status == 303 || self.status == 307 ||
             self.status == 308))
            return length;
        try {
            if (self.on_chunk && !self.on_chunk(std::string_view(bytes, length))) {
                self.stopped = true;
                return 0;
            }
            return length;
        } catch (...) {
            self.callback_failed = true;
            return 0;
        }
    }

    static std::size_t header(char* bytes, std::size_t size, std::size_t count,
                              void* userdata) noexcept {
        auto& self = *static_cast<Transfer*>(userdata);
        const auto length = size * count;
        self.activity = Clock::now();
        try {
            const std::string_view line(bytes, length);
            if (line.starts_with("HTTP/")) {
                self.response.headers.clear();
                const auto space = line.find(' ');
                self.status = space == std::string_view::npos
                                  ? 0
                                  : std::atoi(std::string(line.substr(space + 1)).c_str());
            } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
                const auto key = strings::to_lower(strings::trim(line.substr(0, colon)));
                auto value = strings::trim(line.substr(colon + 1));
                if (key == "content-length" && self.content_length) {
                    std::uint64_t total = 0;
                    const auto parsed =
                        std::from_chars(value.data(), value.data() + value.size(), total);
                    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
                        *self.content_length = total;
                }
                if (key == "location" && self.request.follow_redirects) {
                    char* current = nullptr;
                    curl_easy_getinfo(self.easy, CURLINFO_EFFECTIVE_URL, &current);
                    if (current && strings::to_lower(current).starts_with("https:") &&
                        strings::to_lower(value).starts_with("http:")) {
                        self.insecure_redirect = true;
                        return 0;
                    }
                }
                self.response.headers[key] = std::move(value);
            }
            return length;
        } catch (...) {
            self.callback_failed = true;
            return 0;
        }
    }
};

class CurlHttpClient final : public HttpClient {
public:
    std::string backend_name() const override { return "libcurl"; }

    Result<HttpResponse> send(const HttpRequest& request, CancellationToken cancel) override {
        std::string body;
        bool truncated = false;
        auto response = perform(
            request,
            [&](std::string_view chunk) {
                const auto room =
                    request.max_response_bytes - std::min(body.size(), request.max_response_bytes);
                body.append(chunk.substr(0, room));
                if (chunk.size() > room) {
                    truncated = true;
                    return false;
                }
                return true;
            },
            cancel, false);
        if (!response) return response;
        response->body = std::move(body);
        response->truncated = truncated;
        return response;
    }

    Result<HttpResponse> stream(const HttpRequest& request, const HttpStreamCallback& on_chunk,
                                CancellationToken cancel) override {
        return perform(request, on_chunk, cancel, true);
    }

    Status download(const HttpRequest& request, const std::filesystem::path& destination,
                    const std::function<void(std::uint64_t, std::uint64_t)>& on_progress,
                    CancellationToken cancel) override {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "request cancelled");
        std::error_code ec;
        if (!destination.parent_path().empty())
            std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec)
            return make_error(ErrorCode::IoError, "cannot create download directory", ec.message());
        auto partial = destination;
        partial += ".part";
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file) return make_error(ErrorCode::IoError, "cannot create download file");
        std::uint64_t received = 0;
        bool write_failed = false;
        std::uint64_t total = 0;
        auto response = perform(
            request,
            [&](std::string_view chunk) {
                file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                if (!file) {
                    write_failed = true;
                    return false;
                }
                received += chunk.size();
                if (on_progress) on_progress(received, total);
                return true;
            },
            cancel, false, &total);
        file.close();
        write_failed = write_failed || !file;
        if (!response || !response->ok() || write_failed) {
            std::filesystem::remove(partial, ec);
            if (write_failed) return make_error(ErrorCode::IoError, "cannot write download file");
            if (!response) return response.error();
            return make_error(ErrorCode::NetworkError,
                              "download failed with HTTP " + std::to_string(response->status_code));
        }
        std::filesystem::rename(partial, destination, ec);
        if (ec) {
            const auto detail = ec.message();
            std::filesystem::remove(partial, ec);
            return make_error(ErrorCode::IoError, "cannot finish download", detail);
        }
        return ok_status();
    }

private:
    std::shared_ptr<CurlGlobal> global_ = curl_global();
    std::mutex pool_mutex_;
    std::vector<std::unique_ptr<Transport>> idle_;

    struct Lease {
        CurlHttpClient& owner;
        std::unique_ptr<Transport> transport;

        ~Lease() {
            if (transport->easy) curl_easy_reset(transport->easy);
            std::lock_guard lock(owner.pool_mutex_);
            if (owner.idle_.size() < 4) owner.idle_.push_back(std::move(transport));
        }
    };

    std::unique_ptr<Transport> acquire() {
        std::lock_guard lock(pool_mutex_);
        if (idle_.empty()) return std::make_unique<Transport>();
        auto transport = std::move(idle_.back());
        idle_.pop_back();
        return transport;
    }

    Result<HttpResponse> perform(const HttpRequest& request, const HttpStreamCallback& on_chunk,
                                 CancellationToken cancel, bool streaming,
                                 std::uint64_t* content_length = nullptr) {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "request cancelled");
        if (global_->status != CURLE_OK)
            return make_error(ErrorCode::NetworkError, "libcurl initialization failed");
        if (!strings::to_lower(request.url).starts_with("http://") &&
            !strings::to_lower(request.url).starts_with("https://"))
            return make_error(ErrorCode::InvalidArgument,
                              "HTTP transport requires an http or https URL");
        Lease lease{*this, acquire()};
        auto& transport = *lease.transport;
        if (!transport.easy || !transport.multi)
            return make_error(ErrorCode::NetworkError, "cannot allocate HTTP transport");
        CURL* easy = transport.easy;
        Transfer transfer{easy, request, on_chunk, cancel, content_length};
        Headers headers;
        for (const auto& [name, value] : request.headers) {
            auto* added = curl_slist_append(headers.value, (name + ": " + value).c_str());
            if (!added) return make_error(ErrorCode::NetworkError, "cannot allocate HTTP headers");
            headers.value = added;
        }
        std::array<char, CURL_ERROR_SIZE> detail{};
        CURLcode option_error = CURLE_OK;
        const auto set = [&](auto option, auto value) {
            const auto status = curl_easy_setopt(easy, option, value);
            if (option_error == CURLE_OK) option_error = status;
        };
        set(CURLOPT_URL, request.url.c_str());
        set(CURLOPT_USERAGENT, "LocalCode/" LOCALCODE_VERSION);
        set(CURLOPT_HTTPHEADER, headers.value);
        set(CURLOPT_ERRORBUFFER, detail.data());
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_CONNECTTIMEOUT_MS,
            static_cast<long>(std::clamp<std::int64_t>(request.connect_timeout.count(), 0,
                                                       std::numeric_limits<long>::max())));
        set(CURLOPT_FOLLOWLOCATION, request.follow_redirects ? 1L : 0L);
        set(CURLOPT_MAXREDIRS, static_cast<long>(std::min<std::size_t>(
                                   request.max_redirects, std::numeric_limits<long>::max())));
#if LIBCURL_VERSION_NUM >= 0x075500
        set(CURLOPT_PROTOCOLS_STR, "http,https");
        set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        set(CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        set(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        set(CURLOPT_WRITEFUNCTION, &Transfer::body);
        set(CURLOPT_WRITEDATA, &transfer);
        set(CURLOPT_HEADERFUNCTION, &Transfer::header);
        set(CURLOPT_HEADERDATA, &transfer);
        if (request.method == HttpMethod::Head) set(CURLOPT_NOBODY, 1L);
        if (request.method == HttpMethod::Post) {
            set(CURLOPT_POST, 1L);
            set(CURLOPT_POSTFIELDS, request.body.data());
            set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        }
        if (option_error != CURLE_OK)
            return make_error(ErrorCode::NetworkError, "cannot configure HTTP request",
                              curl_easy_strerror(option_error));
        if (curl_multi_add_handle(transport.multi, easy) != CURLM_OK)
            return make_error(ErrorCode::NetworkError, "cannot start HTTP request");

        struct Attachment {
            Transport& transport;

            ~Attachment() { curl_multi_remove_handle(transport.multi, transport.easy); }
        } attachment{transport};

        const auto started = Clock::now();
        CURLcode result = CURLE_OK;
        for (;;) {
            if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "request cancelled");
            const auto since = streaming ? transfer.activity : started;
            if (request.timeout.count() > 0 && Clock::now() - since >= request.timeout)
                return make_error(ErrorCode::Timeout, "HTTP request timed out");
            int running = 0;
            if (curl_multi_perform(transport.multi, &running) != CURLM_OK)
                return make_error(ErrorCode::NetworkError, "HTTP transfer failed");
            if (content_length) {
                curl_off_t length = -1;
                if (curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) ==
                        CURLE_OK &&
                    length >= 0)
                    *content_length = static_cast<std::uint64_t>(length);
            }
            if (!running) {
                int remaining = 0;
                auto* message = curl_multi_info_read(transport.multi, &remaining);
                if (!message || message->msg != CURLMSG_DONE)
                    return make_error(ErrorCode::NetworkError,
                                      "HTTP transfer has no completion result");
                result = message->data.result;
                break;
            }
            if (curl_multi_poll(transport.multi, nullptr, 0, 100, nullptr) != CURLM_OK)
                return make_error(ErrorCode::NetworkError, "HTTP polling failed");
        }
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "request cancelled");
        if (transfer.insecure_redirect)
            return make_error(ErrorCode::NetworkError, "HTTPS redirect to HTTP refused");
        if (transfer.callback_failed)
            return make_error(ErrorCode::Internal, "HTTP callback failed");
        if (result != CURLE_OK && !transfer.stopped) {
            const auto code =
                result == CURLE_OPERATION_TIMEDOUT ? ErrorCode::Timeout : ErrorCode::NetworkError;
            return make_error(code, "HTTP request failed",
                              detail[0] ? detail.data() : curl_easy_strerror(result));
        }
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        transfer.response.status_code = static_cast<int>(status);
        char* final_url = nullptr;
        curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &final_url);
        transfer.response.final_url = final_url ? final_url : request.url;
        transfer.response.duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        return std::move(transfer.response);
    }
};
}   // namespace

std::shared_ptr<HttpClient> HttpClient::create() {
    return std::make_shared<CurlHttpClient>();
}
}   // namespace localcode
