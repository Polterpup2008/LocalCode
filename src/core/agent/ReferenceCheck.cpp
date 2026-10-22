#include "ReferenceCheck.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>

#include "../util/Strings.h"

namespace localcode {
namespace {

// "the first ten", "first 10", "10 numbers". Words up to twenty, because past
// that people write digits.
std::optional<int> requested_count(std::string_view lowered) {
    static const std::pair<const char*, int> kWords[] = {
        {"twenty", 20},  {"nineteen", 19}, {"eighteen", 18}, {"seventeen", 17}, {"sixteen", 16},
        {"fifteen", 15}, {"fourteen", 14}, {"thirteen", 13}, {"twelve", 12},    {"eleven", 11},
        {"ten", 10},     {"nine", 9},      {"eight", 8},     {"seven", 7},      {"six", 6},
        {"five", 5},     {"four", 4},      {"three", 3},     {"two", 2},
    };
    // A number word elsewhere in the request is not a sequence length:
    // "height" contains "eight", and "written" contains "ten". Prefer an
    // explicit ordinal count, then a count attached to the sequence noun.
    static const std::regex first{
        R"(\bfirst\s+(\d{1,3}|twenty|nineteen|eighteen|seventeen|sixteen|fifteen|fourteen|thirteen|twelve|eleven|ten|nine|eight|seven|six|five|four|three|two)\b)"};
    static const std::regex counted{
        R"(\b(\d{1,3}|twenty|nineteen|eighteen|seventeen|sixteen|fifteen|fourteen|thirteen|twelve|eleven|ten|nine|eight|seven|six|five|four|three|two)\s+(?:fibonacci\s+)?(?:numbers|terms|values|squares|factorials)\b)"};
    const std::string text{lowered};
    std::smatch match;
    if (std::regex_search(text, match, first) || std::regex_search(text, match, counted)) {
        const auto captured = match[1].str();
        for (const auto& [word, value] : kWords)
            if (captured == word) return value;
        try {
            const int value = std::stoi(captured);
            if (value > 0 && value <= 200) return value;
        } catch (...) {
        }
    }

    // "from 1 through 5", "1 to 5", "between 1 and 10" - a range rather than a
    // count, and the ordinary way to ask for squares or factorials. Only from
    // 1, because a sequence starting elsewhere is a different series and
    // guessing which would be the mistake this whole checker exists to catch.
    static const std::regex range{R"(\b(?:from\s+)?1\s*(?:through|thru|to|-|\.\.)\s*(\d{1,3})\b)"};
    if (std::regex_search(text, match, range)) {
        try {
            const int last = std::stoi(match[1].str());
            if (last > 1 && last <= 200) return last;
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::optional<std::vector<long long>> fibonacci(int count, long long first, long long second) {
    std::vector<long long> values;
    long long a = first;
    long long b = second;
    for (int i = 0; i < count; ++i) {
        values.push_back(a);
        if (i + 1 == count) break;
        const long long next_a = b;
        // The final requested value does not require computing its successor.
        if (i + 2 < count) {
            if (a > std::numeric_limits<long long>::max() - b) return std::nullopt;
            b = a + b;
        }
        a = next_a;
    }
    return values;
}

std::string render(const std::vector<long long>& values) {
    std::vector<std::string> parts;
    parts.reserve(values.size());
    for (const auto value : values)
        parts.push_back(std::to_string(value));
    return strings::join(parts, " ");
}

// The numbers in a piece of text, in order. Used to compare an expectation with
// a reference without caring how it was punctuated or laid out.
std::vector<long long> numbers_in(std::string_view text) {
    std::vector<long long> values;
    std::string current;
    const auto flush = [&] {
        if (current.empty()) return;
        try {
            values.push_back(std::stoll(current));
        } catch (...) {
        }
        current.clear();
    };
    for (const unsigned char c : text) {
        if (std::isdigit(c) != 0 || (c == '-' && current.empty())) {
            current.push_back(static_cast<char>(c));
        } else {
            flush();
        }
    }
    flush();
    return values;
}

// --- Statistics over an explicit sample list -------------------------------
//
// "the median and mean of the samples 8, 1, 3, 2" determines two numbers, and a
// criterion that states a different one is wrong however cleanly it settles.
// The median of an even-sized set is the classic plausible-wrong-answer: taking
// the upper middle value gives 3 where the answer is 2.5, and 3 is a real
// number from the data.
//
// Deliberately narrow. It fires only when the request names the statistic AND
// lists the samples, and it compares only against a value the expectation
// writes out as "median = X". Anything vaguer is left alone.
// The value the expectation assigns to `name`, as in "median=2.5" or
// "median is 2.5". Absent when it does not state one.
std::optional<double> stated_value(std::string_view expectation, const std::string& name) {
    const std::regex assignment{name + R"(\s*(?:=|:|\bis\b)\s*(-?\d+(?:\.\d+)?))",
                                std::regex::icase};
    const std::string owned{expectation};
    std::smatch match;
    if (!std::regex_search(owned, match, assignment)) return std::nullopt;
    try {
        return std::stod(match[1].str());
    } catch (...) {
        return std::nullopt;
    }
}

// The samples a request lists, when it lists any. Requires at least three so a
// stray pair of numbers in prose is not mistaken for a data set.
std::optional<std::vector<double>> sample_list(std::string_view request) {
    const auto lowered = strings::to_lower(request);
    const auto word_character = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    std::size_t marker = 0;
    for (;;) {
        marker = lowered.find("samples", marker);
        if (marker == std::string::npos) return std::nullopt;
        const auto after = marker + 7;
        if ((marker == 0 || !word_character(lowered[marker - 1])) &&
            (after == lowered.size() || !word_character(lowered[after])))
            break;
        marker = after;
    }
    std::size_t at = marker + 7;
    const auto whitespace = [&] {
        while (at < request.size() && std::isspace(static_cast<unsigned char>(request[at])))
            ++at;
    };
    whitespace();
    if (at < request.size() && (request[at] == ':' || request[at] == '=')) {
        ++at;
        whitespace();
    }
    char closing = 0;
    if (at < request.size()) {
        if (request[at] == '[') closing = ']';
        if (request[at] == '{') closing = '}';
        if (request[at] == '(') closing = ')';
        if (closing) {
            ++at;
            whitespace();
        }
    }
    std::vector<double> values;
    const auto finish = [&]() -> std::optional<std::vector<double>> {
        if (values.size() < 3) return std::nullopt;
        return values;
    };
    for (;;) {
        const auto start = at;
        if (at < request.size() && (request[at] == '-' || request[at] == '+')) ++at;
        const auto digits = at;
        while (at < request.size() && std::isdigit(static_cast<unsigned char>(request[at])))
            ++at;
        if (at == digits) return std::nullopt;
        if (at + 1 < request.size() && request[at] == '.' &&
            std::isdigit(static_cast<unsigned char>(request[at + 1]))) {
            ++at;
            while (at < request.size() && std::isdigit(static_cast<unsigned char>(request[at])))
                ++at;
        }
        try {
            const auto number = std::stod(std::string{request.substr(start, at - start)});
            if (!std::isfinite(number)) return std::nullopt;
            values.push_back(number);
        } catch (...) {
            return std::nullopt;
        }
        if (values.size() > 128) return std::nullopt;
        if (at == request.size()) return closing ? std::nullopt : finish();
        if (closing && request[at] == closing) return finish();
        // No units, exponent suffixes, ranges or ellipses: their meaning is not
        // established by this deliberately small reference grammar.
        if (request[at] == '.') {
            if (closing || (at + 1 < request.size() &&
                            (request[at + 1] == '.' ||
                             std::isdigit(static_cast<unsigned char>(request[at + 1])))))
                return std::nullopt;
            return finish();
        }
        bool separator = false;
        if (std::isspace(static_cast<unsigned char>(request[at]))) {
            separator = true;
            whitespace();
        }
        if (at < request.size() && (request[at] == ',' || request[at] == ';')) {
            separator = true;
            ++at;
            whitespace();
            if (at == request.size()) return std::nullopt;
        }
        if (!separator) return std::nullopt;
        if (at == request.size()) return closing ? std::nullopt : finish();
        if (closing && request[at] == closing) return finish();
        const auto tail = lowered.substr(at);
        if (tail.starts_with("and ")) {
            at += 4;
            whitespace();
            if (at == request.size() || !(std::isdigit(static_cast<unsigned char>(request[at])) ||
                                          request[at] == '-' || request[at] == '+'))
                return std::nullopt;
        } else if (tail.starts_with("or ") || tail.starts_with("to ") ||
                   tail.starts_with("through "))
            return std::nullopt;
        if (at < request.size() && (std::isdigit(static_cast<unsigned char>(request[at])) ||
                                    request[at] == '-' || request[at] == '+'))
            continue;
        // Prose following an unbracketed list is not more data. An opened list
        // must close explicitly rather than silently accepting unknown syntax.
        return closing ? std::nullopt : finish();
    }
}

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n % 2 == 1) return values[n / 2];
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

double mean_of(const std::vector<double>& values) {
    double total = 0.0;
    for (const double v : values)
        total += v;
    return total / static_cast<double>(values.size());
}

// Compares the statistics a request determines with the ones an expectation
// states. Empty when there is nothing to compare.
std::string statistic_conflict(std::string_view request, std::string_view expectation) {
    const auto samples = sample_list(request);
    if (!samples) return {};
    // A negative-test input is not the output value the criterion asserts.
    // This checker deliberately declines prose outside a small positive claim
    // grammar rather than disputing a correct rejection/conditional test.
    static const std::regex output_claim{
        R"(^\s*(?:(?:the\s+)?(?:program|output|result)\s+){0,3}(?:(?:prints?|outputs?|reports?|produces?|matches|equals)\s+)?["'`]?(?:the\s+)?(?:median|mean)\b)",
        std::regex::icase};
    static const std::regex input_context{
        R"(\b(?:if|when|input|expectation|reject(?:s|ed|ing|ion)?|invalid|incorrect|wrong|conflict|mismatch)\b)",
        std::regex::icase};
    const std::string claimed{strings::trim(expectation)};
    if (claimed.empty() || claimed.front() == '"' || claimed.front() == '\'' ||
        claimed.front() == '`')
        return {};
    if (!std::regex_search(claimed, output_claim) || std::regex_search(claimed, input_context))
        return {};
    const auto lowered = strings::to_lower(request);

    const auto compare = [&](const char* word, double reference) -> std::string {
        if (lowered.find(word) == std::string::npos) return {};
        const auto stated = stated_value(expectation, word);
        if (!stated) return {};
        if (std::abs(*stated - reference) < 1e-9) return {};
        std::ostringstream out;
        out << "the " << word << " of those samples is " << reference << ", and the "
            << "expectation says " << *stated
            << ". Work out which the task asks for before treating either as correct.";
        return out.str();
    };

    if (auto conflict = compare("median", median_of(*samples)); !conflict.empty()) return conflict;
    if (auto conflict = compare("mean", mean_of(*samples)); !conflict.empty()) return conflict;
    return {};
}

}   // namespace

std::optional<std::vector<long long>> reference_sequence(std::string_view request) {
    const auto lowered = strings::to_lower(request);

    if (lowered.find("fibonacci") != std::string::npos) {
        const auto count = requested_count(lowered);
        if (!count) return std::nullopt;

        // Both conventions are in use and the request usually says which. When
        // it does not, LocalCode has no basis for choosing and says nothing -
        // an arbitrary pick here would be the same mistake the model made,
        // wearing a different hat.
        const bool from_zero = lowered.find("from 0") != std::string::npos ||
                               lowered.find("starting at 0") != std::string::npos ||
                               lowered.find("starting from 0") != std::string::npos ||
                               lowered.find("start at 0") != std::string::npos ||
                               lowered.find("begins with 0") != std::string::npos ||
                               lowered.find("0 1 1") != std::string::npos;
        const bool from_one = lowered.find("from 1") != std::string::npos ||
                              lowered.find("starting at 1") != std::string::npos ||
                              lowered.find("starting from 1") != std::string::npos ||
                              lowered.find("1 1 2") != std::string::npos;
        if (from_zero == from_one) return std::nullopt;   // both, or neither
        return from_zero ? fibonacci(*count, 0, 1) : fibonacci(*count, 1, 1);
    }

    if (lowered.find("square") != std::string::npos && lowered.find("root") == std::string::npos) {
        const auto count = requested_count(lowered);
        if (!count) return std::nullopt;
        std::vector<long long> values;
        for (int i = 1; i <= *count; ++i)
            values.push_back(static_cast<long long>(i) * i);
        return values;
    }

    if (lowered.find("factorial") != std::string::npos) {
        const auto count = requested_count(lowered);
        if (!count || *count > 20) return std::nullopt;   // 21! overflows
        std::vector<long long> values;
        long long running = 1;
        for (int i = 1; i <= *count; ++i) {
            running *= i;
            values.push_back(running);
        }
        return values;
    }

    return std::nullopt;
}

ReferenceVerdict check_against_reference(std::string_view request, std::string_view expectation) {
    ReferenceVerdict verdict;

    // A statistic the request determines outright - the median of a listed
    // sample set - which is a different shape from a sequence and is checked
    // first because it is the more exact of the two.
    if (auto conflict = statistic_conflict(request, expectation); !conflict.empty()) {
        verdict.applicable = true;
        verdict.agrees = false;
        verdict.derivation = "computed from the samples in the request";
        verdict.conflict = conflict;
        return verdict;
    }

    const auto reference = reference_sequence(request);
    if (!reference || reference->empty()) return verdict;   // no opinion

    // The expectation has to actually state the sequence before it can be
    // compared with one. Prose that merely mentions a number is not a claim
    // about the answer, and treating it as one produces a conflict that is
    // simply wrong - which is far worse than having no opinion, because it
    // blocks a correct run.
    //
    // Measured: the criterion "prints the first ten Fibonacci numbers starting
    // from 0, one per line" yielded the single number 0, which was compared
    // against the ten-value reference and reported as a disagreement. The
    // criterion was right. The check was not.
    //
    // So: only compare when as many numbers are stated as the reference has.
    // Fewer means the text is describing the requirement rather than spelling
    // it out, and this has nothing to say about it.
    const auto claimed = numbers_in(expectation);
    if (claimed.size() < reference->size()) return verdict;

    verdict.applicable = true;
    verdict.reference = render(*reference);
    verdict.derivation =
        "computed from the request: " + std::to_string(reference->size()) + " values";

    // The sequence has to appear in the text, not to be the whole of it. A
    // criterion states its numbers surrounded by prose that has numbers of its
    // own, and those are not claims about the answer.
    //
    // Measured: "prints the first ten Fibonacci numbers (0, 1, 1, 2, 3, 5, 8,
    // 13, 21, 34) starting from 0, one per line" yields eleven numbers - the
    // ten, plus the 0 from "starting from 0" - and comparing the whole list
    // reported a disagreement with the sequence it had just quoted correctly.
    // The criterion was right; the check was not, and a wrong conflict blocks
    // a correct run.
    const auto contains_reference = std::search(claimed.begin(), claimed.end(), reference->begin(),
                                                reference->end()) != claimed.end();
    if (contains_reference) {
        verdict.agrees = true;
        return verdict;
    }

    verdict.agrees = false;
    verdict.conflict = "the request determines " + verdict.reference + ", and the expectation is " +
                       render(claimed) +
                       ". Work out which the user asked for before treating either as "
                       "correct - do not simply keep your own.";
    return verdict;
}

}   // namespace localcode
