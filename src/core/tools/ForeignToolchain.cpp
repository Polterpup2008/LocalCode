#include "ForeignToolchain.h"

#include <algorithm>
#include <regex>

#include "../util/Strings.h"

namespace localcode {
namespace {

struct Signature {
    const char* toolchain;
    const char* pattern;
    // The program that would exist if it were installed. Empty when no single
    // executable settles it - a missing probe means "ask", not "absent".
    const char* probe;
};

// Deliberately short, and deliberately made of things that cannot plausibly
// appear in a project that does not use the toolchain. `#include <QWidget>`
// belongs to Qt and nothing else; `class Foo : MonoBehaviour` is Unity and
// nothing else. Anything more inventive than that produces false positives,
// and a false positive here is worse than silence: it teaches people to
// ignore the warning.
const std::vector<Signature>& signatures() {
    static const std::vector<Signature> kSignatures = {
        // Unreal. The editor is the build system, which is what makes it
        // unbuildable from here rather than merely absent.
        {"Unreal Engine", R"(#\s*include\s*"CoreMinimal\.h")", "UnrealEditor"},
        {"Unreal Engine", R"(\bGENERATED_BODY\s*\(\s*\))", "UnrealEditor"},
        {"Unreal Engine", R"(^\s*(UCLASS|USTRUCT|UENUM|UPROPERTY|UFUNCTION)\s*\()", "UnrealEditor"},
        {"Unreal Engine", R"(#\s*include\s*"(Engine|GameFramework)/)", "UnrealEditor"},

        // Unity.
        {"Unity", R"(^\s*using\s+UnityEngine\s*;)", "Unity"},
        {"Unity", R"(:\s*MonoBehaviour\b)", "Unity"},

        // Godot's script runtime.
        {"Godot", R"(^\s*extends\s+(Node|Node2D|Node3D|Control|CharacterBody[23]D)\b)", "godot"},
        {"Godot", R"(#\s*include\s*<godot_cpp/)", "godot"},

        // Toolkits that need headers and libraries the compiler has to find.
        {"Qt", R"(#\s*include\s*<Q(Application|Widget|MainWindow|Object)>)", "qmake"},
        {"SDL", R"(#\s*include\s*[<"]SDL[23]?/?[A-Za-z_]*\.h[>"])", ""},
        {"raylib", R"(#\s*include\s*[<"]raylib\.h[>"])", ""},
        {"SFML", R"(#\s*include\s*<SFML/)", ""},
        {"Boost", R"(#\s*include\s*<boost/)", ""},

        // Platform SDKs that cannot be built on this machine at all.
        {"Apple UIKit/AppKit", R"(#\s*import\s*<(UIKit|Cocoa|AppKit)/)", ""},
        {"Android SDK", R"(^\s*import\s+android\.)", ""},
    };
    return kSignatures;
}

}  // namespace

std::string ToolchainSignal::describe() const {
    std::string out = toolchain;
    if (line > 0) out += " (line " + std::to_string(line) + ")";
    if (!matched.empty()) out += ": " + matched;
    return out;
}

std::vector<ToolchainSignal> detect_foreign_toolchains(std::string_view path,
                                                       std::string_view content) {
    (void)path;
    static const auto compiled = [] {
        std::vector<std::pair<std::regex, const Signature*>> result;
        for (const auto& signature : signatures()) {
            try {
                result.emplace_back(std::regex{signature.pattern, std::regex::optimize},
                                    &signature);
            } catch (const std::regex_error&) {
                // A malformed pattern here is a bug in this file, not a user
                // problem; skipping it beats taking down every file write.
            }
        }
        return result;
    }();

    std::vector<ToolchainSignal> found;
    int line_number = 0;
    for (const auto& line : strings::split_lines(content)) {
        ++line_number;
        if (line.empty()) continue;
        for (const auto& [expression, signature] : compiled) {
            if (!std::regex_search(line, expression)) continue;
            // One report per toolchain per file. Forty Unreal includes are one
            // decision, not forty.
            const bool already = std::any_of(
                found.begin(), found.end(), [&](const ToolchainSignal& seen) {
                    return seen.toolchain == signature->toolchain;
                });
            if (already) break;

            ToolchainSignal signal;
            signal.toolchain = signature->toolchain;
            signal.matched = strings::utf8_truncate(strings::trim(line), 160);
            signal.line = line_number;
            signal.probe = signature->probe;
            found.push_back(std::move(signal));
            break;
        }
    }
    return found;
}

}  // namespace localcode
