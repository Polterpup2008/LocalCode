#include "Result.h"

namespace localcode {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Unknown:          return "unknown";
        case ErrorCode::InvalidArgument:  return "invalid_argument";
        case ErrorCode::NotFound:         return "not_found";
        case ErrorCode::AlreadyExists:    return "already_exists";
        case ErrorCode::PermissionDenied: return "permission_denied";
        case ErrorCode::Timeout:          return "timeout";
        case ErrorCode::Cancelled:        return "cancelled";
        case ErrorCode::IoError:          return "io_error";
        case ErrorCode::ParseError:       return "parse_error";
        case ErrorCode::NetworkError:     return "network_error";
        case ErrorCode::BackendError:     return "backend_error";
        case ErrorCode::NotSupported:     return "not_supported";
        case ErrorCode::Internal:         return "internal";
    }
    return "unknown";
}

std::string Error::to_string() const {
    std::string out{localcode::to_string(code)};
    out += ": ";
    out += message;
    if (!detail.empty()) {
        out += " (";
        out += detail;
        out += ")";
    }
    return out;
}

}  // namespace localcode
