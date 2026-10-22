// ---------------------------------------------------------------------------
// Code written against something that is not on the machine.
//
// Asked for a game, a model wrote `#include "CoreMinimal.h"`, stubbed an input
// struct, and announced it would "create the level in Unreal". There was no
// Unreal installation anywhere. The run could not have reached a build, so it
// could not have reached a behaviour check, so it could not have finished -
// and nothing said so until the end.
//
// The bar for a signature here is high on purpose. A false positive teaches
// people to ignore the warning, which is worse than not having one.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include "tools/ForeignToolchain.h"

using namespace localcode;

namespace {

bool names(const std::vector<ToolchainSignal>& signals, std::string_view toolchain) {
    for (const auto& signal : signals) {
        if (signal.toolchain == toolchain) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("the Unreal file from the game run is recognised", "[toolchain]") {
    // The actual text, near enough.
    const auto signals = detect_foreign_toolchains("Source/Game/Input.h", R"(
#include "CoreMinimal.h"

struct FInputActions
{
    const FString ActionLookYaw = "LookYaw";
    const FString ActionJump = "Jump";
};
)");
    REQUIRE_FALSE(signals.empty());
    CHECK(names(signals, "Unreal Engine"));
    CHECK(signals[0].line == 2);
    CHECK(signals[0].probe == "UnrealEditor");
}

TEST_CASE("the other Unreal signatures fire too", "[toolchain]") {
    CHECK(names(detect_foreign_toolchains("A.h", "class AMyActor {\n    GENERATED_BODY()\n};\n"),
                "Unreal Engine"));
    CHECK(names(detect_foreign_toolchains("A.h", "UPROPERTY(EditAnywhere)\nint Health;\n"),
                "Unreal Engine"));
    CHECK(names(detect_foreign_toolchains("A.cpp", "#include \"GameFramework/Actor.h\"\n"),
                "Unreal Engine"));
}

TEST_CASE("other engines and platform SDKs are recognised", "[toolchain]") {
    CHECK(names(detect_foreign_toolchains("P.cs", "using UnityEngine;\n"), "Unity"));
    CHECK(names(detect_foreign_toolchains("P.cs", "public class P : MonoBehaviour {}\n"), "Unity"));
    CHECK(names(detect_foreign_toolchains("p.gd", "extends CharacterBody2D\n"), "Godot"));
    CHECK(names(detect_foreign_toolchains("v.mm", "#import <UIKit/UIKit.h>\n"),
                "Apple UIKit/AppKit"));
    CHECK(names(detect_foreign_toolchains("A.java", "import android.os.Bundle;\n"),
                "Android SDK"));
}

TEST_CASE("libraries the compiler has to find are flagged as well", "[toolchain]") {
    CHECK(names(detect_foreign_toolchains("main.cpp", "#include <SDL2/SDL.h>\n"), "SDL"));
    CHECK(names(detect_foreign_toolchains("main.cpp", "#include \"raylib.h\"\n"), "raylib"));
    CHECK(names(detect_foreign_toolchains("main.cpp", "#include <QApplication>\n"), "Qt"));
    CHECK(names(detect_foreign_toolchains("main.cpp", "#include <boost/asio.hpp>\n"), "Boost"));
}

TEST_CASE("ordinary code raises nothing", "[toolchain]") {
    // This is the case that matters most. Every false positive here spends the
    // user's attention on nothing and trains them to skip the real one.
    const auto signals = detect_foreign_toolchains("src/stats.cpp", R"(
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "stats.h"

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}
)");
    CHECK(signals.empty());
}

TEST_CASE("near misses do not fire", "[toolchain]") {
    // A game that is not an engine's game.
    CHECK(detect_foreign_toolchains("game.cpp",
                                    "// A little console game.\n"
                                    "#include <iostream>\n"
                                    "int main() { std::cout << \"guess a number\\n\"; }\n")
              .empty());
    // Words that merely mention the thing.
    CHECK(detect_foreign_toolchains("README.md",
                                    "We considered Unreal and Unity and chose neither.\n")
              .empty());
    // A local header whose name is not the engine's.
    CHECK(detect_foreign_toolchains("a.cpp", "#include \"Core.h\"\n").empty());
    CHECK(detect_foreign_toolchains("a.js", "import android from './android';\n").empty());
}

TEST_CASE("one report per toolchain per file", "[toolchain]") {
    // Forty Unreal includes are one decision, not forty.
    std::string source;
    for (int i = 0; i < 40; ++i) source += "#include \"CoreMinimal.h\"\n";
    const auto signals = detect_foreign_toolchains("A.h", source);
    CHECK(signals.size() == 1);
}

TEST_CASE("a file using two of them reports both", "[toolchain]") {
    const auto signals = detect_foreign_toolchains(
        "main.cpp", "#include <QApplication>\n#include <boost/asio.hpp>\n");
    CHECK(signals.size() == 2);
    CHECK(names(signals, "Qt"));
    CHECK(names(signals, "Boost"));
}

TEST_CASE("a signal that has no single program to look for says so", "[toolchain]") {
    // "No probe" must not read as "not installed" - SDL is a library, and no
    // executable settles whether its headers are reachable.
    const auto signals = detect_foreign_toolchains("main.cpp", "#include <SDL.h>\n");
    REQUIRE(signals.size() == 1);
    CHECK(signals[0].probe.empty());
    CHECK_FALSE(signals[0].probe_failed);
}

TEST_CASE("a rendered signal names the toolchain and the line", "[toolchain]") {
    const auto signals = detect_foreign_toolchains("Input.h",
                                                   "// input\n#include \"CoreMinimal.h\"\n");
    REQUIRE_FALSE(signals.empty());
    const auto text = signals[0].describe();
    INFO(text);
    CHECK(text.find("Unreal Engine") != std::string::npos);
    CHECK(text.find("line 2") != std::string::npos);
    CHECK(text.find("CoreMinimal.h") != std::string::npos);
}
