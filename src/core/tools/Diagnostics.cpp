#include "Diagnostics.h"

#include <algorithm>
#include <map>
#include <regex>

#include "../util/Strings.h"

namespace localcode {
namespace {

DiagnosticSeverity severity_from(std::string_view word) {
    const auto lowered = strings::to_lower(word);
    if (lowered.find("warning") != std::string::npos) return DiagnosticSeverity::Warning;
    if (lowered.find("note") != std::string::npos || lowered.find("remark") != std::string::npos) {
        return DiagnosticSeverity::Note;
    }
    return DiagnosticSeverity::Error;
}

DiagnosticOrigin origin_from_code(std::string_view code) {
    if (strings::starts_with(code, "LNK")) return DiagnosticOrigin::Linker;
    if (strings::starts_with(code, "C") || strings::starts_with(code, "E")) {
        return DiagnosticOrigin::Compiler;
    }
    return DiagnosticOrigin::Unknown;
}

int to_int(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (...) {
        return 0;
    }
}

std::string tidy(std::string_view text) {
    return strings::utf8_truncate(strings::trim(text), 400);
}

// --- The formats -----------------------------------------------------------
//
// Each returns true when it recognised the line and filled `out`.

// src\main.cpp(12,5): error C2065: 'x': undeclared identifier
// src\main.cpp(12): warning C4100: 'argc': unreferenced formal parameter
bool parse_msvc_compiler(const std::string& line, Diagnostic& out) {
    static const std::regex pattern{
        R"(^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal\s+)?(error|warning|note)\s+([A-Z]+\d+)\s*:\s*(.*)$)",
        std::regex::optimize | std::regex::icase};
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) return false;

    out.file = strings::trim(match[1].str());
    out.line = to_int(match[2].str());
    out.column = match[3].matched ? to_int(match[3].str()) : 0;
    out.severity = severity_from(match[5].str());
    out.code = strings::to_upper(match[6].str());
    out.message = tidy(match[7].str());
    out.origin = origin_from_code(out.code);
    return true;
}

// main.obj : error LNK2019: unresolved external symbol "void __cdecl foo(void)"
// LINK : fatal error LNK1120: 1 unresolved externals
bool parse_msvc_linker(const std::string& line, Diagnostic& out) {
    static const std::regex pattern{
        R"(^\s*(.*?)\s*:\s*(fatal\s+)?(error|warning)\s+(LNK\d+)\s*:\s*(.*)$)",
        std::regex::optimize | std::regex::icase};
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) return false;

    // The part before the colon is the object or library, not a source file
    // with a line in it, so it goes in `file` with no line number.
    out.file = strings::trim(match[1].str());
    if (strings::iequals(out.file, "LINK")) out.file.clear();
    out.severity = severity_from(match[3].str());
    out.code = strings::to_upper(match[4].str());
    out.message = tidy(match[5].str());
    out.origin = DiagnosticOrigin::Linker;
    return true;
}

// src/main.cpp:12:5: error: expected ';' before '}' token
// src/main.cpp:12: warning: unused variable 'x'
bool parse_gcc_clang(const std::string& line, Diagnostic& out) {
    static const std::regex pattern{
        R"(^\s*(.+?):(\d+)(?::(\d+))?:\s*(fatal\s+)?(error|warning|note):\s*(.*)$)",
        std::regex::optimize};
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) return false;

    out.file = strings::trim(match[1].str());
    out.line = to_int(match[2].str());
    out.column = match[3].matched ? to_int(match[3].str()) : 0;
    out.severity = severity_from(match[5].str());
    out.message = tidy(match[6].str());
    out.origin = DiagnosticOrigin::Compiler;

    // Clang appends the switch that produced it: "[-Wunused-variable]".
    static const std::regex flag{R"(\[-W([A-Za-z0-9-]+)\]\s*$)"};
    std::smatch flag_match;
    if (std::regex_search(out.message, flag_match, flag)) out.code = "-W" + flag_match[1].str();
    return true;
}

// error[E0308]: mismatched types
bool parse_rust(const std::string& line, Diagnostic& out) {
    static const std::regex pattern{R"(^\s*(error|warning)(?:\[([A-Z]\d+)\])?:\s*(.*)$)",
                                    std::regex::optimize};
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) return false;
    // Without a code this pattern is broad enough to swallow any line starting
    // with "error:", which is usually right but never carries a location.
    out.severity = severity_from(match[1].str());
    out.code = match[2].matched ? match[2].str() : std::string{};
    out.message = tidy(match[3].str());
    out.origin = DiagnosticOrigin::Compiler;
    return true;
}

// CMake Error at CMakeLists.txt:14 (add_executable):
bool parse_cmake(const std::string& line, Diagnostic& out) {
    static const std::regex pattern{
        R"(^\s*CMake (Error|Warning|Deprecation Error|Deprecation Warning)(?: at (.+?):(\d+).*)?:?\s*(.*)$)",
        std::regex::optimize};
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) return false;

    out.severity = severity_from(match[1].str());
    if (match[2].matched) {
        out.file = strings::trim(match[2].str());
        out.line = to_int(match[3].str());
    }
    out.message = tidy(match[4].str());
    if (out.message.empty()) out.message = "see the build log for the details that follow";
    out.origin = DiagnosticOrigin::BuildSystem;
    return true;
}

// FAILED: src/CMakeFiles/app.dir/main.cpp.obj
// make: *** [Makefile:12: all] Error 2
bool parse_build_step(const std::string& line, Diagnostic& out) {
    static const std::regex ninja{R"(^FAILED:\s*(.*)$)", std::regex::optimize};
    static const std::regex make{R"(^make(?:\[\d+\])?:\s*\*\*\*\s*(.*)$)", std::regex::optimize};
    std::smatch match;
    if (std::regex_match(line, match, ninja)) {
        out.severity = DiagnosticSeverity::Error;
        out.origin = DiagnosticOrigin::BuildSystem;
        out.message = "build step failed: " + tidy(match[1].str());
        return true;
    }
    if (std::regex_match(line, match, make)) {
        out.severity = DiagnosticSeverity::Error;
        out.origin = DiagnosticOrigin::BuildSystem;
        out.message = tidy(match[1].str());
        return true;
    }
    return false;
}

// Test runners and crashes. These do not report a compile problem, but a build
// log that contains one is not a passing build either.
bool parse_runtime(const std::string& line, Diagnostic& out) {
    static const std::vector<std::pair<std::regex, DiagnosticOrigin>> kPatterns = {
        {std::regex{R"(^\s*(\d+)\s*(?:tests?)?\s*fail(?:ed|ures?)\b.*$)",
                    std::regex::optimize | std::regex::icase},
         DiagnosticOrigin::TestRunner},
        {std::regex{R"(^.*\bThe following tests FAILED\b.*$)", std::regex::optimize},
         DiagnosticOrigin::TestRunner},
        {std::regex{R"(^.*\b(ASSERTION FAILED|assertion failed)\b.*$)", std::regex::optimize},
         DiagnosticOrigin::TestRunner},
        {std::regex{
             R"(^.*\b(Segmentation fault|Access violation|Traceback \(most recent call last\)|panicked at|terminate called)\b.*$)",
             std::regex::optimize},
         DiagnosticOrigin::Runtime},
    };
    for (const auto& [pattern, origin] : kPatterns) {
        if (!std::regex_match(line, pattern)) continue;
        out.severity = DiagnosticSeverity::Error;
        out.origin = origin;
        out.message = tidy(line);
        return true;
    }
    return false;
}

}  // namespace

std::string_view to_string(DiagnosticSeverity severity) noexcept {
    switch (severity) {
        case DiagnosticSeverity::Error:   return "error";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Note:    return "note";
    }
    return "error";
}

std::string_view to_string(DiagnosticOrigin origin) noexcept {
    switch (origin) {
        case DiagnosticOrigin::Compiler:    return "compiler";
        case DiagnosticOrigin::Linker:      return "linker";
        case DiagnosticOrigin::BuildSystem: return "build system";
        case DiagnosticOrigin::TestRunner:  return "test runner";
        case DiagnosticOrigin::Runtime:     return "runtime";
        case DiagnosticOrigin::Unknown:     return "toolchain";
    }
    return "toolchain";
}

std::string Diagnostic::describe() const {
    std::string out;
    if (!file.empty()) {
        out += file;
        if (line > 0) {
            out += "(" + std::to_string(line);
            if (column > 0) out += "," + std::to_string(column);
            out += ")";
        }
        out += ": ";
    }
    out += to_string(severity);
    if (!code.empty()) out += " " + code;
    out += ": " + message;
    return out;
}

std::string Diagnostic::identity() const {
    // Deliberately not the raw line: the same missing header reported from
    // forty translation units differs only in the file, and collapsing those
    // is the whole reason this exists.
    return std::string{to_string(severity)} + "|" + code + "|" + message;
}

std::size_t DiagnosticSet::error_count() const {
    return static_cast<std::size_t>(
        std::count_if(items.begin(), items.end(),
                      [](const Diagnostic& d) { return d.severity == DiagnosticSeverity::Error; }));
}

std::size_t DiagnosticSet::warning_count() const {
    return static_cast<std::size_t>(std::count_if(
        items.begin(), items.end(),
        [](const Diagnostic& d) { return d.severity == DiagnosticSeverity::Warning; }));
}

std::string DiagnosticSet::summarise(std::size_t max_reported) const {
    if (items.empty()) return {};

    // Group by what makes two reports the same problem, keeping the first
    // occurrence - the one with a real location - as the representative.
    struct Group {
        Diagnostic  representative;
        std::size_t count = 0;
        std::size_t first_seen = 0;
        std::vector<std::string> other_places;
    };
    std::map<std::string, Group> groups;
    std::size_t index = 0;
    for (const auto& item : items) {
        auto& group = groups[item.identity()];
        if (group.count == 0) {
            group.representative = item;
            group.first_seen = index;
        } else if (!item.file.empty() && item.file != group.representative.file &&
                   group.other_places.size() < 3) {
            group.other_places.push_back(item.file);
        }
        ++group.count;
        ++index;
    }

    std::vector<const Group*> ordered;
    ordered.reserve(groups.size());
    for (const auto& [key, group] : groups) ordered.push_back(&group);
    std::sort(ordered.begin(), ordered.end(), [](const Group* a, const Group* b) {
        // Errors first, then in the order the toolchain reported them: the
        // first error is usually the one that caused the rest.
        if (a->representative.severity != b->representative.severity) {
            return static_cast<int>(a->representative.severity) <
                   static_cast<int>(b->representative.severity);
        }
        return a->first_seen < b->first_seen;
    });

    const auto errors = error_count();
    const auto warnings = warning_count();

    std::string out = "Diagnostics: " + std::to_string(errors) + " error" +
                      (errors == 1 ? "" : "s");
    if (warnings > 0) {
        out += ", " + std::to_string(warnings) + " warning" + (warnings == 1 ? "" : "s");
    }
    out += "\n";

    std::size_t shown = 0;
    for (const auto* group : ordered) {
        if (shown >= max_reported) break;
        out += "  " + group->representative.describe();
        if (group->count > 1) {
            out += "   (" + std::to_string(group->count) + " times";
            if (!group->other_places.empty()) {
                out += ", also in " + strings::join(group->other_places, ", ");
            }
            out += ")";
        }
        out += "\n";
        ++shown;
    }
    if (ordered.size() > shown) {
        out += "  ... and " + std::to_string(ordered.size() - shown) +
               " more distinct diagnostic(s); the full log has all of them\n";
    }
    return out;
}

DiagnosticSet parse_diagnostics(std::string_view text) {
    DiagnosticSet set;
    // A build that fails in forty translation units at once should not fill the
    // process with regex work; the summary collapses them anyway.
    constexpr std::size_t kMaxParsed = 400;

    for (const auto& line : strings::split_lines(text)) {
        if (set.items.size() >= kMaxParsed) break;
        const auto trimmed = strings::trim(line);
        if (trimmed.empty()) continue;

        Diagnostic diagnostic;
        diagnostic.raw = strings::utf8_truncate(trimmed, 500);

        // Order matters: the MSVC linker form and the MSVC compiler form both
        // end in "code: message", and gcc's "file:line:col:" would swallow a
        // Windows path's drive colon if it went first.
        const bool recognised = parse_msvc_compiler(trimmed, diagnostic) ||
                                parse_msvc_linker(trimmed, diagnostic) ||
                                parse_gcc_clang(trimmed, diagnostic) ||
                                parse_cmake(trimmed, diagnostic) ||
                                parse_build_step(trimmed, diagnostic) ||
                                parse_rust(trimmed, diagnostic) ||
                                parse_runtime(trimmed, diagnostic);
        if (!recognised) continue;
        if (diagnostic.message.empty()) diagnostic.message = diagnostic.raw;
        set.items.push_back(std::move(diagnostic));
    }
    return set;
}

}  // namespace localcode
