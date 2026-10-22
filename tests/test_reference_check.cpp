// ---------------------------------------------------------------------------
// Checking the model's expectation against something that is not the model.
//
// Asked for the first ten Fibonacci numbers, a model wrote its own criterion as
// "1, 1, 2, 3, 5, 8, 13, 21, 34, 55". LocalCode would have compared the
// program's output against that, found them equal, and reported success. Every
// check would have been real; the answer would still have been wrong.
//
// The hardest requirement here is silence. This knows almost nothing, and
// where it knows nothing it must say nothing - never "agreed".
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "agent/ReferenceCheck.h"

using namespace localcode;

TEST_CASE("reference counts come from sequence requests rather than word substrings",
          "[reference][audit]") {
    const auto sequence =
        reference_sequence("The height calculation prints the first 12 Fibonacci numbers from 0.");
    REQUIRE(sequence.has_value());
    CHECK(sequence->size() == 12);
    CHECK(sequence->back() == 89);
    CHECK_FALSE(reference_sequence("Compute the Fibonacci height starting from 0").has_value());
}

TEST_CASE("Fibonacci references stop before signed arithmetic overflows", "[reference][audit]") {
    const auto zero = reference_sequence("print the first 93 Fibonacci numbers from 0");
    REQUIRE(zero.has_value());
    CHECK(zero->size() == 93);
    CHECK(zero->back() == 7540113804746346429LL);
    CHECK_FALSE(reference_sequence("print the first 94 Fibonacci numbers from 0").has_value());
    const auto one = reference_sequence("print the first 92 Fibonacci numbers from 1");
    REQUIRE(one.has_value());
    CHECK(one->back() == 7540113804746346429LL);
    CHECK_FALSE(reference_sequence("print the first 93 Fibonacci numbers from 1").has_value());
}

TEST_CASE("plain language statistical assignments are independently checked",
          "[reference][audit]") {
    const auto verdict =
        check_against_reference("print the median of the samples 8, 1, 3, 2", "the median is 3");
    CHECK(verdict.applicable);
    CHECK_FALSE(verdict.agrees);
    CHECK(verdict.conflict.find("2.5") != std::string::npos);
}

TEST_CASE("negative output cannot ground a positive reference sequence", "[reference][audit]") {
    const auto verdict =
        check_against_reference("print the first five squares", "-1 -4 -9 -16 -25");
    CHECK(verdict.applicable);
    CHECK_FALSE(verdict.agrees);
    CHECK_FALSE(verdict.conflict.empty());
}

TEST_CASE("the sequence the request determines is computed", "[reference]") {
    const auto from_zero =
        reference_sequence("print the first ten Fibonacci numbers starting from 0, one per line");
    REQUIRE(from_zero.has_value());
    CHECK(*from_zero == std::vector<long long>{0, 1, 1, 2, 3, 5, 8, 13, 21, 34});

    const auto from_one =
        reference_sequence("print the first ten Fibonacci numbers starting from 1");
    REQUIRE(from_one.has_value());
    CHECK(*from_one == std::vector<long long>{1, 1, 2, 3, 5, 8, 13, 21, 34, 55});
}

TEST_CASE("an ambiguous request gets no opinion", "[reference]") {
    // Both conventions are in use. Picking one would be the same mistake the
    // model made, wearing a different hat.
    CHECK_FALSE(reference_sequence("print the first ten Fibonacci numbers").has_value());
    CHECK_FALSE(reference_sequence("print some Fibonacci numbers from 0").has_value());
}

TEST_CASE("the model's wrong Fibonacci criterion is caught", "[reference][fibonacci]") {
    // The exact situation, from a real run.
    const auto verdict = check_against_reference(
        "Write a C++ program that prints the first ten Fibonacci numbers starting from 0, "
        "one per line.",
        "1, 1, 2, 3, 5, 8, 13, 21, 34, 55");

    REQUIRE(verdict.applicable);
    CHECK_FALSE(verdict.agrees);
    CHECK(verdict.reference == "0 1 1 2 3 5 8 13 21 34");
    INFO(verdict.conflict);
    CHECK(verdict.conflict.find("0 1 1 2 3 5 8 13 21 34") != std::string::npos);
    CHECK(verdict.conflict.find("1 1 2 3 5 8 13 21 34 55") != std::string::npos);
    // It must not simply assert its own answer over the model's.
    CHECK(verdict.conflict.find("Work out which") != std::string::npos);
}

TEST_CASE("a correct expectation is agreed with", "[reference][fibonacci]") {
    const auto verdict = check_against_reference(
        "print the first ten Fibonacci numbers starting from 0", "0 1 1 2 3 5 8 13 21 34");
    REQUIRE(verdict.applicable);
    CHECK(verdict.agrees);
    CHECK(verdict.conflict.empty());
}

TEST_CASE("formatting does not decide agreement", "[reference]") {
    for (const char* shape :
         {"0 1 1 2 3 5 8 13 21 34", "0, 1, 1, 2, 3, 5, 8, 13, 21, 34",
          "0\n1\n1\n2\n3\n5\n8\n13\n21\n34\n", "[0, 1, 1, 2, 3, 5, 8, 13, 21, 34]"}) {
        INFO(shape);
        CHECK(check_against_reference("the first ten Fibonacci numbers from 0", shape).agrees);
    }
}

TEST_CASE("squares and factorials are computed too", "[reference]") {
    const auto squares = reference_sequence("print the first five squares");
    REQUIRE(squares.has_value());
    CHECK(*squares == std::vector<long long>{1, 4, 9, 16, 25});

    const auto factorials = reference_sequence("print the first five factorials");
    REQUIRE(factorials.has_value());
    CHECK(*factorials == std::vector<long long>{1, 2, 6, 24, 120});

    // Not a square root.
    CHECK_FALSE(reference_sequence("print the first five square roots").has_value());
}

TEST_CASE("silence is the normal answer, and never means agreement", "[reference]") {
    // This is the requirement that matters most. Almost every real task is
    // something LocalCode cannot compute, and it must say so by having no
    // opinion rather than by approving.
    for (const char* request :
         {"fix the crash when opening an empty file", "add a dark theme", "make the tests pass",
          "refactor the renderer", "print the current time"}) {
        INFO(request);
        const auto verdict = check_against_reference(request, "anything at all");
        CHECK_FALSE(verdict.applicable);
        CHECK_FALSE(verdict.agrees);
        CHECK(verdict.conflict.empty());
    }
}

TEST_CASE("an expectation with no numbers gets no opinion", "[reference]") {
    // "prints ten lines" says nothing about which ten.
    const auto verdict = check_against_reference("print the first ten Fibonacci numbers from 0",
                                                 "ten lines of output");
    CHECK_FALSE(verdict.applicable);
}

TEST_CASE("an absurd count is not computed", "[reference]") {
    CHECK_FALSE(reference_sequence("print the first 5000 Fibonacci numbers from 0").has_value());
    // 21! overflows a 64-bit integer, so it is not claimed.
    CHECK_FALSE(reference_sequence("print the first 30 factorials").has_value());
}

TEST_CASE("prose that mentions a number is not a claim about the answer",
          "[reference][falsepositive]") {
    // Found by a live run, and it blocked a correct one. The criterion
    // "prints the first ten Fibonacci numbers starting from 0, one per line"
    // yielded the single number 0, which was compared against the ten-value
    // reference and reported as a disagreement. The criterion was right; the
    // check was not. A wrong conflict is worse than no opinion.
    const std::string request =
        "Write a C++ program that prints the first ten Fibonacci numbers starting from 0, "
        "one per line.";

    for (const char* criterion :
         {"C++ program prints first ten Fibonacci numbers starting from 0, one per line",
          "Program output matches the expected Fibonacci sequence", "the program prints 10 lines",
          "prints the sequence beginning at 0"}) {
        INFO(criterion);
        const auto verdict = check_against_reference(request, criterion);
        CHECK_FALSE(verdict.applicable);
        CHECK(verdict.conflict.empty());
    }
}

TEST_CASE("a criterion that does spell the sequence out is still compared",
          "[reference][falsepositive]") {
    // The narrowing must not have blunted the thing it exists for.
    const std::string request =
        "Write a C++ program that prints the first ten Fibonacci numbers starting from 0.";

    const auto wrong = check_against_reference(
        request, "Program output matches: 1, 1, 2, 3, 5, 8, 13, 21, 34, 55");
    REQUIRE(wrong.applicable);
    CHECK_FALSE(wrong.agrees);

    const auto right =
        check_against_reference(request, "Program output matches: 0, 1, 1, 2, 3, 5, 8, 13, 21, 34");
    REQUIRE(right.applicable);
    CHECK(right.agrees);
}

TEST_CASE("prose numbers around the sequence are not part of the claim",
          "[reference][regression]") {
    // A criterion states its sequence inside a sentence that has numbers of
    // its own. "starting from 0" contributed an eleventh number, the whole
    // list stopped matching, and a correct run was blocked by a disagreement
    // with a sequence the criterion had just quoted correctly.
    const auto verdict = check_against_reference(
        "Write a C++ program that prints the first ten Fibonacci numbers starting from 0, "
        "one per line.",
        "C++ program prints the first ten Fibonacci numbers (0, 1, 1, 2, 3, 5, 8, 13, 21, 34) "
        "starting from 0, one per line");
    INFO(verdict.conflict);
    CHECK(verdict.applicable);
    CHECK(verdict.agrees);
    CHECK(verdict.conflict.empty());
}

TEST_CASE("a genuinely wrong sequence is still caught", "[reference][regression]") {
    // The guard above must not turn the checker off. A criterion claiming the
    // other Fibonacci convention still disagrees with a request that asked for
    // this one.
    const auto verdict =
        check_against_reference("print the first ten Fibonacci numbers starting from 0",
                                "prints 1, 1, 2, 3, 5, 8, 13, 21, 34, 55 one per line");
    CHECK(verdict.applicable);
    CHECK_FALSE(verdict.agrees);
    CHECK_FALSE(verdict.conflict.empty());
}

TEST_CASE("the median of an even sample set is computed, not accepted", "[reference][statistics]") {
    // The classic plausible-wrong-answer: taking the upper middle value of
    // 1,2,3,8 gives 3 where the answer is 2.5, and 3 is a real number from the
    // data. A criterion claiming it would settle cleanly against a real program.
    const std::string request =
        "The program prints the median and mean of the samples 8, 1, 3, 2. Fix the build, "
        "then check the numbers it prints are right.";

    const auto wrong = check_against_reference(request, "prints median=3 and mean=3.5");
    INFO(wrong.conflict);
    CHECK(wrong.applicable);
    CHECK_FALSE(wrong.agrees);
    CHECK(wrong.conflict.find("2.5") != std::string::npos);

    const auto wrong_mean = check_against_reference(request, "prints median=2.5 and mean=2.5");
    CHECK(wrong_mean.applicable);
    CHECK_FALSE(wrong_mean.agrees);

    // The right answers raise no conflict.
    const auto right = check_against_reference(request, "prints median=2.5 and mean=3.5");
    CHECK(right.conflict.empty());
}

TEST_CASE("statistics are only checked when the request determines them",
          "[reference][statistics]") {
    // Silence is the normal answer. A wrong conflict blocks a correct run,
    // which is worse than having no opinion.
    CHECK(check_against_reference("compute the median of the data", "median=3").conflict.empty());
    CHECK(check_against_reference("print the samples 8, 1, 3, 2", "median=3").conflict.empty());
    // The request lists samples and asks for a median, but the expectation
    // states no value to compare.
    CHECK(check_against_reference("print the median of the samples 8, 1, 3, 2",
                                  "the program prints a median")
              .conflict.empty());
}

TEST_CASE("explicit statistical samples stop before later prose numbers",
          "[reference][statistics][recovery][candidate6]") {
    for (const auto* request :
         {"Find median of samples: 8, 1, 3, 2. The answer 3 is wrong; issue 42 is unrelated.",
          "Find median of samples [8,1,3,2], version 19, examples 10 and 20.",
          "Find median of samples {8; 1; 3; 2}; later values 10 and 20 are metadata.",
          "Find median of samples 8 1 3 2. A separate version is 9.",
          "Find median of samples (8, 1, 3, 2). There are 99 other cases."}) {
        INFO(request);
        const auto good = check_against_reference(request, "median=2.5");
        CHECK(good.conflict.empty());
        const auto bad = check_against_reference(request, "median=3");
        CHECK(bad.applicable);
        CHECK_FALSE(bad.agrees);
        CHECK(bad.conflict.find("is 2.5,") != std::string::npos);
    }
    const std::string actual_objective =
        "Independent verification misses a plain-language statistical claim: for samples "
        "8, 1, 3, 2, the expectation 'the median is 3' raises no conflict even though the "
        "median is 2.5. Reproduce the problem, make the smallest implementation fix, add a "
        "focused regression test, and run the relevant tests.";
    CHECK(check_against_reference(actual_objective, "median=2.5").conflict.empty());
    CHECK(check_against_reference(actual_objective, "median=3").conflict.find("is 2.5,") !=
          std::string::npos);
}

TEST_CASE("bounded sample grammar retains decimals and declines ambiguous datasets",
          "[reference][statistics][recovery][candidate6]") {
    for (const auto* request :
         {"Find median of samples: -5.5, -1.5, +3.5, +7.5. Version 20 follows.",
          "Find median of samples [-5.5; -1.5; 3.5; 7.5], issue 100."}) {
        INFO(request);
        const auto bad = check_against_reference(request, "median=2");
        CHECK(bad.applicable);
        CHECK_FALSE(bad.agrees);
        CHECK(bad.conflict.find("is 1,") != std::string::npos);
    }
    for (const auto* request :
         {"Find median of samples 1-4", "Find median of samples 1, 2, 3...",
          "Find median of samples [1,2,3 or 4]",
          "Find median of samples collected in 2024: three groups 8, 1, 3, 2",
          "Find median of subsamples 8, 1, 3, 2", "Find median of samples [1kg, 2kg, 3kg]",
          "Find median of samples [1e3, 2e3, 3e3]", "Find median of samples [1,2,3"}) {
        INFO(request);
        CHECK_FALSE(check_against_reference(request, "median=99").applicable);
    }
}

TEST_CASE("hypothetical statistical inputs are not claimed output values",
          "[reference][statistics][falsepositive][recovery][candidate6]") {
    const std::string request = "Compute the median of samples 8, 1, 3, 2.";
    for (const auto* description :
         {"when expectation says median=3, a conflict is raised", "\"median=3\" must be rejected",
          "median=3 must be rejected", "'median=3'", "rejects the input 'median=3' with a conflict",
          "The text \"median is 3\" is an intentionally incorrect input",
          "If the input says median=3, report the mismatch rather than accepting it"}) {
        INFO(description);
        const auto verdict = check_against_reference(request, description);
        CHECK_FALSE(verdict.applicable);
        CHECK(verdict.conflict.empty());
    }
    for (const auto* output : {"prints median=3", "prints \"median=3\"", "median is 3"}) {
        INFO(output);
        const auto wrong = check_against_reference(request, output);
        CHECK(wrong.applicable);
        CHECK_FALSE(wrong.agrees);
    }
}
