#pragma once
// ---------------------------------------------------------------------------
// Two ways of appearing to finish without finishing.
//
// The first is the stub: a function that returns a plausible value and a
// comment promising the rest later. It compiles, it links, it runs, and the
// verification stages all go green - because a stub is, technically, working
// code. Only reading it reveals that nothing was implemented.
//
// The second is worse, because it is aimed at the check itself: making a test
// pass by disabling it, deleting its assertions, or editing the expected value
// to match whatever the code produced. That converts a red signal into a green
// one without touching the defect.
//
// Neither can be detected with certainty from text - `return nullptr;` is a
// stub in one function and correct in another, and a deleted assertion may be
// a test that was genuinely wrong. So findings carry a confidence and a reason,
// and are reported rather than enforced. LocalCode says what it noticed; it
// does not refuse the edit or pretend to know the author's intent.
// ---------------------------------------------------------------------------
#include <string>
#include <string_view>
#include <vector>

namespace localcode {

enum class IntegrityConfidence { Low, Medium, High };

[[nodiscard]] std::string_view to_string(IntegrityConfidence confidence) noexcept;

struct StubFinding {
    std::string         file;
    int                 line = 0;
    std::string         pattern;      // the marker that matched
    IntegrityConfidence confidence = IntegrityConfidence::Low;
    std::string         reason;       // why this looks unfinished
    std::string         excerpt;      // the line itself
    // Whether this file is test code. A placeholder in a test fixture is
    // ordinary; the same text in production code is not.
    bool                in_test_code = false;
    // Set by the caller: whether this run wrote the file. A pre-existing TODO
    // is the project's business, not the agent's.
    bool                introduced_by_agent = false;

    [[nodiscard]] std::string describe() const;
};

// Detects a test file from its path, using the conventions of the common
// ecosystems rather than a single project's layout.
[[nodiscard]] bool looks_like_test_file(std::string_view path);

[[nodiscard]] std::vector<StubFinding> scan_for_stubs(std::string_view path,
                                                      std::string_view content);

// --- Weakening the check ---------------------------------------------------
enum class TamperKind {
    TestDisabled,       // skipped, ignored, or renamed out of the run
    AssertionRemoved,   // the file has fewer assertions than it did
    ExpectationChanged, // an expected value was edited
    TestDeleted,        // whole test cases are gone
};

[[nodiscard]] std::string_view to_string(TamperKind kind) noexcept;

struct TamperFinding {
    std::string         file;
    TamperKind          kind = TamperKind::TestDisabled;
    IntegrityConfidence confidence = IntegrityConfidence::Low;
    std::string         detail;
    int                 line = 0;

    [[nodiscard]] std::string describe() const;
};

// Counts the assertion-like statements in a piece of source. Approximate by
// design: it is used to compare a file with its earlier self, where the same
// approximation applies to both sides.
[[nodiscard]] std::size_t count_assertions(std::string_view content);
[[nodiscard]] std::size_t count_test_cases(std::string_view content);

// Compares a test file with what it was before this edit. `before` empty means
// the file is new, and nothing was weakened.
[[nodiscard]] std::vector<TamperFinding> scan_for_test_tampering(std::string_view path,
                                                                 std::string_view before,
                                                                 std::string_view after);

}  // namespace localcode
