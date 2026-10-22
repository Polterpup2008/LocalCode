#include "CodeIntegrity.h"

#include <algorithm>
#include <regex>

#include "../util/Strings.h"

namespace localcode {
namespace {

struct StubPattern {
    const char*         pattern;
    const char*         label;
    IntegrityConfidence confidence;
    const char*         reason;
};

// High confidence means the text says outright that the code is unfinished.
// Medium means it is a marker people leave on unfinished work. Low means the
// shape is suspicious and might be perfectly correct.
const std::vector<StubPattern>& stub_patterns() {
    static const std::vector<StubPattern> kPatterns = {
        // Says so in as many words.
        {R"(\bNotImplementedException\b)", "NotImplementedException",
         IntegrityConfidence::High, "throws instead of doing the work"},
        {R"(\bNotImplementedError\b)", "NotImplementedError",
         IntegrityConfidence::High, "raises instead of doing the work"},
        {R"(\btodo!\s*\()", "todo!()", IntegrityConfidence::High,
         "a Rust todo!() panics when reached"},
        {R"(\bunimplemented!\s*\()", "unimplemented!()", IntegrityConfidence::High,
         "a Rust unimplemented!() panics when reached"},
        // A custom delimiter: the pattern itself ends in )" , which would
        // otherwise close the raw string two characters early.
        {R"rx(\bpanic\(\s*"(not implemented|unimplemented|TODO)")rx",
         "panic(\"not implemented\")", IntegrityConfidence::High,
         "panics rather than doing the work"},
        {R"((?://|#|/\*)\s*(not implemented|unimplemented|implement (this|me) later|stub\b))",
         "\"not implemented\" comment", IntegrityConfidence::High,
         "the comment states the work was not done"},
        {R"((?://|#|/\*)\s*(placeholder|dummy implementation|fake implementation))",
         "placeholder comment", IntegrityConfidence::High,
         "the comment describes the code as a placeholder"},
        {R"(\bthrow\s+new\s+Error\(\s*["'](TODO|not implemented))", "throw new Error(\"TODO\")",
         IntegrityConfidence::High, "throws rather than doing the work"},

        // Markers people leave on work in progress.
        {R"(\b(TODO|FIXME)\b)", "TODO/FIXME", IntegrityConfidence::Medium,
         "marks something left to do"},
        {R"(\b(XXX|HACK)\b)", "XXX/HACK", IntegrityConfidence::Medium,
         "marks something the author was not happy with"},
        {R"((?://|#)\s*for now\b)", "\"for now\" comment", IntegrityConfidence::Medium,
         "the comment marks the code as temporary"},

        // Shapes that are often, but not always, a stub.
        {R"(^\s*(return\s+(true|0|nullptr|null|None|\{\}|\[\])\s*;?\s*)(?://|#)\s*.*\b(stub|placeholder|for now|always)\b)",
         "hardcoded return with a caveat", IntegrityConfidence::High,
         "returns a fixed value and the comment says it is not the real one"},
        {R"(^\s*pass\s*(#.*)?$)", "empty Python body", IntegrityConfidence::Low,
         "the body does nothing - correct for an interface, a stub otherwise"},
        {R"(\bassert\s*\(\s*(true|1)\s*\))", "assert(true)", IntegrityConfidence::High,
         "an assertion that cannot fail checks nothing"},
        {R"(\bexpect\(\s*true\s*\)\.\s*(toBe|toEqual)\(\s*true\s*\))", "expect(true).toBe(true)",
         IntegrityConfidence::High, "an assertion that cannot fail checks nothing"},
    };
    return kPatterns;
}

const std::vector<std::pair<std::regex, TamperKind>>& disable_patterns() {
    static const std::vector<std::pair<std::regex, TamperKind>> kPatterns = {
        // GoogleTest
        {std::regex{R"(\bDISABLED_[A-Za-z0-9_]+)", std::regex::optimize}, TamperKind::TestDisabled},
        // Jest / Mocha / Jasmine
        {std::regex{R"(\b(describe|it|test)\s*\.\s*(skip|todo)\s*\()", std::regex::optimize},
         TamperKind::TestDisabled},
        {std::regex{R"(\bx(describe|it)\s*\()", std::regex::optimize}, TamperKind::TestDisabled},
        // pytest / unittest
        {std::regex{R"(@(pytest\.mark\.)?(skip|skipif|xfail))", std::regex::optimize},
         TamperKind::TestDisabled},
        {std::regex{R"(@unittest\.skip)", std::regex::optimize}, TamperKind::TestDisabled},
        // Rust
        {std::regex{R"(#\[ignore\])", std::regex::optimize}, TamperKind::TestDisabled},
        // Catch2 hides a test case when its tag list starts with a dot.
        {std::regex{R"(TEST_CASE\s*\([^)]*"\s*\[\s*\.)", std::regex::optimize},
         TamperKind::TestDisabled},
        // CTest
        {std::regex{R"(set_tests_properties\s*\([^)]*DISABLED\s+(TRUE|ON))", std::regex::optimize},
         TamperKind::TestDisabled},
    };
    return kPatterns;
}

std::vector<std::pair<std::regex, const char*>> compiled_stub_patterns() {
    std::vector<std::pair<std::regex, const char*>> compiled;
    for (const auto& entry : stub_patterns()) {
        try {
            compiled.emplace_back(
                std::regex{entry.pattern, std::regex::optimize | std::regex::icase}, entry.label);
        } catch (const std::regex_error&) {
            // A malformed pattern here is a bug in this file, not a user
            // problem; skipping it beats taking down every file write.
        }
    }
    return compiled;
}

}  // namespace

std::string_view to_string(IntegrityConfidence confidence) noexcept {
    switch (confidence) {
        case IntegrityConfidence::Low:    return "low";
        case IntegrityConfidence::Medium: return "medium";
        case IntegrityConfidence::High:   return "high";
    }
    return "low";
}

std::string_view to_string(TamperKind kind) noexcept {
    switch (kind) {
        case TamperKind::TestDisabled:       return "a test was disabled";
        case TamperKind::AssertionRemoved:   return "assertions were removed";
        case TamperKind::ExpectationChanged: return "an expected value was changed";
        case TamperKind::TestDeleted:        return "a test was deleted";
    }
    return "a test was changed";
}

std::string StubFinding::describe() const {
    std::string out = file;
    if (line > 0) out += ":" + std::to_string(line);
    out += "  " + pattern + " (" + std::string{to_string(confidence)} + " confidence) - " + reason;
    if (!excerpt.empty()) out += "\n      " + excerpt;
    return out;
}

std::string TamperFinding::describe() const {
    std::string out = file;
    if (line > 0) out += ":" + std::to_string(line);
    out += "  " + std::string{to_string(kind)} + " (" + std::string{to_string(confidence)} +
           " confidence)";
    if (!detail.empty()) out += " - " + detail;
    return out;
}

bool looks_like_test_file(std::string_view path) {
    const auto lowered = strings::to_lower(path);
    // Directory conventions.
    static const char* kDirectories[] = {"/test/", "/tests/", "\\test\\", "\\tests\\",
                                         "/spec/", "\\spec\\", "__tests__"};
    for (const char* directory : kDirectories) {
        if (lowered.find(directory) != std::string::npos) return true;
    }
    if (strings::starts_with(lowered, "test/") || strings::starts_with(lowered, "tests/") ||
        strings::starts_with(lowered, "test\\") || strings::starts_with(lowered, "tests\\")) {
        return true;
    }
    // Filename conventions.
    const auto slash = lowered.find_last_of("/\\");
    const auto name = slash == std::string::npos ? lowered : lowered.substr(slash + 1);
    return strings::starts_with(name, "test_") || strings::starts_with(name, "test-") ||
           name.find("_test.") != std::string::npos ||
           name.find(".test.") != std::string::npos ||
           name.find(".spec.") != std::string::npos ||
           name.find("_spec.") != std::string::npos ||
           name.find("tests.") != std::string::npos;
}

std::vector<StubFinding> scan_for_stubs(std::string_view path, std::string_view content) {
    std::vector<StubFinding> findings;
    static const auto compiled = compiled_stub_patterns();
    const auto& patterns = stub_patterns();
    const bool in_tests = looks_like_test_file(path);

    int line_number = 0;
    for (const auto& line : strings::split_lines(content)) {
        ++line_number;
        if (line.empty()) continue;
        for (std::size_t i = 0; i < compiled.size() && i < patterns.size(); ++i) {
            if (!std::regex_search(line, compiled[i].first)) continue;
            StubFinding finding;
            finding.file = std::string{path};
            finding.line = line_number;
            finding.pattern = patterns[i].label;
            finding.confidence = patterns[i].confidence;
            finding.reason = patterns[i].reason;
            finding.excerpt = strings::utf8_truncate(strings::trim(line), 200);
            finding.in_test_code = in_tests;
            findings.push_back(std::move(finding));
            // One finding per line: a line saying "TODO: not implemented"
            // is one problem, not two.
            break;
        }
        // A file that is nothing but stubs would otherwise fill the result.
        if (findings.size() >= 50) break;
    }
    return findings;
}

std::size_t count_assertions(std::string_view content) {
    static const std::regex pattern{
        R"(\b(CHECK|REQUIRE|CHECK_FALSE|REQUIRE_FALSE|CHECK_THROWS|REQUIRE_THROWS|)"
        R"(ASSERT_[A-Z]+|EXPECT_[A-Z]+|assert|assertEqual|assertTrue|assertFalse|)"
        R"(assert_eq!|assert_ne!|expect|should|XCTAssert[A-Za-z]*)\s*[!(])",
        std::regex::optimize};

    std::size_t count = 0;
    for (const auto& line : strings::split_lines(content)) {
        auto begin = std::sregex_iterator{line.begin(), line.end(), pattern};
        count += static_cast<std::size_t>(std::distance(begin, std::sregex_iterator{}));
    }
    return count;
}

std::size_t count_test_cases(std::string_view content) {
    static const std::regex pattern{
        R"(\b(TEST_CASE|TEST_F|TEST_P|TEST|SCENARIO|BOOST_AUTO_TEST_CASE)\s*\(|)"
        R"(\b(def\s+test_[A-Za-z0-9_]+|fn\s+[A-Za-z0-9_]+\s*\(\s*\)\s*\{)|)"
        R"(\b(it|test|describe)\s*\(\s*["'])",
        std::regex::optimize};

    std::size_t count = 0;
    for (const auto& line : strings::split_lines(content)) {
        auto begin = std::sregex_iterator{line.begin(), line.end(), pattern};
        count += static_cast<std::size_t>(std::distance(begin, std::sregex_iterator{}));
    }
    return count;
}

std::vector<TamperFinding> scan_for_test_tampering(std::string_view path, std::string_view before,
                                                   std::string_view after) {
    std::vector<TamperFinding> findings;
    if (!looks_like_test_file(path)) return findings;

    // --- Disabling ------------------------------------------------------
    // Only markers the edit introduced count. A suite that was already
    // skipping a flaky test is not this run's doing.
    int line_number = 0;
    for (const auto& line : strings::split_lines(after)) {
        ++line_number;
        for (const auto& [pattern, kind] : disable_patterns()) {
            if (!std::regex_search(line, pattern)) continue;
            const bool present_before = [&] {
                for (const auto& old_line : strings::split_lines(before)) {
                    if (strings::trim(old_line) == strings::trim(line)) return true;
                }
                return false;
            }();
            if (present_before) continue;
            TamperFinding finding;
            finding.file = std::string{path};
            finding.line = line_number;
            finding.kind = kind;
            finding.confidence = IntegrityConfidence::High;
            finding.detail = strings::utf8_truncate(strings::trim(line), 200);
            findings.push_back(std::move(finding));
            break;
        }
    }

    // A new file cannot have removed anything.
    if (strings::trim(before).empty()) return findings;

    // --- Removal --------------------------------------------------------
    const auto assertions_before = count_assertions(before);
    const auto assertions_after = count_assertions(after);
    if (assertions_after < assertions_before) {
        TamperFinding finding;
        finding.file = std::string{path};
        finding.kind = TamperKind::AssertionRemoved;
        // Losing most of them is a different act from losing one while
        // rewriting a block, and the report should not treat them alike.
        const auto lost = assertions_before - assertions_after;
        finding.confidence = lost * 2 >= assertions_before ? IntegrityConfidence::High
                                                           : IntegrityConfidence::Medium;
        finding.detail = std::to_string(assertions_before) + " assertion(s) became " +
                         std::to_string(assertions_after) + " - " + std::to_string(lost) +
                         " no longer check anything";
        findings.push_back(std::move(finding));
    }

    const auto cases_before = count_test_cases(before);
    const auto cases_after = count_test_cases(after);
    if (cases_after < cases_before) {
        TamperFinding finding;
        finding.file = std::string{path};
        finding.kind = TamperKind::TestDeleted;
        finding.confidence = IntegrityConfidence::High;
        finding.detail = std::to_string(cases_before) + " test(s) became " +
                         std::to_string(cases_after);
        findings.push_back(std::move(finding));
    }

    return findings;
}

}  // namespace localcode
