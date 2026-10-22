// Parsers for git's porcelain formats. No repository required.
#include <catch2/catch_test_macros.hpp>

#include "git/GitService.h"
#include "platform/PathEncoding.h"
#include "platform/Process.h"
#include "support/TempDirectory.h"

using namespace localcode;

TEST_CASE("Git repository roots preserve UTF-8 paths on Windows", "[git][unicode][audit]") {
    test::TempDirectory temporary;
    const auto root = temporary.path() / path_from_utf8("répo 路径 🦍");
    std::filesystem::create_directories(root);
    const auto executable = find_executable("git");
    if (!executable) SKIP("Git is required for the real Unicode repository check");
    ProcessRequest initialise;
    initialise.executable = *executable;
    initialise.arguments = {"init", "-q"};
    initialise.working_directory = root;
    const auto initialized = ProcessRunner::run(initialise);
    REQUIRE(initialized);
    REQUIRE(initialized->success());
    GitService git;
    const auto detected = git.repository_root(root);
    REQUIRE(detected);
    CHECK(path_to_utf8(*detected) == path_to_utf8(root));
    const auto status = git.status(root);
    REQUIRE(status);
    CHECK(status->is_repository);
}

TEST_CASE("porcelain v2 status is parsed", "[git]") {
    const std::string output =
        "# branch.oid 1234567890abcdef1234567890abcdef12345678\n"
        "# branch.head main\n"
        "# branch.upstream origin/main\n"
        "# branch.ab +2 -1\n"
        "1 .M N... 100644 100644 100644 aaaa bbbb src/modified.cpp\n"
        "1 M. N... 100644 100644 100644 cccc dddd src/staged.cpp\n"
        "1 MM N... 100644 100644 100644 eeee ffff src/both.cpp\n"
        "? untracked.txt\n"
        "? build/output.log\n";

    const auto status = GitService::parse_status_porcelain_v2(output);

    CHECK(status.is_repository);
    CHECK(status.branch == "main");
    CHECK(status.upstream == "origin/main");
    CHECK(status.ahead == 2);
    CHECK(status.behind == 1);
    CHECK(status.head_commit.rfind("1234567890", 0) == 0);
    CHECK_FALSE(status.clean);
    REQUIRE(status.files.size() == 5);

    const auto* modified = status.file("src/modified.cpp");
    REQUIRE(modified != nullptr);
    CHECK(modified->worktree_change == GitChange::Modified);
    CHECK_FALSE(modified->staged());

    const auto* staged = status.file("src/staged.cpp");
    REQUIRE(staged != nullptr);
    CHECK(staged->staged());

    const auto* untracked = status.file("untracked.txt");
    REQUIRE(untracked != nullptr);
    CHECK(untracked->untracked);
}

TEST_CASE("a clean tree is reported as clean", "[git]") {
    const auto status =
        GitService::parse_status_porcelain_v2("# branch.oid abc123\n# branch.head main\n");
    CHECK(status.clean);
    CHECK(status.files.empty());
}

TEST_CASE("a detached HEAD is recognised", "[git]") {
    const auto status = GitService::parse_status_porcelain_v2("# branch.head (detached)\n");
    CHECK(status.detached_head);
}

TEST_CASE("renames carry the original path", "[git]") {
    const std::string output =
        "# branch.head main\n"
        "2 R. N... 100644 100644 100644 aaaa bbbb R100 new/path.cpp\told/path.cpp\n";

    const auto status = GitService::parse_status_porcelain_v2(output);
    REQUIRE(status.files.size() == 1);
    CHECK(status.files[0].path == "new/path.cpp");
    CHECK(status.files[0].original_path == "old/path.cpp");
}

TEST_CASE("conflicts are flagged", "[git]") {
    const std::string output =
        "# branch.head main\n"
        "u UU N... 100644 100644 100644 100644 aaa bbb ccc conflicted.cpp\n";

    const auto status = GitService::parse_status_porcelain_v2(output);
    REQUIRE(status.files.size() == 1);
    CHECK(status.files[0].conflicted);
}

TEST_CASE("a unified diff is parsed into files and hunks", "[git][diff]") {
    const std::string diff =
        "diff --git a/src/main.cpp b/src/main.cpp\n"
        "index 1234567..89abcde 100644\n"
        "--- a/src/main.cpp\n"
        "+++ b/src/main.cpp\n"
        "@@ -10,7 +10,7 @@ int main() {\n"
        "     int total = 0;\n"
        "-    for (int i = 0; i <= count; ++i) {\n"
        "+    for (int i = 0; i < count; ++i) {\n"
        "         total += values[i];\n"
        "     }\n"
        "diff --git a/README.md b/README.md\n"
        "--- a/README.md\n"
        "+++ b/README.md\n"
        "@@ -1,2 +1,3 @@\n"
        " # Title\n"
        "+A new line.\n"
        " Body\n";

    const auto result = GitService::parse_unified_diff(diff);

    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0].path == "src/main.cpp");
    CHECK(result.files[0].additions == 1);
    CHECK(result.files[0].deletions == 1);
    REQUIRE(result.files[0].hunks.size() == 1);
    CHECK(result.files[0].hunks[0].old_start == 10);
    CHECK(result.files[0].hunks[0].new_start == 10);

    CHECK(result.files[1].path == "README.md");
    CHECK(result.files[1].additions == 1);
    CHECK(result.total_additions == 2);
    CHECK(result.total_deletions == 1);
}

TEST_CASE("added and deleted files are classified", "[git][diff]") {
    const std::string diff =
        "diff --git a/new.cpp b/new.cpp\n"
        "new file mode 100644\n"
        "--- /dev/null\n"
        "+++ b/new.cpp\n"
        "@@ -0,0 +1,2 @@\n"
        "+int main() {}\n"
        "+\n"
        "diff --git a/gone.cpp b/gone.cpp\n"
        "deleted file mode 100644\n"
        "--- a/gone.cpp\n"
        "+++ /dev/null\n"
        "@@ -1,1 +0,0 @@\n"
        "-int old() {}\n";

    const auto result = GitService::parse_unified_diff(diff);
    REQUIRE(result.files.size() == 2);
    CHECK(result.files[0].change == GitChange::Added);
    CHECK(result.files[1].change == GitChange::Deleted);
}

TEST_CASE("a binary diff is flagged rather than parsed", "[git][diff]") {
    const std::string diff =
        "diff --git a/image.png b/image.png\n"
        "index 111..222 100644\n"
        "Binary files a/image.png and b/image.png differ\n";

    const auto result = GitService::parse_unified_diff(diff);
    REQUIRE(result.files.size() == 1);
    CHECK(result.files[0].binary);
}

TEST_CASE("an empty diff yields no files", "[git][diff]") {
    CHECK(GitService::parse_unified_diff("").files.empty());
}

TEST_CASE("the log format is parsed", "[git]") {
    const std::string output =
        "abc123def456\x1f"
        "abc123d\x1fJane Developer\x1fjane@example.com\x1f"
        "1700000000\x1f"
        "Fix the off-by-one\x1f"
        "A longer body.\x1fparent1 parent2\x1e"
        "111222333\x1f"
        "1112223\x1f"
        "Bob\x1f"
        "bob@example.com\x1f"
        "1699000000\x1f"
        "Add the feature\x1f\x1fparent0\x1e";

    const auto commits = GitService::parse_log(output);

    REQUIRE(commits.size() == 2);
    CHECK(commits[0].hash == "abc123def456");
    CHECK(commits[0].short_hash == "abc123d");
    CHECK(commits[0].author_name == "Jane Developer");
    CHECK(commits[0].subject == "Fix the off-by-one");
    CHECK(commits[0].body == "A longer body.");
    REQUIRE(commits[0].parents.size() == 2);
    CHECK(commits[1].subject == "Add the feature");
}

TEST_CASE("a subject containing separators does not corrupt the parse", "[git]") {
    // Record and field separators are control characters precisely so that
    // ordinary punctuation in a commit message cannot break this.
    const std::string output =
        "aaa\x1f"
        "aaa\x1f"
        "Author\x1f"
        "a@b.c\x1f"
        "1700000000\x1f"
        "Fix: handle a, b | c and \"quotes\"\x1f\x1f\x1e";

    const auto commits = GitService::parse_log(output);
    REQUIRE(commits.size() == 1);
    CHECK(commits[0].subject == "Fix: handle a, b | c and \"quotes\"");
}

TEST_CASE("git status describes each file readably", "[git]") {
    GitFileStatus file;
    file.untracked = true;
    CHECK(file.describe() == "untracked");

    GitFileStatus modified;
    modified.worktree_change = GitChange::Modified;
    CHECK(modified.describe().find("modified") != std::string::npos);

    GitFileStatus conflicted;
    conflicted.conflicted = true;
    CHECK(conflicted.describe() == "conflicted");
}

TEST_CASE("standard unified headers parse multiple files and tab-separated timestamps",
          "[git][diff][bare-unified]") {
    const std::string patch =
        "--- a/dir/a file.txt\told timestamp\r\n"
        "+++ b/dir/a file.txt\tnew timestamp\r\n"
        "@@ -1 +1 @@ context\r\n-old\r\n+new\r\n"
        "--- /dev/null\n+++ b/created.txt\n@@ -0,0 +1,2 @@\n+one\n+two\n";
    const auto parsed = GitService::parse_unified_diff(patch);
    REQUIRE(parsed.files.size() == 2);
    CHECK(parsed.raw == patch);
    CHECK(parsed.files[0].old_path == "dir/a file.txt");
    CHECK(parsed.files[0].path == "dir/a file.txt");
    CHECK(parsed.files[0].change == GitChange::Modified);
    REQUIRE(parsed.files[0].hunks.size() == 1);
    CHECK(parsed.files[0].hunks[0].old_count == 1);
    CHECK(parsed.files[0].hunks[0].new_count == 1);
    CHECK(parsed.files[1].change == GitChange::Added);
    CHECK(parsed.files[1].path == "created.txt");
    CHECK(parsed.total_additions == 3);
    CHECK(parsed.total_deletions == 1);
}

TEST_CASE("counted hunk data keeps header-looking source inside its actual file",
          "[git][diff][bare-unified]") {
    const std::string body = "--- old data\n+++ new data\n diff --git a/phantom b/phantom\n";
    for (const auto& prefix : {std::string{}, std::string{"diff --git a/data.txt b/data.txt\n"}}) {
        const auto parsed = GitService::parse_unified_diff(
            prefix + "--- a/data.txt\n+++ b/data.txt\n@@ -1,2 +1,2 @@\n" + body);
        REQUIRE(parsed.files.size() == 1);
        CHECK(parsed.files[0].path == "data.txt");
        REQUIRE(parsed.files[0].hunks.size() == 1);
        CHECK(parsed.files[0].hunks[0].lines ==
              std::vector<std::string>{"--- old data", "+++ new data",
                                       " diff --git a/phantom b/phantom"});
        CHECK(parsed.total_additions == 1);
        CHECK(parsed.total_deletions == 1);
    }
}
