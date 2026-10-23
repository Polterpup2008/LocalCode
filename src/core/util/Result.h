#pragma once
// ---------------------------------------------------------------------------
// A small, allocation-light result type used across the core library.
//
// LocalCode never throws across subsystem boundaries: tools, process execution,
// HTTP and storage all report failure as data so the agent loop can feed the
// error back to the model instead of unwinding the stack.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace localcode {

enum class ErrorCode {
    Unknown = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PermissionDenied,   // blocked by the security policy
    Timeout,
    Cancelled,
    IoError,
    ParseError,
    NetworkError,
    BackendError,
    NotSupported,
    Internal,
    InvalidToolCall,
};

std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode   code = ErrorCode::Unknown;
    std::string message;
    std::string detail;   // optional: raw stderr, HTTP body, sqlite message...

    Error() = default;
    Error(ErrorCode c, std::string msg, std::string det = {})
        : code(c), message(std::move(msg)), detail(std::move(det)) {}

    [[nodiscard]] std::string to_string() const;
};

inline Error make_error(ErrorCode c, std::string msg, std::string det = {}) {
    return Error{c, std::move(msg), std::move(det)};
}

// --- Result<T> -------------------------------------------------------------
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T&        value()       { return std::get<0>(storage_); }
    const T&  value() const { return std::get<0>(storage_); }
    T*        operator->()       { return &std::get<0>(storage_); }
    const T*  operator->() const { return &std::get<0>(storage_); }
    T&        operator*()        { return std::get<0>(storage_); }
    const T&  operator*() const  { return std::get<0>(storage_); }

    const Error& error() const { return std::get<1>(storage_); }

    T value_or(T fallback) const { return ok() ? std::get<0>(storage_) : std::move(fallback); }

private:
    std::variant<T, Error> storage_;
};

// --- Result<void> ----------------------------------------------------------
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), has_error_(true) {}  // NOLINT

    [[nodiscard]] bool ok() const noexcept { return !has_error_; }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const { return error_; }

private:
    Error error_;
    bool  has_error_ = false;
};

using Status = Result<void>;

inline Status ok_status() { return Status{}; }

}  // namespace localcode
