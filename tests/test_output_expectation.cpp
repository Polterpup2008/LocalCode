// ---------------------------------------------------------------------------
// The comparison LocalCode makes for itself.
//
// The model says what the output has to be; this decides whether it was. The
// tests below are written around the failure that motivated it - a program
// that printed 1 3 6 12 24 48 96 192 384 768 and was reported as printing the
// first ten Fibonacci numbers.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "tools/OutputExpectation.h"

using namespace localcode;

namespace {

Json expectation_json(std::initializer_list<std::pair<const std::string, Json>> fields) {
    Json json = Json::object();
    for (const auto& [key, value] : fields)
        json[key] = value;
    return json;
}

OutputExpectation parse(const Json& json) {
    auto parsed = OutputExpectation::from_json(json);
    REQUIRE(parsed.ok());
    return *parsed;
}

}   // namespace

TEST_CASE("whitespace is collapsed before comparing", "[expectation]") {
    CHECK(normalise_whitespace("  0 1 1\n2   3\t5  ") == "0 1 1 2 3 5");
    CHECK(normalise_whitespace("") == "");
    CHECK(normalise_whitespace("   \n\t ") == "");
}

TEST_CASE("the Fibonacci output is caught", "[expectation][fibonacci]") {
    const auto expectation =
        parse(expectation_json({{"description", "prints the first ten Fibonacci numbers"},
                                {"stdout_equals", "0 1 1 2 3 5 8 13 21 34"}}));

    const auto outcome =
        evaluate_expectation(expectation, "1 3 6 12 24 48 96 192 384 768\n", "", 0);

    REQUIRE(outcome.evaluated);
    CHECK_FALSE(outcome.satisfied);
    REQUIRE(outcome.failures.size() == 1);
    // Both sides are in the failure text, so the discrepancy can be seen rather
    // than taken on trust.
    CHECK(outcome.failures[0].find("0 1 1 2 3 5 8 13 21 34") != std::string::npos);
    CHECK(outcome.failures[0].find("1 3 6 12 24 48 96 192 384 768") != std::string::npos);
}

TEST_CASE("the right output is accepted whatever shape it is printed in",
          "[expectation][fibonacci]") {
    const auto expectation =
        parse(expectation_json({{"description", "prints the first ten Fibonacci numbers"},
                                {"stdout_equals", "0 1 1 2 3 5 8 13 21 34"}}));

    const auto outcome =
        evaluate_expectation(expectation, "0\n1\n1\n2\n3\n5\n8\n13\n21\n34\n", "", 0);

    CHECK(outcome.satisfied);
    CHECK(outcome.failures.empty());
}

TEST_CASE("exit code zero does not satisfy an expectation about output", "[expectation]") {
    // The whole point: the program succeeded and still did the wrong thing.
    const auto expectation = parse(expectation_json(
        {{"description", "prints the answer"}, {"stdout_contains", Json::array({"42"})}}));

    const auto outcome = evaluate_expectation(expectation, "41\n", "", 0);
    CHECK_FALSE(outcome.satisfied);
}

TEST_CASE("a single string is accepted where a list belongs", "[expectation][parsing]") {
    const auto expectation =
        parse(expectation_json({{"description", "greets"}, {"stdout_contains", "hello"}}));
    REQUIRE(expectation.stdout_contains.size() == 1);
    CHECK(evaluate_expectation(expectation, "well, hello there", "", 0).satisfied);
}

TEST_CASE("an expectation that checks nothing is refused", "[expectation][parsing]") {
    auto parsed = OutputExpectation::from_json(expectation_json({{"description", "it works"}}));
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().message.find("nothing to check") != std::string::npos);
}

TEST_CASE("an expectation without a description describes itself", "[expectation][parsing]") {
    // Recording "behaviour verified" with no statement of what was verified
    // would be a verdict nobody can audit - so the statement is written from
    // the comparison rather than the comparison being refused.
    //
    // This is not a relaxation of the audit. A derived description is taken
    // from the check that actually ran, so unlike a sentence supplied by the
    // caller it cannot describe something other than what was compared. What
    // is lost is the link to the task's own wording, and that link is carried
    // by the explicit `criterion` field instead.
    auto parsed =
        OutputExpectation::from_json(expectation_json({{"stdout_contains", Json::array({"ok"})}}));
    REQUIRE(parsed.ok());
    CHECK(parsed->description_derived);
    CHECK(parsed->description.find(R"(stdout contains "ok")") != std::string::npos);
}

TEST_CASE("a broken regular expression is reported, not ignored", "[expectation][parsing]") {
    auto parsed = OutputExpectation::from_json(
        expectation_json({{"description", "matches"}, {"stdout_matches", "([unclosed"}}));
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().message.find("regular expression") != std::string::npos);
}

TEST_CASE("absence is checkable too", "[expectation]") {
    const auto expectation = parse(expectation_json(
        {{"description", "runs without warnings"}, {"stdout_absent", Json::array({"warning"})}}));

    CHECK(evaluate_expectation(expectation, "all good\n", "", 0).satisfied);

    const auto bad = evaluate_expectation(expectation, "warning: unused variable\n", "", 0);
    CHECK_FALSE(bad.satisfied);
    CHECK(bad.failures[0].find("was not supposed to contain") != std::string::npos);
}

TEST_CASE("every stated check is evaluated, not just the first", "[expectation]") {
    const auto expectation =
        parse(expectation_json({{"description", "prints two things and exits cleanly"},
                                {"exit_code", 0},
                                {"stdout_contains", Json::array({"alpha", "beta"})}}));

    const auto outcome = evaluate_expectation(expectation, "alpha only\n", "", 3);
    REQUIRE(outcome.evaluated);
    CHECK_FALSE(outcome.satisfied);
    CHECK(outcome.passed.size() == 1);     // alpha
    CHECK(outcome.failures.size() == 2);   // exit code, and beta
}

TEST_CASE("stderr can be the stream under test", "[expectation]") {
    const auto expectation =
        parse(expectation_json({{"description", "reports the bad argument on stderr"},
                                {"check_stderr", true},
                                {"stdout_contains", Json::array({"unknown option"})}}));

    CHECK(evaluate_expectation(expectation, "", "unknown option --qq\n", 2).satisfied);
    CHECK_FALSE(evaluate_expectation(expectation, "unknown option --qq\n", "", 2).satisfied);
}

TEST_CASE("no expectation means nothing was evaluated", "[expectation]") {
    // Absence must read as "not checked", never as "checked and fine".
    OutputExpectation none;
    const auto outcome = evaluate_expectation(none, "anything", "", 0);
    CHECK_FALSE(outcome.evaluated);
    CHECK_FALSE(outcome.satisfied);
}

TEST_CASE("the criterion a check settles is named by number", "[expectation]") {
    const auto expectation = parse(expectation_json({{"description", "prints the total"},
                                                     {"criterion", 2},
                                                     {"stdout_contains", Json::array({"total"})}}));
    CHECK(expectation.criterion == 2);

    const auto unnumbered = parse(expectation_json(
        {{"description", "prints the total"}, {"stdout_contains", Json::array({"total"})}}));
    CHECK(unnumbered.criterion == 0);
}

TEST_CASE("one comparison deduplicates criteria and refuses malformed positive bindings",
          "[expectation][multiple-criteria]") {
    const auto valid = OutputExpectation::from_json(
        Json{{"stdout_equals", "fixture: PASS"}, {"criterion", Json::array({1, 3, 1})}});
    REQUIRE(valid);
    CHECK(valid->criterion == 1);
    CHECK(valid->criteria == std::vector<int>{1, 3});
    for (const auto& invalid : {Json::array(), Json::array({1, 33}), Json::array({1, 2.5}),
                                Json::array({1, "2"}), Json::array({1, true})}) {
        INFO(invalid.dump());
        CHECK_FALSE(OutputExpectation::from_json(
            Json{{"stdout_equals", "fixture: PASS"}, {"criterion", invalid}}));
    }
}

TEST_CASE("a comparison without a description is completed, not refused",
          "[expectation][regression]") {
    // Measured at the last step of an otherwise clean run: the program was
    // written, built and run, and the expectation named the exact output.
    // LocalCode refused it twice for want of a label and the run ended one
    // call short of verifying itself. The comparison was complete; only the
    // sentence describing it was missing, and that is derivable.
    const auto parsed =
        OutputExpectation::from_json(Json{{"stdout_equals", "0\n1\n1\n2\n3\n5\n8\n13\n21\n34\n"}});
    REQUIRE(parsed);
    CHECK(parsed->has_any_check());
    CHECK(parsed->description_derived);
    // Derived from the comparison, so it describes what is actually checked.
    CHECK(parsed->description.find("stdout is") != std::string::npos);
    CHECK(parsed->description.find("0") != std::string::npos);

    // A description that was given is kept exactly, and not marked derived.
    const auto given =
        OutputExpectation::from_json(Json{{"description", "prints the first ten Fibonacci numbers"},
                                          {"stdout_equals", "0 1 1 2 3 5 8 13 21 34"}});
    REQUIRE(given);
    CHECK(given->description == "prints the first ten Fibonacci numbers");
    CHECK_FALSE(given->description_derived);
}

TEST_CASE("an expectation that checks nothing is still refused", "[expectation][regression]") {
    // The relaxation above must not become "anything goes". An expectation
    // with no comparison in it would record a behaviour check that checked
    // nothing, which is the failure this whole subsystem exists to prevent.
    const auto empty = OutputExpectation::from_json(Json{{"description", "it works"}});
    CHECK_FALSE(empty);
}
