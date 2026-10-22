#include "CriterionStrength.h"

#include <cctype>
#include <regex>

#include "../util/Strings.h"

namespace localcode {
namespace {

bool mentions_any(std::string_view lowered, std::initializer_list<const char*> words) {
    for (const char* word : words) {
        if (lowered.find(word) != std::string_view::npos) return true;
    }
    return false;
}

// Words that describe a program doing something, as opposed to being built.
bool describes_behaviour(std::string_view lowered) {
    return mentions_any(lowered, {"crash", "print", "output", "outputs", "returns", "return ",
                                  "prints", "when opening", "at runtime", "wrong answer",
                                  "incorrect", "produces", "result", "behaves", "behaviour",
                                  "behavior", "segfault", "hang", "exception", "throws"});
}

bool describes_building(std::string_view lowered) {
    return mentions_any(lowered, {"compile", "build", "link", "configure", "cmake"});
}

// Read concrete assignments conservatively. Bare strings need '='; prose
// connectors and ':' accept only numbers or quoted strings. Otherwise ordinary
// descriptions such as "output is outdated" or "README.md: the output..." would
// invent a required value and block a perfectly adequate plan.
std::string demanded_value(std::string_view objective) {
    static const std::regex assignment{
        R"re(\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*("[^"\r\n]{1,512}"|[-A-Za-z0-9_.]+)|(?::|\bis\b|\bequals\b|\bof\b)\s*("[^"\r\n]{1,512}"|[-+]?\d+(?:\.\d+)?\b)))re"};
    const std::string text{objective};
    std::smatch match;
    if (!std::regex_search(text, match, assignment)) return {};

    // "count = 4." ends a sentence, and the full stop is not part of the value.
    // A decimal point between digits is.
    auto value = match[2].matched ? match[2].str() : match[3].str();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    while (!value.empty() &&
           (value.back() == '.' || value.back() == ',' || value.back() == '"')) {
        if (value.back() == '.' && value.size() >= 2 &&
            std::isdigit(static_cast<unsigned char>(value[value.size() - 2])) != 0 &&
            value.find('.') != value.size() - 1) {
            break;
        }
        value.pop_back();
    }
    return value;
}

}  // namespace

ObjectiveDemand demand_of(std::string_view objective) {
    const auto lowered = strings::to_lower(objective);
    const bool behaviour = describes_behaviour(lowered);
    const bool building = describes_building(lowered);

    // Behaviour wins when both appear: "make it build and print the right
    // answer" demands the answer, and the build is on the way there.
    if (behaviour) return ObjectiveDemand::RuntimeBehaviour;
    if (building) return ObjectiveDemand::BuildOnly;
    return ObjectiveDemand::Unknown;
}

StrengthFinding assess_plan_strength(std::string_view objective,
                                     const std::vector<std::string>& criteria) {
    StrengthFinding finding;
    if (criteria.empty()) return finding;

    const auto demand = demand_of(objective);
    if (demand != ObjectiveDemand::RuntimeBehaviour) return finding;

    // Does anything in the plan talk about what the program does?
    bool covers_behaviour = false;
    for (const auto& criterion : criteria) {
        const auto lowered = strings::to_lower(criterion);
        if (describes_behaviour(lowered) || mentions_any(lowered, {"run", "runs", "executes"})) {
            covers_behaviour = true;
            break;
        }
    }

    if (!covers_behaviour) {
        finding.insufficient = true;
        finding.reason =
            "this task is about what the program does when it runs, and no criterion here is. "
            "Every one of them can be satisfied by code that builds and still does the wrong "
            "thing - which is what the task says is happening now.";
        finding.suggestion =
            "Add a criterion naming the behaviour that has to be true afterwards, and settle "
            "it by running the program under an expectation.";
        return finding;
    }

    // The request names a required value. A criterion that mentions the field
    // without the value is satisfied by any value at all, including the one
    // being complained about.
    const auto value = demanded_value(objective);
    if (!value.empty()) {
        bool covers_value = false;
        for (const auto& criterion : criteria) {
            if (criterion.find(value) != std::string::npos) {
                covers_value = true;
                break;
            }
        }
        if (!covers_value) {
            finding.insufficient = true;
            finding.reason = "the request asks for " + value +
                             " specifically, and no criterion mentions it. A check that the "
                             "field is present passes whatever the value turns out to be.";
            finding.suggestion =
                "Add a criterion that states the required value, not just the field.";
        }
    }
    return finding;
}

}  // namespace localcode
