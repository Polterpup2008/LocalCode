// ---------------------------------------------------------------------------
// Path policy: the boundary that decides what the agent may modify.
//
// These are the most important tests in the suite. A hole here means a model
// can be talked into writing outside the workspace.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "security/PathPolicy.h"
#include "platform/PathEncoding.h"
#include "support/TempDirectory.h"

using namespace localcode;
using localcode::test::TempDirectory;

TEST_CASE("UTF-8 policy paths preserve native Unicode identity and containment", "[path][unicode][security]") {
    TempDirectory directory;
    const auto root = directory.path() / path_from_utf8("projet é 路径 🦍");
    std::filesystem::create_directories(root);
    const auto relative = std::string("calcul é 路径 🦍.py");
    const auto expected = root / path_from_utf8(relative);
    { std::ofstream out(expected); out << "owned Unicode file\n"; }
    PathPolicy policy{root};
    for (const auto& input : {relative, path_to_utf8(expected), std::string("/workspace/") + relative}) {
        const auto decision = policy.evaluate(input, PathAccess::Write);
        REQUIRE(decision.allowed);
        CHECK(decision.inside_workspace);
        CHECK(decision.canonical == PathPolicy::canonicalize(expected));
        CHECK(path_to_utf8(decision.relative) == relative);
    }
    CHECK_FALSE(policy.evaluate("../outside.txt", PathAccess::Write).allowed);
    const auto invalid = policy.evaluate(std::string("bad-") + char(-1), PathAccess::Write);
    CHECK_FALSE(invalid.allowed);
    CHECK(invalid.reason == "path must be valid UTF-8");
}

TEST_CASE("relative paths resolve inside the workspace", "[path][security]") {
    TempDirectory workspace{"lc-ws"};
    workspace.write("src/main.cpp", "int main() {}\n");

    PathPolicy policy{workspace.path()};

    auto decision = policy.evaluate("src/main.cpp", PathAccess::Read);
    REQUIRE(decision.allowed);
    REQUIRE(decision.inside_workspace);
    CHECK(decision.relative.generic_string() == "src/main.cpp");

    auto write = policy.evaluate("src/main.cpp", PathAccess::Write);
    CHECK(write.allowed);
}

TEST_CASE("parent traversal cannot escape the workspace", "[path][security]") {
    TempDirectory root{"lc-root"};
    const auto workspace_dir = root.make_directory("workspace");
    root.write("secret.txt", "do not touch");

    PathPolicy policy{workspace_dir};

    SECTION("simple traversal") {
        auto decision = policy.evaluate("../secret.txt", PathAccess::Write);
        CHECK_FALSE(decision.allowed);
        CHECK_FALSE(decision.inside_workspace);
    }
    SECTION("traversal buried in the middle") {
        auto decision = policy.evaluate("src/../../secret.txt", PathAccess::Write);
        CHECK_FALSE(decision.allowed);
    }
    SECTION("many levels up") {
        auto decision = policy.evaluate("../../../../../../etc/passwd", PathAccess::Write);
        CHECK_FALSE(decision.allowed);
    }
    SECTION("traversal that comes back inside is fine") {
        auto decision = policy.evaluate("src/../include/header.h", PathAccess::Create);
        CHECK(decision.allowed);
        CHECK(decision.inside_workspace);
    }
}

TEST_CASE("reading outside the workspace is allowed, writing is not", "[path][security]") {
    TempDirectory root{"lc-root"};
    const auto workspace_dir = root.make_directory("workspace");
    const auto outside = root.write("sibling/notes.txt", "reference material");

    PathPolicy policy{workspace_dir};

    // Reading a header from the SDK or a sibling checkout is ordinary work.
    auto read = policy.evaluate(outside.string(), PathAccess::Read);
    CHECK(read.allowed);
    CHECK_FALSE(read.inside_workspace);

    // Writing there is not.
    auto write = policy.evaluate(outside.string(), PathAccess::Write);
    CHECK_FALSE(write.allowed);
    CHECK(write.reason.find("workspace") != std::string::npos);
}

TEST_CASE("read confinement can be switched on", "[path][security]") {
    TempDirectory root{"lc-root"};
    const auto workspace_dir = root.make_directory("workspace");
    const auto outside = root.write("sibling/notes.txt", "reference");

    PathPolicy policy{workspace_dir};
    policy.set_allow_read_outside_workspace(false);

    CHECK_FALSE(policy.evaluate(outside.string(), PathAccess::Read).allowed);
    CHECK(policy.evaluate("inside.txt", PathAccess::Read).allowed);
}

TEST_CASE("explicitly granted roots become writable", "[path][security]") {
    TempDirectory root{"lc-root"};
    const auto workspace_dir = root.make_directory("workspace");
    const auto extra = root.make_directory("build-output");

    PathPolicy policy{workspace_dir};
    CHECK_FALSE(policy.evaluate((extra / "artifact.bin").string(), PathAccess::Create).allowed);

    policy.add_writable_root(extra);
    CHECK(policy.evaluate((extra / "artifact.bin").string(), PathAccess::Create).allowed);
}

TEST_CASE("no workspace means nothing may be written", "[path][security]") {
    PathPolicy policy;
    auto decision = policy.evaluate("C:/anything.txt", PathAccess::Write);
    CHECK_FALSE(decision.allowed);

    // A relative path with no workspace cannot even be resolved.
    auto relative = policy.evaluate("file.txt", PathAccess::Read);
    CHECK_FALSE(relative.allowed);
}

TEST_CASE("syntactically dangerous paths are rejected", "[path][security]") {
    CHECK(PathPolicy::validate_syntax("").empty() == false);
    CHECK_FALSE(PathPolicy::validate_syntax(std::string("a\0b", 3)).empty());

#if defined(_WIN32)
    SECTION("device namespace") {
        CHECK_FALSE(PathPolicy::validate_syntax(R"(\\?\C:\Windows\System32)").empty());
        CHECK_FALSE(PathPolicy::validate_syntax(R"(\\.\PhysicalDrive0)").empty());
    }
    SECTION("UNC paths") {
        CHECK_FALSE(PathPolicy::validate_syntax(R"(\\server\share\file.txt)").empty());
    }
    SECTION("alternate data streams") {
        CHECK_FALSE(PathPolicy::validate_syntax("notes.txt:hidden").empty());
        CHECK_FALSE(PathPolicy::validate_syntax("src/file.cpp:$DATA").empty());
        // A drive letter is the one legal colon.
        CHECK(PathPolicy::validate_syntax("C:/projects/file.cpp").empty());
    }
    SECTION("reserved device names") {
        CHECK_FALSE(PathPolicy::validate_syntax("CON").empty());
        CHECK_FALSE(PathPolicy::validate_syntax("src/NUL.txt").empty());
        CHECK_FALSE(PathPolicy::validate_syntax("COM1").empty());
        CHECK_FALSE(PathPolicy::validate_syntax("src/LPT9.cpp").empty());
        CHECK(PathPolicy::validate_syntax("src/console.cpp").empty());
    }
    SECTION("trailing dots and spaces are stripped by Win32 and so refused") {
        CHECK_FALSE(PathPolicy::validate_syntax("workspace/.. /escape").empty());
        CHECK_FALSE(PathPolicy::validate_syntax("dir /file.txt").empty());
        CHECK_FALSE(PathPolicy::validate_syntax("name./file").empty());
    }
    SECTION("characters Windows cannot represent") {
        CHECK_FALSE(PathPolicy::validate_syntax("file<name>.txt").empty());
        CHECK_FALSE(PathPolicy::validate_syntax("a|b.txt").empty());
    }
#endif
}

TEST_CASE("dangerous syntax is refused through evaluate as well", "[path][security]") {
    TempDirectory workspace{"lc-ws"};
    PathPolicy policy{workspace.path()};

#if defined(_WIN32)
    CHECK_FALSE(policy.evaluate("notes.txt:stream", PathAccess::Write).allowed);
    CHECK_FALSE(policy.evaluate(R"(\\?\C:\Windows)", PathAccess::Read).allowed);
    CHECK_FALSE(policy.evaluate("NUL", PathAccess::Write).allowed);
#endif
    CHECK_FALSE(policy.evaluate(std::string("bad\0path", 8), PathAccess::Read).allowed);
}

TEST_CASE("protected system locations are never writable", "[path][security]") {
    // Deliberately makes the system root the workspace: even then, writes into
    // a protected location must be refused.
    const auto& protected_roots = PathPolicy::protected_roots();
    if (protected_roots.empty()) return;   // nothing to assert on this machine

    const auto& target = protected_roots.front();
    PathPolicy policy{target.parent_path().empty() ? target : target.parent_path()};

    auto decision = policy.evaluate((target / "localcode-should-not-write.txt").string(),
                                    PathAccess::Create);
    CHECK_FALSE(decision.allowed);
}

TEST_CASE("is_within compares whole components", "[path][security]") {
    const std::filesystem::path root = "C:/projects/game";

    CHECK(PathPolicy::is_within(root, "C:/projects/game"));
    CHECK(PathPolicy::is_within(root, "C:/projects/game/src/main.cpp"));

    // The classic prefix bug: "game-backup" must not count as inside "game".
    CHECK_FALSE(PathPolicy::is_within(root, "C:/projects/game-backup/secret"));
    CHECK_FALSE(PathPolicy::is_within(root, "C:/projects/gamex"));
    CHECK_FALSE(PathPolicy::is_within(root, "C:/projects"));
    CHECK_FALSE(PathPolicy::is_within(root, "D:/projects/game/src"));
}

#if defined(_WIN32)
TEST_CASE("a directory junction cannot be used to escape", "[path][security][junction]") {
    // A lexical containment check passes for workspace/link/... while the real
    // target is outside. Canonicalising through the OS is what catches it.
    TempDirectory root{"lc-root"};
    const auto workspace_dir = root.make_directory("workspace");
    const auto secret_dir = root.make_directory("secret");
    {
        std::ofstream out(secret_dir / "keys.txt");
        out << "sensitive";
    }

    const auto link = workspace_dir / "link";
    const std::string command = "cmd.exe /c mklink /J \"" + link.string() + "\" \"" +
                                secret_dir.string() + "\" >nul 2>&1";
    const int rc = std::system(command.c_str());
    if (rc != 0 || !std::filesystem::exists(link)) {
        // Junction creation can be unavailable; skip rather than fail.
        WARN("could not create a junction; skipping the junction escape test");
        return;
    }

    PathPolicy policy{workspace_dir};
    auto decision = policy.evaluate("link/keys.txt", PathAccess::Write);

    INFO("canonical: " << decision.canonical.string());
    CHECK_FALSE(decision.allowed);
    CHECK_FALSE(decision.inside_workspace);
}
#endif

TEST_CASE("unicode and spaces in paths are handled", "[path][unicode]") {
    TempDirectory workspace{"lc-ws"};
    workspace.write("dossier de tests/caf\xC3\xA9/r\xC3\xA9sum\xC3\xA9.txt", "contenu");

    PathPolicy policy{workspace.path()};
    auto decision = policy.evaluate("dossier de tests/caf\xC3\xA9/r\xC3\xA9sum\xC3\xA9.txt",
                                    PathAccess::Read);
    CHECK(decision.allowed);
    CHECK(decision.inside_workspace);
}

TEST_CASE("a very long path is still contained", "[path][security]") {
    TempDirectory workspace{"lc-ws"};
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "subdirectory-with-a-long-name/";
    deep += "file.txt";

    PathPolicy policy{workspace.path()};
    auto decision = policy.evaluate(deep, PathAccess::Create);
    // Whether it can be created is a filesystem question; containment is not.
    CHECK(decision.inside_workspace == decision.allowed);
    if (decision.allowed) CHECK(decision.inside_workspace);
}

TEST_CASE("a container project root is read as this workspace", "[path][security][recovery]") {
    // Three runs died on this. Models name the project root /workspace, /app
    // or /repo - the mount points of the containers their training data came
    // from - and refusing does not shift it. Run 11 is the proof: the model
    // wrote "The workspace is a temp directory, not /workspace. Let me write
    // the file there" and then asked for /workspace/fibonacci.cpp again, twice
    // more, and the run was stopped having written nothing.
    TempDirectory workspace{"lc-ws"};
    workspace.write("existing.txt", "here\n");
    PathPolicy policy{workspace.path()};

    auto write = policy.evaluate("/workspace/fibonacci.cpp", PathAccess::Create);
    REQUIRE(write.allowed);
    CHECK(write.inside_workspace);
    CHECK(write.relative.generic_string() == "fibonacci.cpp");
    // And it is recorded, so the tool can say the file did not land where it
    // was asked for.
    CHECK(write.reinterpreted_from == "/workspace/fibonacci.cpp");

    // The root named alone is the workspace root.
    auto listing = policy.evaluate("/workspace", PathAccess::Read);
    REQUIRE(listing.allowed);
    CHECK(listing.canonical == policy.workspace_root());

    // Backslashes and case are the same convention wearing a different hat.
    CHECK(policy.evaluate(R"(\workspace\fibonacci.cpp)", PathAccess::Create).allowed);
    CHECK(policy.evaluate("/Workspace/fibonacci.cpp", PathAccess::Create).allowed);

    // Nested paths keep their shape.
    auto nested = policy.evaluate("/workspace/src/main.cpp", PathAccess::Create);
    REQUIRE(nested.allowed);
    CHECK(nested.relative.generic_string() == "src/main.cpp");
}

TEST_CASE("the placeholder rule does not widen the boundary", "[path][security]") {
    TempDirectory workspace{"lc-ws"};
    PathPolicy policy{workspace.path()};

    // A prefix that merely starts with the same letters is not the virtual
    // root: /workspaces is a different directory, and so is /workspace-old.
    CHECK_FALSE(policy.evaluate("/workspacex/file.cpp", PathAccess::Create).allowed);
    CHECK_FALSE(policy.evaluate("/workspaces/file.cpp", PathAccess::Create).allowed);
    CHECK_FALSE(policy.evaluate("/workspace-old/file.cpp", PathAccess::Create).allowed);

    // Only /workspace is virtual. The other container conventions are ordinary
    // directory names that a user could legitimately mean, so they are refused
    // rather than silently redirected.
    CHECK_FALSE(policy.evaluate("/app/file.cpp", PathAccess::Create).allowed);
    CHECK_FALSE(policy.evaluate("/repo/file.cpp", PathAccess::Create).allowed);
    CHECK_FALSE(policy.evaluate("/project/file.cpp", PathAccess::Create).allowed);
    CHECK_FALSE(policy.evaluate("/code/file.cpp", PathAccess::Create).allowed);
    CHECK_FALSE(policy.evaluate("/home/user/project/file.cpp", PathAccess::Create).allowed);

    // Nor are ordinary absolute paths anywhere else.
    CHECK_FALSE(policy.evaluate("/etc/passwd", PathAccess::Write).allowed);
    CHECK_FALSE(policy.evaluate("/usr/lib/thing.so", PathAccess::Write).allowed);
    // A drive-letter spelling is relative on POSIX. Use a native absolute
    // sibling so this assertion checks the same boundary on every host.
    const auto outside = workspace.path().parent_path() / "outside" / "hosts";
    REQUIRE(outside.is_absolute());
    CHECK_FALSE(policy.evaluate(path_to_utf8(outside), PathAccess::Write).allowed);

    // Traversal out of a reinterpreted path is still traversal.
    auto escape = policy.evaluate("/workspace/../../secret.txt", PathAccess::Write);
    CHECK_FALSE(escape.allowed);

    // Nothing that resolves normally is marked as reinterpreted.
    CHECK(policy.evaluate("src/main.cpp", PathAccess::Create).reinterpreted_from.empty());
}
