#pragma once
// String helpers shared by the tools, parsers and the UI-facing formatters.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace localcode::strings {

[[nodiscard]] std::string trim(std::string_view s);
[[nodiscard]] std::string to_lower(std::string_view s);
[[nodiscard]] std::string to_upper(std::string_view s);
[[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix);
[[nodiscard]] bool ends_with(std::string_view s, std::string_view suffix);
[[nodiscard]] bool iequals(std::string_view a, std::string_view b);
[[nodiscard]] bool icontains(std::string_view haystack, std::string_view needle);

[[nodiscard]] std::vector<std::string> split(std::string_view s, char delim, bool keep_empty = true);
[[nodiscard]] std::vector<std::string> split_lines(std::string_view s);
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view sep);

// Reduces runs of blank lines to at most `keep` of them, leaving fenced code
// blocks alone.
//
// A degenerating model emits newlines. One reply in a real run carried several
// hundred consecutive blank lines between two sentences, and LocalCode passed
// them straight through - so the honest note explaining what had not been
// verified arrived several screens below the answer it belonged to.
[[nodiscard]] std::string collapse_blank_lines(std::string_view text, std::size_t keep = 2);
[[nodiscard]] std::string replace_all(std::string_view s, std::string_view from, std::string_view to);

// Case-insensitive glob supporting '*' and '?'. Used for file filters and the
// command policy, never for path containment checks.
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text);

// --- UTF-8 -----------------------------------------------------------------
// Returns true when `s` is well-formed UTF-8 (rejects overlongs, surrogates and
// out-of-range code points).
[[nodiscard]] bool is_valid_utf8(std::string_view s);

// Replaces malformed sequences with U+FFFD so arbitrary file bytes can be shown
// in the UI and fed to a model without corrupting the JSON wire format.
[[nodiscard]] std::string sanitize_utf8(std::string_view s);

// Number of Unicode code points (invalid bytes count as one each).
[[nodiscard]] std::size_t utf8_length(std::string_view s);

// Truncates without splitting a UTF-8 sequence.
[[nodiscard]] std::string utf8_truncate(std::string_view s, std::size_t max_bytes);

// Renders control characters as their C escapes so they can be seen. A form
// feed sitting in the middle of a path is invisible in a log and is the single
// most common way a Windows path gets damaged in transit; printing it as \f
// turns an unexplainable failure into an obvious one.
[[nodiscard]] std::string visible(std::string_view s);

// Replaces the values of secret-looking keys in a JSON-ish string with
// "<redacted>". Traces are written to disk and pasted into bug reports, and a
// command that legitimately carries a token in its environment should not put
// that token in both.
//
// Keys are matched by name - token, secret, password, key, credential,
// authorization, passwd, api_key and their obvious spellings - so this is a
// filter on how values are displayed, not a claim to have found every secret.
[[nodiscard]] std::string redact_secrets(std::string_view json_text);

// --- Formatting ------------------------------------------------------------
[[nodiscard]] std::string human_bytes(std::uint64_t bytes);
[[nodiscard]] std::string human_duration_ms(std::int64_t ms);

// Keeps the head and the tail of a long text and marks what was dropped. This is
// the single place tool results are shortened, so the model always sees an
// explicit "omitted" marker rather than a silent cut.
struct Excerpt {
    std::string text;
    bool        truncated = false;
    std::size_t original_bytes = 0;
    std::size_t original_lines = 0;
};
[[nodiscard]] Excerpt excerpt(std::string_view s, std::size_t head_lines,
                              std::size_t tail_lines, std::size_t max_bytes);

}  // namespace localcode::strings
