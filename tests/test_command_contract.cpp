// ---------------------------------------------------------------------------
// The command contract.
//
// Every case here is a shape that a real run actually sent, and every one of
// them cost a benchmark run before it was handled. They are not variations on
// a theme for the sake of coverage: each maps to a distinct way that "run the
// program you just built" failed on Windows.
//
//   relative path            the ordinary case, and the one to keep working
//   backslash path           what a model copies out of an MSBuild log
//   absolute path            what it copies out of the same log, in full
//   spaces in the path       "Program Files" is not two arguments
//   Unicode in the path      a workspace under a name that is not ASCII
//   bare name in cwd         a program beside the sources, not on PATH
//   name on PATH             cmake, git, the compiler
//   built artifact by id     the path nobody should have to retype
//   command vs shell_command two representations, one operation
//   path split across argv   ["build","Debug","app.exe"]
//   /workspace prefix        the container root a model learned in training
//   control character        \f from a JSON escape, invisible in a log
//   generator mismatch       a tree remembers what configured it
//   stale cache              a FAILED configure still writes one
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "platform/Environment.h"
#include "platform/Process.h"
#include "support/TempDirectory.h"
#include "tools/BuildDirectory.h"
#include "tools/CommandRequest.h"
#include "tools/OutputExpectation.h"
#include "tools/ToolRegistry.h"
#include "workspace/Workspace.h"

using namespace localcode;
using localcode::test::TempDirectory;

namespace {

// A real program, small and quick, so "did this actually start" is a question
// the test can answer rather than assume. hostname.exe prints one line and
// exits 0.
const std::filesystem::path& sample_program() {
    static const std::filesystem::path path = R"(C:\Windows\System32\HOSTNAME.EXE)";
    return path;
}

struct CommandFixture {
    TempDirectory      temp{"lc-cmd"};
    Workspace          workspace;
    ToolRegistry       registry;
    EnvironmentService environment;
    ToolContext        context;

    CommandFixture() {
        REQUIRE(workspace.open(temp.path()).ok());
        registry.register_builtin_tools();
        context.workspace = &workspace;
        context.environment = &environment;
        context.confirm = [](const ConfirmationRequest&) { return true; };
    }

    // Copies the sample program to a path inside the workspace and returns the
    // path as written, workspace-relative with forward slashes.
    std::string place_program(const std::string& relative) {
        const auto destination = temp.path() / relative;
        std::filesystem::create_directories(destination.parent_path());
        std::error_code ec;
        std::filesystem::copy_file(sample_program(), destination,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
        return relative;
    }

    ToolResult call(const std::string& name, Json arguments) {
        ToolInvocation invocation;
        invocation.tool_name = name;
        invocation.arguments = std::move(arguments);
        invocation.call_id = "test";
        return registry.invoke(invocation, context);
    }

    // True when the program was found and ran to completion successfully.
    static bool ran(const ToolResult& result) {
        return result.ok && result.content.find("exit:     0") != std::string::npos;
    }

    // True when the program was resolved and started at all, whatever it then
    // did. The sample program exits non-zero when given arguments, so tests
    // about argument handling ask this rather than for success.
    static bool started(const ToolResult& result) {
        return result.ok && result.content.find("exit:     ") != std::string::npos;
    }
};

}  // namespace

// --- The shapes that must work ---------------------------------------------

TEST_CASE("a workspace-relative executable runs", "[command][contract]") {
    CommandFixture fixture;
    const auto program = fixture.place_program("build/Debug/app.exe");

    auto result = fixture.call("run_command",
                               Json{{"executable", program}, {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

TEST_CASE("a backslash path names the same program", "[command][contract]") {
    // What a model copies out of an MSBuild log. Windows accepts it, so
    // LocalCode must too - the danger is the JSON escaping on the way in, not
    // the separator itself.
    CommandFixture fixture;
    fixture.place_program("build/Debug/app.exe");

    auto result = fixture.call(
        "run_command", Json{{"executable", R"(build\Debug\app.exe)"}, {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

TEST_CASE("an absolute path names the same program", "[command][contract]") {
    CommandFixture fixture;
    fixture.place_program("build/Debug/app.exe");
    const auto absolute = (fixture.temp.path() / "build" / "Debug" / "app.exe").string();

    auto result = fixture.call("run_command",
                               Json{{"executable", absolute}, {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

TEST_CASE("spaces in a path are not argument separators", "[command][contract]") {
    // The reason argv exists. A path with a space in it is one argument, and
    // nothing along the way may split it - which is exactly what a shell line
    // would do without quoting.
    CommandFixture fixture;
    const auto program = fixture.place_program("my build/Debug out/app.exe");

    auto result = fixture.call("run_command",
                               Json{{"executable", program}, {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

TEST_CASE("a non-ASCII path names the same program", "[command][contract]") {
    // Workspaces live under user names, and user names are not all ASCII. The
    // path travels as UTF-8 through JSON and has to arrive as UTF-16 at
    // CreateProcessW.
    CommandFixture fixture;
    const auto program = fixture.place_program("bygg-\xC3\xA5\xC3\xA4\xC3\xB6/app.exe");

    auto result = fixture.call("run_command",
                               Json{{"executable", program}, {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

TEST_CASE("a bare name is found beside the sources, not only on PATH",
          "[command][contract]") {
    // Measured: a model built app.exe into the workspace root, ran "app.exe",
    // and was told it did not exist because only PATH had been searched. The
    // program was in the working directory the command was about to run in.
    CommandFixture fixture;
    fixture.place_program("app.exe");

    auto result = fixture.call("run_command",
                               Json{{"executable", "app.exe"}, {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

TEST_CASE("a name on PATH still resolves", "[command][contract]") {
    CommandFixture fixture;
    auto result = fixture.call("run_command",
                               Json{{"executable", "hostname"}, {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

// --- The shapes that must be refused ---------------------------------------

TEST_CASE("command and shell_command describing different things is refused",
          "[command][contract][conflict]") {
    // The defect this whole contract exists for. Sent by a real run:
    //
    //   "command":       ["build", "RelWithDebInfo", "fibonacci.exe"]
    //   "shell_command": "cd /workspace && build/RelWithDebInfo/fibonacci.exe"
    //
    // LocalCode ran the first, discarded the second without a word, and the
    // run died looking for a program called "build".
    CommandFixture fixture;

    auto result = fixture.call(
        "run_command",
        Json{{"command", Json::array({"build", "RelWithDebInfo", "fibonacci.exe"})},
             {"shell_command", "cd /workspace && build/RelWithDebInfo/fibonacci.exe"}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("conflicting command") != std::string::npos);
    // Both readings are shown, so the caller can see which one it meant.
    CHECK(result.error.find("build RelWithDebInfo fibonacci.exe") != std::string::npos);
    CHECK(result.error.find("cd /workspace &&") != std::string::npos);
    // And neither was run.
    CHECK(result.error.find("exit:") == std::string::npos);
}

TEST_CASE("executable and shell_command describing different things is refused",
          "[command][contract][conflict]") {
    CommandFixture fixture;
    auto result = fixture.call("run_command", Json{{"executable", "cmake"},
                                                   {"arguments", Json::array({"--version"})},
                                                   {"shell_command", "ctest --output-on-failure"}});
    REQUIRE_FALSE(result.ok);
    CHECK(result.summary.find("conflicting command") != std::string::npos);
}

TEST_CASE("two forms saying the same thing run once and say so",
          "[command][contract][conflict]") {
    // Duplication is not a conflict. It is run, and the redundancy is pointed
    // out rather than silently dropped - the whole failure being fixed here is
    // a parameter that vanished without comment.
    CommandFixture fixture;

    auto result = fixture.call("run_command",
                               Json{{"command", Json::array({"hostname"})},
                                    {"shell_command", "hostname"}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
    CHECK(result.content.find("both described this same command") != std::string::npos);
}

TEST_CASE("a path split across argv elements is reported, not rejoined",
          "[command][contract][conflict]") {
    // LocalCode can see what was meant. It says so and does not act on it:
    // deciding that three arguments were one path is a guess, and executing a
    // guess is worse than refusing.
    CommandFixture fixture;
    fixture.place_program("build/Debug/app.exe");

    auto result = fixture.call(
        "run_command", Json{{"command", Json::array({"build", "Debug", "app.exe"})}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("invalid command shape") != std::string::npos);
    CHECK(result.error.find("build/Debug/app.exe") != std::string::npos);
    // Nothing ran.
    CHECK(result.error.find("exit:") == std::string::npos);
}

TEST_CASE("a control character in a path is named, not hidden",
          "[command][contract][encoding]") {
    // "build\\RelWithDebInfo\\fib.exe" in a JSON string arrives as build +
    // RelWithDebInfo + form feed + ib.exe. In a log the form feed is invisible
    // and the path merely looks wrong for no reason.
    CommandFixture fixture;

    auto result = fixture.call("run_command",
                               Json{{"executable", std::string("build\fib.exe")},
                                    {"arguments", Json::array()}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("damaged") != std::string::npos);
    // Shown as an escape rather than emitted raw.
    CHECK(result.summary.find("\\f") != std::string::npos);
    CHECK(result.error.find("cannot appear in a filename") != std::string::npos);
}

TEST_CASE("the virtual /workspace prefix reaches the real workspace",
          "[command][contract][workspace]") {
    CommandFixture fixture;
    fixture.place_program("app.exe");

    auto result = fixture.call("run_command",
                               Json{{"executable", "/workspace/app.exe"},
                                    {"arguments", Json::array()}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
}

// --- Running what was built ------------------------------------------------

TEST_CASE("a built artifact runs by name with no path at all",
          "[command][contract][artifact]") {
    CommandFixture fixture;
    const auto path = fixture.place_program("build/RelWithDebInfo/fibonacci.exe");

    ResolvedResource resource;
    resource.id = "build:fibonacci";
    resource.kind = "executable";
    resource.name = "fibonacci";
    resource.path = path;
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    auto result = fixture.call("run_artifact", Json{{"artifact", "build:fibonacci"}});
    INFO(result.content);
    CHECK(CommandFixture::ran(result));
    CHECK(result.content.find("build:fibonacci -> build/RelWithDebInfo/fibonacci.exe") !=
          std::string::npos);

    // The bare name works too, because that is what a model will reach for.
    auto by_name = fixture.call("run_artifact", Json{{"artifact", "fibonacci"}});
    CHECK(CommandFixture::ran(by_name));
}

TEST_CASE("an unknown artifact lists what there is", "[command][contract][artifact]") {
    CommandFixture fixture;

    auto nothing = fixture.call("run_artifact", Json{{"artifact", "fibonacci"}});
    REQUIRE_FALSE(nothing.ok);
    CHECK(nothing.error.find("No build in this workspace has produced a program yet") !=
          std::string::npos);

    ResolvedResource resource;
    resource.id = "build:demo";
    resource.name = "demo";
    resource.path = "build/demo.exe";
    resource.source = "test";
    fixture.workspace.record_resolved_resources({resource});

    auto wrong = fixture.call("run_artifact", Json{{"artifact", "fibonacci"}});
    REQUIRE_FALSE(wrong.ok);
    CHECK(wrong.error.find("build:demo") != std::string::npos);
}

TEST_CASE("an artifact that has been deleted says so rather than failing obscurely",
          "[command][contract][artifact]") {
    CommandFixture fixture;
    ResolvedResource resource;
    resource.id = "build:gone";
    resource.name = "gone";
    resource.path = "build/gone.exe";
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    auto result = fixture.call("run_artifact", Json{{"artifact", "gone"}});
    REQUIRE_FALSE(result.ok);
    CHECK(result.summary.find("no longer exists") != std::string::npos);
    CHECK(result.error.find("build again") != std::string::npos);
}

// --- CMake build-directory state -------------------------------------------

TEST_CASE("a build tree reports the generator it holds", "[command][cmake][state]") {
    TempDirectory temp{"lc-tree"};
    temp.write("build/CMakeCache.txt",
               "# This is the CMakeCache file.\n"
               "CMAKE_GENERATOR:INTERNAL=Ninja\n"
               "CMAKE_HOME_DIRECTORY:INTERNAL=C:/src/demo\n");
    std::filesystem::create_directories(temp.path() / "build" / "CMakeFiles");

    const auto state = inspect_build_directory(temp.path() / "build");
    CHECK(state.exists);
    CHECK(state.configured);
    CHECK(state.generator == "Ninja");
    CHECK(state.source_dir == "C:/src/demo");
    // CMakeCache.txt and CMakeFiles/ are both CMake's own output.
    CHECK(state.generated_only);
    CHECK(state.unrecognised.empty());
}

TEST_CASE("a build tree holding anything else is not called disposable",
          "[command][cmake][state]") {
    // The safety rule: a directory called "build" is not automatically CMake's
    // to remove. Something a person put there means the tree is theirs.
    TempDirectory temp{"lc-tree"};
    temp.write("build/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n");
    temp.write("build/notes.md", "keep me\n");

    const auto state = inspect_build_directory(temp.path() / "build");
    CHECK(state.configured);
    CHECK_FALSE(state.generated_only);
    REQUIRE(state.unrecognised.size() == 1);
    CHECK(state.unrecognised.front() == "notes.md");
}

TEST_CASE("an unconfigured directory is never generated output", "[command][cmake][state]") {
    TempDirectory temp{"lc-tree"};
    std::filesystem::create_directories(temp.path() / "build");

    const auto state = inspect_build_directory(temp.path() / "build");
    CHECK(state.exists);
    CHECK_FALSE(state.configured);
    CHECK_FALSE(state.generated_only);
}

TEST_CASE("the requested generator is read off the command line",
          "[command][cmake][state]") {
    CHECK(requested_generator({"cmake", "-S", ".", "-B", "build", "-G", "Ninja"}) == "Ninja");
    CHECK(requested_generator({"cmake", "-GNinja"}) == "Ninja");
    CHECK(requested_generator({"cmake", "-G", "Visual Studio 17 2022", "-A", "x64"}) ==
          "Visual Studio 17 2022");
    CHECK(requested_generator({"cmake", "--build", "build"}).empty());

    CHECK(same_generator("Visual Studio 17 2022", "visual  studio 17 2022"));
    CHECK_FALSE(same_generator("Ninja", "Visual Studio 17 2022"));
}

TEST_CASE("a configure into a tree with another generator is refused with the reason",
          "[command][cmake][state]") {
    // The run-12 loop: Ninja fails because ninja is not on PATH, the failed
    // configure still writes a cache, Visual Studio then fails with "does not
    // match the generator used previously", and neither message mentions the
    // other. Eight alternating attempts before the run was stopped.
    CommandFixture fixture;
    fixture.temp.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\n");
    fixture.temp.write("build/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n");
    std::filesystem::create_directories(fixture.temp.path() / "build" / "CMakeFiles");

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"-S", ".", "-B", "build", "-G", "Visual Studio 17 2022"})}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);

    CHECK(result.summary.find("generator conflict") != std::string::npos);
    // Both generators named, and the cache identified as what decides.
    CHECK(result.error.find("configured with Ninja") != std::string::npos);
    CHECK(result.error.find("Visual Studio 17 2022") != std::string::npos);
    // Two safe recoveries, neither of which is "LocalCode deleted it for you".
    CHECK(result.error.find("delete_path") != std::string::npos);
    CHECK(result.error.find("-B build-visual") != std::string::npos);
    // CMake was never started, so the raw mismatch never reached the caller.
    CHECK(result.error.find("exit:") == std::string::npos);
}

TEST_CASE("a tree with unknown files is offered a new directory, not deletion",
          "[command][cmake][state]") {
    CommandFixture fixture;
    fixture.temp.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\n");
    fixture.temp.write("build/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n");
    fixture.temp.write("build/handwritten.txt", "mine\n");

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"-S", ".", "-B", "build", "-G", "Visual Studio 17 2022"})}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("handwritten.txt") != std::string::npos);
    CHECK(result.error.find("must not simply be deleted") != std::string::npos);
    CHECK(result.error.find("delete_path") == std::string::npos);
}

TEST_CASE("the same generator is not a conflict", "[command][cmake][state]") {
    CommandFixture fixture;
    fixture.temp.write("build/CMakeCache.txt", "CMAKE_GENERATOR:INTERNAL=Ninja\n");

    // Reconfiguring with the generator already recorded is ordinary and must
    // not be blocked. It fails for its own reasons here (no CMakeLists.txt),
    // which is the point: the refusal above is specific.
    auto result = fixture.call(
        "run_command", Json{{"executable", "cmake"},
                            {"arguments", Json::array({"-S", ".", "-B", "build", "-G", "Ninja"})}});
    CHECK(result.summary.find("generator conflict") == std::string::npos);
}

// --- The parser on its own -------------------------------------------------

TEST_CASE("shell semantics are recognised only when present", "[command][contract][shell]") {
    CHECK(needs_shell_semantics("a && b"));
    CHECK(needs_shell_semantics("a | b"));
    CHECK(needs_shell_semantics("a > out.txt"));
    CHECK(needs_shell_semantics("echo $HOME"));
    CHECK(needs_shell_semantics("echo %PATH%"));

    CHECK_FALSE(needs_shell_semantics("cmake --build build"));
    CHECK_FALSE(needs_shell_semantics("build/Debug/app.exe --flag"));
    // Inside quotes these are data, not operators.
    CHECK_FALSE(needs_shell_semantics(R"(echo "a && b")"));
}

TEST_CASE("a plain shell line is run directly and says why",
          "[command][contract][shell]") {
    const auto parsed = parse_command_request(
        Json{{"shell_command", "cmake --build build"}});
    REQUIRE(parsed.valid);
    CHECK_FALSE(parsed.use_shell);
    CHECK(parsed.executable == "cmake");
    CHECK(parsed.arguments == std::vector<std::string>{"--build", "build"});
    CHECK(parsed.note.find("needs no shell") != std::string::npos);
}

TEST_CASE("a line that needs a shell keeps one", "[command][contract][shell]") {
    const auto parsed = parse_command_request(
        Json{{"shell_command", "vcvars64.bat && cmake --build build"}});
    REQUIRE(parsed.valid);
    CHECK(parsed.use_shell);
    CHECK(parsed.form == CommandForm::Shell);
}

TEST_CASE("separators do not make two forms disagree", "[command][contract][conflict]") {
    // build\Debug\app.exe and build/Debug/app.exe are the same program. A
    // caller that writes one in the array and the other in the shell line has
    // not contradicted itself, and refusing that would be pedantry.
    const auto parsed = parse_command_request(
        Json{{"command", Json::array({R"(build\Debug\app.exe)"})},
             {"shell_command", "build/Debug/app.exe"}});
    CHECK(parsed.valid);
}

TEST_CASE("an empty request is refused with the shape to use",
          "[command][contract]") {
    const auto parsed = parse_command_request(Json::object());
    CHECK_FALSE(parsed.valid);
    CHECK(parsed.error_summary == "no command given");
    CHECK(parsed.error_detail.find("\"executable\"") != std::string::npos);
}

TEST_CASE("executable naming the program and command carrying the argv agree",
          "[command][contract][conflict]") {
    // Not a contradiction: the same command written twice, with the arguments
    // in the second copy. Reading it as a conflict deadlocked a whole run -
    // the model sent this exact shape four times and had nowhere to go.
    const auto parsed = parse_command_request(
        Json{{"executable", "cmake"},
             {"command", Json::array({"cmake", "-S", ".", "-B", "build", "-G", "Ninja"})},
             {"working_directory", "/workspace"}});
    REQUIRE(parsed.valid);
    CHECK(parsed.executable == "cmake");
    CHECK(parsed.arguments ==
          std::vector<std::string>{"-S", ".", "-B", "build", "-G", "Ninja"});
    CHECK(parsed.note.find("both named this command") != std::string::npos);

    // A genuinely different program in `command` is still a conflict.
    const auto conflict = parse_command_request(
        Json{{"executable", "cmake"}, {"command", Json::array({"ctest", "--verbose"})}});
    CHECK_FALSE(conflict.valid);
    CHECK(conflict.error_summary.find("conflicting command") != std::string::npos);

    // And explicit arguments still have to match.
    const auto explicit_mismatch = parse_command_request(
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"--version"})},
             {"command", Json::array({"cmake", "--build", "build"})}});
    CHECK_FALSE(explicit_mismatch.valid);
}

TEST_CASE("the virtual prefix resolves in arguments, not only in the program",
          "[command][contract][workspace]") {
    // Rewriting the executable but not the arguments is worse than not
    // rewriting at all: the same prefix then works in one position and fails
    // in another for no visible reason. Measured - `cmake --build
    // /workspace/build` failed twice before the model wrote out the absolute
    // path by hand.
    CommandFixture fixture;
    fixture.place_program("app.exe");
    std::filesystem::create_directories(fixture.temp.path() / "build");

    auto result = fixture.call("run_command",
                               Json{{"executable", "app.exe"},
                                    {"arguments", Json::array({"/workspace/build"})}});
    INFO(result.content);
    REQUIRE(CommandFixture::started(result));

    // The argument reached the program as a real path...
    const auto root = fixture.temp.path().generic_string();
    CHECK(result.content.find(root + "/build") != std::string::npos);
    // ...and the substitution was reported rather than made quietly.
    CHECK(result.content.find("/workspace/build") != std::string::npos);
    CHECK(result.content.find("resolved to this workspace") != std::string::npos);
}

TEST_CASE("paths that are not the virtual prefix are passed through untouched",
          "[command][contract][workspace]") {
    CommandFixture fixture;
    fixture.place_program("app.exe");

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "app.exe"},
             {"arguments", Json::array({"--out", "build/report.txt", "/etc/passwd", "-DX=1"})}});
    INFO(result.content);
    REQUIRE(CommandFixture::started(result));
    CHECK(result.content.find("resolved to this workspace") == std::string::npos);
    CHECK(result.content.find("/etc/passwd") != std::string::npos);
    CHECK(result.content.find("-DX=1") != std::string::npos);
}

TEST_CASE("a generator whose build program is missing is refused before it runs",
          "[command][cmake][state]") {
    // The root of three failed runs. -G Ninja cannot work here because ninja
    // is not on PATH, and the failure is not the harmless part: the failed
    // configure still writes a cache, so the next configure with a working
    // generator fails with an unrelated-looking mismatch. One run spent eight
    // steps alternating between those two errors.
    //
    // Whether ninja is on PATH is a fact LocalCode holds before starting the
    // command. Nothing is written, so nothing is poisoned.
    CommandFixture fixture;
    fixture.temp.write("CMakeLists.txt",
                       "cmake_minimum_required(VERSION 3.16)\nproject(app CXX)\n");

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"-S", ".", "-B", "build", "-G", "Ninja"})}});

    // Skipped rather than asserted backwards on a machine that does have ninja
    // on PATH - there the command is legitimate and must not be refused.
    if (find_executable("ninja")) {
        CHECK(result.summary.find("generator unavailable") == std::string::npos);
        return;
    }

    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("generator unavailable") != std::string::npos);
    CHECK(result.error.find("Drop the -G") != std::string::npos);
    // The reason the refusal exists is stated, not just the refusal.
    CHECK(result.error.find("still writes a cache") != std::string::npos);

    // And the tree really was left alone.
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "build" / "CMakeCache.txt"));
}

TEST_CASE("a generator that needs nothing external is not refused",
          "[command][cmake][state]") {
    // The check must be specific. Visual Studio finds its own toolchain, so a
    // configure asking for it is ordinary.
    CHECK(generator_requires_program("Visual Studio 17 2022").empty());
    CHECK(generator_requires_program("Xcode").empty());
    CHECK(generator_requires_program("Ninja") == "ninja");
    CHECK(generator_requires_program("Ninja Multi-Config") == "ninja");
    CHECK(generator_requires_program("Unix Makefiles") == "make");
    CHECK(generator_requires_program("NMake Makefiles") == "nmake");
    CHECK(generator_requires_program("MinGW Makefiles") == "mingw32-make");
    CHECK(generator_requires_program("").empty());
}

TEST_CASE("a conflict is answered with calls to copy, not advice",
          "[command][contract][conflict]") {
    // A refusal a small model can act on is one it can copy. Measured: given
    // "send one of them, not both", a model resent the identical call - the
    // only concrete text in front of it was the call it had just made.
    //
    // The shape it sent, twice, with "cmake" left over from the previous turn:
    const auto parsed = parse_command_request(
        Json{{"command", Json::array({"/workspace/build/RelWithDebInfo/fibonacci.exe"})},
             {"executable", "cmake"},
             {"working_directory", "/workspace"}});
    REQUIRE_FALSE(parsed.valid);
    INFO(parsed.error_detail);

    // Both readings are offered as calls that can be sent as they stand.
    CHECK(parsed.error_detail.find(
              R"({"executable": "/workspace/build/RelWithDebInfo/fibonacci.exe", "arguments": []})") !=
          std::string::npos);
    CHECK(parsed.error_detail.find(R"({"executable": "cmake", "arguments": []})") !=
          std::string::npos);
    // And the usual cause is named, because that is what makes it fixable.
    CHECK(parsed.error_detail.find("leftover 'executable' from an earlier call") !=
          std::string::npos);
}

TEST_CASE("a passing check that settles nothing says how to settle something",
          "[command][contract][verification]") {
    // A run reached "behaviour verified", saw its criteria stay open, and
    // re-ran the identical check three times. The mechanism - criteria are
    // settled by number - was never in front of it at the moment it mattered.
    //
    // This tells it. It does not settle anything itself: matching a check to a
    // criterion by how similar the sentences read is the plausible inference
    // the whole subsystem exists to remove.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {2, "Program builds successfully with CMake", false},
            {3, "Program runs and outputs the correct Fibonacci sequence", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "prints something"},
                             {"exit_code", 0}}}});
    REQUIRE(CommandFixture::ran(result));
    INFO(result.content);

    CHECK(result.content.find("settled no acceptance criterion") != std::string::npos);
    // The real plan numbers, not a renumbering of the open ones - settling is
    // by position in the whole plan, so 2 and 3 must stay 2 and 3.
    CHECK(result.content.find("  2. Program builds successfully") != std::string::npos);
    CHECK(result.content.find("  3. Program runs and outputs") != std::string::npos);
    // Open items keep their numbers, but a passing command must not suggest
    // that its one observation proves every unrelated open criterion.
    CHECK(result.content.find("Choose the corresponding criterion") != std::string::npos);
    CHECK(result.content.find(R"("criterion": [2, 3])") == std::string::npos);
    CHECK(result.content.find("  1. ") == std::string::npos);
}

TEST_CASE("a check that named its criterion is not lectured about it",
          "[command][contract][verification]") {
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "it runs", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "runs"}, {"exit_code", 0}, {"criterion", 1}}}});
    REQUIRE(CommandFixture::ran(result));
    CHECK(result.content.find("settled no acceptance criterion") == std::string::npos);
}

TEST_CASE("an artifact can be named by the path it was reported under",
          "[command][contract][artifact]") {
    // A caller shown "build/Debug/app.exe" and passing it straight back has
    // identified the resource exactly. Measured: a run did this and was told
    // "no such artifact" for a program LocalCode had recorded itself.
    CommandFixture fixture;
    const auto path = fixture.place_program("build/Debug/app.exe");

    ResolvedResource resource;
    resource.id = "build:app";
    resource.name = "app";
    resource.path = path;
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    CHECK(fixture.call("run_artifact", Json{{"artifact", "build/Debug/app.exe"}}).ok);
    // Separators spell the same path on Windows.
    CHECK(fixture.call("run_artifact", Json{{"artifact", R"(build\Debug\app.exe)"}}).ok);
    // A path that was never recorded is still refused.
    CHECK_FALSE(fixture.call("run_artifact", Json{{"artifact", "build/Debug/other.exe"}}).ok);
}

TEST_CASE("expect accepts the names a caller reaches for", "[expectation][aliases]") {
    // Spellings of one parameter, not readings of an ambiguous request. A run
    // that had built and was ready to verify sent {"output": "..."} and was
    // told its expectation stated nothing to check, with the exact output
    // sitting inside it.
    for (const char* key : {"output", "stdout", "expected_output", "equals"}) {
        INFO(key);
        const auto parsed = OutputExpectation::from_json(Json{{key, "0 1 1 2 3"}});
        REQUIRE(parsed);
        REQUIRE(parsed->stdout_equals.has_value());
        CHECK(*parsed->stdout_equals == "0 1 1 2 3");
    }
    // The documented name still wins when both are present.
    const auto both = OutputExpectation::from_json(
        Json{{"stdout_equals", "correct"}, {"output", "other"}});
    REQUIRE(both);
    CHECK(*both->stdout_equals == "correct");
}

TEST_CASE("an empty expectation names the keys it will accept", "[expectation][aliases]") {
    const auto parsed = OutputExpectation::from_json(Json{{"description", "it works"}});
    REQUIRE_FALSE(parsed);
    INFO(parsed.error().detail);
    CHECK(parsed.error().detail.find("stdout_equals") != std::string::npos);
    CHECK(parsed.error().detail.find("stdout_contains") != std::string::npos);
    CHECK(parsed.error().detail.find("\"criterion\": 1") != std::string::npos);
}

TEST_CASE("an empty stdout_equals that fails names the check that was meant",
          "[command][contract][verification]") {
    // Measured: a build that exited 0 was checked with "stdout_equals": "",
    // failed on its own output, and settled "Program builds successfully" as
    // NOT met. The build had succeeded. The comparison is legitimate - it asks
    // for silence - but it is almost never the one intended, and the run
    // recorded a false negative about its own work.
    CommandFixture fixture;

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "Program builds successfully"},
                             {"stdout_equals", ""},
                             {"criterion", 2}}}});
    REQUIRE(result.ok);
    INFO(result.content);

    CHECK(result.summary.find("behaviour WRONG") != std::string::npos);
    CHECK(result.content.find("asks for this command to print nothing") != std::string::npos);
    // The check that was actually meant, ready to send, with the criterion kept.
    CHECK(result.content.find(R"("exit_code": 0, "criterion": 2)") != std::string::npos);
    // And that a corrected check replaces this verdict rather than being stuck.
    CHECK(result.content.find("replaces this result") != std::string::npos);
}

TEST_CASE("a command that really is silent still passes an empty stdout_equals",
          "[command][contract][verification]") {
    // The check must keep working for what it actually says. Rejecting it
    // outright would remove a legitimate way to require silence.
    CommandFixture fixture;
    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmd"},
             {"arguments", Json::array({"/d", "/s", "/c", "exit 0"})},
             {"expect", Json{{"description", "prints nothing"}, {"stdout_equals", ""}}}});
    REQUIRE(result.ok);
    INFO(result.content);
    CHECK(result.summary.find("behaviour verified") != std::string::npos);
    CHECK(result.content.find("asks for this command to print nothing") == std::string::npos);
}

TEST_CASE("an artifact runs from any working directory", "[command][contract][artifact]") {
    // The recorded path is workspace-relative, and a relative executable
    // resolves against the working directory - which the caller sets
    // separately. Passing the relative form made the meaning of an id depend
    // on a parameter that has nothing to do with it.
    //
    // Measured: run_artifact "build:app" with working_directory
    // "/workspace/build" looked for build/build/RelWithDebInfo/app.exe and
    // reported the program missing. The whole point of an id is that the path
    // stops being anybody's problem.
    CommandFixture fixture;
    const auto path = fixture.place_program("build/RelWithDebInfo/app.exe");

    ResolvedResource resource;
    resource.id = "build:app";
    resource.name = "app";
    resource.path = path;
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    // From the workspace root, where the relative path would also have worked.
    CHECK(CommandFixture::ran(fixture.call("run_artifact", Json{{"artifact", "build:app"}})));

    // And from a directory where it would not have.
    auto elsewhere = fixture.call("run_artifact",
                                  Json{{"artifact", "build:app"},
                                       {"working_directory", "build"}});
    INFO(elsewhere.content);
    CHECK(CommandFixture::ran(elsewhere));

    auto deeper = fixture.call("run_artifact",
                               Json{{"artifact", "build:app"},
                                    {"working_directory", "/workspace/build/RelWithDebInfo"}});
    INFO(deeper.content);
    CHECK(CommandFixture::ran(deeper));
}

TEST_CASE("an artifact id sent to run_command is named, not called damaged",
          "[command][contract][artifact]") {
    // The build report offers run_artifact { "artifact": "build:fibonacci" }
    // and a caller can take the id from it while keeping the tool it was
    // already using. Measured: LocalCode saw the colon, decided the path had
    // been damaged by JSON escaping, and gave a confidently wrong diagnosis.
    CommandFixture fixture;
    const auto path = fixture.place_program("build/Debug/fibonacci.exe");

    ResolvedResource resource;
    resource.id = "build:fibonacci";
    resource.name = "fibonacci";
    resource.path = path;
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    auto result = fixture.call("run_command",
                               Json{{"executable", "build:fibonacci"},
                                    {"arguments", Json::array()}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);

    CHECK(result.summary.find("artifact id, not a path") != std::string::npos);
    CHECK(result.summary.find("damaged") == std::string::npos);
    CHECK(result.error.find(R"(run_artifact { "artifact": "build:fibonacci" })") !=
          std::string::npos);
    // The path is offered too, for a caller that would rather use run_command.
    CHECK(result.error.find("build/Debug/fibonacci.exe") != std::string::npos);
}

TEST_CASE("a successful build with no expectation says how to settle a criterion",
          "[command][contract][verification]") {
    // A build succeeding is the whole evidence for "it builds", and on its own
    // it settles nothing. Measured: a model ran the same successful build three
    // times trying to settle "Program builds successfully with CMake", because
    // nothing it did carried an expectation to point at the criterion.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {2, "Program builds successfully with CMake", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"}, {"arguments", Json::array({"--version"})}});
    REQUIRE(result.ok);
    INFO(result.content);

    CHECK(result.content.find("settled no acceptance criterion") != std::string::npos);
    CHECK(result.content.find("Set expect.exit_code to 0") != std::string::npos);
    CHECK(result.content.find("of that build/test criterion") != std::string::npos);
}

TEST_CASE("an ordinary command is not lectured about criteria",
          "[command][contract][verification]") {
    // Saying this after every successful command would be noise, and noise is
    // what buries the line that matters.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "it works", false}};
    };

    auto result = fixture.call("run_command",
                               Json{{"executable", "hostname"}, {"arguments", Json::array()}});
    REQUIRE(CommandFixture::ran(result));
    CHECK(result.content.find("settled no acceptance criterion") == std::string::npos);
}

TEST_CASE("an expectation missing only its comparison is handed back completed",
          "[expectation][aliases]") {
    // Measured: {"description": "Build the project with CMake and verify it
    // compiles successfully", "criterion": 2} - correct in every respect
    // except that it compared nothing. A model that has got as far as naming a
    // criterion has understood the mechanism and is one key short; giving it a
    // list of key names and making it rebuild the call spends that turn again.
    const auto parsed = OutputExpectation::from_json(
        Json{{"description", "Build the project with CMake"}, {"criterion", 2}});
    REQUIRE_FALSE(parsed);
    INFO(parsed.error().detail);

    // Its own words, in a call that would work, with the criterion kept.
    CHECK(parsed.error().detail.find(
              R"("description": "Build the project with CMake", "exit_code": 0, "criterion": 2)") !=
          std::string::npos);
    CHECK(parsed.error().detail.find(R"("stdout_equals": "<the exact output>", "criterion": 2)") !=
          std::string::npos);
}

TEST_CASE("an expectation with nothing at all still gets the key list",
          "[expectation][aliases]") {
    const auto parsed = OutputExpectation::from_json(Json::object().is_null()
                                                         ? Json::object()
                                                         : Json{{"check_stderr", true}});
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().detail.find("The keys it accepts") != std::string::npos);
    // Nothing to complete, so no fabricated example of the caller's intent.
    CHECK(parsed.error().detail.find("This call with a comparison added") == std::string::npos);
}

TEST_CASE("a criterion named by its wording resolves to its number",
          "[command][contract][verification]") {
    // Measured: a run sent "criterion": "Program runs and outputs the correct
    // sequence: 0, 1, 1, 2, 3, 5, 8, 13, 21, 34". json_int could not read it,
    // the number silently became zero, and the check ran, passed, settled
    // nothing and said nothing about why.
    //
    // Quoting a criterion back identifies it exactly, so this is a lookup.
    // Only an exact one: anything looser would be matching a check to a
    // criterion by how similar the sentences read.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {1, "Program prints the first ten Fibonacci numbers", false},
            {2, "Program builds successfully with CMake", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "it runs"},
                             {"exit_code", 0},
                             {"criterion", "Program builds successfully with CMake"}}}});
    REQUIRE(result.ok);
    INFO(result.content);
    CHECK(json_int(result.structured, "expectation_criterion", 0) == 2);
    // Resolved, so the "you settled nothing" note must not appear.
    CHECK(result.content.find("settled no acceptance criterion") == std::string::npos);
}

TEST_CASE("a criterion that matches nothing is refused with the numbers",
          "[command][contract][verification]") {
    // The lookup is exact. Wording that does not match any criterion is not
    // guessed at - it is refused, with the plan in front of the caller.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {1, "Program prints the first ten Fibonacci numbers", true},
            {2, "Program builds successfully with CMake", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "it runs"},
                             {"exit_code", 0},
                             {"criterion", "something nobody wrote down"}}}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("'criterion' must be a number") != std::string::npos);
    CHECK(result.error.find("1. Program prints the first ten") != std::string::npos);
    CHECK(result.error.find("(already settled)") != std::string::npos);
    CHECK(result.error.find("2. Program builds successfully") != std::string::npos);
}

TEST_CASE("a criterion given as a numeric string still works",
          "[command][contract][verification]") {
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "it runs", false}};
    };
    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "it runs"}, {"exit_code", 0}, {"criterion", "1"}}}});
    REQUIRE(result.ok);
    CHECK(json_int(result.structured, "expectation_criterion", 0) == 1);
}

TEST_CASE("running the built program with no expectation says it proved nothing",
          "[command][contract][verification]") {
    // The moment where "it ran" is most easily mistaken for "it is right".
    // Measured across three runs: the program was run with no expectation,
    // nothing was settled, and each run then rebuilt the project repeatedly
    // because the build was the last thing that had visibly worked.
    CommandFixture fixture;
    const auto path = fixture.place_program("build/Debug/app.exe");

    ResolvedResource resource;
    resource.id = "build:app";
    resource.name = "app";
    resource.path = path;
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {1, "Program prints the first ten Fibonacci numbers", false}};
    };

    auto result = fixture.call("run_artifact", Json{{"artifact", "build:app"}});
    REQUIRE(CommandFixture::ran(result));
    INFO(result.content);

    CHECK(result.content.find("not the same as it being right") != std::string::npos);
    CHECK(result.content.find("1. Program prints the first ten") != std::string::npos);
    // The example offers an output comparison, not an exit code - the output
    // is the whole point of this criterion.
    CHECK(result.content.find("expect.stdout_equals") != std::string::npos);
    CHECK(result.content.find("expect.criterion to the matching runtime criterion") !=
          std::string::npos);
    // And it warns against copying the observed output without checking it.
    CHECK(result.content.find("from the task's expected result") != std::string::npos);
}

TEST_CASE("a path shattered in command but correct in executable is not a conflict",
          "[command][contract][conflict]") {
    // Measured: this exact call, sent five times against a "these disagree"
    // refusal, until the run was stopped. They did not disagree - joining the
    // pieces with a slash gives back the executable character for character,
    // and the correct spelling was sitting in the same call.
    const auto parsed = parse_command_request(
        Json{{"command", Json::array({"build", "RelWithDebInfo", "fibonacci.exe"})},
             {"executable", "build/RelWithDebInfo/fibonacci.exe"}});
    REQUIRE(parsed.valid);
    CHECK(parsed.executable == "build/RelWithDebInfo/fibonacci.exe");
    CHECK(parsed.arguments.empty());
    CHECK(parsed.note.find("split across several elements") != std::string::npos);

    // Backslashes spell the same path.
    CHECK(parse_command_request(
              Json{{"command", Json::array({"build", "Debug", "app.exe"})},
                   {"executable", R"(build\Debug\app.exe)"}})
              .valid);

    // A command that is genuinely a program plus arguments is untouched by
    // this: joining "cmake --build build" with slashes is not the executable.
    const auto real = parse_command_request(
        Json{{"command", Json::array({"cmake", "--build", "build"})},
             {"executable", "ctest"}});
    CHECK_FALSE(real.valid);
    CHECK(real.error_summary.find("conflicting command") != std::string::npos);

    // And explicit arguments still have to agree.
    const auto with_arguments = parse_command_request(
        Json{{"command", Json::array({"build", "Debug", "app.exe"})},
             {"executable", "build/Debug/app.exe"},
             {"arguments", Json::array({"--flag"})}});
    CHECK_FALSE(with_arguments.valid);
}

TEST_CASE("only one structured form is advertised", "[command][contract][schema]") {
    // Offering two structured spellings of one thing was the defect, not the
    // fix for it. A small model copies the shape of its own previous call, and
    // a stale 'executable' carried forward beside a fresh 'command' produced a
    // conflict it could not climb out of - five identical retries before the
    // run was stopped. One documented form means there is no second field to
    // carry forward.
    ToolRegistry registry;
    registry.register_builtin_tools();

    const auto* tool = registry.find("run_command");
    REQUIRE(tool != nullptr);
    const auto properties = tool->parameters_schema().at("properties");

    CHECK(properties.contains("executable"));
    CHECK(properties.contains("arguments"));
    CHECK(properties.contains("shell_command"));
    CHECK_FALSE(properties.contains("command"));

    // Still parsed, so anything already sending it keeps working.
    const auto legacy = parse_command_request(
        Json{{"command", Json::array({"cmake", "--version"})}});
    REQUIRE(legacy.valid);
    CHECK(legacy.executable == "cmake");
    CHECK(legacy.arguments == std::vector<std::string>{"--version"});
}

TEST_CASE("a description belonging to another criterion is pointed out",
          "[command][contract][verification]") {
    // Measured: a run settled criterion 1 five times while passing criterion
    // 2's exact text as the description, and criterion 2 stayed open until the
    // run was stopped. Both strings are exact, so noticing they point at
    // different criteria is an observation, not a similarity judgement.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {1, "Build succeeds without errors", false},
            {2, "The program prints exactly ten Fibonacci numbers starting from 0", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description",
                              "The program prints exactly ten Fibonacci numbers starting from 0"},
                             {"exit_code", 0},
                             {"criterion", 1}}}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.summary.find("description and the criterion number disagree") !=
          std::string::npos);
    CHECK(result.error.find("criterion 2's wording") != std::string::npos);
    // It does not choose: both ways out are offered.
    CHECK(result.error.find("LocalCode will not pick") != std::string::npos);
}

TEST_CASE("a description matching its own criterion is not flagged",
          "[command][contract][verification]") {
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "Build succeeds without errors", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "Build succeeds without errors"},
                             {"exit_code", 0},
                             {"criterion", 1}}}});
    CHECK(result.ok);
    CHECK(result.summary.find("disagree") == std::string::npos);
}

TEST_CASE("a description that is nobody's criterion is left alone",
          "[command][contract][verification]") {
    // Only an exact match against a different criterion is an inconsistency.
    // Ordinary prose that resembles one is not, and refusing it would be the
    // similarity judgement this avoids.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "Build succeeds without errors", false},
                                             {2, "It prints ten numbers", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "it printed ten numbers, one per line"},
                             {"exit_code", 0},
                             {"criterion", 1}}}});
    CHECK(result.ok);
}

TEST_CASE("a check quoting a criterion word for word settles that criterion",
          "[command][contract][verification]") {
    // Measured across four runs: a complete, correct expectation - the right
    // output, the right description, a passing comparison - settling nothing,
    // three or four times in a row, until the run was stopped. The description
    // was the criterion's own wording, reproduced exactly.
    //
    // Quoting the criterion as what this check proves is as explicit as giving
    // its position. This is the same exact-string observation as the mismatch
    // refusal, read the other way.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {1, "C++ program prints the first ten Fibonacci numbers", false},
            {2, "Program builds successfully with CMake", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "C++ program prints the first ten Fibonacci numbers"},
                             {"exit_code", 0}}}});
    REQUIRE(result.ok);
    INFO(result.content);
    CHECK(json_int(result.structured, "expectation_criterion", 0) == 1);
    CHECK(result.content.find("names acceptance criterion 1 word for word") != std::string::npos);
    // Having settled one, it must not also nag about settling nothing.
    CHECK(result.content.find("settled no acceptance criterion") == std::string::npos);
}

TEST_CASE("prose that merely resembles a criterion settles nothing",
          "[command][contract][verification]") {
    // Deciding that a description means a criterion because it sounds like one
    // is the inference this whole subsystem exists to remove.
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {1, "C++ program prints the first ten Fibonacci numbers", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "the program prints ten Fibonacci numbers"},
                             {"exit_code", 0}}}});
    REQUIRE(result.ok);
    CHECK(json_int(result.structured, "expectation_criterion", 0) == 0);
    CHECK(result.content.find("settled no acceptance criterion") != std::string::npos);
}

TEST_CASE("a wording shared by two criteria is left to the caller",
          "[command][contract][verification]") {
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "it works", false}, {2, "it works", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "it works"}, {"exit_code", 0}}}});
    REQUIRE(result.ok);
    CHECK(json_int(result.structured, "expectation_criterion", 0) == 0);
}

TEST_CASE("an invented absolute path is named as unsupported, not merely missing",
          "[command][contract][paths]") {
    // Measured: a run wrote to /scratchpad/fib60/main.cpp, then
    // /scratchpad/fib60, four times, and was stopped. "No such file" invites
    // the next neighbouring guess; saying that nothing supports the path
    // existing does not.
    //
    // Nothing is rewritten. Deciding /scratchpad/x meant <root>/x would hide a
    // real mistake and make one prefix mean two things.
    CommandFixture fixture;

    auto result = fixture.call("write_file",
                               Json{{"path", "/scratchpad/fib60/main.cpp"},
                                    {"content", "int main() { return 0; }\n"}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);

    CHECK(result.error.rfind("UNRESOLVED ABSOLUTE PATH", 0) == 0);
    CHECK(result.error.find("nothing supports it existing") != std::string::npos);
    CHECK(result.error.find(fixture.temp.path().generic_string()) != std::string::npos);
    CHECK(result.error.find("main.cpp") != std::string::npos);
    // The file really was not created anywhere.
    CHECK_FALSE(std::filesystem::exists(fixture.temp.path() / "main.cpp"));
}

TEST_CASE("an unsupported path lists the resources it could have named",
          "[command][contract][paths]") {
    CommandFixture fixture;
    const auto path = fixture.place_program("build/Debug/app.exe");

    ResolvedResource resource;
    resource.id = "build:app";
    resource.name = "app";
    resource.path = path;
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    auto result = fixture.call("read_file", Json{{"path", "/nowhere/at/all/app.exe"}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("build:app") != std::string::npos);
    CHECK(result.error.find("build/Debug/app.exe") != std::string::npos);
}

TEST_CASE("ordinary paths are not called unsupported", "[command][contract][paths]") {
    // The check must be specific or it becomes noise on every missing file.
    CommandFixture fixture;
    fixture.temp.write("src/main.cpp", "int main() {}\n");

    // A file that simply is not there yet, inside the workspace.
    auto missing = fixture.call("read_file", Json{{"path", "src/other.cpp"}});
    REQUIRE_FALSE(missing.ok);
    CHECK(missing.error.find("UNRESOLVED ABSOLUTE PATH") == std::string::npos);

    // An absolute path that is real, but out of bounds for writing.
    TempDirectory elsewhere{"lc-outside"};
    elsewhere.write("x.cpp", "int main() {}\n");
    auto refused = fixture.call(
        "write_file",
        Json{{"path", (elsewhere.path() / "x.cpp").string()}, {"content", "x\n"}});
    REQUIRE_FALSE(refused.ok);
    CHECK(refused.error.find("UNRESOLVED ABSOLUTE PATH") == std::string::npos);
    CHECK(refused.error.find("This workspace is") != std::string::npos);

    // And /workspace still resolves rather than being called unsupported.
    CHECK(fixture.call("write_file",
                       Json{{"path", "/workspace/ok.cpp"}, {"content", "int main(){}\n"}}).ok);
}

TEST_CASE("an invented path to a real program gives the real location",
          "[command][contract][paths]") {
    // Measured: a run spent its last turns on a hand-built MSVC path with
    // "Microsoft MSVC" where the real directory is "MSVC". LocalCode had
    // resolved cl.exe minutes earlier and answered "not found".
    CommandFixture fixture;

    auto result = fixture.call(
        "run_command",
        Json{{"executable", R"(C:\Nowhere\Invented\Path\hostname.exe)"},
             {"arguments", Json::array()}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("hostname.exe is at:") != std::string::npos);
    CHECK(result.error.find("HOSTNAME.EXE") != std::string::npos);
}

TEST_CASE("a tool run with no arguments settles nothing and says why",
          "[command][contract][verification]") {
    // Measured: a model settled "Program builds successfully with CMake"
    // against `cmake` on its own, five times. Bare cmake prints usage and does
    // not build, so the check failed and the criterion went down as NOT met
    // while the real build had succeeded minutes earlier. LocalCode said only
    // "exit 1".
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{
            {2, "Program builds successfully with CMake", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "Program builds successfully with CMake"},
                             {"exit_code", 0},
                             {"criterion", 2}}}});
    // Refused, not noted. Bare cmake exits 0, so a note in the body loses to a
    // summary line reading "OK cmake (exit 0)" - three runs sent this same
    // call five times each, reading success at the top.
    REQUIRE_FALSE(result.ok);
    INFO(result.error);

    CHECK(result.summary.find("with no arguments did nothing") != std::string::npos);
    CHECK(result.error.find("prints its usage and exits") != std::string::npos);
    // The check that was meant, ready to send, with the criterion kept.
    CHECK(result.error.find(R"("arguments": ["--build", "build"])") != std::string::npos);
    CHECK(result.error.find(R"("criterion": 2)") != std::string::npos);
    // Nothing was settled either way.
    CHECK_FALSE(json_bool(result.structured, "expectation_checked", false));
}

TEST_CASE("a real command with arguments is unaffected",
          "[command][contract][verification]") {
    // The rule keys on "no arguments at all", so an ordinary command that
    // happens to mention usage in its output is untouched.
    CommandFixture fixture;
    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"--version"})},
             {"expect", Json{{"description", "cmake is available"}, {"exit_code", 0}}}});
    REQUIRE(result.ok);
    INFO(result.content);
    CHECK(result.content.find("printed its usage") == std::string::npos);
    CHECK(result.summary.find("behaviour verified") != std::string::npos);
}

TEST_CASE("a real path is never called an artifact id", "[command][contract][artifact]") {
    // Resource lookup accepts a recorded path, which is right for run_artifact
    // and very wrong in run_command's not-found message: it told a caller that
    // "build/RelWithDebInfo/app.exe" was an id and not a path, five times,
    // about a path. The whole run was lost to it.
    CommandFixture fixture;

    ResolvedResource resource;
    resource.id = "build:app";
    resource.name = "app";
    resource.path = "build/RelWithDebInfo/app.exe";   // recorded, but not on disk
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    auto by_path = fixture.call("run_command",
                                Json{{"executable", "build/RelWithDebInfo/app.exe"},
                                     {"arguments", Json::array()}});
    REQUIRE_FALSE(by_path.ok);
    INFO(by_path.error);
    CHECK(by_path.summary.find("artifact id, not a path") == std::string::npos);
    // And it is not told to "use the path exactly as written" - it did. The
    // spelling is right and the file is gone, which is a different fact.
    CHECK(by_path.summary.find("not there now") != std::string::npos);
    CHECK(by_path.error.find("the path is right") != std::string::npos);
    CHECK(by_path.error.find("build:app") != std::string::npos);

    // The id itself still gets the redirect.
    auto by_id = fixture.call("run_command",
                              Json{{"executable", "build:app"}, {"arguments", Json::array()}});
    REQUIRE_FALSE(by_id.ok);
    CHECK(by_id.summary.find("artifact id, not a path") != std::string::npos);
}

TEST_CASE("executable naming the program and shell_command carrying the line agree",
          "[command][contract][conflict]") {
    // The same shape as executable + command, with the arguments in the shell
    // line instead. Treating one as agreement and the other as a conflict was
    // an inconsistency in this contract, not a distinction.
    //
    // Measured: {"executable": "cmake", "shell_command": "cmake -S . -B build
    // -G Ninja ..."} refused five times as two different commands, until the
    // run was stopped. It is one command, written once carelessly and once in
    // full.
    const auto parsed = parse_command_request(
        Json{{"executable", "cmake"},
             {"shell_command", "cmake -S . -B build -G Ninja"}});
    REQUIRE(parsed.valid);
    CHECK(parsed.executable == "cmake");
    CHECK(parsed.arguments ==
          std::vector<std::string>{"-S", ".", "-B", "build", "-G", "Ninja"});
    CHECK_FALSE(parsed.use_shell);
    CHECK(parsed.note.find("carried the whole line") != std::string::npos);

    // A line that genuinely needs a shell keeps one.
    const auto piped = parse_command_request(
        Json{{"executable", "cmake"}, {"shell_command", "cmake --build build && ctest"}});
    REQUIRE(piped.valid);
    CHECK(piped.use_shell);

    // A shell line for a different program is still a conflict.
    const auto conflict = parse_command_request(
        Json{{"executable", "cmake"}, {"shell_command", "ctest --output-on-failure"}});
    CHECK_FALSE(conflict.valid);
    CHECK(conflict.error_summary.find("conflicting command") != std::string::npos);

    // And explicit arguments still have to match.
    const auto explicit_mismatch = parse_command_request(
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"--version"})},
             {"shell_command", "cmake --build build"}});
    CHECK_FALSE(explicit_mismatch.valid);
}

TEST_CASE("running a tool bare to read its help is still allowed",
          "[command][contract][verification]") {
    // Without an expectation attached there is nothing being claimed, and
    // looking at a tool's usage is a legitimate thing to do.
    CommandFixture fixture;
    auto result = fixture.call("run_command",
                               Json{{"executable", "cmake"}, {"arguments", Json::array()}});
    REQUIRE(result.ok);
    INFO(result.content);
    CHECK(result.content.find("printed its usage and did no work") != std::string::npos);
}

TEST_CASE("a shell line that only echoes the exit code is named as such",
          "[command][contract][conflict]") {
    // Measured: `cmake -S . -B build -G "Visual Studio 17 2022" ... ; echo
    // "EXIT_CODE=$?"` alongside the same command in executable + arguments.
    // That is not a second command so much as a workaround for something
    // LocalCode already reports, and "these disagree" gives no reason to drop
    // it.
    const auto parsed = parse_command_request(
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"-S", ".", "-B", "build"})},
             {"shell_command", R"(cmake -S . -B build; echo "EXIT_CODE=$?")"}});
    REQUIRE_FALSE(parsed.valid);
    INFO(parsed.error_detail);

    CHECK(parsed.error_summary.find("already reported") != std::string::npos);
    CHECK(parsed.error_detail.find("the \"exit:\" line") != std::string::npos);
    CHECK(parsed.error_detail.find(R"("exit_code": 0)") != std::string::npos);
    // And the command to send instead, ready to copy.
    CHECK(parsed.error_detail.find(
              R"({"executable": "cmake", "arguments": ["-S", ".", "-B", "build"]})") !=
          std::string::npos);

    // A shell line that genuinely does something else is still an ordinary
    // conflict, not this.
    const auto other = parse_command_request(
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"-S", ".", "-B", "build"})},
             {"shell_command", "ctest --output-on-failure"}});
    CHECK_FALSE(other.valid);
    CHECK(other.error_summary.find("conflicting command") != std::string::npos);
}

TEST_CASE("one check can settle several criteria when the caller names them",
          "[command][contract][verification]") {
    // Running the program once really does establish "prints the first ten
    // Fibonacci numbers" and "runs and produces the correct output" - the same
    // evidence, named twice in the plan. Making the caller run it twice to say
    // so is bookkeeping for its own sake, and bookkeeping is where a small
    // model spends its errors: every reliability sample so far failed here and
    // nowhere else.
    //
    // The caller names the list. Nothing is inferred.
    const auto parsed = OutputExpectation::from_json(
        Json{{"description", "prints the sequence"},
             {"stdout_equals", "0 1 1 2"},
             {"criterion", Json::array({1, 3})}});
    REQUIRE(parsed);
    CHECK(parsed->criteria == std::vector<int>{1, 3});
    // The single-number field still carries the first, so anything reading one
    // number keeps working.
    CHECK(parsed->criterion == 1);

    // A single number still produces a one-entry list.
    const auto single = OutputExpectation::from_json(
        Json{{"description", "x"}, {"exit_code", 0}, {"criterion", 2}});
    REQUIRE(single);
    CHECK(single->criteria == std::vector<int>{2});
    CHECK(single->criterion == 2);

    // Junk in the list is dropped rather than settling criterion 0.
    const auto messy = OutputExpectation::from_json(
        Json{{"description", "x"},
             {"exit_code", 0},
             {"criterion", Json::array({0, 2, -1})}});
    REQUIRE(messy);
    CHECK(messy->criteria == std::vector<int>{2});
}

TEST_CASE("a run reports every criterion it settled", "[command][contract][verification]") {
    CommandFixture fixture;
    fixture.context.acceptance_criteria = [] {
        return std::vector<CriterionSummary>{{1, "prints the numbers", false},
                                             {3, "runs and produces correct output", false}};
    };

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "hostname"},
             {"arguments", Json::array()},
             {"expect", Json{{"description", "it runs"},
                             {"exit_code", 0},
                             {"criterion", Json::array({1, 3})}}}});
    REQUIRE(result.ok);
    INFO(result.content);
    REQUIRE(result.structured.contains("expectation_criteria"));
    CHECK(result.structured.at("expectation_criteria") == Json::array({1, 3}));
    CHECK(result.content.find("settled no acceptance criterion") == std::string::npos);
}

TEST_CASE("an output check attached to the build is refused, not failed",
          "[command][contract][verification]") {
    // Measured in the adversarial benchmark: a run fixed all three defects
    // correctly - the median, the missing semicolon, the stub - derived the
    // right answer unprompted, and then compared "median=2.5\nmean=3.5"
    // against `cmake --build`. A build's stdout is a build log, so the
    // comparison could only fail. LocalCode recorded correct work as a failed
    // criterion and the run died believing its own fix was wrong.
    CommandFixture fixture;

    ResolvedResource resource;
    resource.id = "build:stats";
    resource.name = "stats";
    resource.path = "build/Debug/stats.exe";
    resource.source = "MSBuild link line";
    fixture.workspace.record_resolved_resources({resource});

    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"--version"})},
             {"expect", Json{{"description", "prints the right numbers"},
                             {"stdout_equals", "median=2.5\nmean=3.5\n"},
                             {"criterion", 2}}}});
    REQUIRE_FALSE(result.ok);
    INFO(result.error);

    CHECK(result.summary.find("attached to the build") != std::string::npos);
    CHECK(result.error.find("says nothing about whether the program is right") !=
          std::string::npos);
    CHECK(result.error.find(R"(run_artifact { "artifact": "build:stats")") != std::string::npos);
    CHECK(result.error.find(R"("exit_code": 0)") != std::string::npos);
    // Nothing was settled either way.
    CHECK_FALSE(json_bool(result.structured, "expectation_checked", false));
}

TEST_CASE("an exit-code check on a build is untouched",
          "[command][contract][verification]") {
    // "the project builds, exit_code 0" is exactly the right check for a build
    // command, and must keep working.
    CommandFixture fixture;
    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"--version"})},
             {"expect", Json{{"description", "the project builds"}, {"exit_code", 0}}}});
    REQUIRE(result.ok);
    CHECK(result.summary.find("behaviour verified") != std::string::npos);
}

TEST_CASE("a passing output check on a build is left alone",
          "[command][contract][verification]") {
    // The refusal is for a check that FAILED on a build log. One that passes is
    // measuring something real - a build that must print a particular line -
    // and refusing it would be overreach.
    CommandFixture fixture;
    auto result = fixture.call(
        "run_command",
        Json{{"executable", "cmake"},
             {"arguments", Json::array({"--version"})},
             {"expect", Json{{"description", "cmake reports a version"},
                             {"stdout_matches", "cmake version"}}}});
    REQUIRE(result.ok);
    CHECK(result.summary.find("behaviour verified") != std::string::npos);
}
