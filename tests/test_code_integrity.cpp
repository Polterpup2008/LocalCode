// ---------------------------------------------------------------------------
// Code that compiles without doing the job, and tests edited into passing.
//
// Both are ways of reaching a green build without solving the problem, and
// both are invisible to every other check in the system: a stub links, and a
// deleted assertion makes the suite greener, not redder.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "tools/CodeIntegrity.h"

using namespace localcode;

namespace {

bool has_pattern(const std::vector<StubFinding>& findings, std::string_view pattern) {
    for (const auto& finding : findings) {
        if (finding.pattern.find(pattern) != std::string::npos) return true;
    }
    return false;
}

bool has_kind(const std::vector<TamperFinding>& findings, TamperKind kind) {
    for (const auto& finding : findings) {
        if (finding.kind == kind) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("code that says it is not implemented is found", "[integrity][stubs]") {
    const auto findings = scan_for_stubs("src/render.cpp", R"(
void Renderer::draw() {
    // TODO: implement this properly
    throw new NotImplementedException();
}
)");
    REQUIRE_FALSE(findings.empty());
    CHECK(has_pattern(findings, "NotImplementedException"));
    CHECK(has_pattern(findings, "TODO"));
}

TEST_CASE("the highest-confidence findings say so outright", "[integrity][stubs]") {
    const auto rust = scan_for_stubs("src/lib.rs", "fn area() -> f64 { todo!() }\n");
    REQUIRE(rust.size() == 1);
    CHECK(rust[0].confidence == IntegrityConfidence::High);

    const auto marker = scan_for_stubs("src/lib.rs", "let x = 1; // FIXME later\n");
    REQUIRE(marker.size() == 1);
    CHECK(marker[0].confidence == IntegrityConfidence::Medium);
}

TEST_CASE("a hardcoded return with a caveat is a stub", "[integrity][stubs]") {
    const auto findings = scan_for_stubs("src/auth.cpp",
                                         "bool authenticate() {\n"
                                         "    return true;  // always succeeds for now\n"
                                         "}\n");
    REQUIRE_FALSE(findings.empty());
    CHECK(findings[0].confidence == IntegrityConfidence::High);
    CHECK(findings[0].line == 2);
}

TEST_CASE("an assertion that cannot fail is reported", "[integrity][stubs]") {
    const auto cpp = scan_for_stubs("tests/test_thing.cpp", "    assert(true);\n");
    REQUIRE_FALSE(cpp.empty());
    CHECK(cpp[0].confidence == IntegrityConfidence::High);
    CHECK(cpp[0].in_test_code);

    const auto js = scan_for_stubs("src/app.test.js", "  expect(true).toBe(true);\n");
    REQUIRE_FALSE(js.empty());
    CHECK(js[0].in_test_code);
}

TEST_CASE("finished code produces no findings", "[integrity][stubs]") {
    const auto findings = scan_for_stubs("src/math.cpp", R"(
int add(int a, int b) {
    return a + b;
}

std::vector<int> fibonacci(int count) {
    std::vector<int> result;
    int previous = 0;
    int current = 1;
    for (int i = 0; i < count; ++i) {
        result.push_back(previous);
        const int next = previous + current;
        previous = current;
        current = next;
    }
    return result;
}
)");
    CHECK(findings.empty());
}

TEST_CASE("test files are recognised across ecosystems", "[integrity][paths]") {
    CHECK(looks_like_test_file("tests/test_verification.cpp"));
    CHECK(looks_like_test_file("src/components/Button.test.tsx"));
    CHECK(looks_like_test_file("app/models/user_spec.rb"));
    CHECK(looks_like_test_file("pkg/thing/thing_test.go"));
    CHECK(looks_like_test_file("src/__tests__/helpers.js"));
    CHECK(looks_like_test_file(R"(L:\project\tests\suite.cpp)"));

    CHECK_FALSE(looks_like_test_file("src/main.cpp"));
    CHECK_FALSE(looks_like_test_file("src/latest.cpp"));
    CHECK_FALSE(looks_like_test_file("include/protest.h"));
}

TEST_CASE("a newly disabled test is reported", "[integrity][tampering]") {
    const std::string before = "TEST(Maths, AddsUp) { EXPECT_EQ(add(2, 2), 4); }\n";
    const std::string after = "TEST(Maths, DISABLED_AddsUp) { EXPECT_EQ(add(2, 2), 4); }\n";

    const auto findings = scan_for_test_tampering("tests/maths_test.cpp", before, after);
    REQUIRE_FALSE(findings.empty());
    CHECK(has_kind(findings, TamperKind::TestDisabled));
    CHECK(findings[0].confidence == IntegrityConfidence::High);
}

TEST_CASE("a test that was already skipped is not this run's doing",
          "[integrity][tampering]") {
    const std::string source =
        "@pytest.mark.skip(reason=\"flaky on CI\")\n"
        "def test_upload():\n"
        "    assert upload() == 200\n";
    // Unchanged file: the skip was there before, so nothing was weakened here.
    const auto findings = scan_for_test_tampering("tests/test_upload.py", source, source);
    CHECK_FALSE(has_kind(findings, TamperKind::TestDisabled));
}

TEST_CASE("removed assertions are counted", "[integrity][tampering]") {
    const std::string before =
        "TEST_CASE(\"maths\") {\n"
        "    CHECK(add(1, 1) == 2);\n"
        "    CHECK(add(2, 2) == 4);\n"
        "    CHECK(add(3, 3) == 6);\n"
        "    REQUIRE(add(0, 0) == 0);\n"
        "}\n";
    const std::string after =
        "TEST_CASE(\"maths\") {\n"
        "    CHECK(add(1, 1) == 2);\n"
        "}\n";

    const auto findings = scan_for_test_tampering("tests/test_maths.cpp", before, after);
    REQUIRE(has_kind(findings, TamperKind::AssertionRemoved));
    for (const auto& finding : findings) {
        if (finding.kind != TamperKind::AssertionRemoved) continue;
        // Losing three of four is not the same as losing one while rewriting.
        CHECK(finding.confidence == IntegrityConfidence::High);
        CHECK(finding.detail.find("3 no longer check anything") != std::string::npos);
    }
}

TEST_CASE("a deleted test case is reported", "[integrity][tampering]") {
    const std::string before =
        "TEST_CASE(\"a\") { CHECK(f() == 1); }\n"
        "TEST_CASE(\"b\") { CHECK(g() == 2); }\n";
    const std::string after = "TEST_CASE(\"a\") { CHECK(f() == 1); }\n";

    const auto findings = scan_for_test_tampering("tests/test_thing.cpp", before, after);
    CHECK(has_kind(findings, TamperKind::TestDeleted));
}

TEST_CASE("adding tests is not tampering", "[integrity][tampering]") {
    const std::string before = "TEST_CASE(\"a\") { CHECK(f() == 1); }\n";
    const std::string after =
        "TEST_CASE(\"a\") { CHECK(f() == 1); }\n"
        "TEST_CASE(\"b\") { CHECK(g() == 2); }\n";
    CHECK(scan_for_test_tampering("tests/test_thing.cpp", before, after).empty());
}

TEST_CASE("a new test file has weakened nothing", "[integrity][tampering]") {
    CHECK(scan_for_test_tampering("tests/test_new.cpp", "",
                                  "TEST_CASE(\"a\") { CHECK(f() == 1); }\n")
              .empty());
}

TEST_CASE("production code is not judged as a test file", "[integrity][tampering]") {
    // Deleting an assert from production code is refactoring, not tampering,
    // and reporting it as tampering would train people to ignore the warning.
    const std::string before = "void f() { assert(x); do_work(); }\n";
    const std::string after = "void f() { do_work(); }\n";
    CHECK(scan_for_test_tampering("src/worker.cpp", before, after).empty());
}

TEST_CASE("a rendered finding names the file, the line and the reason",
          "[integrity][rendering]") {
    const auto findings = scan_for_stubs("src/render.cpp",
                                         "void draw() {\n"
                                         "    throw new NotImplementedException();\n"
                                         "}\n");
    REQUIRE_FALSE(findings.empty());
    const auto text = findings[0].describe();
    INFO(text);
    CHECK(text.find("src/render.cpp:2") != std::string::npos);
    CHECK(text.find("high confidence") != std::string::npos);
}
