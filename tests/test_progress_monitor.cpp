// No-progress detection: the guard against a model looping forever.
#include <catch2/catch_test_macros.hpp>

#include "agent/ProgressMonitor.h"

using namespace localcode;

TEST_CASE("changed source read arguments without new returned evidence do not reset a stall",
          "[progress][navigation][recovery]") {
    ProgressMonitor monitor;
    bool stalled = false;
    for (int i = 0; i < 12; ++i) {
        monitor.note_iteration();
        const auto assessment =
            monitor.observe("read_file", {{"max_bytes", 1000 + i}}, "same bounded source body",
                            true, false, false, false);
        stalled = stalled || assessment.verdict == ProgressVerdict::Stalled;
    }
    CHECK(stalled);
}

TEST_CASE("new source evidence is progress even when arguments and short summary are unchanged",
          "[progress][navigation][recovery]") {
    ProgressMonitor monitor;
    for (int i = 0; i < 12; ++i) {
        monitor.note_iteration();
        const auto assessment =
            monitor.observe("read_file", {{"path", "module.cpp"}},
                            "same header and first source lines", true, false, false, true);
        CHECK(assessment.verdict == ProgressVerdict::Fine);
    }
}

TEST_CASE("invented file patterns with zero returned files do not reset a stall",
          "[progress][navigation][recovery]") {
    ProgressMonitor monitor;
    bool stalled = false;
    for (int i = 0; i < 12; ++i) {
        monitor.note_iteration();
        const auto assessment =
            monitor.observe("search_files", {{"pattern", "missing" + std::to_string(i)}},
                            "Found 0 file(s) matching the pattern", true, false);
        stalled = stalled || assessment.verdict == ProgressVerdict::Stalled;
    }
    CHECK(stalled);
}

TEST_CASE("normal varied work is not flagged", "[progress]") {
    ProgressMonitor monitor;

    CHECK(
        monitor.observe("read_file", Json{{"path", "a.cpp"}}, "read a.cpp", true, false).verdict ==
        ProgressVerdict::Fine);
    CHECK(
        monitor.observe("read_file", Json{{"path", "b.cpp"}}, "read b.cpp", true, false).verdict ==
        ProgressVerdict::Fine);
    CHECK(
        monitor.observe("replace_in_file", Json{{"path", "a.cpp"}}, "edited", true, true).verdict ==
        ProgressVerdict::Fine);
    CHECK(monitor.observe("run_command", Json{{"command", "cmake"}}, "OK exit 0", true, false)
              .verdict == ProgressVerdict::Fine);
}

TEST_CASE("the same call with the same result is caught", "[progress]") {
    ProgressMonitor monitor;
    const Json arguments{{"command", "cmake --build build"}};

    CHECK(monitor.observe("run_command", arguments, "FAILED exit 1", false, false).verdict ==
          ProgressVerdict::Fine);
    CHECK(monitor.observe("run_command", arguments, "FAILED exit 1", false, false).verdict ==
          ProgressVerdict::Fine);

    auto third = monitor.observe("run_command", arguments, "FAILED exit 1", false, false);
    CHECK(third.verdict != ProgressVerdict::Fine);
    CHECK_FALSE(third.guidance.empty());
}

TEST_CASE("cosmetic differences do not hide a repeat", "[progress]") {
    ProgressMonitor monitor;

    // Whitespace and key order must not make these look like different calls.
    monitor.observe("run_command", Json{{"a", 1}, {"b", "x  y"}}, "same", false, false);
    monitor.observe("run_command", Json{{"b", "x y"}, {"a", 1}}, "same", false, false);
    auto third =
        monitor.observe("run_command", Json{{"a", 1}, {"b", "x   y"}}, "same", false, false);
    CHECK(third.verdict != ProgressVerdict::Fine);
}

TEST_CASE("varying numbers in a result do not hide a repeated failure", "[progress]") {
    ProgressMonitor monitor;
    const Json arguments{{"command", "ctest"}};

    monitor.observe("run_command", arguments, "FAILED after 1023 ms", false, false);
    monitor.observe("run_command", arguments, "FAILED after 1187 ms", false, false);
    auto third = monitor.observe("run_command", arguments, "FAILED after 998 ms", false, false);
    CHECK(third.verdict != ProgressVerdict::Fine);
}

TEST_CASE("oscillation between two edits is caught", "[progress]") {
    ProgressMonitor monitor;
    const Json a{{"path", "x.cpp"}, {"find", "alpha"}};
    const Json b{{"path", "x.cpp"}, {"find", "beta"}};

    monitor.observe("replace_in_file", a, "edited", true, true);
    monitor.observe("replace_in_file", b, "edited", true, true);
    monitor.observe("replace_in_file", a, "edited", true, true);
    auto fourth = monitor.observe("replace_in_file", b, "edited", true, true);

    CHECK(fourth.verdict == ProgressVerdict::Oscillating);
    CHECK(fourth.guidance.find("alternating") != std::string::npos);
}

TEST_CASE("many turns with no file change is a stall", "[progress]") {
    ProgressMonitor monitor;
    for (int i = 0; i < 25; ++i) {
        monitor.note_iteration();
        auto assessment = monitor.observe("search_text", Json{{"query", "q" + std::to_string(i)}},
                                          "no matches " + std::to_string(i), true, false);
        if (assessment.verdict == ProgressVerdict::Stalled ||
            assessment.verdict == ProgressVerdict::Exhausted) {
            SUCCEED("stall detected after " << i << " turns");
            return;
        }
    }
    FAIL("a long run with no file changes was never flagged");
}

TEST_CASE("repeated warnings eventually stop the run", "[progress]") {
    ProgressMonitor monitor;
    monitor.set_max_warnings(2);
    const Json arguments{{"command", "make"}};

    bool stopped = false;
    for (int i = 0; i < 20 && !stopped; ++i) {
        auto assessment = monitor.observe("run_command", arguments, "FAILED", false, false);
        if (assessment.should_stop) {
            stopped = true;
            CHECK(assessment.verdict == ProgressVerdict::Exhausted);
            CHECK_FALSE(assessment.guidance.empty());
        }
    }
    CHECK(stopped);
}

TEST_CASE("reset clears the accumulated state", "[progress]") {
    ProgressMonitor monitor;
    const Json arguments{{"command", "x"}};
    for (int i = 0; i < 5; ++i)
        monitor.observe("run_command", arguments, "FAILED", false, false);
    CHECK(monitor.warnings_issued() > 0);

    monitor.reset();
    CHECK(monitor.warnings_issued() == 0);
    CHECK(monitor.observe("run_command", arguments, "FAILED", false, false).verdict ==
          ProgressVerdict::Fine);
}

TEST_CASE("thresholds are configurable", "[progress]") {
    ProgressMonitor monitor;
    monitor.set_identical_call_threshold(2);

    const Json arguments{{"x", 1}};
    CHECK(monitor.observe("t", arguments, "same", true, false).verdict == ProgressVerdict::Fine);
    CHECK(monitor.observe("t", arguments, "same", true, false).verdict != ProgressVerdict::Fine);
}

TEST_CASE("verifying is progress even though it changes no files", "[progress][regression]") {
    // The monitor counted only file changes as progress, so a model that had
    // finished building and was correctly settling its acceptance criteria was
    // told it had "taken 14 turns without changing any file - if you have
    // finished investigating, make the change", and then stopped for
    // repeating itself. Measured in two runs that were three checks from done.
    ProgressMonitor monitor;

    // Twelve turns of checks, each settling a criterion. Nothing on disk moves.
    ProgressAssessment last;
    for (int i = 0; i < 16; ++i) {
        monitor.note_iteration();
        last = monitor.observe("run_command", Json{{"executable", "app.exe"}, {"criterion", i}},
                               "OK app.exe (exit 0)  [behaviour verified]",
                               /*succeeded=*/true, /*changed_files=*/false,
                               /*advanced_verification=*/true);
    }
    CHECK(last.verdict == ProgressVerdict::Fine);
}

TEST_CASE("turns that settle nothing and change nothing are still caught",
          "[progress][regression]") {
    // The relaxation above must not switch the check off. A run that is
    // neither changing files nor settling anything is stalled, and saying so
    // is the whole point of this monitor.
    ProgressMonitor monitor;

    ProgressAssessment last;
    for (int i = 0; i < 16; ++i) {
        monitor.note_iteration();
        last = monitor.observe("run_command", Json{{"command", "echo " + std::to_string(i)}},
                               "OK exit 0", /*succeeded=*/true,
                               /*changed_files=*/false, /*advanced_verification=*/false);
    }
    CHECK(last.verdict != ProgressVerdict::Fine);
}

TEST_CASE("distinct successful repository reads advance investigation", "[progress][regression]") {
    ProgressMonitor monitor;
    for (int i = 0; i < 20; ++i) {
        monitor.note_iteration();
        const auto result =
            monitor.observe("read_file", Json{{"path", "src/module-" + std::to_string(i) + ".cpp"}},
                            "Read source (30 lines)", true, false);
        CHECK(result.verdict == ProgressVerdict::Fine);
    }
}

TEST_CASE("discovery does not excuse repeated reads or failures", "[progress][regression]") {
    ProgressMonitor monitor;
    for (int i = 0; i < 3; ++i) {
        monitor.note_iteration();
        monitor.observe("read_file", Json{{"path", "same.cpp"}}, "Read same.cpp", true, false);
    }
    CHECK(monitor.warnings_issued() == 1);
    monitor.reset();
    for (int i = 0; i < 15; ++i) {
        monitor.note_iteration();
        monitor.observe("read_file", Json{{"path", std::to_string(i)}}, "missing", false, false);
    }
    CHECK(monitor.warnings_issued() > 0);
}

TEST_CASE("trusted process outcome identity also controls identical-call comparison",
          "[progress][process][recovery][candidate6]") {
    ProgressMonitor monitor;
    const Json arguments = {{"executable", "diagnostic"}, {"arguments", {"same"}}};
    for (int code : {2, 3, 4}) {
        INFO(code);
        const auto identity =
            Json{{"exit_code", code}, {"timed_out", false}, {"cancelled", false}}.dump();
        const auto assessment =
            monitor.observe("run_command", arguments, "Command exit " + std::to_string(code), false,
                            false, false, std::nullopt, identity);
        CHECK(assessment.verdict == ProgressVerdict::Fine);
    }
    monitor.reset();
    ProgressAssessment repeated;
    for (int i = 0; i < 3; ++i)
        repeated = monitor.observe("run_command", arguments, "Command exit 2", false, false, false,
                                   std::nullopt, "exit_code=2");
    CHECK(repeated.verdict == ProgressVerdict::RepeatedIdenticalCall);
}
