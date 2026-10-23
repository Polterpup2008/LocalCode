#include "Process.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

#include "../util/Log.h"
#include "../util/Strings.h"
#include "PathEncoding.h"
#include "Paths.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif
extern char** environ;
#endif

namespace localcode {
namespace {

void observe_output(const std::function<void(std::string_view, bool)>& callback,
                    std::string_view chunk, bool is_stderr, bool& enabled) noexcept {
    if (!callback || !enabled) return;
    try {
        callback(chunk, is_stderr);
    } catch (...) {
        enabled = false;
        // A presentation observer must not interrupt pipe draining or child cleanup.
        try {
            log_error(LogChannel::App, "process output observer threw; disabled for this stream");
        } catch (
            ...) {   // NOLINT(bugprone-empty-catch): logging cannot escape the worker boundary.
        }
    }
}

std::string env_value(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
#endif
}

// Collects output with a byte cap while still counting everything that arrived
// and optionally spooling the complete stream to a file.
class OutputCollector {
public:
    OutputCollector(std::size_t cap, std::ofstream* spool, std::mutex* spool_mutex)
        : cap_(cap), spool_(spool), spool_mutex_(spool_mutex) {}

    void append(std::string_view chunk, const char* label) {
        std::lock_guard lock(mutex_);
        total_ += chunk.size();
        if (buffer_.size() < cap_) {
            const std::size_t room = cap_ - buffer_.size();
            buffer_.append(chunk.substr(0, room));
            if (chunk.size() > room) truncated_ = true;
        } else {
            truncated_ = true;
        }
        if (spool_) {
            std::lock_guard spool_lock(*spool_mutex_);
            spool_->write(label, static_cast<std::streamsize>(std::strlen(label)));
            spool_->write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        }
    }

    std::string take() {
        std::lock_guard lock(mutex_);
        return std::move(buffer_);
    }

    std::uint64_t total() const { return total_; }

    bool truncated() const { return truncated_; }

private:
    std::mutex mutex_;
    std::string buffer_;
    std::size_t cap_;
    std::uint64_t total_ = 0;
    bool truncated_ = false;
    std::ofstream* spool_;
    std::mutex* spool_mutex_;
};

}   // namespace

std::string quote_argument(std::string_view argument) {
#if defined(_WIN32)
    // CommandLineToArgvW rules: backslashes are literal unless they precede a
    // quote, in which case they must be doubled.
    const bool needs_quotes =
        argument.empty() || argument.find_first_of(" \t\n\v\"") != std::string_view::npos;
    if (!needs_quotes) return std::string{argument};

    std::string out;
    out.push_back('"');
    for (std::size_t i = 0; i < argument.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < argument.size() && argument[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == argument.size()) {
            out.append(backslashes * 2, '\\');   // before the closing quote
            break;
        }
        if (argument[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(argument[i]);
        }
    }
    out.push_back('"');
    return out;
#else
    const bool safe = !argument.empty() && argument.find_first_not_of(
                                               "abcdefghijklmnopqrstuvwxyz"
                                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                               "0123456789._-+=/:@,") == std::string_view::npos;
    if (safe) return std::string{argument};
    std::string out = "'";
    for (char c : argument) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
#endif
}

bool is_windows_cmd_script(const ProcessRequest& request) {
#if defined(_WIN32)
    if (request.arguments.size() != 4 || !strings::iequals(request.arguments[0], "/d") ||
        !strings::iequals(request.arguments[1], "/s") ||
        !strings::iequals(request.arguments[2], "/c") ||
        !strings::iequals(request.executable.filename().string(), "cmd.exe"))
        return false;
    wchar_t directory[32768];
    const UINT length = GetSystemDirectoryW(directory, static_cast<UINT>(std::size(directory)));
    if (!length || length >= std::size(directory)) return false;
    auto candidate = request.executable.lexically_normal();
    candidate.make_preferred();
    const auto system_cmd = (std::filesystem::path(directory) / L"cmd.exe").wstring();
    return _wcsicmp(candidate.c_str(), system_cmd.c_str()) == 0;
#else
    (void)request;
    return false;
#endif
}

std::string ProcessRunner::display_command(const ProcessRequest& request) {
    auto executable = request.executable;
    executable.make_preferred();
    // cmd.exe needs its native backslashes even though the text passed to
    // the UTF-16 command-line converter must remain UTF-8.
    const auto native_utf8 = executable.u8string();
    std::string out = quote_argument(
        {reinterpret_cast<const char*>(native_utf8.data()), native_utf8.size()});
    if (is_windows_cmd_script(request)) {
        // /s removes precisely these enclosing quotes. Inner quotes belong to
        // cmd's script language; C-runtime backslash escaping corrupts paths.
        return out + " /d /s /c \"" + request.arguments.back() + "\"";
    }
    for (const auto& arg : request.arguments) {
        out.push_back(' ');
        out += quote_argument(arg);
    }
    return out;
}

std::optional<std::filesystem::path> find_executable(
    std::string_view name, const std::filesystem::path& working_directory) {
    if (name.empty()) return std::nullopt;

    std::error_code ec;
    const auto candidate = path_from_utf8(name);

    auto is_runnable = [](const std::filesystem::path& p) {
        std::error_code e;
        if (!std::filesystem::is_regular_file(p, e)) return false;
#if !defined(_WIN32)
        return ::access(p.c_str(), X_OK) == 0;
#else
        return true;
#endif
    };

    // Explicit path (absolute, or containing a separator): resolve directly.
    if (candidate.is_absolute() || candidate.has_parent_path()) {
        const auto base = candidate.is_absolute()
                              ? candidate
                              : (working_directory.empty() ? std::filesystem::current_path(ec)
                                                           : working_directory) /
                                    candidate;
#if defined(_WIN32)
        if (!base.has_extension()) {
            for (const auto& ext : {".exe", ".cmd", ".bat", ".com"}) {
                auto with_ext = base;
                with_ext += ext;
                if (is_runnable(with_ext)) return std::filesystem::weakly_canonical(with_ext, ec);
            }
        }
#endif
        if (is_runnable(base)) return std::filesystem::weakly_canonical(base, ec);
        return std::nullopt;
    }

    const auto path_env = env_value("PATH");
#if defined(_WIN32)
    const char separator = ';';
    auto extensions = strings::split(env_value("PATHEXT"), ';', false);
    if (extensions.empty()) extensions = {".COM", ".EXE", ".BAT", ".CMD"};
#else
    const char separator = ':';
    const std::vector<std::string> extensions{""};
#endif

    for (const auto& dir : strings::split(path_env, separator, false)) {
        if (dir.empty()) continue;
        const std::filesystem::path base = std::filesystem::path{dir} / candidate;
        if (candidate.has_extension() && is_runnable(base)) {
            return std::filesystem::weakly_canonical(base, ec);
        }
        for (const auto& ext : extensions) {
            auto with_ext = base;
            if (!ext.empty()) with_ext += ext;
            if (is_runnable(with_ext)) return std::filesystem::weakly_canonical(with_ext, ec);
        }
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> find_all_executables(std::string_view name) {
    std::vector<std::filesystem::path> found;
    const auto path_env = env_value("PATH");
#if defined(_WIN32)
    const char separator = ';';
    auto extensions = strings::split(env_value("PATHEXT"), ';', false);
    if (extensions.empty()) extensions = {".COM", ".EXE", ".BAT", ".CMD"};
#else
    const char separator = ':';
    const std::vector<std::string> extensions{""};
#endif
    std::error_code ec;
    for (const auto& dir : strings::split(path_env, separator, false)) {
        if (dir.empty()) continue;
        const std::filesystem::path base = std::filesystem::path{dir} / std::filesystem::path{name};
        for (const auto& ext : extensions) {
            auto with_ext = base;
            if (!ext.empty() && !base.has_extension()) with_ext += ext;
            if (std::filesystem::is_regular_file(with_ext, ec)) {
                auto canonical = std::filesystem::weakly_canonical(with_ext, ec);
                if (std::find(found.begin(), found.end(), canonical) == found.end()) {
                    found.push_back(canonical);
                }
            }
        }
    }
    return found;
}

// ===========================================================================
#if defined(_WIN32)
// ===========================================================================
namespace {

struct HandleCloser {
    HANDLE h = nullptr;

    ~HandleCloser() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    HANDLE release() {
        HANDLE tmp = h;
        h = nullptr;
        return tmp;
    }
};

struct ProcessAttributes {
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    ~ProcessAttributes() {
        if (list) DeleteProcThreadAttributeList(list);
    }
};

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string narrow(const wchar_t* wide, std::size_t length) {
    if (length == 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), out.data(), needed, nullptr,
                        nullptr);
    return out;
}

// Builds a UTF-16 environment block: "K=V\0K=V\0\0", sorted case-insensitively
// as Windows requires.
std::wstring build_environment_block(const ProcessRequest& request) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;

    if (request.inherit_environment) {
        LPWCH block = GetEnvironmentStringsW();
        if (block) {
            for (const wchar_t* p = block; *p;) {
                const std::wstring entry{p};
                p += entry.size() + 1;
                // Skip the "=C:=..." drive-current-directory pseudo variables.
                const auto eq = entry.find(L'=', 1);
                if (eq == std::wstring::npos || entry[0] == L'=') continue;
                entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
            }
            FreeEnvironmentStringsW(block);
        }
    }

    for (const auto& var : request.environment) {
        const auto name = widen(var.name);
        const auto value = widen(var.value);
        auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& kv) {
            return _wcsicmp(kv.first.c_str(), name.c_str()) == 0;
        });
        if (it != entries.end())
            it->second = value;
        else
            entries.emplace_back(name, value);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
    });

    std::wstring block;
    for (const auto& [name, value] : entries) {
        block += name;
        block.push_back(L'=');
        block += value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty()) block.push_back(L'\0');
    return block;
}

void pump(HANDLE pipe, OutputCollector& collector, const char* label,
          const std::function<void(std::string_view, bool)>& callback, bool is_stderr) {
    char buffer[16384];
    bool observer_enabled = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof buffer, &read, nullptr) || read == 0) break;
        const std::string_view chunk{buffer, read};
        collector.append(chunk, label);
        observe_output(callback, chunk, is_stderr, observer_enabled);
    }
}

}   // namespace

Result<ProcessResult> ProcessRunner::run(const ProcessRequest& request, CancellationToken cancel) {
    ProcessResult result;
    result.display_command = display_command(request);
    result.working_directory = request.working_directory;
    result.full_log_path = request.full_log_path;

    if (request.executable.empty()) {
        return make_error(ErrorCode::InvalidArgument, "no executable specified");
    }

    if (std::any_of(request.arguments.begin(), request.arguments.end(), [](const auto& argument) {
            return argument.find('\0') != std::string::npos;
        })) {
        return make_error(ErrorCode::InvalidArgument,
                          "a process argument contains an embedded NUL");
    }

    DWORD_PTR cpu_mask = 0;
    if (!request.cpu_allowance.empty()) {
        if (GetActiveProcessorGroupCount() != 1)
            return make_error(ErrorCode::NotSupported, "CPU allowance requires one Windows processor group.");
        DWORD_PTR allowed = 0, system = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system))
            return make_error(ErrorCode::Internal, "Cannot query the inherited CPU allowance.");
        for (const auto cpu : request.cpu_allowance) {
            if (cpu >= sizeof(DWORD_PTR) * 8 || !(allowed & (DWORD_PTR{1} << cpu)) ||
                !(system & (DWORD_PTR{1} << cpu)) || (cpu_mask & (DWORD_PTR{1} << cpu)))
                return make_error(ErrorCode::InvalidArgument, "CPU allowance contains unavailable or duplicate processors.");
            cpu_mask |= DWORD_PTR{1} << cpu;
        }
    }

    std::error_code ec;
    if (!request.working_directory.empty() &&
        !std::filesystem::is_directory(request.working_directory, ec)) {
        return make_error(ErrorCode::NotFound, "working directory does not exist: " +
                                                   request.working_directory.string());
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HandleCloser out_read, out_write, err_read, err_write, in_read, in_write;
    if (!CreatePipe(&out_read.h, &out_write.h, &sa, 0) ||
        !CreatePipe(&err_read.h, &err_write.h, &sa, 0) ||
        !CreatePipe(&in_read.h, &in_write.h, &sa, 0)) {
        return make_error(ErrorCode::Internal, "CreatePipe failed",
                          "win32 error " + std::to_string(GetLastError()));
    }
    // Only the child end of each pipe may be inherited.
    if (!SetHandleInformation(out_read.h, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(err_read.h, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(in_write.h, HANDLE_FLAG_INHERIT, 0)) {
        return make_error(ErrorCode::Internal, "cannot restrict pipe inheritance",
                          "win32 error " + std::to_string(GetLastError()));
    }

    // A Job Object with KILL_ON_JOB_CLOSE guarantees the whole process tree
    // dies with us, even if LocalCode itself is killed.
    HandleCloser job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (cpu_mask) {
        // Install the allowance with atomic job membership before any child
        // instruction runs. Thread scheduling remains free within this pool.
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_AFFINITY;
        limits.BasicLimitInformation.Affinity = cpu_mask;
    }
    if (!job.h || !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits,
                                           sizeof limits)) {
        return make_error(ErrorCode::Internal, "cannot protect the process tree",
                          "win32 error " + std::to_string(GetLastError()));
    }

    // Install job membership as part of creation: an abrupt parent exit must
    // not leave a suspended, unassigned child. Inherit only the three pipe ends.
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
    if (!attribute_bytes) {
        return make_error(ErrorCode::Internal, "cannot size process attributes",
                          "win32 error " + std::to_string(GetLastError()));
    }
    std::vector<unsigned char> attribute_storage(attribute_bytes);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 2, 0, &attribute_bytes)) {
        return make_error(ErrorCode::Internal, "cannot initialize process attributes",
                          "win32 error " + std::to_string(GetLastError()));
    }
    ProcessAttributes attributes{attribute_list};
    std::array<HANDLE, 3> inherited{in_read.h, out_write.h, err_write.h};
    if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.h,
                                   sizeof job.h, nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof inherited, nullptr, nullptr)) {
        return make_error(ErrorCode::Internal, "cannot install process isolation",
                          "win32 error " + std::to_string(GetLastError()));
    }

    std::wstring command_line = widen(result.display_command);
    command_line.push_back(L'\0');
    const auto environment_block = build_environment_block(request);
    const std::wstring exe = request.executable.wstring();
    const std::wstring cwd =
        request.working_directory.empty() ? std::wstring{} : request.working_directory.wstring();

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read.h;
    si.StartupInfo.hStdOutput = out_write.h;
    si.StartupInfo.hStdError = err_write.h;
    si.lpAttributeList = attribute_list;

    PROCESS_INFORMATION pi{};
    const DWORD flags =
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;

    const BOOL created = CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                        flags, (LPVOID)environment_block.data(),
                                        cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    if (!created) {
        const DWORD err = GetLastError();
        return make_error(ErrorCode::NotFound, "cannot start " + request.executable.string(),
                          "win32 error " + std::to_string(err));
    }

    HandleCloser process{pi.hProcess};
    HandleCloser thread{pi.hThread};
    if (cpu_mask) {
        DWORD_PTR actual = 0, system = 0;
        if (!GetProcessAffinityMask(process.h, &actual, &system) || actual != cpu_mask) {
            TerminateJobObject(job.h, 1);
            WaitForSingleObject(process.h, 5000);
            return make_error(ErrorCode::NotSupported, "The child did not inherit the requested CPU allowance.");
        }
    }

    // Close our copies of the child ends so reads see EOF when the child exits.
    CloseHandle(out_write.release());
    CloseHandle(err_write.release());

    const auto start = std::chrono::steady_clock::now();

    // Kill the tree the moment the user presses Stop.
    std::atomic<bool> cancelled{false};
    HANDLE job_handle = job.h;
    CancellationRegistration registration{cancel, [&cancelled, job_handle, proc = pi.hProcess]() {
                                              cancelled.store(true);
                                              if (job_handle)
                                                  TerminateJobObject(job_handle, 1);
                                              else
                                                  TerminateProcess(proc, 1);
                                          }};
    if (request.on_started) {
        try {
            request.on_started(pi.dwProcessId);
        } catch (...) {
            log_error(LogChannel::App, "process start observer threw");
        }
    }

    std::ofstream spool;
    std::mutex spool_mutex;
    if (!request.full_log_path.empty()) {
        std::filesystem::create_directories(request.full_log_path.parent_path(), ec);
        spool.open(request.full_log_path, std::ios::binary | std::ios::trunc);
        if (spool.is_open()) {
            spool << "$ " << result.display_command << "\n"
                  << "# cwd: " << request.working_directory.string() << "\n\n";
        }
    }
    std::ofstream* spool_ptr = spool.is_open() ? &spool : nullptr;

    OutputCollector out_collector{request.max_captured_bytes, spool_ptr, &spool_mutex};
    OutputCollector err_collector{request.max_captured_bytes, spool_ptr, &spool_mutex};

    std::thread out_thread(pump, out_read.h, std::ref(out_collector), "", request.on_output, false);
    std::thread err_thread(pump, err_read.h, std::ref(err_collector), "", request.on_output, true);

    if (!request.stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(in_write.h, request.stdin_data.data(),
                  static_cast<DWORD>(request.stdin_data.size()), &written, nullptr);
    }
    CloseHandle(in_write.release());   // signal EOF on the child's stdin

    const DWORD timeout_ms =
        request.timeout.count() > 0 ? static_cast<DWORD>(request.timeout.count()) : INFINITE;
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    const DWORD wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    if (wait == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job.h)
            TerminateJobObject(job.h, 1);
        else
            TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    // A successful root may have launched background children that still hold
    // the captured pipes. Reap them before joining the readers, otherwise even
    // a three-second request can block indefinitely after its root has exited.
    TerminateJobObject(job.h, 1);
    if (wait == WAIT_FAILED) WaitForSingleObject(pi.hProcess, 5000);

    out_thread.join();
    err_thread.join();

    if (wait == WAIT_FAILED) {
        return make_error(ErrorCode::Internal, "cannot wait for the child process",
                          "win32 error " + std::to_string(wait_error));
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    result.started = true;
    result.cancelled = cancelled.load();
    result.exit_code = static_cast<int>(exit_code);
    result.stdout_text = out_collector.take();
    result.stderr_text = err_collector.take();
    result.stdout_truncated = out_collector.truncated();
    result.stderr_truncated = err_collector.truncated();
    result.stdout_total_bytes = out_collector.total();
    result.stderr_total_bytes = err_collector.total();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    if (spool_ptr) {
        *spool_ptr << "\n# exit code: " << result.exit_code
                   << (result.timed_out ? " (timed out)" : "")
                   << (result.cancelled ? " (cancelled)" : "") << "\n";
        spool_ptr->close();
    }
    return result;
}

// ===========================================================================
#else   // POSIX
// ===========================================================================
namespace {

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}   // namespace

Result<ProcessResult> ProcessRunner::run(const ProcessRequest& request, CancellationToken cancel) {
    ProcessResult result;
    result.display_command = display_command(request);
    result.working_directory = request.working_directory;
    result.full_log_path = request.full_log_path;

    if (request.executable.empty()) {
        return make_error(ErrorCode::InvalidArgument, "no executable specified");
    }

    if (std::any_of(request.arguments.begin(), request.arguments.end(), [](const auto& argument) {
            return argument.find('\0') != std::string::npos;
        })) {
        return make_error(ErrorCode::InvalidArgument,
                          "a process argument contains an embedded NUL");
    }

#if defined(__linux__)
    cpu_set_t cpu_allowance{};
    if (!request.cpu_allowance.empty()) {
        cpu_set_t allowed{};
        if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
            return make_error(ErrorCode::NotSupported, "Cannot query the inherited CPU allowance.");
        for (const auto cpu : request.cpu_allowance) {
            if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed) || CPU_ISSET(cpu, &cpu_allowance))
                return make_error(ErrorCode::InvalidArgument, "CPU allowance contains unavailable or duplicate processors.");
            CPU_SET(cpu, &cpu_allowance);
        }
    }
#else
    if (!request.cpu_allowance.empty())
        return make_error(ErrorCode::NotSupported, "CPU allowance is unavailable on this platform.");
#endif

    int out_pipe[2], err_pipe[2], in_pipe[2];
    if (::pipe(out_pipe) != 0) return make_error(ErrorCode::Internal, "pipe() failed");
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return make_error(ErrorCode::Internal, "pipe() failed");
    }
    if (::pipe(in_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return make_error(ErrorCode::Internal, "pipe() failed");
    }
    // fork() succeeding does not mean the requested program started. The
    // child reports setup/exec errors here; successful exec closes this pipe.
    int launch_pipe[2];
#if defined(__linux__)
    const auto launch_pipe_status = ::pipe2(launch_pipe, O_CLOEXEC);
#else
    const auto launch_pipe_status = ::pipe(launch_pipe);
#endif
    if (launch_pipe_status != 0) {
        for (const int fd :
             {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], in_pipe[0], in_pipe[1]})
            ::close(fd);
        return make_error(ErrorCode::Internal, "cannot create the process launch pipe");
    }
#if !defined(__linux__)
    if (::fcntl(launch_pipe[1], F_SETFD, FD_CLOEXEC) == -1 ||
        ::fcntl(launch_pipe[0], F_SETFD, FD_CLOEXEC) == -1) {
        for (const int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1], in_pipe[0],
                             in_pipe[1], launch_pipe[0], launch_pipe[1]})
            ::close(fd);
        return make_error(ErrorCode::Internal, "cannot configure the process launch pipe");
    }
#endif
    struct LaunchError {
        int stage;
        int error;
    };

    std::vector<std::string> argv_storage;
    argv_storage.push_back(request.executable.string());
    for (const auto& a : request.arguments)
        argv_storage.push_back(a);
    std::vector<char*> argv;
    for (auto& s : argv_storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    if (request.inherit_environment) {
        for (char** e = environ; *e; ++e)
            env_storage.emplace_back(*e);
    }
    for (const auto& var : request.environment) {
        const std::string prefix = var.name + "=";
        auto it = std::find_if(env_storage.begin(), env_storage.end(),
                               [&](const std::string& s) { return s.rfind(prefix, 0) == 0; });
        if (it != env_storage.end())
            *it = prefix + var.value;
        else
            env_storage.push_back(prefix + var.value);
    }
    std::vector<char*> envp;
    for (auto& s : env_storage)
        envp.push_back(s.data());
    envp.push_back(nullptr);

    const auto start = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(launch_pipe[0]);
        ::close(launch_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return make_error(ErrorCode::Internal, "fork() failed");
    }

    if (pid == 0) {
        ::close(launch_pipe[0]);
        const auto launch_failed = [&](int stage) {
            const LaunchError failure{stage, errno};
            ssize_t written;
            do {
                written = ::write(launch_pipe[1], &failure, sizeof(failure));
            } while (written < 0 && errno == EINTR);
            ::_exit(127);
        };
        // Child: become a process-group leader so we can kill the whole tree.
        ::setpgid(0, 0);
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
#if defined(__linux__)
        if (!request.cpu_allowance.empty()) {
            cpu_set_t actual{};
            if (::sched_setaffinity(0, sizeof(cpu_allowance), &cpu_allowance) != 0 ||
                ::sched_getaffinity(0, sizeof(actual), &actual) != 0)
                launch_failed(1);
            if (std::memcmp(&actual, &cpu_allowance, sizeof(actual)) != 0) {
                errno = EINVAL;
                launch_failed(1);
            }
        }
#endif
        if (!request.working_directory.empty()) {
            if (::chdir(request.working_directory.c_str()) != 0) launch_failed(2);
        }
        ::execve(request.executable.c_str(), argv.data(), envp.data());
        launch_failed(3);
    }

    ::setpgid(pid, pid);   // also set in the parent to avoid a race
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    ::close(in_pipe[0]);
    ::close(launch_pipe[1]);
    std::atomic<bool> cancelled{false};
    CancellationRegistration registration{cancel, [&cancelled, pid]() {
                                              cancelled.store(true);
                                              ::kill(-pid, SIGKILL);
                                          }};
    set_nonblocking(launch_pipe[0]);
    LaunchError launch_error{};
    std::size_t received = 0;
    int launch_read_error = 0;
    bool launch_timed_out = false;
    while (received < sizeof(launch_error)) {
        if (cancelled.load()) break;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (request.timeout.count() > 0 && elapsed >= request.timeout) {
            launch_timed_out = true;
            break;
        }
        const auto count = ::read(launch_pipe[0], reinterpret_cast<char*>(&launch_error) + received,
                                  sizeof(launch_error) - received);
        if (count > 0)
            received += static_cast<std::size_t>(count);
        else if (count == 0)
            break;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd launch_fd{launch_pipe[0], POLLIN, 0};
            ::poll(&launch_fd, 1, 20);
        } else if (errno != EINTR) {
            launch_read_error = errno;
            break;
        }
    }
    ::close(launch_pipe[0]);
    if (received != 0 || launch_read_error != 0 || launch_timed_out || cancelled.load()) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::close(in_pipe[1]);
        ::kill(-pid, SIGKILL);
        int failed_status = 0;
        while (::waitpid(pid, &failed_status, 0) < 0 && errno == EINTR) {
        }
        if (cancelled.load())
            return make_error(ErrorCode::Cancelled, "process launch was cancelled");
        if (launch_timed_out)
            return make_error(ErrorCode::Timeout, "process launch timed out");
        if (received != sizeof(launch_error))
            return make_error(ErrorCode::Internal, "cannot read the child process launch result");
        const auto code = launch_error.error == ENOENT || launch_error.error == ENOTDIR
                              ? ErrorCode::NotFound
                          : launch_error.error == EACCES || launch_error.error == EPERM
                              ? ErrorCode::PermissionDenied
                              : ErrorCode::IoError;
        const char* message = launch_error.stage == 1 ? "cannot apply the child CPU allowance"
                              : launch_error.stage == 2
                                  ? "cannot enter the process working directory"
                                  : "cannot start the process";
        return make_error(code, message, std::system_category().message(launch_error.error));
    }

    if (!request.stdin_data.empty()) {
        [[maybe_unused]] ssize_t n =
            ::write(in_pipe[1], request.stdin_data.data(), request.stdin_data.size());
    }
    ::close(in_pipe[1]);

    if (request.on_started) {
        try {
            request.on_started(static_cast<std::uint32_t>(pid));
        } catch (...) {
            log_error(LogChannel::App, "process start observer threw");
        }
    }

    std::ofstream spool;
    std::mutex spool_mutex;
    if (!request.full_log_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(request.full_log_path.parent_path(), ec);
        spool.open(request.full_log_path, std::ios::binary | std::ios::trunc);
        if (spool.is_open()) {
            spool << "$ " << result.display_command
                  << "\n# cwd: " << request.working_directory.string() << "\n\n";
        }
    }
    std::ofstream* spool_ptr = spool.is_open() ? &spool : nullptr;

    OutputCollector out_collector{request.max_captured_bytes, spool_ptr, &spool_mutex};
    OutputCollector err_collector{request.max_captured_bytes, spool_ptr, &spool_mutex};

    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);

    bool out_open = true, err_open = true;
    bool observer_enabled = true;
    while (out_open || err_open) {
        struct pollfd fds[2];
        int count = 0;
        if (out_open) fds[count++] = {out_pipe[0], POLLIN, 0};
        if (err_open) fds[count++] = {err_pipe[0], POLLIN, 0};

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (request.timeout.count() > 0 && elapsed >= request.timeout) {
            result.timed_out = true;
            ::kill(-pid, SIGKILL);
            break;
        }
        ::poll(fds, static_cast<nfds_t>(count), 100);

        int index = 0;
        auto drain = [&](int fd, OutputCollector& collector, bool is_stderr, bool& open_flag) {
            char buffer[16384];
            for (;;) {
                const ssize_t n = ::read(fd, buffer, sizeof buffer);
                if (n > 0) {
                    const std::string_view chunk{buffer, static_cast<std::size_t>(n)};
                    collector.append(chunk, "");
                    observe_output(request.on_output, chunk, is_stderr, observer_enabled);
                } else if (n == 0) {
                    open_flag = false;
                    break;
                } else {
                    break;   // EAGAIN
                }
            }
        };
        if (out_open) {
            if (fds[index].revents) drain(out_pipe[0], out_collector, false, out_open);
            ++index;
        }
        if (err_open) {
            if (fds[index].revents) drain(err_pipe[0], err_collector, true, err_open);
        }
    }

    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);

    result.started = true;
    result.cancelled = cancelled.load();
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    result.stdout_text = out_collector.take();
    result.stderr_text = err_collector.take();
    result.stdout_truncated = out_collector.truncated();
    result.stderr_truncated = err_collector.truncated();
    result.stdout_total_bytes = out_collector.total();
    result.stderr_total_bytes = err_collector.total();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    if (spool_ptr) {
        *spool_ptr << "\n# exit code: " << result.exit_code << "\n";
        spool_ptr->close();
    }
    return result;
}

#endif

}   // namespace localcode
