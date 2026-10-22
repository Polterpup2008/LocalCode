#include "OutputExpectation.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "../util/Strings.h"

namespace localcode {
namespace {

// Small models write a single string where an array belongs about as often as
// they get it right, so both are accepted.
std::vector<std::string> string_or_array(const Json& json, std::string_view key) {
    std::vector<std::string> values;
    if (!json.is_object() || !json.contains(key)) return values;
    const auto& field = json.at(std::string{key});
    if (field.is_string()) {
        values.push_back(field.get<std::string>());
    } else if (field.is_array()) {
        for (const auto& item : field) {
            if (item.is_string())
                values.push_back(item.get<std::string>());
            else if (item.is_number())
                values.push_back(Json(item).dump());
        }
    } else if (field.is_number()) {
        values.push_back(field.dump());
    }
    return values;
}

std::string excerpt_for_report(std::string_view text) {
    const auto trimmed = strings::trim(text);
    if (trimmed.empty()) return "(nothing)";
    return strings::utf8_truncate(trimmed, 400);
}

}   // namespace

std::string normalise_whitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const unsigned char c : text) {
        if (std::isspace(c) != 0) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

Result<OutputExpectation> OutputExpectation::from_json(const Json& json) {
    OutputExpectation expectation;
    if (json.is_null()) return expectation;
    if (!json.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "'expect' must be an object",
                          "For example: {\"description\": \"prints the first ten Fibonacci "
                          "numbers\", \"stdout_equals\": \"0 1 1 2 3 5 8 13 21 34\"}");
    }
    expectation.present = true;
    expectation.description = strings::trim(json_string(json, "description"));
    expectation.use_stderr = json_bool(json, "check_stderr", false);
    // One number, or several. "criterion": [1, 3] says this check settles both.
    if (json.contains("criterion") && json.at("criterion").is_array()) {
        if (json.at("criterion").empty() || json.at("criterion").size() > 32)
            return make_error(ErrorCode::InvalidArgument,
                              "'criterion' must contain 1 to 32 criterion numbers");
        for (const auto& entry : json.at("criterion")) {
            // Retain the legacy contract for nonpositive integer placeholders:
            // they name no criterion. Other malformed bindings are refused.
            if (entry.is_number_integer() && entry <= 0) continue;
            if (!entry.is_number_integer() || entry > 32)
                return make_error(ErrorCode::InvalidArgument,
                                  "every 'criterion' array entry must be an integer from 1 to 32");
            const int value = entry.get<int>();
            if (std::find(expectation.criteria.begin(), expectation.criteria.end(), value) ==
                expectation.criteria.end())
                expectation.criteria.push_back(value);
        }
        if (!expectation.criteria.empty()) expectation.criterion = expectation.criteria.front();
    } else {
        expectation.criterion = static_cast<int>(json_int(json, "criterion", 0));
        if (expectation.criterion < 0) expectation.criterion = 0;
        if (expectation.criterion > 0) expectation.criteria.push_back(expectation.criterion);
    }
    // A criterion named by its wording rather than its number. json_int reads
    // "2" but not "Program runs and outputs the correct sequence", and the
    // fallback used to be a silent zero - the check ran, passed, settled
    // nothing, and said nothing about why.
    if (expectation.criterion == 0 && json.contains("criterion") &&
        json.at("criterion").is_string()) {
        expectation.criterion_text = strings::trim(json.at("criterion").get<std::string>());
    }

    if (json.contains("exit_code") && !json.at("exit_code").is_null()) {
        expectation.exit_code = static_cast<int>(json_int(json, "exit_code"));
    }
    expectation.stdout_contains = string_or_array(json, "stdout_contains");
    expectation.stdout_absent = string_or_array(json, "stdout_absent");

    // The names a caller reaches for when it means "this is the output". These
    // are spellings of one parameter, not readings of an ambiguous request, so
    // accepting them decides nothing. Measured: a run that had built and was
    // ready to verify sent {"output": "0\n1\n1\n..."} and was told that its
    // expectation stated nothing to check, with the exact output sitting in
    // it.
    static const char* kEqualsKeys[] = {"stdout_equals", "output", "stdout", "expected_output",
                                        "equals"};
    for (const char* key : kEqualsKeys) {
        if (!json.contains(key) || json.at(key).is_null()) continue;
        if (!json.at(key).is_string()) continue;
        expectation.stdout_equals = json_string(json, key);
        break;
    }
    if (json.contains("stdout_matches") && !json.at("stdout_matches").is_null()) {
        const auto pattern = json_string(json, "stdout_matches");
        try {
            std::regex probe{pattern, std::regex::ECMAScript};
            (void)probe;
        } catch (const std::regex_error& error) {
            return make_error(ErrorCode::InvalidArgument,
                              "'stdout_matches' is not a valid regular expression",
                              std::string{error.what()} + "  pattern: " + pattern);
        }
        expectation.stdout_matches = pattern;
    }

    if (!expectation.has_any_check()) {
        // The caller's own words, put back into a call that would work. A
        // model that has got as far as naming a criterion has understood the
        // mechanism and is one key short; handing it a list of key names and
        // making it rebuild the call is how that turn gets spent again.
        //
        // Measured: "expect": {"description": "Build the project with CMake
        // and verify it compiles successfully", "criterion": 2} - correct in
        // every respect except that it compared nothing.
        std::string completed;
        if (!expectation.description.empty() || expectation.criterion > 0) {
            const auto tail = expectation.criterion > 0
                                  ? ", \"criterion\": " + std::to_string(expectation.criterion)
                                  : std::string{};
            completed =
                "\n\nThis call with a comparison added. If the command only has to succeed:\n"
                "  \"expect\": { \"description\": \"" +
                expectation.description + "\", \"exit_code\": 0" + tail +
                " }\n"
                "If it has to print something in particular:\n"
                "  \"expect\": { \"description\": \"" +
                expectation.description + "\", \"stdout_equals\": \"<the exact output>\"" + tail +
                " }\n";
        }
        return make_error(
            ErrorCode::InvalidArgument, "'expect' was given but states nothing to check",
            "An expectation with no comparison in it would record a behaviour check that "
            "checked nothing." +
                completed +
                "\nThe keys it accepts:\n"
                "  stdout_equals    the complete output, compared whitespace-insensitively\n"
                "  stdout_contains  text that must appear (a string, or a list of them)\n"
                "  stdout_absent    text that must not appear\n"
                "  stdout_matches   a regular expression the output must match\n"
                "  exit_code        the exit code the command must produce\n\n"
                "For example:\n"
                "  \"expect\": { \"description\": \"prints the first ten Fibonacci numbers\",\n"
                "               \"stdout_equals\": \"0 1 1 2 3 5 8 13 21 34\",\n"
                "               \"criterion\": 1 }");
    }
    // A missing description is not a reason to refuse a check that is already
    // complete. The comparison itself is exact - LocalCode can state what it
    // is, and does, rather than making the caller author a sentence before it
    // is allowed to compare anything.
    //
    // Measured at the last step of an otherwise clean run: the program was
    // built, run, and the expectation named the exact output. LocalCode
    // refused it twice for want of a label, and the run ended one call short
    // of verifying itself.
    //
    // Nothing is weakened by filling this in. The comparison and its verdict
    // are unchanged; only the human-readable name for it is derived, and it is
    // derived from the comparison rather than invented.
    if (expectation.description.empty()) {
        expectation.description =
            "the output " + strings::join(expectation.describe_checks(), ", and ");
        expectation.description_derived = true;
    }
    return expectation;
}

bool OutputExpectation::has_any_check() const {
    return exit_code.has_value() || !stdout_contains.empty() || !stdout_absent.empty() ||
           stdout_equals.has_value() || stdout_matches.has_value();
}

std::vector<std::string> OutputExpectation::describe_checks() const {
    std::vector<std::string> checks;
    const char* stream = use_stderr ? "stderr" : "stdout";
    if (exit_code) checks.push_back("exit code is " + std::to_string(*exit_code));
    for (const auto& needle : stdout_contains) {
        checks.push_back(std::string{stream} + " contains \"" + needle + "\"");
    }
    for (const auto& needle : stdout_absent) {
        checks.push_back(std::string{stream} + " does not contain \"" + needle + "\"");
    }
    if (stdout_equals) {
        checks.push_back(std::string{stream} + " is \"" + *stdout_equals +
                         "\" (ignoring how the whitespace falls)");
    }
    if (stdout_matches) {
        checks.push_back(std::string{stream} + " matches /" + *stdout_matches + "/");
    }
    return checks;
}

ExpectationOutcome evaluate_expectation(const OutputExpectation& expectation,
                                        std::string_view stdout_text, std::string_view stderr_text,
                                        int exit_code) {
    ExpectationOutcome outcome;
    if (!expectation.present || !expectation.has_any_check()) return outcome;
    outcome.evaluated = true;

    const std::string_view stream = expectation.use_stderr ? stderr_text : stdout_text;
    const char* stream_name = expectation.use_stderr ? "stderr" : "stdout";
    const auto normalised = normalise_whitespace(stream);

    if (expectation.exit_code) {
        if (exit_code == *expectation.exit_code) {
            outcome.passed.push_back("exit code is " + std::to_string(exit_code));
        } else {
            outcome.failures.push_back("expected exit code " +
                                       std::to_string(*expectation.exit_code) + ", got " +
                                       std::to_string(exit_code));
        }
    }

    for (const auto& needle : expectation.stdout_contains) {
        // Compared on the normalised text as well, so an expectation written on
        // one line still matches output printed on several.
        const bool found = stream.find(needle) != std::string_view::npos ||
                           normalised.find(normalise_whitespace(needle)) != std::string::npos;
        if (found) {
            outcome.passed.push_back(std::string{stream_name} + " contains \"" + needle + "\"");
        } else {
            outcome.failures.push_back(std::string{stream_name} + " does not contain \"" + needle +
                                       "\"\n    actual " + std::string{stream_name} + ": " +
                                       excerpt_for_report(stream));
        }
    }

    for (const auto& needle : expectation.stdout_absent) {
        const bool found = stream.find(needle) != std::string_view::npos ||
                           normalised.find(normalise_whitespace(needle)) != std::string::npos;
        if (found) {
            outcome.failures.push_back(std::string{stream_name} +
                                       " was not supposed to contain \"" + needle +
                                       "\", but does\n    actual " + std::string{stream_name} +
                                       ": " + excerpt_for_report(stream));
        } else {
            outcome.passed.push_back(std::string{stream_name} + " does not contain \"" + needle +
                                     "\"");
        }
    }

    if (expectation.stdout_equals) {
        const auto expected = normalise_whitespace(*expectation.stdout_equals);
        if (normalised == expected) {
            outcome.passed.push_back(std::string{stream_name} + " is exactly what was expected");
        } else {
            outcome.failures.push_back(
                std::string{stream_name} + " does not match\n    expected: " +
                excerpt_for_report(expected) + "\n    actual:   " + excerpt_for_report(normalised));
        }
    }

    if (expectation.stdout_matches) {
        bool matched = false;
        try {
            const std::regex pattern{*expectation.stdout_matches, std::regex::ECMAScript};
            const std::string subject{stream};
            matched = std::regex_search(subject, pattern);
        } catch (const std::regex_error&) {
            matched = false;
        }
        if (matched) {
            outcome.passed.push_back(std::string{stream_name} + " matches /" +
                                     *expectation.stdout_matches + "/");
        } else {
            outcome.failures.push_back(
                std::string{stream_name} + " does not match /" + *expectation.stdout_matches +
                "/\n    actual " + std::string{stream_name} + ": " + excerpt_for_report(stream));
        }
    }

    outcome.satisfied = outcome.failures.empty();
    return outcome;
}

}   // namespace localcode
