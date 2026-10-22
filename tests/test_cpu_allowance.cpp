#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <limits>
#include <fstream>
#include <thread>
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "platform/Process.h"
#include "support/TempDirectory.h"
#include "util/Json.h"
#include "util/Strings.h"
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

using namespace localcode;
namespace {
std::vector<std::uint32_t> inherited_cpus() {
    std::vector<std::uint32_t> cpus;
#if defined(_WIN32)
    DWORD_PTR mask = 0, system = 0;
    REQUIRE(GetProcessAffinityMask(GetCurrentProcess(), &mask, &system));
    for (std::uint32_t i = 0; i < sizeof(mask) * 8; ++i)
        if (mask & (DWORD_PTR{1} << i)) cpus.push_back(i);
#elif defined(__linux__)
    cpu_set_t mask{};
    REQUIRE(sched_getaffinity(0, sizeof(mask), &mask) == 0);
    for (std::uint32_t i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &mask)) cpus.push_back(i);
#endif
    return cpus;
}
std::string printed(const std::vector<std::uint32_t>& cpus) {
    std::string out = "cpus=";
    for (const auto cpu : cpus) {
        if (out.back() != '=') out += ',';
        out += std::to_string(cpu);
    }
    return out;
}
ProcessRequest probe() {
    ProcessRequest request;
    request.executable = path_from_utf8(LOCALCODE_TEST_SERVER_FIXTURE);
    request.arguments = {"--fixture-cpu-allowance"};
    request.timeout = std::chrono::seconds(10);
    return request;
}
}  // namespace

TEST_CASE("owned children see the CPU allowance at their first application operation",
          "[process][cpu-allowance]") {
#if defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
    if (GetActiveProcessorGroupCount() != 1) {
        auto request = probe();
        request.cpu_allowance = {0};
        const auto result = ProcessRunner::run(request);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::NotSupported);
        return;
    }
#endif
    const auto before = inherited_cpus();
    REQUIRE_FALSE(before.empty());
    auto request = probe();
    SECTION("empty preserves the inherited allowance") {}
    SECTION("one logical processor") { request.cpu_allowance = {before.front()}; }
    SECTION("two logical processors when available") {
        request.cpu_allowance.assign(before.begin(), before.begin() + std::min<std::size_t>(2, before.size()));
    }
    const auto result = ProcessRunner::run(request);
    REQUIRE(result);
    INFO(result->stderr_text);
    REQUIRE(result->success());
    CHECK(strings::trim(result->stdout_text) == printed(request.cpu_allowance.empty() ? before : request.cpu_allowance));
    CHECK(inherited_cpus() == before);
#else
    auto request = probe();
    request.cpu_allowance = {0};
    const auto result = ProcessRunner::run(request);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::NotSupported);
#endif
}

TEST_CASE("invalid CPU allowances fail before starting a child", "[process][cpu-allowance]") {
#if defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
    if (GetActiveProcessorGroupCount() != 1) SKIP("single-group validation exercised separately");
#endif
    const auto before = inherited_cpus();
    REQUIRE_FALSE(before.empty());
    auto request = probe();
    SECTION("duplicates") { request.cpu_allowance = {before.front(), before.front()}; }
    SECTION("unrepresentable CPU") { request.cpu_allowance = {std::numeric_limits<std::uint32_t>::max()}; }
    bool started = false;
    request.on_started = [&](std::uint32_t) { started = true; };
    const auto result = ProcessRunner::run(request);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(started);
    CHECK(inherited_cpus() == before);
#endif
}

TEST_CASE("CPU allowance retains cancellation and timeout cleanup", "[process][cpu-allowance][lifecycle]") {
#if defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
    if (GetActiveProcessorGroupCount() != 1) SKIP("requires a representable CPU allowance");
#endif
    const auto before = inherited_cpus();
    REQUIRE_FALSE(before.empty());
    auto request = probe();
    request.arguments = {"--fixture-cpu-allowance-wait"};
    request.cpu_allowance = {before.front()};
    CancellationSource cancellation;
    bool timeout = false;
    SECTION("cancel after the child independently reports its allowance") {
        request.on_output = [&](std::string_view chunk, bool error) {
            if (!error && chunk.find("cpus=") != std::string_view::npos) cancellation.cancel();
        };
    }
    SECTION("deadline still terminates the owned child") {
        timeout = true;
        request.timeout = std::chrono::seconds(2);
    }
    const auto start = std::chrono::steady_clock::now();
    const auto result = ProcessRunner::run(request, cancellation.token());
    REQUIRE(result);
    CHECK_FALSE(result->success());
    CHECK(strings::trim(result->stdout_text).starts_with(printed(request.cpu_allowance)));
    CHECK(result->timed_out == timeout);
    CHECK(result->cancelled == !timeout);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(8));
    CHECK(inherited_cpus() == before);
#endif
}

TEST_CASE("CPU allowance follows a descendant into its first native operation", "[process][cpu-allowance][lifecycle]") {
#if defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
    if (GetActiveProcessorGroupCount() != 1) SKIP("requires a representable CPU allowance");
    const auto shell = find_executable("cmd");
#else
    const auto shell = find_executable("sh");
#endif
    REQUIRE(shell);
    const auto before = inherited_cpus();
    REQUIRE_FALSE(before.empty());
    auto request = probe();
    request.cpu_allowance = {before.front()};
    const auto command = quote_argument(path_to_utf8(request.executable)) + " --fixture-cpu-allowance";
    request.executable = *shell;
#if defined(_WIN32)
    request.arguments = {"/d", "/s", "/c", command};
#else
    request.arguments = {"-c", command + "; :"};
#endif
    const auto result = ProcessRunner::run(request);
    REQUIRE(result);
    INFO(result->stderr_text);
    REQUIRE(result->success());
    CHECK(strings::trim(result->stdout_text) == printed(request.cpu_allowance));
    CHECK(inherited_cpus() == before);
#endif
}

TEST_CASE("CPU allowance owner helper", "[.][cpu-owner-helper]") {
#if defined(_WIN32)
    wchar_t marker[32768]{}, cpu[32]{};
    const auto marker_size = GetEnvironmentVariableW(L"LOCALCODE_CPU_CHILD_PID_FILE", marker, 32768);
    const auto cpu_size = GetEnvironmentVariableW(L"LOCALCODE_CPU_CHILD_INDEX", cpu, 32);
    if (!marker_size || !cpu_size) SKIP("only launched by the owned CPU allowance test");
    REQUIRE(marker_size < 32768);
    REQUIRE(cpu_size < 32);
    auto request = probe();
    request.arguments = {"--fixture-cpu-allowance-wait"};
    request.cpu_allowance = {static_cast<std::uint32_t>(std::stoul(cpu))};
    request.timeout = std::chrono::seconds(60);
    request.on_started = [&](std::uint32_t pid) {
        std::ofstream(std::filesystem::path(marker)) << pid << '\n';
    };
    const auto result = ProcessRunner::run(request);
    REQUIRE(result);
#else
    SKIP("Windows Job Object owner-exit helper");
#endif
}

TEST_CASE("owned native CPU allowance diagnostic", "[.][cpu-pool-native]") {
    const auto* config_file = std::getenv("LOCALCODE_NATIVE_CPU_REQUEST");
    if (!config_file) SKIP("explicit public-model diagnostic only");
    std::ifstream input(path_from_utf8(config_file));
    REQUIRE(input);
    Json config;
    input >> config;
    ProcessRequest request;
    request.executable = path_from_utf8(config.at("server").get<std::string>());
    request.arguments = config.at("arguments").get<std::vector<std::string>>();
    request.cpu_allowance = config.at("cpus").get<std::vector<std::uint32_t>>();
    request.working_directory = request.executable.parent_path();
    request.timeout = std::chrono::seconds(120);
    request.on_started = [&](std::uint32_t pid) {
        std::ofstream(path_from_utf8(config.at("pid_file").get<std::string>())) << pid;
    };
    const auto stop_file = path_from_utf8(config.at("stop_file").get<std::string>());
    CancellationSource cancellation;
    std::jthread watcher([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (std::filesystem::exists(stop_file)) { cancellation.cancel(); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const auto result = ProcessRunner::run(request, cancellation.token());
    watcher.request_stop();
    Json receipt;
    if (result) {
        receipt = {{"started", result->started}, {"cancelled", result->cancelled},
                   {"exit_code", result->exit_code}, {"stdout", result->stdout_text},
                   {"stderr", result->stderr_text}};
    } else {
        receipt = {{"error", result.error().to_string()}};
    }
    std::ofstream(path_from_utf8(config.at("result_file").get<std::string>())) << receipt.dump(2);
    REQUIRE(result);
    REQUIRE(result->started);
    REQUIRE(result->cancelled);
}

TEST_CASE("hard owner exit keeps CPU-limited children in kill-on-close ownership",
          "[process][cpu-allowance][lifecycle]") {
#if defined(_WIN32)
    if (GetActiveProcessorGroupCount() != 1) SKIP("requires a representable CPU allowance");
    const auto before = inherited_cpus();
    REQUIRE_FALSE(before.empty());
    localcode::test::TempDirectory directory{"lc-cpu-owner"};
    const auto marker = directory.path() / "child.pid";
    wchar_t windows[32768]{};
    const auto windows_size = GetWindowsDirectoryW(windows, 32768);
    REQUIRE(windows_size > 0);
    REQUIRE(windows_size < 32768);
    // Minimal private environment; no global setting or production home changes.
    std::vector<wchar_t> environment;
    for (const auto& entry : {
             L"LOCALCODE_CPU_CHILD_INDEX=" + std::to_wstring(before.front()),
             L"LOCALCODE_CPU_CHILD_PID_FILE=" + marker.wstring(),
             L"SystemRoot=" + std::wstring(windows),
             L"TEMP=" + directory.path().wstring(),
             L"TMP=" + directory.path().wstring()}) {
        environment.insert(environment.end(), entry.begin(), entry.end());
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    const auto executable = AppPaths::executable_path().wstring();
    auto command = L"\"" + executable + L"\" \"CPU allowance owner helper\" --reporter compact";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &information));
    CloseHandle(information.hThread);
    struct OwnedTestHandle {
        HANDLE value;
        ~OwnedTestHandle() {
            if (WaitForSingleObject(value, 0) == WAIT_TIMEOUT) {
                TerminateProcess(value, 1);
                WaitForSingleObject(value, 5000);
            }
            CloseHandle(value);
        }
    } owner{information.hProcess};
    DWORD child_pid = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!child_pid && std::chrono::steady_clock::now() < deadline) {
        std::ifstream receipt(marker);
        receipt >> child_pid;
        if (!child_pid) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(child_pid != 0);
    const auto child_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                         FALSE, child_pid);
    REQUIRE(child_handle != nullptr);
    OwnedTestHandle child{child_handle};
    wchar_t image[32768]{};
    DWORD image_size = 32768;
    REQUIRE(QueryFullProcessImageNameW(child.value, 0, image, &image_size));
    REQUIRE(std::filesystem::equivalent(std::filesystem::path(image), path_from_utf8(LOCALCODE_TEST_SERVER_FIXTURE)));
    DWORD_PTR actual = 0, system = 0;
    REQUIRE(GetProcessAffinityMask(child.value, &actual, &system));
    CHECK(actual == (DWORD_PTR{1} << before.front()));
    REQUIRE(TerminateProcess(owner.value, 1));
    REQUIRE(WaitForSingleObject(owner.value, 5000) == WAIT_OBJECT_0);
    // There is no outer test-runner Job Object around this owner. Its own
    // handle close, rather than our normal runner cleanup, must reap the child.
    CHECK(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0);
    CHECK(inherited_cpus() == before);
#endif
}
