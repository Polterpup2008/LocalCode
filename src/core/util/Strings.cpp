#include "Strings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>

#include "Json.h"

namespace localcode::strings {
namespace {

constexpr char kWhitespace[] = " \t\r\n\f\v";

char lower_ascii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

std::string trim(std::string_view s) {
    const auto begin = s.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(kWhitespace);
    return std::string{s.substr(begin, end - begin + 1)};
}

std::string to_lower(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), lower_ascii);
    return out;
}

std::string to_upper(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    }
    return true;
}

bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower_ascii(haystack[i + j]) == lower_ascii(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

std::vector<std::string> split(std::string_view s, char delim, bool keep_empty) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto pos = s.find(delim, start);
        std::string_view piece = (pos == std::string_view::npos)
                                     ? s.substr(start)
                                     : s.substr(start, pos - start);
        if (keep_empty || !piece.empty()) out.emplace_back(piece);
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            std::size_t end = i;
            if (end > start && s[end - 1] == '\r') --end;
            out.emplace_back(s.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < s.size()) {
        std::size_t end = s.size();
        if (end > start && s[end - 1] == '\r') --end;
        out.emplace_back(s.substr(start, end - start));
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

std::string collapse_blank_lines(std::string_view text, std::size_t keep) {
    const auto lines = split_lines(text);
    std::string out;
    out.reserve(text.size());

    std::size_t run = 0;
    bool in_fence = false;
    bool first = true;
    for (const auto& line : lines) {
        // Inside a fenced block the blank lines are the author's, not noise.
        if (starts_with(trim(line), "```")) in_fence = !in_fence;

        const bool blank = !in_fence && trim(line).empty();
        if (blank) {
            ++run;
            if (run > keep) continue;
        } else {
            run = 0;
        }
        if (!first) out.push_back('\n');
        out.append(line);
        first = false;
    }
    return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string{s};
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (true) {
        const auto hit = s.find(from, pos);
        if (hit == std::string_view::npos) {
            out.append(s.substr(pos));
            break;
        }
        out.append(s.substr(pos, hit - pos));
        out.append(to);
        pos = hit + from.size();
    }
    return out;
}

bool glob_match(std::string_view pattern, std::string_view text) {
    // Iterative backtracking matcher: linear memory, no recursion blow-up on
    // adversarial patterns such as "*a*a*a*a*a*b".
    std::size_t p = 0, t = 0;
    std::size_t star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' ||
                                   lower_ascii(pattern[p]) == lower_ascii(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// --- UTF-8 -----------------------------------------------------------------
namespace {

// Decodes one code point. Returns the sequence length, or 0 when malformed.
std::size_t decode_utf8(std::string_view s, std::size_t i, std::uint32_t& cp) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { cp = b0; return 1; }

    std::size_t len = 0;
    std::uint32_t value = 0;
    if ((b0 & 0xE0) == 0xC0) { len = 2; value = b0 & 0x1Fu; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; value = b0 & 0x0Fu; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; value = b0 & 0x07u; }
    else return 0;                                    // continuation or 5/6-byte

    if (i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        const auto bk = static_cast<unsigned char>(s[i + k]);
        if ((bk & 0xC0) != 0x80) return 0;
        value = (value << 6) | (bk & 0x3Fu);
    }
    // Reject overlong encodings, UTF-16 surrogates and values past U+10FFFF.
    static constexpr std::uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < kMin[len]) return 0;
    if (value > 0x10FFFF) return 0;
    if (value >= 0xD800 && value <= 0xDFFF) return 0;
    cp = value;
    return len;
}

}  // namespace

bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t len = decode_utf8(s, i, cp);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

std::string sanitize_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t len = decode_utf8(s, i, cp);
        if (len == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

std::string visible(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\f': out += "\\f"; break;
            case '\b': out += "\\b"; break;
            case '\v': out += "\\v"; break;
            case 0x1B: out += "\\e"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\x";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string redact_secrets(std::string_view json_text) {
    static const std::regex quoted_secret{
        R"rx("([A-Za-z0-9_.-]*(?:token|secret|password|passwd|api[_-]?key|credential|authorization|auth)[A-Za-z0-9_.-]*)"\s*:\s*"(?:[^"\\]|\\.)*")rx",
        std::regex::icase};
    static const std::regex authorization{
        R"rx((\bauthorization\s*:\s*)(?:bearer|basic)\s+[^\s,;"']+)rx", std::regex::icase};
    static const std::regex assignment{
        R"rx((\b[A-Za-z0-9_.-]*(?:token|secret|password|passwd|api[_-]?key|credential|authorization|auth)[A-Za-z0-9_.-]*\s*(?:=|:)\s*)(?:"(?:[^"\\]|\\.)*"|'[^']*'|[^\s,;"']+))rx",
        std::regex::icase};
    auto redact_text = [&](const std::string& text) {
        auto safe = std::regex_replace(text, quoted_secret, R"rx("$1": "<redacted>")rx");
        safe = std::regex_replace(safe, authorization, "$1<redacted>");
        return std::regex_replace(safe, assignment, "$1<redacted>");
    };
    auto secret_key = [](std::string_view key) {
        const auto lower = to_lower(key);
        for (const auto* word : {"token", "secret", "password", "passwd", "credential",
                                 "authorization", "auth", "api_key", "api-key", "apikey"})
            if (lower.find(word) != std::string::npos) return true;
        return false;
    };

    // Chat and tool content are JSON strings containing decoded source text
    // or another serialized JSON document. Redacting only the outer dump
    // misses both escaped inner keys and ordinary .env/header assignments.
    auto redact_value = [&](auto&& self, Json& value, int depth) -> void {
        if (depth > 32) {
            value = "<redacted: deeply nested data>";
        } else if (value.is_object()) {
            for (auto item = value.begin(); item != value.end(); ++item) {
                if (item.value().is_string() && secret_key(item.key()))
                    item.value() = "<redacted>";
                else
                    self(self, item.value(), depth + 1);
            }
        } else if (value.is_array()) {
            for (auto& item : value)
                self(self, item, depth + 1);
        } else if (value.is_string()) {
            const auto text = value.get<std::string>();
            auto nested = Json::parse(text, nullptr, false);
            if (nested.is_object() || nested.is_array()) {
                self(self, nested, depth + 1);
                value = nested.dump();
            } else {
                value = redact_text(text);
            }
        }
    };
    auto document = Json::parse(json_text, nullptr, false);
    if (document.is_discarded()) return redact_text(std::string{json_text});
    redact_value(redact_value, document, 0);
    return document.dump();
}

std::size_t utf8_length(std::string_view s) {
    std::size_t count = 0, i = 0;
    while (i < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t len = decode_utf8(s, i, cp);
        i += (len == 0) ? 1 : len;
        ++count;
    }
    return count;
}

std::string utf8_truncate(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string{s};
    std::size_t cut = max_bytes;
    // Walk back to a lead byte so we never emit half of a sequence.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return std::string{s.substr(0, cut)};
}

// --- Formatting ------------------------------------------------------------
std::string human_bytes(std::uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) { value /= 1024.0; ++unit; }
    char buf[64];
    if (unit == 0) {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof buf, "%.2f %s", value, kUnits[unit]);
    }
    return buf;
}

std::string human_duration_ms(std::int64_t ms) {
    char buf[64];
    if (ms < 1000) {
        std::snprintf(buf, sizeof buf, "%lldms", static_cast<long long>(ms));
    } else if (ms < 60000) {
        std::snprintf(buf, sizeof buf, "%.2fs", static_cast<double>(ms) / 1000.0);
    } else {
        const long long total = ms / 1000;
        std::snprintf(buf, sizeof buf, "%lldm%02llds", total / 60, total % 60);
    }
    return buf;
}

Excerpt excerpt(std::string_view s, std::size_t head_lines, std::size_t tail_lines,
                std::size_t max_bytes) {
    Excerpt result;
    result.original_bytes = s.size();

    const auto lines = split_lines(s);
    result.original_lines = lines.size();

    if (s.size() <= max_bytes && lines.size() <= head_lines + tail_lines) {
        result.text = sanitize_utf8(s);
        return result;
    }

    result.truncated = true;
    std::vector<std::string> kept;
    const std::size_t head = std::min(head_lines, lines.size());
    for (std::size_t i = 0; i < head; ++i) kept.push_back(lines[i]);

    const std::size_t tail_start =
        (lines.size() > tail_lines) ? std::max(head, lines.size() - tail_lines) : head;
    const std::size_t omitted = tail_start - head;
    if (omitted > 0) {
        kept.push_back("... [" + std::to_string(omitted) + " lines omitted] ...");
    }
    for (std::size_t i = tail_start; i < lines.size(); ++i) kept.push_back(lines[i]);

    std::string joined = join(kept, "\n");
    if (joined.size() > max_bytes) {
        joined = utf8_truncate(joined, max_bytes);
        joined += "\n... [output truncated at " + human_bytes(max_bytes) + "] ...";
    }
    result.text = sanitize_utf8(joined);
    return result;
}

}  // namespace localcode::strings
