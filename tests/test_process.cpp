// Process execution: argument fidelity, exit codes, timeout, cancellation and
// the output cap.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "platform/Environment.h"
#include "platform/PathEncoding.h"
#include "platform/Process.h"
#include "inference/OpenAiCompatBackend.h"
#include "support/TempDirectory.h"
#include "tools/CommandInspection.h"
#include "tools/ToolRegistry.h"
#include "util/Json.h"
#include "util/Strings.h"
#include "workspace/Workspace.h"

using namespace localcode;
using localcode::test::TempDirectory;

namespace {

// A small portable program to drive: the system's own shell.
ProcessRequest shell_command(const std::string& script) {
    ProcessRequest request;
#if defined(_WIN32)
    auto shell = find_executable("cmd");
    REQUIRE(shell.has_value());
    request.executable = *shell;
    request.arguments = {"/d", "/s", "/c", script};
#else
    auto shell = find_executable("sh");
    REQUIRE(shell.has_value());
    request.executable = *shell;
    request.arguments = {"-c", script};
#endif
    request.timeout = std::chrono::seconds(30);
    return request;
}

}   // namespace

TEST_CASE("a failing cancellation callback cannot skip remaining cleanup", "[cancel]") {
    CancellationSource source;
    auto token = source.token();
    int cleaned = 0;
    token.on_cancel([] { throw std::runtime_error("cleanup failure"); });
    token.on_cancel([&] { ++cleaned; });
    CHECK_NOTHROW(source.cancel());
    CHECK(token.cancelled());
    CHECK(cleaned == 1);
    CHECK_NOTHROW(source.cancel());
    CHECK(cleaned == 1);
    CHECK_NOTHROW(token.on_cancel([] { throw std::runtime_error("late cleanup failure"); }));
    CHECK(token.on_cancel([&] { ++cleaned; }) == 0);
    CHECK(cleaned == 2);
}

TEST_CASE("destroying a registration disables cancellation already queued for delivery",
          "[cancel]") {
    CancellationSource source;
    auto token = source.token();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto released = release.get_future().share();
    // cancel() selects both callbacks before running either one. Hold the first
    // so the second can be destroyed after selection but before invocation.
    token.on_cancel([&] {
        entered.set_value();
        released.wait();
    });
    std::atomic<int> calls = 0;
    auto registration = std::make_unique<CancellationRegistration>(token, [&] { ++calls; });
    std::jthread cancelling([&] { source.cancel(); });
    CHECK(entered_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    registration.reset();
    release.set_value();
    cancelling.join();
    CHECK(calls.load() == 0);
}

TEST_CASE("a command runs and its output is captured", "[process]") {
    auto request = shell_command("echo hello-from-localcode");
    auto result = ProcessRunner::run(request);

    REQUIRE(result.ok());
    CHECK(result->started);
    CHECK(result->exit_code == 0);
    CHECK(result->success());
    CHECK(result->stdout_text.find("hello-from-localcode") != std::string::npos);
    CHECK(result->duration_ms >= 0);
}

TEST_CASE("throwing output observers do not terminate process cleanup", "[process][observer]") {
    auto request = shell_command("echo observer-output");
    request.on_output = [](std::string_view, bool) {
        throw std::runtime_error("fixture output observer failure");
    };
    auto result = ProcessRunner::run(request);
    REQUIRE(result);
    CHECK(result->exit_code == 0);
    CHECK(result->stdout_text.find("observer-output") != std::string::npos);
    request.on_output = [](std::string_view, bool) { throw 7; };
    result = ProcessRunner::run(request);
    REQUIRE(result);
    CHECK(result->exit_code == 0);
    CHECK(result->stdout_text.find("observer-output") != std::string::npos);
}

TEST_CASE("a non-zero exit code is reported, not treated as an error", "[process]") {
    int exit_code = 3;
    SECTION("ordinary nonzero exit") {}
    SECTION("exit 127 is distinct from an exec failure") {
        exit_code = 127;
    }
    auto request = shell_command("exit " + std::to_string(exit_code));
    auto result = ProcessRunner::run(request);

    // The tool worked; the command failed. Those are different things.
    REQUIRE(result.ok());
    CHECK(result->started);
    CHECK(result->exit_code == exit_code);
    CHECK_FALSE(result->success());
}

TEST_CASE("stderr is captured separately", "[process]") {
#if defined(_WIN32)
    auto request = shell_command("echo to-stderr 1>&2");
#else
    auto request = shell_command("echo to-stderr 1>&2");
#endif
    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    CHECK(result->stderr_text.find("to-stderr") != std::string::npos);
}

TEST_CASE("a missing executable fails to start", "[process]") {
    ProcessRequest request;
    request.executable = "this-program-does-not-exist-anywhere-12345";
    bool started = false;
    request.on_started = [&](std::uint32_t) { started = true; };
    auto result = ProcessRunner::run(request);
    CHECK_FALSE(result.ok());
    CHECK_FALSE(started);
}

TEST_CASE("an invalid executable is a launch error rather than a program exit", "[process]") {
#if defined(_WIN32)
    const auto original_error_mode = GetThreadErrorMode();
#endif
    TempDirectory temp{"lc-invalid-executable"};
    const auto executable = temp.write("invalid-program.exe", "not an executable image\n");
#if !defined(_WIN32)
    std::filesystem::permissions(executable, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
#endif
    ProcessRequest request;
    request.executable = executable;
    request.timeout = std::chrono::seconds(2);
    bool started = false;
    request.on_started = [&](std::uint32_t) { started = true; };
    const auto result = ProcessRunner::run(request);
    CHECK_FALSE(result.ok());
    CHECK_FALSE(started);
#if defined(_WIN32)
    CHECK(GetThreadErrorMode() == original_error_mode);
#endif
}

#if !defined(_WIN32)
TEST_CASE("cancellation before launch never reports a started child", "[process][cancel]") {
    auto request = shell_command("sleep 30");
    request.timeout = std::chrono::seconds(60);
    bool started = false;
    request.on_started = [&](std::uint32_t) { started = true; };
    CancellationSource source;
    source.cancel();
    const auto result = ProcessRunner::run(request, source.token());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(started);
}

TEST_CASE("an executable permission failure never reports a started child", "[process]") {
    TempDirectory temp{"lc-unexecutable"};
    ProcessRequest request;
    request.executable = temp.write("unexecutable", "#!/bin/sh\nexit 0\n");
    std::filesystem::permissions(request.executable, std::filesystem::perms::owner_read |
                                                         std::filesystem::perms::owner_write);
    bool started = false;
    request.on_started = [&](std::uint32_t) { started = true; };
    const auto result = ProcessRunner::run(request);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::PermissionDenied);
    CHECK_FALSE(started);
}
#endif

TEST_CASE("a hung process is killed at the timeout", "[process][timeout]") {
#if defined(_WIN32)
    // ping against the loopback address is the portable "sleep" on Windows.
    auto request = shell_command("ping -n 30 127.0.0.1 >nul");
#else
    auto request = shell_command("sleep 30");
#endif
    request.timeout = std::chrono::milliseconds(1200);

    const auto started = std::chrono::steady_clock::now();
    auto result = ProcessRunner::run(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);

    REQUIRE(result.ok());
    CHECK(result->timed_out);
    CHECK_FALSE(result->success());
    CHECK(elapsed.count() < 15);   // it must not have waited the full 30s
}

TEST_CASE("cancellation stops a running command promptly", "[process][cancel]") {
#if defined(_WIN32)
    auto request = shell_command("ping -n 30 127.0.0.1 >nul");
#else
    auto request = shell_command("sleep 30");
#endif
    request.timeout = std::chrono::seconds(60);

    CancellationSource source;
    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        source.cancel();
    });

    const auto started = std::chrono::steady_clock::now();
    auto result = ProcessRunner::run(request, source.token());
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    canceller.join();

    REQUIRE(result.ok());
    CHECK(result->cancelled);
    CHECK(elapsed.count() < 15);
}

TEST_CASE("a child process tree is terminated with its parent", "[process][cancel]") {
    // The shell spawns a long-running child; cancelling must take both down,
    // which is what the Job Object is for.
#if defined(_WIN32)
    auto request = shell_command("ping -n 40 127.0.0.1 >nul");
#else
    auto request = shell_command("sh -c 'sleep 40' ");
#endif
    request.timeout = std::chrono::seconds(90);

    CancellationSource source;
    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        source.cancel();
    });

    const auto started = std::chrono::steady_clock::now();
    auto result = ProcessRunner::run(request, source.token());
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    canceller.join();

    REQUIRE(result.ok());
    CHECK(elapsed.count() < 20);
}

TEST_CASE("huge output is capped but counted in full", "[process][output]") {
#if defined(_WIN32)
    auto request = shell_command("for /L %i in (1,1,20000) do @echo line-%i");
#else
    auto request = shell_command("for i in $(seq 1 20000); do echo line-$i; done");
#endif
    request.max_captured_bytes = 8 * 1024;
    request.timeout = std::chrono::seconds(90);

    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());

    // The captured text is bounded, but the true size is still reported - which
    // is what lets a tool say "abridged" honestly.
    CHECK(result->stdout_text.size() <= 8 * 1024 + 64);
    CHECK(result->stdout_total_bytes > result->stdout_text.size());
    CHECK(result->stdout_truncated);
}

TEST_CASE("the full log is spooled to a file", "[process][output]") {
    TempDirectory temp{"lc-proc"};
    auto request = shell_command("echo spooled-line");
    request.full_log_path = temp.path() / "command.log";

    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    REQUIRE(std::filesystem::exists(request.full_log_path));

    std::ifstream in(request.full_log_path);
    const std::string contents{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
    CHECK(contents.find("spooled-line") != std::string::npos);
    CHECK(contents.find("exit code") != std::string::npos);
}

TEST_CASE("Unicode command logs remain valid when replayed as model history",
          "[process][unicode][recovery]") {
    TempDirectory temp{"lc-unicode-log"};
    const auto folder = temp.path() / path_from_utf8("sortie é 路径 🦍");
    std::filesystem::create_directory(folder);
    auto request = shell_command("echo unicode-log-probe");
    request.working_directory = folder;
    request.full_log_path = folder / "command.log";
    const auto process = ProcessRunner::run(request);
    REQUIRE(process);
    REQUIRE(process->success());
    std::ifstream log(request.full_log_path, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(log), {}};
    CHECK(strings::is_valid_utf8(contents));
    CHECK(contents.find(path_to_utf8(folder)) != std::string::npos);

    auto result = ToolResult::success("Command completed", process->stdout_text);
    result.full_log_path = request.full_log_path;
    const auto text = result.to_model_text();
    CHECK(strings::is_valid_utf8(text));
    CHECK(text.find(path_to_utf8(request.full_log_path)) != std::string::npos);
    struct ReplayBackend : OpenAiCompatBackend {
        using OpenAiCompatBackend::OpenAiCompatBackend;
        using OpenAiCompatBackend::build_payload;
    } backend{{}, nullptr};
    ChatRequest replay;
    replay.messages = {ChatMessage::user("Run the command."),
                       ChatMessage::tool_result("run", "run_command", text)};
    CHECK_NOTHROW(backend.build_payload(replay).dump());

    request.working_directory = folder / "missing";
    const auto missing = ProcessRunner::run(request);
    REQUIRE_FALSE(missing);
    CHECK(strings::is_valid_utf8(missing.error().to_string()));
#if defined(_WIN32)
    CHECK(missing.error().to_string().find(path_to_utf8(request.working_directory)) != std::string::npos);
#endif
}

TEST_CASE("the working directory is honoured", "[process]") {
    TempDirectory temp{"lc-cwd"};
    temp.write("marker-file.txt", "x");

#if defined(_WIN32)
    auto request = shell_command("dir /b");
#else
    auto request = shell_command("ls");
#endif
    request.working_directory = temp.path();

    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    CHECK(result->stdout_text.find("marker-file.txt") != std::string::npos);
}

TEST_CASE("a non-existent working directory is refused", "[process]") {
    TempDirectory temp{"lc-missing-cwd"};
    auto request = shell_command("echo x");
    request.working_directory = temp.path() / "directory-that-does-not-exist";
    bool started = false;
    request.on_started = [&](std::uint32_t) { started = true; };
    auto result = ProcessRunner::run(request);
    CHECK_FALSE(result.ok());
    CHECK_FALSE(started);
}

TEST_CASE("environment variables are passed through", "[process]") {
#if defined(_WIN32)
    auto request = shell_command("echo %LOCALCODE_TEST_VAR%");
#else
    auto request = shell_command("echo $LOCALCODE_TEST_VAR");
#endif
    request.environment.push_back({"LOCALCODE_TEST_VAR", "value-42"});

    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    CHECK(result->stdout_text.find("value-42") != std::string::npos);
}

TEST_CASE("arguments with spaces and quotes survive intact", "[process][quoting]") {
    // The whole reason commands are argv vectors rather than strings.
#if defined(_WIN32)
    auto shell = find_executable("cmd");
    REQUIRE(shell.has_value());
    ProcessRequest request;
    request.executable = *shell;
    request.arguments = {"/d", "/s", "/c", "echo", "a b c"};
#else
    auto shell = find_executable("echo");
    REQUIRE(shell.has_value());
    ProcessRequest request;
    request.executable = *shell;
    request.arguments = {"a b c"};
#endif
    request.timeout = std::chrono::seconds(20);

    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    CHECK(result->stdout_text.find("a b c") != std::string::npos);
}

#if defined(_WIN32)
TEST_CASE("cmd shell scripts preserve quoted paths and command chaining", "[process][quoting]") {
    TempDirectory temp{"lc-cmd-quoted"};
    temp.write("quoted name.txt", "quoted-path-sentinel-824619\n");
    auto request = shell_command("type \"quoted name.txt\" && echo pipeline-finished");
    request.working_directory = temp.path();
    REQUIRE(is_windows_cmd_script(request));
    const auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    CHECK(result->success());
    CHECK(result->stdout_text.find("quoted-path-sentinel-824619") != std::string::npos);
    CHECK(result->stdout_text.find("pipeline-finished") != std::string::npos);
    CHECK(result->stderr_text.empty());

    // Naming a project binary cmd.exe does not change its native argv ABI.
    request.executable = temp.path() / "cmd.exe";
    CHECK_FALSE(is_windows_cmd_script(request));
    CHECK(ProcessRunner::display_command(request).find("\\\"quoted name.txt\\\"") !=
          std::string::npos);
}
#endif

TEST_CASE("embedded NUL process arguments are refused before launch", "[process][quoting]") {
    auto request = shell_command("echo must-not-run");
    request.arguments.push_back(std::string("visible\0hidden", 14));
    const auto result = ProcessRunner::run(request);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("argument quoting follows the platform rules", "[process][quoting]") {
#if defined(_WIN32)
    CHECK(quote_argument("simple") == "simple");
    CHECK(quote_argument("has space") == "\"has space\"");
    CHECK(quote_argument("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(quote_argument(R"(C:\path\)") == R"(C:\path\)");
    CHECK(quote_argument(R"(C:\my path\)") == "\"C:\\my path\\\\\"");
#else
    CHECK(quote_argument("simple") == "simple");
    CHECK(quote_argument("has space") == "'has space'");
#endif
    CHECK_FALSE(quote_argument("").empty());
}

TEST_CASE("find_executable locates common tools", "[process]") {
#if defined(_WIN32)
    CHECK(find_executable("cmd").has_value());
#else
    CHECK(find_executable("sh").has_value());
#endif
    CHECK_FALSE(find_executable("definitely-not-a-real-program-xyz").has_value());
    CHECK_FALSE(find_executable("").has_value());
}

TEST_CASE("stdin is delivered to the child", "[process]") {
#if defined(_WIN32)
    auto request = shell_command("findstr .");
#else
    auto request = shell_command("cat");
#endif
    request.stdin_data = "piped-input\n";

    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    CHECK(result->stdout_text.find("piped-input") != std::string::npos);
}

// ---------------------------------------------------------------------------
// A model server is a long-lived child holding gigabytes of VRAM. If LocalCode
// exits without reaping it, the GPU stays occupied and the next load fails in a
// way that looks like a corrupt model file. On Windows the Job Object with
// KILL_ON_JOB_CLOSE is what prevents that; this asserts the mechanism rather
// than trusting it.
// ---------------------------------------------------------------------------
TEST_CASE("a child outlives neither its runner nor a cancellation", "[process][lifecycle]") {
#if defined(_WIN32)
    auto request = shell_command("ping -n 60 127.0.0.1 >nul");
#else
    auto request = shell_command("sleep 60");
#endif
    request.timeout = std::chrono::seconds(120);

    CancellationSource source;
    std::thread canceller([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        source.cancel();
    });

    const auto started = std::chrono::steady_clock::now();
    auto result = ProcessRunner::run(request, source.token());
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    canceller.join();

    REQUIRE(result.ok());
    CHECK(result->cancelled);
    // run() must not return until the child is actually gone: returning early
    // would leave the process alive with nothing tracking it.
    CHECK(elapsed.count() < 20);
}

TEST_CASE("a grandchild is reaped with the tree", "[process][lifecycle]") {
    // The shell spawns a second process that would outlive it if only the
    // immediate child were terminated.
#if defined(_WIN32)
    auto request = shell_command("start /b ping -n 60 127.0.0.1 >nul & ping -n 60 127.0.0.1 >nul");
#else
    auto request = shell_command("sleep 60 & sleep 60");
#endif
    request.timeout = std::chrono::milliseconds(1500);

    const auto started = std::chrono::steady_clock::now();
    auto result = ProcessRunner::run(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);

    REQUIRE(result.ok());
    CHECK(result->timed_out);
    CHECK(elapsed.count() < 20);
}

#if defined(_WIN32)
TEST_CASE("a successful root cannot leave descendants holding output pipes",
          "[process][lifecycle]") {
    auto request = shell_command("start /b ping -n 20 127.0.0.1 & exit /b 0");
    request.timeout = std::chrono::seconds(3);

    const auto started = std::chrono::steady_clock::now();
    auto result = ProcessRunner::run(request);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result.ok());
    CHECK(result->exit_code == 0);
    CHECK_FALSE(result->timed_out);
    CHECK(elapsed < std::chrono::seconds(3));
}
#endif

// ---------------------------------------------------------------------------
// Visual Studio ships CMake and Ninja inside its own installation and puts
// neither on PATH. A machine with a complete C++ toolchain therefore reported
// "cmake: not found", and an agent asked to build something told the operator to
// go and download CMake - the exact dead end the discovery order exists to
// prevent. Guard it: wherever Visual Studio's C++ tools are present, so is CMake.
// ---------------------------------------------------------------------------

TEST_CASE("Visual Studio's bundled build tools are discovered", "[environment]") {
#if defined(_WIN32)
    EnvironmentService service;
    const auto& report = service.report();

    bool has_cxx_toolset = false;
    for (const auto& install : report.visual_studio) {
        if (install.has_cxx_tools) has_cxx_toolset = true;
    }
    if (!has_cxx_toolset) {
        WARN("no Visual Studio C++ toolset on this machine; skipping");
        return;
    }

    // Not "is cmake on PATH" - "can this machine build a CMake project at all".
    // resolve_executable is what run_command and the find_executable tool use;
    // find_executable alone searches PATH and would answer no here.
    const auto cmake = resolve_executable("cmake");
    INFO("Visual Studio is installed with C++ tools, so CMake ships with it");
    CHECK(cmake.has_value());

    const auto ctest = resolve_executable("ctest");
    CHECK(ctest.has_value());

    // And the primitive really is the narrower one, which is why the tools must
    // not call it directly.
    if (cmake && !find_executable("cmake")) {
        SUCCEED("cmake was found only via the well-known locations, as intended");
    }
#else
    SUCCEED("Visual Studio discovery is Windows-only");
#endif
}

// ---------------------------------------------------------------------------
// "Not installed" must never be said about something that is installed.
//
// Watched live: a 3B model asked whether the MSVC toolchain existed, was told
// three times that msbuild, vcvars64.bat and Visual Studio were "not installed"
// - on a machine running Visual Studio 2022 with a full toolchain, whose paths
// LocalCode had already printed in the model's own system prompt - and
// reasonably concluded C++ was not viable. It wrote a Cargo.toml and switched
// the whole task to Rust.
//
// The model was not hallucinating. It was told something false.
// ---------------------------------------------------------------------------

TEST_CASE("Visual Studio's own tools resolve by name", "[environment][honesty]") {
#if defined(_WIN32)
    EnvironmentService service;
    const auto& report = service.report();

    bool has_visual_studio = false;
    for (const auto& install : report.visual_studio) {
        if (install.has_cxx_tools) has_visual_studio = true;
    }
    if (!has_visual_studio) {
        WARN("no Visual Studio C++ toolset on this machine; skipping");
        return;
    }

    // MSBuild ships with Visual Studio and is not on PATH.
    INFO("Visual Studio is installed, so MSBuild is too");
    CHECK(resolve_executable("msbuild").has_value());

    // A model is as likely to write the extension as not.
    CHECK(resolve_executable("msbuild.exe").has_value());

    // The developer-environment script, whose path the system prompt prints.
    CHECK(resolve_executable("vcvars64.bat").has_value());
    CHECK(resolve_executable("vcvars64").has_value());
#else
    SUCCEED("Visual Studio resolution is Windows-only");
#endif
}

TEST_CASE("a name LocalCode does not know is not reported as absent", "[environment][honesty]") {
    EnvironmentService service;
    // Nothing in the catalogue answers to this, and that must not be turned
    // into a claim that the machine lacks it.
    const auto info = service.detect_tool("unreal-engine");
    CHECK_FALSE(info.found);
    // The distinguishing signal the tool layer keys on: no display name means
    // "not in the catalogue", as opposed to "looked for and absent".
    CHECK(info.display_name.empty());
}

// ---------------------------------------------------------------------------
// The MSVC compiler needs its developer environment, and cannot be given one
// by a model.
//
// cl.exe needs INCLUDE, LIB and LIBPATH. They come from vcvars64.bat, which
// sets them in a shell and exits, so they cannot be carried across two separate
// commands. Watching a 3B model try - appending the build to vcvars64.bat four
// times, each returning zero and doing nothing - is what made this necessary.
// ---------------------------------------------------------------------------

TEST_CASE("the MSVC environment is captured and applied", "[environment][msvc]") {
#if defined(_WIN32)
    if (!resolve_executable("cl")) {
        WARN("no MSVC compiler on this machine; skipping");
        return;
    }

    // Only the compiler and its immediate friends need it. cmake and ninja
    // deliberately do not: cmake with the Visual Studio generator finds the
    // toolchain itself, and forcing variables underneath it would change which
    // compiler a configured project uses.
    CHECK(needs_msvc_environment("cl.exe"));
    CHECK(needs_msvc_environment("C:/some/path/link.exe"));
    CHECK(needs_msvc_environment("rc.exe"));
    CHECK_FALSE(needs_msvc_environment("cmake.exe"));
    CHECK_FALSE(needs_msvc_environment("ninja.exe"));
    CHECK_FALSE(needs_msvc_environment("git.exe"));

    const auto& developer = msvc_environment();
    INFO("captured " << developer.size() << " variables");
    REQUIRE_FALSE(developer.empty());

    // The three that decide whether the compiler can find anything.
    auto has = [&developer](const char* name) {
        for (const auto& [key, value] : developer) {
            if (strings::iequals(key, name) && !value.empty()) return true;
        }
        return false;
    };
    CHECK(has("INCLUDE"));
    CHECK(has("LIB"));
    CHECK(has("PATH"));
#else
    SUCCEED("the MSVC environment is Windows-only");
#endif
}

TEST_CASE("cl compiles with no developer environment in the parent", "[environment][msvc][slow]") {
#if defined(_WIN32)
    auto compiler = resolve_executable("cl");
    if (!compiler || msvc_environment().empty()) {
        WARN("no usable MSVC toolchain on this machine; skipping");
        return;
    }

    TempDirectory temp{"lc-cl"};
    temp.write("hello.cpp",
               "#include <iostream>\n"
               "int main() { std::cout << \"ok\" << std::endl; return 0; }\n");

    ProcessRequest request;
    request.executable = *compiler;
    request.arguments = {"/EHsc", "/nologo", "hello.cpp"};
    request.working_directory = temp.path();
    request.timeout = std::chrono::seconds(120);

    // Exactly what the tool does: supply the captured variables and nothing else.
    for (const auto& [name, value] : msvc_environment()) {
        request.environment.push_back({name, value});
    }

    auto result = ProcessRunner::run(request);
    REQUIRE(result.ok());
    INFO(result->stdout_text << result->stderr_text);
    CHECK(result->exit_code == 0);
    CHECK(std::filesystem::exists(temp.path() / "hello.exe"));
#else
    SUCCEED("MSVC is Windows-only");
#endif
}

TEST_CASE("a Ninja tree that chose cl.exe gets the environment cl.exe needs",
          "[environment][msvc]") {
    // Run 10 failed on this. `cmake --build build` started cl.exe with no
    // INCLUDE, every standard header came back as "no include path set", and
    // the model - reading the message literally - spent eight steps adding
    // <vector> to files that already included it.
    //
    // The decision is made from the tree's own cache, never guessed: cl.exe is
    // already the chosen compiler, written down at configure time, so
    // supplying its variables cannot change which compiler is used.
#if defined(_WIN32)
    TempDirectory temp{"lc-cache"};

    CHECK_FALSE(build_tree_drives_msvc_directly(temp.path()));   // no cache at all

    temp.write("ninja-msvc/CMakeCache.txt",
               "CMAKE_GENERATOR:INTERNAL=Ninja\n"
               "CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe\n");
    CHECK(build_tree_drives_msvc_directly(temp.path() / "ninja-msvc"));

    // A Visual Studio generator starts msbuild, which arranges its own
    // environment. Supplying variables underneath it is the thing that must
    // not happen.
    temp.write("vs/CMakeCache.txt",
               "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022\n"
               "CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe\n");
    CHECK_FALSE(build_tree_drives_msvc_directly(temp.path() / "vs"));

    // Ninja with a different toolchain is not MSVC's business.
    temp.write("ninja-clang/CMakeCache.txt",
               "CMAKE_GENERATOR:INTERNAL=Ninja\n"
               "CMAKE_CXX_COMPILER:FILEPATH=C:/msys64/mingw64/bin/clang++.exe\n");
    CHECK_FALSE(build_tree_drives_msvc_directly(temp.path() / "ninja-clang"));

    temp.write("makefiles/CMakeCache.txt",
               "CMAKE_GENERATOR:INTERNAL=NMake Makefiles\n"
               "CMAKE_C_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe\n");
    CHECK(build_tree_drives_msvc_directly(temp.path() / "makefiles"));
#else
    SUCCEED("the MSVC environment is Windows-only");
#endif
}

TEST_CASE("a Ninja configure that cannot find its toolchain is explained",
          "[tools][process][msvc]") {
    // Measured live: a model asked to write C++ found ninja and cl.exe with
    // find_executable at step 3, then ran `cmake -S . -B build -G Ninja` at
    // step 7 and got two errors that name no reason - "unable to find a build
    // program corresponding to Ninja" and "CMAKE_CXX_COMPILER not set". Both
    // programs were installed; neither was on PATH. LocalCode already knew
    // where they were.
#if defined(_WIN32)
    if (!resolve_executable("cmake")) {
        WARN("no cmake on this machine; skipping");
        return;
    }
    TempDirectory temp{"lc-configure"};
    temp.write("CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.16)\n"
               "project(probe LANGUAGES CXX)\n"
               "add_executable(probe main.cpp)\n");
    temp.write("main.cpp", "int main() { return 0; }\n");

    Workspace workspace;
    REQUIRE(workspace.open(temp.path()).ok());
    ToolRegistry registry;
    registry.register_builtin_tools();
    EnvironmentService environment;
    ToolContext context;
    context.workspace = &workspace;
    context.environment = &environment;
    context.confirm = [](const ConfirmationRequest&) { return true; };
    context.log_directory = temp.path() / "logs";

    ToolInvocation invocation;
    invocation.tool_name = "run_command";
    invocation.call_id = "c1";
    invocation.arguments =
        Json{{"command", Json::array({"cmake", "-S", ".", "-B", "build", "-G", "Ninja"})}};
    const auto result = registry.invoke(invocation, context);

    // On a machine that does have ninja on PATH this configure is legitimate
    // and must not be refused. The test is about the other machine - the one
    // LocalCode actually runs on here.
    if (find_executable("ninja")) {
        WARN("ninja is on PATH on this machine; the configure is legitimate");
        return;
    }

    // Refused before starting, rather than explained afterwards. The
    // explanation used to come after the failure, which was too late: the
    // failed configure had already written a cache, and that cache made the
    // next configure fail with a different message about a different thing.
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("generator unavailable") != std::string::npos);
    CHECK(result.error.find("Drop the -G") != std::string::npos);
    CHECK(result.error.find("still writes a cache") != std::string::npos);
    // The path it already knows, rather than advice to go and find it.
    if (resolve_executable("ninja")) {
        CHECK(result.error.find("does exist at") != std::string::npos);
    }
    // Nothing was written, so nothing was poisoned.
    CHECK_FALSE(std::filesystem::exists(temp.path() / "build" / "CMakeCache.txt"));
#else
    SUCCEED("the Visual Studio toolchain layout is Windows-only");
#endif
}

TEST_CASE("a build reports the executable it produced", "[tools][process][build]") {
    // Two runs died here. The build succeeded, and the model then had to write
    // an absolute Windows path into a JSON string to run the result - which
    // lost its backslashes to escapes and produced "executable not found" for a
    // program sitting exactly where it had been left. The build tool already
    // printed the path; it just was not passed on.
    TempDirectory temp{"lc-build-output"};
    const auto cwd = temp.path();

    const auto msbuild = executables_produced(
        "  fibonacci.vcxproj -> " + (cwd / "build/RelWithDebInfo/fibonacci.exe").string() + "\n",
        cwd);
    REQUIRE(msbuild.size() == 1);
    CHECK(msbuild[0] == "build/RelWithDebInfo/fibonacci.exe");

    const auto ninja = executables_produced("[2/2] Linking CXX executable build/app.exe\n", cwd);
    REQUIRE(ninja.size() == 1);
    CHECK(ninja[0] == "build/app.exe");

    const auto nested_ninja =
        resources_produced("[2/2] Linking CXX executable checkout.exe\n", cwd, cwd / "build");
    REQUIRE(nested_ninja.size() == 1);
    CHECK(nested_ninja[0].path == "build/checkout.exe");
    CHECK(nested_ninja[0].id == "build:checkout");
    const auto spaced = executables_produced("[2/2] Linking CXX executable output folder/app.exe\n",
                                             cwd, cwd / "build");
    REQUIRE(spaced.size() == 1);
    CHECK(spaced[0] == "build/output folder/app.exe");
    CHECK(resources_produced("[2/2] Linking CXX shared library pricing.dll\n", cwd).empty());

    // Nothing to report when nothing was linked.
    CHECK(executables_produced("[1/1] Building CXX object main.cpp.obj\n", cwd).empty());

    // The same binary named twice is one binary.
    const auto line = "  a.vcxproj -> " + (cwd / "build/a.exe").string() + "\n";
    const auto repeated = executables_produced(line + line, cwd);
    CHECK(repeated.size() == 1);
}

TEST_CASE("a path corrupted by an invalid JSON escape is recognised too", "[tools][process]") {
    // The second form of the same mistake. C:\Users\... has no escape that maps
    // to a control character - \U is simply invalid - so the JSON comes apart
    // and what arrives is wreckage like `C:", `. No Windows filename can hold a
    // double quote, so this is not a judgement call.
    CHECK_FALSE(escape_damage(R"(C:", )").empty());
    CHECK(escape_damage(R"(C:", )").find("double quote") != std::string::npos);
    CHECK_FALSE(escape_damage("build/a|b.exe").empty());

    // And the legitimate cases stay quiet.
    CHECK(escape_damage("build/RelWithDebInfo/fibonacci.exe").empty());
    CHECK(escape_damage(R"(C:\Program Files\CMake\bin\cmake.exe)").empty());
    CHECK(escape_damage("cmake").empty());
}

TEST_CASE("a program in the working directory is found by name", "[environment][process]") {
    // The last thing standing between a run and a finished task. The model had
    // compiled with `cl fibonacci.cpp`, which puts fibonacci.exe in the
    // directory it ran in; it then asked to run `fibonacci.exe`; find_executable
    // searches PATH, the working directory is not on PATH, and LocalCode said
    // "executable not found" about a file sitting right there. The model had
    // just written "let me now run the verification with expect" - it was one
    // step from settling every criterion.
    TempDirectory temp{"lc-cwd-exe"};
#if defined(_WIN32)
    temp.write("localcode-probe.bat", "@echo off\r\necho probe\r\n");
    // By bare name, the way the model asked for it.
    const auto found = resolve_executable("localcode-probe", temp.path());
    REQUIRE(found.has_value());
    CHECK(found->filename().string() == "localcode-probe.bat");

    // And with the extension spelled out.
    CHECK(resolve_executable("localcode-probe.bat", temp.path()).has_value());
#else
    temp.write("localcode-probe", "#!/bin/sh\necho probe\n");
    std::filesystem::permissions(temp.path() / "localcode-probe",
                                 std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    CHECK(resolve_executable("localcode-probe", temp.path()).has_value());
#endif

    // A name that is nowhere is still nowhere.
    CHECK_FALSE(resolve_executable("localcode-not-a-real-program-xyz", temp.path()));

    // And PATH still wins: cmake is installed, and must not be shadowed by
    // anything the workspace happens to contain.
    if (const auto cmake = resolve_executable("cmake", temp.path())) {
        CHECK(cmake->parent_path() != temp.path());
    }
}
