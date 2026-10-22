// ---------------------------------------------------------------------------
// What a tool result is allowed to prove.
//
// The rules under test: a command establishes only the stage it was actually
// performing, an environment script establishes nothing at all, and no exit
// code ever reaches the behaviour stage.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "agent/EvidenceInterpreter.h"

using namespace localcode;

namespace {

ToolCall command_call() {
    ToolCall call;
    call.id = "call-1";
    call.name = "run_command";
    return call;
}

ToolResult ran(const std::string& command, int exit_code, std::string content = {}) {
    ToolResult result;
    result.ok = true;
    result.content = std::move(content);
    result.structured = {{"command", command},
                         {"exit_code", exit_code},
                         {"duration_ms", 12},
                         {"timed_out", false},
                         {"cancelled", false},
                         {"error_count", exit_code == 0 ? 0 : 1}};
    return result;
}

}  // namespace

TEST_CASE("a command line is split into arguments", "[interpreter][parsing]") {
    const auto args = split_command_line(R"(cmake --build "C:\my build\out" --config Debug)");
    REQUIRE(args.size() == 5);
    CHECK(args[0] == "cmake");
    CHECK(args[2] == R"(C:\my build\out)");
    CHECK(split_command_line("   ").empty());
}

TEST_CASE("configuring is not building", "[interpreter][classification]") {
    CHECK(classify_intent("cmake -S . -B build") == CommandIntent::Configure);
    CHECK(classify_intent("cmake -B build -G Ninja") == CommandIntent::Configure);
    CHECK(classify_intent("cmake --build build") == CommandIntent::Build);
    CHECK(classify_intent("cmake --build build --config Release") == CommandIntent::Build);
}

TEST_CASE("running the program is its own stage", "[interpreter][classification]") {
    CHECK(classify_intent(R"(build\fib.exe)") == CommandIntent::Execution);
    CHECK(classify_intent("./a.out") == CommandIntent::Execution);
    CHECK(classify_intent("python script.py") == CommandIntent::Execution);
    CHECK(classify_intent("node index.js") == CommandIntent::Execution);
    CHECK(classify_intent("cargo run") == CommandIntent::Execution);
}

TEST_CASE("test runners are recognised as tests", "[interpreter][classification]") {
    CHECK(classify_intent("ctest --output-on-failure") == CommandIntent::Test);
    CHECK(classify_intent("cargo test") == CommandIntent::Test);
    CHECK(classify_intent("python -m pytest tests") == CommandIntent::Test);
    CHECK(classify_intent(R"(build\localcode_tests.exe)") == CommandIntent::Test);
}

TEST_CASE("looking around establishes nothing", "[interpreter][classification]") {
    CHECK(classify_intent("git status") == CommandIntent::Inspection);
    CHECK(classify_intent("git log --oneline -5") == CommandIntent::Inspection);
    CHECK(classify_intent("where cmake") == CommandIntent::Inspection);
    CHECK(classify_intent("cmake --version") == CommandIntent::Inspection);
    CHECK(stage_for(CommandIntent::Inspection) == std::nullopt);
    CHECK(stage_for(CommandIntent::Unknown) == std::nullopt);
}

TEST_CASE("an environment script proves nothing about what follows it",
          "[interpreter][classification]") {
    // vcvars64.bat ignores its arguments and exits 0. Scanning the line for the
    // word "cmake" recorded four builds that never happened.
    CHECK(classify_intent(R"("C:\...\vcvars64.bat" cmake -B build)") == CommandIntent::Unknown);
    CHECK(classify_intent("vsdevcmd.bat cmake --build build") == CommandIntent::Unknown);
}

TEST_CASE("wrappers are walked past", "[interpreter][classification]") {
    CHECK(classify_intent("cmd /c cmake --build build") == CommandIntent::Build);
    CHECK(classify_intent("bash -c ctest") == CommandIntent::Test);
}

TEST_CASE("explicit PowerShell scripts establish only execution",
          "[interpreter][classification][opaque-powershell]") {
    // The first two are the user-supplied wrappers from a real failing agent
    // run. Their names and printed claims cannot classify their purpose.
    for (
        const auto* command :
        {"pwsh -NoProfile -File ./audit-focused-tests.ps1", "pwsh -NoProfile -File ./run-tests.ps1",
         R"("C:\Program Files\PowerShell\7\pwsh.exe" -NoProfile -File "C:\work space\run-tests.ps1")",
         "PoWeRsHeLl.ExE -nOpRoFiLe -f ./build.ps1",
         "pwsh -NoLogo -NonInteractive -ExecutionPolicy Bypass -File ./ninja.ps1",
         "pwsh -InputFormat Text -OutputFormat Text -File ./ctest.ps1",
         "pwsh -File ./vcvars64.ps1 cmake --build build",
         "cmd /c pwsh -NoProfile -File ./run-tests.ps1"}) {
        INFO(command);
        CHECK(classify_intent(command) == CommandIntent::Execution);
    }
}

TEST_CASE("opaque and incomplete PowerShell invocations stay unclassified",
          "[interpreter][classification][opaque-powershell]") {
    for (const auto* command :
         {"pwsh -NoProfile -File", "pwsh -NoProfile -File -", "pwsh -File \"\"",
          "pwsh -File -Command", "pwsh -UnknownSwitch -File ./run-tests.ps1",
          "pwsh -ExecutionPolicy -File ./run-tests.ps1", "pwsh -File ./ninja.exe",
          "pwsh ./run-tests.ps1", "pwsh -NoProfile -Command cmake --build build",
          "powershell -c ctest", "pwsh -EncodedCommand YwB0AGUAcwB0AA==",
          "pwsh -NoProfile -enc YwB0AGUAcwB0AA==", "pwsh -Command pwsh -File ./run-tests.ps1"}) {
        INFO(command);
        CHECK(classify_intent(command) == CommandIntent::Unknown);
    }
}

TEST_CASE("a successful opaque script cannot verify its printed claims",
          "[interpreter][stages][opaque-powershell]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"the project builds"}, {"the tests pass"}});
    // Captured green wrapper output: exit 0 and a claim about passing tests
    // establish execution only, until an explicit check is supplied.
    interpret_tool_result(command_call(),
                          ran("pwsh -NoProfile -File ./run-tests.ps1", 0,
                              "All tests passed (8 assertions in 4 test cases)"),
                          ledger);

    CHECK(ledger.holds(Stage::Execution));
    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Test) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);
    CHECK_FALSE(ledger.acceptance_plan()[0].settled);
    CHECK_FALSE(ledger.acceptance_plan()[1].settled);

    const auto assessment = assess_completion(ledger, true);
    CHECK(assessment.blocked);
    for (const auto& reason : assessment.reasons)
        CHECK(reason.find("nothing was built, run or tested") == std::string::npos);
}

TEST_CASE("a successful build records the build stage and nothing above it",
          "[interpreter][stages]") {
    VerificationLedger ledger;
    interpret_tool_result(command_call(), ran("cmake --build build", 0), ledger);

    CHECK(ledger.holds(Stage::Build));
    CHECK(ledger.status(Stage::Execution) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Unknown);
    CHECK(ledger.status(Stage::Objective) == StageStatus::Unknown);
}

TEST_CASE("a failing build is recorded as a failure", "[interpreter][stages]") {
    VerificationLedger ledger;
    interpret_tool_result(command_call(),
                          ran("cmake --build build", 1, "main.cpp(4,1): error C2065: 'x'"),
                          ledger);
    CHECK(ledger.status(Stage::Build) == StageStatus::Fail);
    CHECK_FALSE(ledger.holds(Stage::Build));
}

TEST_CASE("running a program never reaches the behaviour stage", "[interpreter][behaviour]") {
    // Run 9, exactly: configure, build, run, all clean, and the answer never
    // examined.
    VerificationLedger ledger;
    interpret_tool_result(command_call(), ran("cmake -B build", 0), ledger);
    interpret_tool_result(command_call(), ran("cmake --build build", 0), ledger);
    interpret_tool_result(command_call(),
                          ran(R"(build\fib.exe)", 0, "1 3 6 12 24 48 96 192 384 768"), ledger);

    CHECK(ledger.holds(Stage::Configure));
    CHECK(ledger.holds(Stage::Build));
    CHECK(ledger.holds(Stage::Execution));
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Unknown);
}

TEST_CASE("a behaviour check is the only route to the behaviour stage",
          "[interpreter][behaviour]") {
    VerificationLedger ledger;
    auto result = ran(R"(build\fib.exe)", 0, "1 3 6 12 24 48 96 192 384 768");
    result.structured["expectation_checked"] = true;
    result.structured["expectation_satisfied"] = false;
    result.structured["expectation_description"] = "prints the first ten Fibonacci numbers";
    result.structured["expectation_failures"] =
        Json::array({"stdout does not match\n    expected: 0 1 1 2 3 5 8 13 21 34"});

    interpret_tool_result(command_call(), result, ledger);

    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Fail);
    // The execution itself still succeeded. Both facts are true at once, and
    // that is the distinction the ledger exists to hold.
    CHECK(ledger.holds(Stage::Execution));
}

TEST_CASE("a satisfied behaviour check settles the criterion it names",
          "[interpreter][behaviour]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"it builds"}, {"it prints the first ten Fibonacci numbers"}});

    auto result = ran(R"(build\fib.exe)", 0, "0 1 1 2 3 5 8 13 21 34");
    result.structured["expectation_checked"] = true;
    result.structured["expectation_satisfied"] = true;
    result.structured["expectation_description"] = "prints the first ten Fibonacci numbers";
    result.structured["expectation_criterion"] = 2;

    interpret_tool_result(command_call(), result, ledger);

    CHECK(ledger.acceptance_plan()[1].settled);
    CHECK(ledger.acceptance_plan()[1].satisfied);
    // The one it did not name stays open.
    CHECK_FALSE(ledger.acceptance_plan()[0].settled);
}

TEST_CASE("a criterion number out of range settles nothing", "[interpreter][behaviour]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"it builds"}});

    auto result = ran("app.exe", 0, "ok");
    result.structured["expectation_checked"] = true;
    result.structured["expectation_satisfied"] = true;
    result.structured["expectation_description"] = "prints ok";
    result.structured["expectation_criterion"] = 7;

    interpret_tool_result(command_call(), result, ledger);
    CHECK_FALSE(ledger.acceptance_plan()[0].settled);
}

TEST_CASE("a test run that found no tests is not a passing test stage",
          "[interpreter][stages]") {
    VerificationLedger ledger;
    interpret_tool_result(command_call(),
                          ran("ctest", 0, "No tests were found!!!"), ledger);

    CHECK(ledger.status(Stage::Test) == StageStatus::NotApplicable);
    CHECK_FALSE(ledger.holds(Stage::Test));
    CHECK(reports_no_tests_ran("Total Tests: 0"));
    CHECK(reports_no_tests_ran("collected 0 items"));
    CHECK_FALSE(reports_no_tests_ran("Total Tests: 12"));
}

TEST_CASE("writing a file makes the previous build stale", "[interpreter][staleness]") {
    VerificationLedger ledger;
    interpret_tool_result(command_call(), ran("cmake --build build", 0), ledger);
    REQUIRE(ledger.holds(Stage::Build));

    ToolCall write;
    write.id = "call-2";
    write.name = "write_file";
    ToolResult written;
    written.ok = true;
    written.evidence.push_back(Evidence{EvidenceKind::FileWritten, "src/main.cpp", "24 lines",
                                        true, 0});

    interpret_tool_result(write, written, ledger);

    CHECK(ledger.status(Stage::Build) == StageStatus::Stale);
    CHECK(ledger.revision() == 1);
}

TEST_CASE("a cancelled command proves nothing either way", "[interpreter][stages]") {
    VerificationLedger ledger;
    auto result = ran("cmake --build build", 1);
    result.structured["cancelled"] = true;
    interpret_tool_result(command_call(), result, ledger);
    CHECK(ledger.status(Stage::Build) == StageStatus::Unknown);
}

TEST_CASE("a timed-out build is a failure, not a pass", "[interpreter][stages]") {
    VerificationLedger ledger;
    auto result = ran("cmake --build build", 0);
    result.structured["timed_out"] = true;
    interpret_tool_result(command_call(), result, ledger);
    CHECK(ledger.status(Stage::Build) == StageStatus::Fail);
}

TEST_CASE("acceptance criteria arrive through the tool that sets them",
          "[interpreter][acceptance]") {
    VerificationLedger ledger;
    ToolCall call;
    call.id = "call-3";
    call.name = "set_acceptance_criteria";
    ToolResult result;
    result.ok = true;
    result.structured = {{"acceptance_criteria",
                          Json::array({"the project builds", "it prints ten numbers"})}};

    interpret_tool_result(call, result, ledger);
    REQUIRE(ledger.acceptance_plan().size() == 2);
    CHECK(ledger.acceptance_plan()[0].description == "the project builds");
}

TEST_CASE("the definition of done cannot be rewritten once results are in",
          "[interpreter][acceptance]") {
    VerificationLedger ledger;
    ledger.set_acceptance_plan({{"it prints the first ten Fibonacci numbers"}});
    ledger.settle_check(0, false, "E001");

    ToolCall call;
    call.id = "call-4";
    call.name = "set_acceptance_criteria";
    ToolResult result;
    result.ok = true;
    result.structured = {{"acceptance_criteria", Json::array({"it prints some numbers"})}};

    interpret_tool_result(call, result, ledger);

    REQUIRE(ledger.acceptance_plan().size() == 1);
    CHECK(ledger.acceptance_plan()[0].description ==
          "it prints the first ten Fibonacci numbers");
}

TEST_CASE("run_artifact reaches the ledger like any other command",
          "[evidence][regression]") {
    // run_artifact runs a command through exactly the same path and produces
    // exactly the same record; only the way the program was named differs.
    // Excluding it made every behaviour check and every criterion settled
    // through it invisible - a run could verify its work perfectly and be told
    // none of its criteria had been settled. Measured in four runs, each
    // repeating a correct, passing check until it was stopped.
    VerificationLedger ledger;
    ledger.set_acceptance_plan({AcceptanceCheck{"the program prints the first ten"}});

    ToolCall call;
    call.name = "run_artifact";

    ToolResult result;
    result.ok = true;
    result.structured = {{"command", "build/Debug/app.exe"},
                         {"exit_code", 0},
                         {"expectation_checked", true},
                         {"expectation_satisfied", true},
                         {"expectation_description", "the program prints the first ten"},
                         {"expectation_criterion", 1}};

    interpret_tool_result(call, result, ledger, "print the first ten Fibonacci numbers");

    REQUIRE(ledger.acceptance_plan().size() == 1);
    CHECK(ledger.acceptance_plan()[0].settled);
    CHECK(ledger.acceptance_plan()[0].satisfied);
    CHECK(ledger.status(Stage::Behaviour) == StageStatus::Pass);
}
