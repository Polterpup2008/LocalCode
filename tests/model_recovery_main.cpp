// Explicit real managed-server crash acceptance. Never registered as a CI test.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "agent/Agent.h"
#include "inference/BackendManager.h"
#include "model/GgufMetadata.h"
#include "platform/Paths.h"
#include "util/Log.h"
#include "util/Strings.h"
#include "workspace/Workspace.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#endif

namespace {
using namespace localcode;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class OwnedCrash {
public:
    OwnedCrash(const ServerStatus& current, const std::filesystem::path& binary) {
        if (current.state != ServerState::Ready || current.process_id == 0)
            throw std::runtime_error("no Ready owned process to capture");
#if defined(_WIN32)
        process_ = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                               FALSE, current.process_id);
        if (!process_) throw std::runtime_error("cannot capture owned process handle");
        wchar_t path[32768];
        DWORD length = 32768;
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool identified = QueryFullProcessImageNameW(process_, 0, path, &length) &&
                                GetProcessTimes(process_, &created, &exited, &kernel, &user);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = created.dwLowDateTime;
        ticks.HighPart = created.dwHighDateTime;
        const auto created_ms =
            static_cast<std::int64_t>(ticks.QuadPart / 10000ULL) - 11644473600000LL;
        std::error_code ec;
        if (!identified || !std::filesystem::equivalent(std::filesystem::path(path), binary, ec) ||
            created_ms < current.started_unix_ms || created_ms > current.ready_unix_ms) {
            CloseHandle(process_);
            process_ = nullptr;
            throw std::runtime_error("owned process image/generation did not match; kill refused");
        }
#else
        pid_ = static_cast<pid_t>(current.process_id);
#endif
    }

    ~OwnedCrash() {
#if defined(_WIN32)
        if (process_) CloseHandle(process_);
#endif
    }

    OwnedCrash(const OwnedCrash&) = delete;
    OwnedCrash& operator=(const OwnedCrash&) = delete;

    bool kill() const {
#if defined(_WIN32)
        return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT &&
               TerminateProcess(process_, 74) != FALSE;
#else
        return ::kill(pid_, SIGKILL) == 0;
#endif
    }

private:
#if defined(_WIN32)
    HANDLE process_ = nullptr;
#else
    pid_t pid_ = 0;
#endif
};

struct CrashProbe {
    std::mutex mutex;
    std::condition_variable changed;
    Json events = Json::array();
    BackendManager manager;   // last: joins its observers before their state disappears

    CrashProbe() {
        manager.set_http_client(HttpClient::create());
        manager.set_state_callback([this](const ServerStatus& status) {
            {
                std::lock_guard lock(mutex);
                events.push_back({{"state", to_string(status.state)},
                                  {"at_unix_ms", now_unix_ms()},
                                  {"pid", status.process_id},
                                  {"restart_count", status.restart_count},
                                  {"message", strings::utf8_truncate(status.message, 1000)}});
            }
            changed.notify_all();
        });
    }

    bool wait(ServerState state, std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 180s, [&] {
            const auto current = manager.server_status();
            return current.state == state && current.restart_count == count;
        });
    }
};

Json chat(const BackendPtr& backend, const ModelProfile& model,
          std::function<void()> on_first = {}) {
    ChatRequest request;
    request.messages = {ChatMessage::user("What is 17 multiplied by 23? Give only the number.")};
    request.sampling = model.sampling;
    request.sampling.max_tokens = 1024;
    request.enable_reasoning = model.reasoning_enabled;
    StreamHandlers handlers;
    handlers.on_first_token = std::move(on_first);
    CancellationSource cancel;
    std::mutex deadline_mutex;
    std::condition_variable_any deadline_condition;
    std::jthread deadline([&](std::stop_token stop) {
        std::unique_lock lock(deadline_mutex);
        deadline_condition.wait_for(lock, stop, 120s, [] { return false; });
        if (!stop.stop_requested()) cancel.cancel();
    });
    const auto start = Clock::now();
    auto result = backend->chat(request, handlers, cancel.token());
    if (!result)
        return {
            {"wall_ms", elapsed(start)}, {"error", result.error().to_string()}, {"correct", false}};
    return {{"wall_ms", elapsed(start)},
            {"answer", strings::trim(result->message.content)},
            {"correct", strings::trim(result->message.content) == "391"}};
}
}   // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: localcode_model_recovery output.json qwen.gguf server.exe\n";
        return 2;
    }
    Json report{{"schema", 2},
                {"scenarios", Json::array()},
                {"notes",
                 {"Kills only the PID reported as owned by production BackendManager.",
                  "Uses real Qwen HTTP, Agent and file tool; no task/tool replay.",
                  "Other GPU applications may contend; timings are observations."}}};
    std::filesystem::path output;
    auto save = [&] {
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file << report.dump(2) << '\n';
        if (!file) throw std::runtime_error("cannot save crash evidence");
    };
    try {
        output = std::filesystem::absolute(argv[1]);
        app_paths().set_root(output.parent_path() / "model-recovery-home");
        if (!app_paths().ensure_directories()) throw std::runtime_error("cannot create probe home");
        auto metadata = read_gguf_metadata(argv[2]);
        if (!metadata) throw std::runtime_error(metadata.error().to_string());
        auto model = ModelProfile::from_gguf(*metadata);
        model.context_size = 16384;
        model.backend = BackendPreference::LlamaCpp;
        model.server_binary = argv[3];
        model.sampling.max_tokens = 2048;
        const auto workspace_path = output.parent_path() / "recovery-tool-workspace";
        std::filesystem::create_directories(workspace_path);
        {
            std::ofstream file(workspace_path / "recovery-proof.txt");
            file << "The acceptance code is 824619. Read this file before answering.\n";
        }
        for (const std::string scenario : {"idle", "generation", "after_tool"}) {
            std::cout << "Crash scenario: " << scenario << std::endl;
            CrashProbe probe;
            auto ready = probe.manager.activate(model);
            if (!ready) throw std::runtime_error(ready.error().to_string());
            const auto initial = probe.manager.server_status();
            OwnedCrash initial_crash{initial, model.server_binary};
            const auto crash_start = Clock::now();
            Clock::time_point killed_at{};
            bool killed = false;
            auto kill = [&] {
                if (initial_crash.kill()) {
                    killed_at = Clock::now();
                    killed = true;
                }
            };
            Json row{{"scenario", scenario},
                     {"initial_pid", initial.process_id},
                     {"initial_endpoint", initial.base_url}};
            if (scenario == "idle") kill();
            if (scenario == "generation")
                row["interrupted_request"] = chat(*ready, model, [&] {
                    if (!killed) kill();
                });
            if (scenario == "after_tool") {
                Workspace workspace;
                if (!workspace.open(workspace_path)) throw std::runtime_error("workspace failed");
                ToolRegistry tools;
                tools.register_builtin_tools();
                for (const auto& name : tools.names())
                    if (name != "read_file_range") tools.remove(name);
                tools.set_read_only(true);
                Agent agent;
                agent.set_backend(*ready);
                agent.set_profile(model);
                agent.set_workspace(&workspace);
                agent.set_tools(&tools);
                AgentConfig config;
                config.max_iterations = 4;
                config.wall_clock_budget = 180s;
                agent.set_config(config);
                Json completed_tools = Json::array();
                agent.set_event_sink([&](const AgentEvent& event) {
                    if (event.type != AgentEventType::ToolCompleted) return;
                    completed_tools.push_back({{"name", event.tool_call.name},
                                               {"ok", event.tool_result.ok},
                                               {"call_id", event.tool_call.id}});
                    if (event.tool_result.ok && !killed) kill();
                });
                const auto summary = agent.run(
                    "Use read_file_range to read recovery-proof.txt. Report its acceptance code. "
                    "The code must come from the file, not a guess. Do not modify anything.",
                    {});
                row["interrupted_task"] = {{"completed", summary.completed},
                                           {"stop_reason", summary.stop_reason},
                                           {"tool_calls", summary.tool_calls},
                                           {"final_message", summary.final_message},
                                           {"tools", completed_tools}};
            }
            row["killed_owned_pid"] = killed;
            const bool recovered = killed && probe.wait(ServerState::Ready, 1);
            row["recovered"] = recovered;
            row["scenario_to_recovered_ms"] = elapsed(crash_start);
            row["crash_to_recovered_ms"] = killed ? Json(elapsed(killed_at)) : Json{};
            const auto after = probe.manager.server_status();
            row["recovered_pid"] = after.process_id;
            row["same_endpoint"] = after.base_url == initial.base_url;
            row["same_model"] = after.model_id == initial.model_id;
            if (recovered) row["subsequent_request"] = chat(*ready, model);
            // Verify the budget does not silently reset after a successful recovery.
            if (scenario == "idle" && recovered) {
                OwnedCrash second_crash{after, model.server_binary};
                row["second_kill"] = second_crash.kill();
                row["bounded_failed"] = probe.wait(ServerState::Failed, 1);
            }
            probe.manager.deactivate();
            row["stopped"] = probe.manager.server_status().state == ServerState::Stopped;
            {
                std::lock_guard lock(probe.mutex);
                row["events"] = probe.events;
            }
            report["scenarios"].push_back(row);
            save();
            if (!recovered || !row["subsequent_request"].value("correct", false) ||
                !row["stopped"].get<bool>() ||
                (scenario == "idle" && !row.value("bounded_failed", false)))
                throw std::runtime_error(
                    "real crash acceptance failed; partial evidence preserved");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
