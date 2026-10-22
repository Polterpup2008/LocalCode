#include "Cancellation.h"

#include <exception>

#include "Log.h"

namespace localcode {

void CancellationToken::invoke_callback(const std::function<void()>& callback) noexcept {
    if (!callback) return;
    try {
        callback();
    } catch (const std::exception& error) {
        try {
            log_error(LogChannel::App, "cancellation callback failed", {{"error", error.what()}});
        } catch (...) {   // NOLINT(bugprone-empty-catch): logging failure must not abort remaining
                          // cleanup.
        }
    } catch (...) {
        try {
            log_error(LogChannel::App, "cancellation callback failed with an unknown exception");
        } catch (...) {   // NOLINT(bugprone-empty-catch): logging failure must not abort remaining
                          // cleanup.
        }
    }
}

}   // namespace localcode
