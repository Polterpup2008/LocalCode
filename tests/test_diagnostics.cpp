// ---------------------------------------------------------------------------
// Reading a build log.
//
// The point of parsing rather than line-matching: a model handed forty copies
// of the same missing-header error spends its turn counting them, and a model
// handed a raw MSVC line often invents a plausible file and line number from
// it. Fields cannot be misread the way prose can.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "tools/Diagnostics.h"

using namespace localcode;

TEST_CASE("an MSVC compiler error is parsed into fields", "[diagnostics][msvc]") {
    const auto set = parse_diagnostics(
        R"(src\main.cpp(12,5): error C2065: 'x': undeclared identifier)");

    REQUIRE(set.items.size() == 1);
    const auto& item = set.items[0];
    CHECK(item.severity == DiagnosticSeverity::Error);
    CHECK(item.origin == DiagnosticOrigin::Compiler);
    CHECK(item.code == "C2065");
    CHECK(item.file == R"(src\main.cpp)");
    CHECK(item.line == 12);
    CHECK(item.column == 5);
    CHECK(item.message == "'x': undeclared identifier");
}

TEST_CASE("an MSVC warning without a column still parses", "[diagnostics][msvc]") {
    const auto set = parse_diagnostics(
        R"(src\app.cpp(40): warning C4100: 'argc': unreferenced formal parameter)");
    REQUIRE(set.items.size() == 1);
    CHECK(set.items[0].severity == DiagnosticSeverity::Warning);
    CHECK(set.items[0].line == 40);
    CHECK(set.items[0].column == 0);
    CHECK(set.error_count() == 0);
    CHECK(set.warning_count() == 1);
}

TEST_CASE("a linker error is not filed as a compiler error", "[diagnostics][msvc]") {
    // Different problem, different fix. Saying which saves a turn spent
    // looking for a syntax error that is not there.
    const auto set = parse_diagnostics(
        "main.obj : error LNK2019: unresolved external symbol foo referenced in function main\n"
        "build\\app.exe : fatal error LNK1120: 1 unresolved externals");

    REQUIRE(set.items.size() == 2);
    CHECK(set.items[0].origin == DiagnosticOrigin::Linker);
    CHECK(set.items[0].code == "LNK2019");
    CHECK(set.items[0].file == "main.obj");
    CHECK(set.items[0].line == 0);
    CHECK(set.items[1].code == "LNK1120");
}

TEST_CASE("gcc and clang locations are parsed", "[diagnostics][gcc]") {
    const auto set = parse_diagnostics(
        "src/main.cpp:12:5: error: expected ';' before '}' token\n"
        "src/util.cpp:7:9: warning: unused variable 'n' [-Wunused-variable]");

    REQUIRE(set.items.size() == 2);
    CHECK(set.items[0].file == "src/main.cpp");
    CHECK(set.items[0].line == 12);
    CHECK(set.items[0].column == 5);
    CHECK(set.items[0].severity == DiagnosticSeverity::Error);
    CHECK(set.items[1].severity == DiagnosticSeverity::Warning);
    CHECK(set.items[1].code == "-Wunused-variable");
}

TEST_CASE("a Windows drive letter is not mistaken for a line number",
          "[diagnostics][msvc]") {
    // "C:\src\main.cpp(12,5): error ..." has a colon in it that the gcc form
    // would happily read as a separator.
    const auto set = parse_diagnostics(
        R"(C:\work\src\main.cpp(3,1): error C1004: unexpected end-of-file found)");
    REQUIRE(set.items.size() == 1);
    CHECK(set.items[0].file == R"(C:\work\src\main.cpp)");
    CHECK(set.items[0].line == 3);
}

TEST_CASE("CMake errors carry their location when they have one",
          "[diagnostics][cmake]") {
    const auto set = parse_diagnostics(
        "CMake Error at CMakeLists.txt:14 (add_executable):\n"
        "CMake Warning: something less important");

    REQUIRE(set.items.size() == 2);
    CHECK(set.items[0].origin == DiagnosticOrigin::BuildSystem);
    CHECK(set.items[0].file == "CMakeLists.txt");
    CHECK(set.items[0].line == 14);
    CHECK(set.items[1].severity == DiagnosticSeverity::Warning);
}

TEST_CASE("a failed build step is a diagnostic", "[diagnostics][build]") {
    const auto set = parse_diagnostics(
        "FAILED: src/CMakeFiles/app.dir/main.cpp.obj\n"
        "make[1]: *** [Makefile:12: all] Error 2");
    REQUIRE(set.items.size() == 2);
    CHECK(set.items[0].origin == DiagnosticOrigin::BuildSystem);
    CHECK(set.items[1].origin == DiagnosticOrigin::BuildSystem);
}

TEST_CASE("crashes and test failures are recognised", "[diagnostics][runtime]") {
    const auto set = parse_diagnostics(
        "Segmentation fault (core dumped)\n"
        "The following tests FAILED:\n"
        "3 tests failed");
    CHECK(set.error_count() == 3);
}

TEST_CASE("a clean build produces nothing", "[diagnostics]") {
    const auto set = parse_diagnostics(
        "[1/2] Building CXX object src/main.cpp.obj\n"
        "[2/2] Linking CXX executable app.exe\n"
        "-- Build files have been written to: /tmp/build");
    CHECK(set.empty());
    CHECK(set.summarise().empty());
}

TEST_CASE("the same error from forty files is reported once with a count",
          "[diagnostics][summary]") {
    std::string log;
    for (int i = 0; i < 40; ++i) {
        log += "src/file" + std::to_string(i) +
               ".cpp(1,10): fatal error C1083: Cannot open include file: 'missing.h'\n";
    }
    const auto set = parse_diagnostics(log);
    CHECK(set.items.size() == 40);
    CHECK(set.error_count() == 40);

    const auto summary = set.summarise();
    INFO(summary);
    CHECK(summary.find("40 errors") != std::string::npos);
    CHECK(summary.find("(40 times") != std::string::npos);
    // One representative line, not forty.
    CHECK(summary.find("src/file39.cpp") == std::string::npos);
}

TEST_CASE("errors are reported before warnings", "[diagnostics][summary]") {
    const auto set = parse_diagnostics(
        "a.cpp(1): warning C4100: unreferenced parameter\n"
        "b.cpp(2): error C2065: undeclared identifier\n");

    const auto summary = set.summarise();
    INFO(summary);
    const auto error_at = summary.find("C2065");
    const auto warning_at = summary.find("C4100");
    REQUIRE(error_at != std::string::npos);
    REQUIRE(warning_at != std::string::npos);
    CHECK(error_at < warning_at);
}

TEST_CASE("the summary is capped and says so", "[diagnostics][summary]") {
    std::string log;
    for (int i = 0; i < 30; ++i) {
        log += "src/main.cpp(" + std::to_string(i + 1) + ",1): error C" +
               std::to_string(2000 + i) + ": problem number " + std::to_string(i) + "\n";
    }
    const auto summary = parse_diagnostics(log).summarise(5);
    INFO(summary);
    CHECK(summary.find("and 25 more distinct") != std::string::npos);
}

TEST_CASE("a rendered diagnostic reads the way the compiler wrote it",
          "[diagnostics][rendering]") {
    Diagnostic item;
    item.file = R"(src\main.cpp)";
    item.line = 12;
    item.column = 5;
    item.code = "C2065";
    item.message = "'x': undeclared identifier";
    CHECK(item.describe() == R"(src\main.cpp(12,5): error C2065: 'x': undeclared identifier)");
}
