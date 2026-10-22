// ---------------------------------------------------------------------------
// WinHTTP implementation of HttpClient.
//
// Chosen over libcurl on Windows because it ships with the operating system:
// no vendored TLS stack, no certificate bundle to keep current, and system
// proxy settings work the way the user already configured them.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>

#include "../platform/PathEncoding.h"
#include "../util/Log.h"
#include "../util/Strings.h"
#include "HttpClient.h"

namespace localcode {
namespace {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

std::string win32_message(DWORD code) {
    // WinHTTP errors live in winhttp.dll's message table, not the system one.
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE;
    const HMODULE module = GetModuleHandleW(L"winhttp.dll");
    const DWORD length =
        FormatMessageW(flags, module, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message = length ? strings::trim(narrow({buffer, length})) : std::string{};
    if (buffer) LocalFree(buffer);
    if (message.empty()) message = "WinHTTP error " + std::to_string(code);
    return message;
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

std::optional<ParsedUrl> crack_url(const std::string& url) {
    const auto wide = widen(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof components;
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    components.dwSchemeLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components)) {
        return std::nullopt;
    }

    ParsedUrl parsed;
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) {
        parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (parsed.path.empty()) parsed.path = L"/";
    parsed.port = components.nPort;
    parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return parsed;
}

// Owns a WinHTTP handle and lets another thread close it to abort a blocking
// call. Closing the request handle mid-read is the documented way to cancel.
class SharedHandle {
public:
    void reset(HINTERNET handle) {
        std::lock_guard lock(mutex_);
        if (handle_) WinHttpCloseHandle(handle_);
        handle_ = handle;
    }

    HINTERNET get() {
        std::lock_guard lock(mutex_);
        return handle_;
    }

    void close() {
        std::lock_guard lock(mutex_);
        if (handle_) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    ~SharedHandle() { close(); }

private:
    std::mutex mutex_;
    HINTERNET handle_ = nullptr;
};

struct SessionHandles {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    ParsedUrl origin;
    std::vector<std::pair<std::string, std::string>> headers;
    bool cacheable = false;

    ~SessionHandles() {
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

// Native receive timeouts do not enforce HttpRequest's whole-exchange bound.
// An OS thread-pool timer closes only this request; it creates no polling thread.
class RequestDeadline {
public:
    RequestDeadline(SharedHandle& handle, std::chrono::milliseconds timeout)
        : handle_(handle), timeout_(timeout) {
        if (timeout_.count() > 0)
            timer_ = CreateThreadpoolTimer(expire, this, nullptr);
    }

    ~RequestDeadline() {
        if (!timer_) return;
        SetThreadpoolTimer(timer_, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer_, TRUE);
        CloseThreadpoolTimer(timer_);
    }

    bool valid() const { return timeout_.count() <= 0 || timer_ != nullptr; }
    bool expired() const { return expired_.load(); }

    void arm(std::chrono::milliseconds delay) {
        if (!timer_) return;
        // Negative FILETIME values are relative intervals in 100ns units.
        const auto milliseconds = std::clamp<std::int64_t>(
            delay.count(), 1, std::numeric_limits<std::int64_t>::max() / 10000);
        ULARGE_INTEGER due{};
        due.QuadPart = static_cast<ULONGLONG>(-milliseconds * 10000);
        FILETIME when{due.LowPart, due.HighPart};
        SetThreadpoolTimer(timer_, &when, 0, 0);
    }

    void activity() { arm(timeout_); }

private:
    static void CALLBACK expire(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
        auto& deadline = *static_cast<RequestDeadline*>(context);
        deadline.expired_.store(true);
        deadline.handle_.close();
    }

    SharedHandle& handle_;
    std::chrono::milliseconds timeout_;
    PTP_TIMER timer_ = nullptr;
    std::atomic<bool> expired_{false};
};

class WinHttpClient final : public HttpClient {
public:
    std::string backend_name() const override { return "WinHTTP"; }

    Result<HttpResponse> send(const HttpRequest& request, CancellationToken cancel) override {
        std::string body;
        bool truncated = false;
        auto result = perform(request, cancel, false, [&](std::string_view chunk) {
            if (body.size() + chunk.size() > request.max_response_bytes) {
                const auto room =
                    request.max_response_bytes - std::min(body.size(), request.max_response_bytes);
                body.append(chunk.substr(0, room));
                truncated = true;
                return false;   // stop reading; we have all we are allowed to keep
            }
            body.append(chunk);
            return true;
        });
        if (!result) return result;
        result->body = std::move(body);
        result->truncated = truncated;
        return result;
    }

    Result<HttpResponse> stream(const HttpRequest& request, const HttpStreamCallback& on_chunk,
                                CancellationToken cancel) override {
        return perform(request, cancel, true, on_chunk);
    }

    Status download(const HttpRequest& request, const std::filesystem::path& destination,
                    const std::function<void(std::uint64_t, std::uint64_t)>& on_progress,
                    CancellationToken cancel) override {
        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "request cancelled");
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);

        // Write to a partial file and rename on success, so an interrupted
        // download never looks like a complete one.
        auto partial = destination;
        partial += ".part";
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::IoError, "cannot create " + path_to_utf8(partial));
        }

        std::uint64_t received = 0;
        std::uint64_t total = 0;
        bool write_failed = false;
        auto response = perform(
            request, cancel, false,
            [&](std::string_view chunk) {
                out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                if (!out) {
                    write_failed = true;
                    return false;
                }
                received += chunk.size();
                if (on_progress) on_progress(received, total);
                return out.good();
            },
            &total);

        out.close();
        if (write_failed || !out) {
            std::filesystem::remove(partial, ec);
            return make_error(ErrorCode::IoError, "cannot write " + path_to_utf8(partial));
        }
        if (!response) {
            std::filesystem::remove(partial, ec);
            return response.error();
        }
        if (!response->ok()) {
            std::filesystem::remove(partial, ec);
            return make_error(ErrorCode::NetworkError,
                              "download failed with HTTP " + std::to_string(response->status_code));
        }

        std::filesystem::rename(partial, destination, ec);
        if (ec) {
            std::filesystem::remove(partial, ec);
            return make_error(ErrorCode::IoError,
                              "cannot move download into place: " + path_to_utf8(destination));
        }
        return ok_status();
    }

private:
    std::mutex pool_mutex_;
    std::array<std::unique_ptr<SessionHandles>, 4> idle_;

    // A transport is exclusively borrowed. Credentials and other header state
    // must match before reuse; neither bodies nor URLs' query strings are kept.
    struct Lease {
        WinHttpClient& owner;
        std::unique_ptr<SessionHandles> handles;
        bool complete = false;

        ~Lease() {
            if (!complete || !handles->cacheable) return;
            std::lock_guard lock(owner.pool_mutex_);
            for (auto& idle : owner.idle_) {
                if (!idle) {
                    idle = std::move(handles);
                    break;
                }
            }
        }
    };

    std::unique_ptr<SessionHandles> acquire(const ParsedUrl& url, const HttpRequest& request) {
        {
            std::lock_guard lock(pool_mutex_);
            for (auto& idle : idle_) {
                if (idle && idle->origin.host == url.host && idle->origin.port == url.port &&
                    idle->origin.secure == url.secure && idle->headers == request.headers) {
                    return std::move(idle);
                }
            }
        }
        auto handles = std::make_unique<SessionHandles>();
        // Bound retained keys as well as handle count; oversized requests work
        // normally but do not leave their headers resident after completion.
        std::size_t bytes = url.host.size() * sizeof(wchar_t);
        for (const auto& [name, value] : request.headers) {
            if (name.size() > 64 * 1024 - std::min(bytes, std::size_t{64 * 1024})) return handles;
            bytes += name.size();
            if (value.size() > 64 * 1024 - std::min(bytes, std::size_t{64 * 1024})) return handles;
            bytes += value.size();
        }
        if (bytes <= 64 * 1024 && request.headers.size() <= 128) {
            handles->origin.host = url.host;
            handles->origin.port = url.port;
            handles->origin.secure = url.secure;
            handles->headers = request.headers;
            handles->cacheable = true;
        }
        return handles;
    }

    Result<HttpResponse> perform(const HttpRequest& request, CancellationToken cancel, bool streaming,
                                 const HttpStreamCallback& on_chunk,
                                 std::uint64_t* content_length_out = nullptr) {
        const auto started = std::chrono::steady_clock::now();

        auto parsed = crack_url(request.url);
        if (!parsed) {
            return make_error(ErrorCode::InvalidArgument, "malformed URL: " + request.url);
        }

        if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "request cancelled");
        Lease lease{*this, acquire(*parsed, request)};
        auto& handles = *lease.handles;
        if (!handles.session) {
            const std::wstring user_agent = L"LocalCode/" + widen(LOCALCODE_VERSION);
            handles.session = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!handles.session) {
                return make_error(ErrorCode::NetworkError, "WinHttpOpen failed",
                                  win32_message(GetLastError()));
            }
            // Keep the origin/header keys meaningful at the socket layer too:
            // WinHTTP otherwise permits legacy cross-session connection reuse.
            // Older implementations that lack this option keep fresh-session
            // behavior instead of retaining a session with uncertain isolation.
            BOOL isolated = TRUE;
            if (!WinHttpSetOption(handles.session, WINHTTP_OPTION_DISABLE_GLOBAL_POOLING,
                                  &isolated, sizeof isolated)) {
                handles.cacheable = false;
            }
        }

        const auto connect_ms = static_cast<int>(request.connect_timeout.count());
        const auto io_ms = static_cast<int>(request.timeout.count());
        if (!handles.connection) {
            handles.connection = WinHttpConnect(handles.session, parsed->host.c_str(), parsed->port, 0);
            if (!handles.connection) {
                return make_error(ErrorCode::NetworkError, "cannot connect to " + narrow(parsed->host),
                                  win32_message(GetLastError()));
            }
        }

        const wchar_t* verb = request.method == HttpMethod::Post   ? L"POST"
                              : request.method == HttpMethod::Head ? L"HEAD"
                                                                   : L"GET";
        DWORD flags = WINHTTP_FLAG_REFRESH;
        if (parsed->secure) flags |= WINHTTP_FLAG_SECURE;

        SharedHandle handle;
        handle.reset(WinHttpOpenRequest(handles.connection, verb, parsed->path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!handle.get()) {
            return make_error(ErrorCode::NetworkError, "WinHttpOpenRequest failed",
                              win32_message(GetLastError()));
        }

        // Timeouts belong to this request, including when its parent is reused.
        // Fresh sessions previously kept automatic cookies from reaching the
        // next call. Preserve that stateless behavior while permitting explicit
        // Cookie headers supplied by the caller.
        DWORD disabled = WINHTTP_DISABLE_COOKIES;
        if (!WinHttpSetTimeouts(handle.get(), connect_ms, connect_ms, io_ms, io_ms) ||
            !WinHttpSetOption(handle.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
                              sizeof disabled)) {
            return make_error(ErrorCode::NetworkError, "cannot configure HTTP request",
                              win32_message(GetLastError()));
        }

        DWORD redirect_policy = request.follow_redirects
                                    ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
                                    : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(handle.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                         sizeof redirect_policy);

        // Cancellation closes the request handle, which makes any in-flight
        // send or read return immediately instead of waiting for the timeout.
        std::atomic<bool> was_cancelled{false};
        CancellationRegistration registration{cancel, [&handle, &was_cancelled] {
                                                  was_cancelled.store(true);
                                                  handle.close();
                                              }};
        if (cancel.cancelled()) {
            return make_error(ErrorCode::Cancelled, "request cancelled before it started");
        }
        RequestDeadline deadline{handle, request.timeout};
        if (!deadline.valid()) {
            return make_error(ErrorCode::NetworkError, "cannot create HTTP deadline",
                              win32_message(GetLastError()));
        }
        deadline.arm(request.timeout - std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - started));

        std::wstring header_block;
        for (const auto& [name, value] : request.headers) {
            header_block += widen(name) + L": " + widen(value) + L"\r\n";
        }

        const BOOL sent = WinHttpSendRequest(
            handle.get(),
            header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
            header_block.empty() ? 0 : static_cast<DWORD>(header_block.size()),
            request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
            static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
        if (!sent) {
            const DWORD error = GetLastError();
            if (was_cancelled.load()) {
                return make_error(ErrorCode::Cancelled, "request cancelled");
            }
            if (deadline.expired() || error == ERROR_WINHTTP_TIMEOUT) {
                return make_error(ErrorCode::Timeout, "timed out sending to " + request.url);
            }
            return make_error(ErrorCode::NetworkError, "cannot send request to " + request.url,
                              win32_message(error));
        }

        if (!WinHttpReceiveResponse(handle.get(), nullptr)) {
            const DWORD error = GetLastError();
            if (was_cancelled.load()) {
                return make_error(ErrorCode::Cancelled, "request cancelled");
            }
            if (deadline.expired() || error == ERROR_WINHTTP_TIMEOUT) {
                return make_error(ErrorCode::Timeout, "timed out receiving from " + request.url);
            }
            return make_error(ErrorCode::NetworkError, "no response from " + request.url,
                              win32_message(error));
        }

        HttpResponse response;
        if (streaming) deadline.activity();
        response.final_url = request.url;

        DWORD status = 0;
        DWORD status_size = sizeof status;
        WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX);
        response.status_code = static_cast<int>(status);

        // Raw headers come back as one CRLF-separated block.
        DWORD header_size = 0;
        WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &header_size,
                            WINHTTP_NO_HEADER_INDEX);
        if (header_size > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring raw(header_size / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &header_size,
                                    WINHTTP_NO_HEADER_INDEX)) {
                for (const auto& line : strings::split_lines(narrow(raw))) {
                    const auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    response.headers[strings::to_lower(strings::trim(line.substr(0, colon)))] =
                        strings::trim(line.substr(colon + 1));
                }
            }
        }

        if (content_length_out) {
            const auto length_header = response.header("content-length");
            if (!length_header.empty()) {
                try {
                    *content_length_out = std::stoull(length_header);
                } catch (...) { /* absent or chunked */
                }
            }
        }

        std::string buffer;
        buffer.resize(32 * 1024);
        for (;;) {
            if (cancel.cancelled() || was_cancelled.load()) {
                return make_error(ErrorCode::Cancelled, "request cancelled");
            }
            if (deadline.expired()) {
                return make_error(ErrorCode::Timeout, "timed out reading from " + request.url);
            }
            DWORD read = 0;
            const HINTERNET current = handle.get();
            if (!current) {
                if (deadline.expired()) {
                    return make_error(ErrorCode::Timeout, "timed out reading from " + request.url);
                }
                return make_error(ErrorCode::Cancelled, "request cancelled");
            }
            // A synchronous fixed-size read can coalesce several small SSE
            // writes. Observe available bytes first for streams so activity
            // and visible deltas follow arrivals, even for short responses.
            DWORD available = static_cast<DWORD>(buffer.size());
            const bool queried = !streaming || WinHttpQueryDataAvailable(current, &available);
            const DWORD wanted = available == 0 ? static_cast<DWORD>(buffer.size())
                                                : std::min(available, static_cast<DWORD>(buffer.size()));
            if (!queried || !WinHttpReadData(current, buffer.data(), wanted, &read)) {
                const DWORD error = GetLastError();
                if (was_cancelled.load()) {
                    return make_error(ErrorCode::Cancelled, "request cancelled");
                }
                if (deadline.expired() || error == ERROR_WINHTTP_TIMEOUT) {
                    return make_error(ErrorCode::Timeout, "timed out reading from " + request.url);
                }
                if (error == ERROR_WINHTTP_OPERATION_CANCELLED || error == ERROR_INVALID_HANDLE) {
                    return make_error(ErrorCode::Cancelled, "request cancelled");
                }
                return make_error(ErrorCode::NetworkError, "read failed from " + request.url,
                                  win32_message(error));
            }
            if (deadline.expired()) {
                return make_error(ErrorCode::Timeout, "timed out reading from " + request.url);
            }
            if (read == 0) {
                lease.complete = true;
                break;   // only a fully consumed response returns to the pool
            }

            if (streaming) deadline.activity();

            if (on_chunk && !on_chunk(std::string_view{buffer.data(), read})) break;
        }

        response.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
        return response;
    }
};

}   // namespace

std::shared_ptr<HttpClient> HttpClient::create() {
    return std::make_shared<WinHttpClient>();
}

}   // namespace localcode
