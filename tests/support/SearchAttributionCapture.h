#pragma once
#include <string_view>

// Recorded multi-file search text and the misleading old head/tail projection.
// These are inert regression fixtures, never model input or production policy.
namespace localcode::test {
inline constexpr std::string_view kGroupedSearchCapture =
    R"search(Found 12 match(es) in 5 file(s) for 'on_started' in files matching '*.cpp'.
Searched 170 text file(s).
Order: literal path affinity, then ordinary source/build, auxiliary/config, prose and artifacts; stable ties.

src/core/agent/Agent.cpp
     85	        case AgentEventType::GenerationStats: return "generation_stats";
     86	        case AgentEventType::ProgressWarning: return "progress_warning";
>    87	        case AgentEventType::FinalizationStarted: return "finalization_started";
     88	        case AgentEventType::TaskStateUpdated: return "task_state_updated";
     89	        case AgentEventType::RunFinished: return "run_finished";
  --
    426	                                               .count()}});
    427	    }
>   428	    const auto verification_started = std::chrono::steady_clock::now();
)search"
    "    429\t\n"
    R"search(    430	    // Fold what happened into the task state, which is what the next request
  --
    466	            {{"name", call.name},
    467	             {"elapsed_ms", std::chrono::duration<double, std::milli>(
>   468	                                std::chrono::steady_clock::now() - verification_started)
    469	                                .count()}});
    470	    }
  --

src/core/inference/ServerManager.cpp
    824	        if (restarts == 0)
    825	            set_state(ServerState::Starting, "launching " + config.binary.filename().string());
>   826	        request.on_started = [this](std::uint32_t pid) {
    827	            {
    828	                std::lock_guard lock(mutex_);
  --

src/core/platform/Process.cpp
    547	                                                  TerminateProcess(proc, 1);
    548	                                          }};
>   549	    if (request.on_started) {
    550	        try {
    551	            request.on_started(pi.dwProcessId);
  --
    549	    if (request.on_started) {
    550	        try {
>   551	            request.on_started(pi.dwProcessId);
    552	        } catch (...) {
    553	            log_error(LogChannel::App, "process start observer threw");
  --
    780	                                              ::kill(-pid, SIGKILL);
    781	                                          }};
>   782	    if (request.on_started) {
    783	        try {
    784	            request.on_started(static_cast<std::uint32_t>(pid));
  --
    782	    if (request.on_started) {
    783	        try {
>   784	            request.on_started(static_cast<std::uint32_t>(pid));
    785	        } catch (...) {
    786	            log_error(LogChannel::App, "process start observer threw");
  --

src/gui/dialogs/InferenceBenchmarkDialog.cpp
    675	        auto pid = std::make_shared<std::atomic<std::uint32_t>>(0);
    676	        ownedProcessId_ = pid;
>   677	        request.on_started = [pid](std::uint32_t value) { pid->store(value); };
    678	        ownedCancellation_ = CancellationSource{};
    679	        auto* watcher = new QFutureWatcher<Result<ProcessResult>>(this);
  --

tests/test_cpu_allowance.cpp
     98	    SECTION("unrepresentable CPU") { request.cpu_allowance = {std::numeric_limits<std::uint32_t>::max()}; }
     99	    bool started = false;
>   100	    request.on_started = [&](std::uint32_t) { started = true; };
    101	    const auto result = ProcessRunner::run(request);
    102	    REQUIRE_FALSE(result);
  --
    181	    request.cpu_allowance = {static_cast<std::uint32_t>(std::stoul(cpu))};
    182	    request.timeout = std::chrono::seconds(60);
>   183	    request.on_started = [&](std::uint32_t pid) {
    184	        std::ofstream(std::filesystem::path(marker)) << pid << '\n';
    185	    };
  --
    204	    request.working_directory = request.executable.parent_path();
    205	    request.timeout = std::chrono::seconds(120);
>   206	    request.on_started = [&](std::uint32_t pid) {
    207	        std::ofstream(path_from_utf8(config.at("pid_file").get<std::string>())) << pid;
    208	    };
  --
)search";
inline constexpr std::string_view kMisattributedSearchProjection =
    R"search(Found 12 match(es) in 5 file(s) for 'on_started' in files matching '*.cpp'.
Searched 170 text file(s).
Order: literal path affinity, then ordinary source/build, auxiliary/config, prose and artifacts; stable ties.

src/core/agent/Agent.cpp
     85	        case AgentEventType::GenerationStats: return "generation_stats";
... [75 lines removed to save context; re-run the tool if you need them] ...
>   206	    request.on_started = [&](std::uint32_t pid) {
    207	        std::ofstream(path_from_utf8(config.at("pid_file").get<std::string>())) << pid;
    208	    };
  --)search";
}   // namespace localcode::test
