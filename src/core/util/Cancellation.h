#pragma once
// ---------------------------------------------------------------------------
// Cooperative cancellation.
//
// One token travels from the GUI's Stop button through the agent loop into the
// backend HTTP stream, the running child process and any web request. Every
// long-running operation polls it; process execution additionally registers a
// callback so the child process tree is terminated immediately rather than at
// the next poll.
// ---------------------------------------------------------------------------
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace localcode {

class CancellationSource;

class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] bool cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    // Registers a callback fired on cancellation. If the token is already
    // cancelled the callback runs immediately. Returns a registration id.
    int on_cancel(std::function<void()> callback) {
        if (!state_) return 0;
        std::function<void()> run_now;
        int id = 0;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->cancelled.load(std::memory_order_acquire)) {
                run_now = std::move(callback);
            } else {
                id = state_->next_id++;
                state_->callbacks.emplace(id, std::move(callback));
            }
        }
        invoke_callback(run_now);
        return id;
    }

    void remove_callback(int id) {
        if (!state_ || id == 0) return;
        std::lock_guard lock(state_->mutex);
        state_->callbacks.erase(id);
    }

private:
    friend class CancellationSource;

    // Cancellation must continue to notify child-process and HTTP cleanup even
    // when another subscriber fails. Logging is best effort during shutdown.
    static void invoke_callback(const std::function<void()>& callback) noexcept;

    struct State {
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        std::map<int, std::function<void()>> callbacks;
        int next_id = 1;
    };

    explicit CancellationToken(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<CancellationToken::State>()) {}

    [[nodiscard]] CancellationToken token() const { return CancellationToken{state_}; }

    [[nodiscard]] bool cancelled() const noexcept {
        return state_->cancelled.load(std::memory_order_acquire);
    }

    void cancel() {
        std::map<int, std::function<void()>> to_run;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) return;
            to_run.swap(state_->callbacks);
        }
        for (auto& [id, callback] : to_run) {
            CancellationToken::invoke_callback(callback);
        }
    }

    // Drops the current state so the source can be reused for the next run.
    void reset() { state_ = std::make_shared<CancellationToken::State>(); }

private:
    std::shared_ptr<CancellationToken::State> state_;
};

// RAII helper that unregisters the callback when it leaves scope.
class CancellationRegistration {
public:
    CancellationRegistration(CancellationToken token, std::function<void()> callback)
        : callback_(std::make_shared<Callback>(std::move(callback))),
          token_(std::move(token)),
          id_(token_.on_cancel([state = callback_] {
              std::lock_guard lock(state->mutex);
              if (state->active) state->invoke();
          })) {}

    ~CancellationRegistration() {
        token_.remove_callback(id_);
        // cancel() may already have moved the callback out of the token's map.
        // Disable that pending invocation, or wait for an invocation in flight,
        // before the stack state captured by the subscriber can disappear.
        std::lock_guard lock(callback_->mutex);
        callback_->active = false;
    }

    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;

private:
    struct Callback {
        explicit Callback(std::function<void()> function) : invoke(std::move(function)) {}

        std::recursive_mutex mutex;
        std::function<void()> invoke;
        bool active = true;
    };

    std::shared_ptr<Callback> callback_;
    CancellationToken token_;
    int id_;
};

}   // namespace localcode
