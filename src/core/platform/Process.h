#pragma once
// ---------------------------------------------------------------------------
// Child process execution.
//
// Commands are represented structurally - executable, argument vector, working
// directory, environment - and never as a shell string. Nothing in LocalCode
// concatenates a command line and hands it to system(); that is both a quoting
// hazard and an injection hazard when arguments come from a model.
//
// On Windows creation atomically installs a Job Object with KILL_ON_JOB_CLOSE
// and restricts inherited handles to the child's standard streams. The whole
// tree is killed on timeout, cancellation, owner exit or completion of its root.
// Background descendants cannot outlive a command or keep its output pipes open.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../util/Cancellation.h"
#include "../util/Result.h"

namespace localcode {

struct EnvVar {
    std::string name;
    std::string value;
};

struct ProcessRequest {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;

    // Applied on top of the inherited environment when `inherit_environment`
    // is true, otherwise used as the complete environment.
    std::vector<EnvVar> environment;
    bool inherit_environment = true;

    std::chrono::milliseconds timeout{120000};

    // Captured output is capped per stream; the full stream is still written to
    // `full_log_path` when set, so nothing is lost, only kept out of context.
    std::size_t max_captured_bytes = 256u * 1024;
    std::filesystem::path full_log_path;

    // Optional live feed for the GUI's command output pane. Called from the
    // reader threads, so implementations must be thread-safe.
    std::function<void(std::string_view chunk, bool is_stderr)> on_output;

    // Called on the execution thread after creating the owned process and
    // installing cancellation. The identifier is valid only while run() owns it.
    std::function<void(std::uint32_t process_id)> on_started;

    // Text written to the child's stdin before it is closed.
    std::string stdin_data;

    // Optional machine-local initial CPU allowance, inherited by descendants.
    // Windows also retains a Job Object limit; Linux sets affinity before exec.
    // Empty preserves system scheduling. Resolve topology at
    // launch; never save these indices as a portable user setting.
    // Unsupported or unavailable CPUs fail instead of silently widening it.
    std::vector<std::uint32_t> cpu_allowance;
};

struct ProcessResult {
    std::string display_command;   // human-readable, for logs and the UI
    std::filesystem::path working_directory;
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    bool started = false;

    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    std::uint64_t stdout_total_bytes = 0;
    std::uint64_t stderr_total_bytes = 0;

    std::int64_t duration_ms = 0;
    std::filesystem::path full_log_path;

    [[nodiscard]] bool success() const {
        return started && !timed_out && !cancelled && exit_code == 0;
    }
};

class ProcessRunner {
public:
    // Runs to completion. Never throws; failures to *start* the process are
    // returned as an Error, while a process that ran and failed is a successful
    // Result with a non-zero exit code.
    static Result<ProcessResult> run(const ProcessRequest& request, CancellationToken cancel = {});

    // Renders the request the way a user would type it, with quoting applied.
    static std::string display_command(const ProcessRequest& request);
};

// --- Executable resolution -------------------------------------------------
// Searches PATH (honouring PATHEXT on Windows). An input that already contains
// a separator is resolved relative to `working_directory` instead.
std::optional<std::filesystem::path> find_executable(
    std::string_view name, const std::filesystem::path& working_directory = {});

// All matches, in PATH order - useful for reporting "which python is picked".
std::vector<std::filesystem::path> find_all_executables(std::string_view name);

// Quotes one argument per the CommandLineToArgvW rules (Windows) or POSIX
// shell rules. Exposed for tests and for building display strings.
std::string quote_argument(std::string_view argument);

// The system cmd.exe /d /s /c wrapper parses its final argument as shell text,
// not with CommandLineToArgvW. Used by launch rendering and command policy.
bool is_windows_cmd_script(const ProcessRequest& request);

}   // namespace localcode
