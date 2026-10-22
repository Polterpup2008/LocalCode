#pragma once
// ---------------------------------------------------------------------------
// Reading a build log the way a person does.
//
// A failed build produces thousands of lines, of which about four matter. The
// first version of this pulled out lines that looked like errors and pasted
// them into the result verbatim, which was better than nothing and still left
// the model to do the parsing - and small models parse MSVC output badly, then
// invent a file and line number that sound right.
//
// So diagnostics are parsed into fields: severity, code, file, line, column,
// message. That makes them groupable (forty instances of one missing header
// become one line saying so), sortable (errors before warnings, first before
// last), and countable without regex-matching the same text twice. Anything
// that does not parse is kept as a raw line rather than dropped, because a
// diagnostic format nobody anticipated is still a diagnostic.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace localcode {

enum class DiagnosticSeverity { Error, Warning, Note };

[[nodiscard]] std::string_view to_string(DiagnosticSeverity severity) noexcept;

// Which part of the toolchain produced it. A linker error and a compiler error
// call for different next steps, and saying which is which saves a turn.
enum class DiagnosticOrigin { Compiler, Linker, BuildSystem, TestRunner, Runtime, Unknown };

[[nodiscard]] std::string_view to_string(DiagnosticOrigin origin) noexcept;

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    DiagnosticOrigin   origin = DiagnosticOrigin::Unknown;
    std::string        code;      // C2065, LNK2019, E0308 - empty when the format has none
    std::string        file;
    int                line = 0;  // 0 when the format gave none
    int                column = 0;
    std::string        message;
    std::string        raw;       // the line as it appeared

    [[nodiscard]] bool is_error() const { return severity == DiagnosticSeverity::Error; }
    // "src\main.cpp(12,5): error C2065: 'x': undeclared identifier"
    [[nodiscard]] std::string describe() const;
    // What makes two reports of the same problem the same problem.
    [[nodiscard]] std::string identity() const;
};

struct DiagnosticSet {
    std::vector<Diagnostic> items;

    [[nodiscard]] std::size_t error_count() const;
    [[nodiscard]] std::size_t warning_count() const;
    [[nodiscard]] bool empty() const { return items.empty(); }

    // The block that goes to the model: errors first, grouped by file, repeats
    // collapsed into a count, and no more than `max_reported` of them.
    [[nodiscard]] std::string summarise(std::size_t max_reported = 12) const;
};

[[nodiscard]] DiagnosticSet parse_diagnostics(std::string_view text);

}  // namespace localcode
