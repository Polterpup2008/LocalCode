#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "inference/ServerManager.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

using namespace localcode;
using namespace std::chrono_literals;

namespace {
bool alive(std::uint32_t pid) {
    if (pid == 0) return false;
#if defined(_WIN32)
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD exit = 0;
    const bool running = GetExitCodeProcess(process, &exit) && exit == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

bool kill_owned(std::uint32_t pid) {
    if (pid == 0) return false;
#if defined(_WIN32)
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process) return false;
    const bool killed = TerminateProcess(process, 73) != FALSE;
    CloseHandle(process);
    return killed;
#else
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

struct Lifecycle {
    // Observer state outlives the server's destructor/last notification.
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<ServerStatus> events;
    ModelServer server;

    Lifecycle() {
        server.set_state_callback([this](const ServerStatus& status) {
            {
                std::lock_guard lock(mutex);
                events.push_back(status);
            }
            changed.notify_all();
        });
    }

    bool wait(ServerState state, std::size_t count = 0) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] {
            const auto status = server.status();
            return status.state == state && status.restart_count == count;
        });
    }

    ModelServer::HealthProbe probe() {
        return [this](const std::string&, CancellationToken cancel) -> Status {
            if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "probe cancelled");
            return alive(server.status().process_id)
                       ? ok_status()
                       : Status{make_error(ErrorCode::NetworkError, "fixture not alive")};
        };
    }
};

ServerConfig fixture() {
    ServerConfig config;
    config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
    config.profile.id = "recovery-fixture";
    config.profile.display_name = "Lifecycle fixture";
    config.startup_timeout = 2s;
    return config;
}
}   // namespace

TEST_CASE("owned server recovers once without changing its endpoint", "[server][recovery]") {
    Lifecycle life;
    REQUIRE(life.server.start(fixture(), life.probe()));
    const auto first = life.server.status();
    REQUIRE(first.process_id != 0);
    REQUIRE(kill_owned(first.process_id));
    REQUIRE(life.wait(ServerState::Ready, 1));
    const auto recovered = life.server.status();
    CHECK(recovered.base_url == first.base_url);
    CHECK(recovered.model_id == first.model_id);
    CHECK(recovered.process_id != first.process_id);
    CHECK_FALSE(alive(first.process_id));
    CHECK(alive(recovered.process_id));
    {
        std::lock_guard lock(life.mutex);
        CHECK(std::count_if(life.events.begin(), life.events.end(), [](const auto& event) {
                  return event.state == ServerState::Restarting;
              }) == 1);
    }
    REQUIRE(kill_owned(recovered.process_id));
    REQUIRE(life.wait(ServerState::Failed, 1));
    CHECK(life.server.status().message.find("budget is exhausted") != std::string::npos);
    CHECK(life.server.status().process_id == 0);
    life.server.stop();
    CHECK_FALSE(alive(recovered.process_id));
    CHECK(life.server.status().state == ServerState::Stopped);
    REQUIRE(life.server.start(fixture(), life.probe()));
    CHECK(life.server.status().restart_count == 0);
    const auto manual_pid = life.server.status().process_id;
    life.server.stop();
    CHECK_FALSE(alive(manual_pid));
}

TEST_CASE("server startup failures are not retried automatically", "[server][recovery]") {
    Lifecycle life;
    auto config = fixture();
    config.extra_arguments = {"--fixture-exit"};
    auto started = life.server.start(config, [](const std::string&, CancellationToken) -> Status {
        return make_error(ErrorCode::NetworkError, "not ready");
    });
    CHECK_FALSE(started);
    CHECK(life.server.status().restart_count == 0);
    CHECK(life.server.status().process_id == 0);
    std::lock_guard lock(life.mutex);
    CHECK(std::none_of(life.events.begin(), life.events.end(),
                       [](const auto& event) { return event.state == ServerState::Restarting; }));
}

TEST_CASE("configured tuning is refused before launch when runtime flags are absent",
          "[server][tuning]") {
    for (int option = 0; option < 4; ++option) {
        Lifecycle life;
        auto config = fixture();
        const char* expected = nullptr;
        switch (option) {
            case 0:
                config.profile.threads = 8;
                expected = "--threads";
                break;
            case 1:
                config.profile.batch_size = 128;
                expected = "--batch-size";
                break;
            case 2:
                config.profile.cache_type_k = "q8_0";
                expected = "--cache-type-k";
                break;
            default:
                config.profile.cache_type_v = "q8_0";
                expected = "--cache-type-v";
                break;
        }
        const auto started = life.server.start(config, life.probe());
        REQUIRE_FALSE(started);
        CHECK(started.error().to_string().find(expected) != std::string::npos);
        CHECK(life.server.status().process_id == 0);
        CHECK(std::none_of(life.events.begin(), life.events.end(),
                           [](const auto& event) { return event.state == ServerState::Ready; }));
    }
}

TEST_CASE("Stop during automatic recovery joins the replacement", "[server][recovery]") {
    std::atomic<std::uint32_t> replacement{0};
    Lifecycle life;
    auto config = fixture();
    config.startup_timeout = 30s;
    auto initial_probe = life.probe();
    REQUIRE(life.server.start(
        config, [&, initial_probe](const std::string& url, CancellationToken cancel) -> Status {
            const auto current = life.server.status();
            if (current.restart_count == 0) return initial_probe(url, cancel);
            replacement.store(current.process_id);
            life.changed.notify_all();
            return make_error(ErrorCode::NetworkError, "recovery still loading");
        }));
    REQUIRE(kill_owned(life.server.status().process_id));
    REQUIRE(life.wait(ServerState::Restarting, 1));
    {
        std::unique_lock lock(life.mutex);
        REQUIRE(life.changed.wait_for(lock, 5s, [&] { return replacement.load() != 0; }));
    }
    life.server.stop();
    CHECK(life.server.status().state == ServerState::Stopped);
    CHECK(life.server.status().process_id == 0);
    CHECK_FALSE(alive(replacement.load()));
}

TEST_CASE("recovery timeout fails clearly instead of restarting in a loop", "[server][recovery]") {
    Lifecycle life;
    auto config = fixture();
    config.startup_timeout = 1s;
    auto initial_probe = life.probe();
    REQUIRE(life.server.start(
        config, [&, initial_probe](const std::string& url, CancellationToken cancel) -> Status {
            if (life.server.status().restart_count == 0) return initial_probe(url, cancel);
            return make_error(ErrorCode::NetworkError, "recovery health refused");
        }));
    REQUIRE(kill_owned(life.server.status().process_id));
    REQUIRE(life.wait(ServerState::Failed, 1));
    CHECK(life.server.status().message.find("within 1s") != std::string::npos);
    CHECK(life.server.status().message.find("recovery health refused") != std::string::npos);
    CHECK(life.server.status().process_id == 0);
}

TEST_CASE("disabled recovery leaves an unexpected exit failed", "[server][recovery]") {
    Lifecycle life;
    auto config = fixture();
    config.auto_restart = false;
    REQUIRE(life.server.start(config, life.probe()));
    REQUIRE(kill_owned(life.server.status().process_id));
    REQUIRE(life.wait(ServerState::Failed));
    CHECK(life.server.status().restart_count == 0);
}

TEST_CASE("cancelling startup interrupts readiness and reaps the owned process",
          "[server][recovery]") {
    CancellationSource cancel;
    std::atomic<std::uint32_t> launched{0};
    Lifecycle life;
    auto config = fixture();
    config.startup_timeout = 30s;
    const auto start = std::chrono::steady_clock::now();
    const auto result = life.server.start(
        config,
        [&](const std::string&, CancellationToken) -> Status {
            launched.store(life.server.status().process_id);
            cancel.cancel();
            return make_error(ErrorCode::Cancelled, "test user cancelled startup");
        },
        cancel.token());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK(launched.load() != 0);
    CHECK_FALSE(alive(launched.load()));
    CHECK(life.server.status().state == ServerState::Stopped);
    CHECK(life.server.status().process_id == 0);
}

TEST_CASE("unterminated server output obeys the log ring limit", "[server][log-bounds]") {
    Lifecycle life;
    auto config = fixture();
    config.extra_arguments = {"--fixture-long-log"};
    REQUIRE(life.server.start(config, [&](const std::string&, CancellationToken) -> Status {
        const auto lines = life.server.recent_log(4000);
        const bool received_long_line = std::any_of(
            lines.begin(), lines.end(), [](const auto& line) { return line.size() > 64 * 1024; });
        return received_long_line
                   ? ok_status()
                   : Status{make_error(ErrorCode::NetworkError, "waiting for log fixture")};
    }));
    CHECK(life.server.recent_log(4000).size() <= 3000);
    life.server.stop();
}
