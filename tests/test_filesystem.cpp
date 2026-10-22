// Filesystem helpers: encoding detection, atomic writes, ignore rules, walking.
#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "platform/FileSystem.h"
#include "platform/PathEncoding.h"
#include "util/Strings.h"
#include "support/TempDirectory.h"

using namespace localcode;
using localcode::test::TempDirectory;

TEST_CASE("non-ANSI filenames support atomic writes and directory listings", "[fs][non-ansi]") {
    TempDirectory temp{"lc-unicode-fs"};
    const auto directory = temp.path() / path_from_utf8("répertoire 路径 🦍");
    REQUIRE(std::filesystem::create_directory(directory));
    const auto file = directory / path_from_utf8("réponse 路径 🦍.txt");
    REQUIRE(fs::write_text_atomic(file, "first\n"));
    REQUIRE(fs::write_text_atomic(file, "second\n"));
    const auto read = fs::read_text(file);
    REQUIRE(read);
    CHECK(read->content == "second\n");
    const auto listed = fs::list_directory(directory);
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().name == "réponse 路径 🦍.txt");
}

TEST_CASE("plain UTF-8 text round-trips", "[fs]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write("hello.txt", "line one\nline two\n");

    auto read = fs::read_text(file);
    REQUIRE(read.ok());
    CHECK(read->content == "line one\nline two\n");
    CHECK(read->encoding == fs::TextEncoding::Utf8);
    CHECK(read->line_ending == fs::LineEnding::Lf);
    CHECK(read->line_count == 2);
    CHECK_FALSE(read->truncated);
}

TEST_CASE("CRLF is normalised on read and restored on write", "[fs]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write("crlf.txt", "one\r\ntwo\r\n");

    auto read = fs::read_text(file);
    REQUIRE(read.ok());
    CHECK(read->content == "one\ntwo\n");
    CHECK(read->line_ending == fs::LineEnding::CrLf);

    // Writing back with the detected ending must reproduce the original bytes,
    // so an edit does not show up as "every line changed" in the diff.
    REQUIRE(fs::write_text_atomic(file, read->content, read->encoding, read->line_ending).ok());

    std::ifstream in(file, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(raw == "one\r\ntwo\r\n");
}

TEST_CASE("a UTF-8 BOM is detected and preserved", "[fs][unicode]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write("bom.txt", "\xEF\xBB\xBF" "content\n");

    auto read = fs::read_text(file);
    REQUIRE(read.ok());
    CHECK(read->encoding == fs::TextEncoding::Utf8Bom);
    CHECK(read->content == "content\n");

    REQUIRE(fs::write_text_atomic(file, "changed\n", read->encoding, read->line_ending).ok());
    std::ifstream in(file, std::ios::binary);
    const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(raw.rfind("\xEF\xBB\xBF", 0) == 0);
}

TEST_CASE("UTF-16 source files are decoded", "[fs][unicode]") {
    TempDirectory temp{"lc-fs"};
    // "hi\n" in UTF-16LE with a BOM.
    const std::string utf16{"\xFF\xFE" "h\0i\0\n\0", 8};
    const auto file = temp.write("utf16.txt", utf16);

    auto read = fs::read_text(file);
    REQUIRE(read.ok());
    CHECK(read->encoding == fs::TextEncoding::Utf16Le);
    CHECK(read->content == "hi\n");
}

TEST_CASE("binary files are refused with a clear reason", "[fs]") {
    TempDirectory temp{"lc-fs"};
    std::string binary("\x7F\x45\x4C\x46", 4);
    binary.append(200, '\0');
    const auto file = temp.write("program.bin", binary);

    auto read = fs::read_text(file);
    REQUIRE_FALSE(read.ok());
    CHECK(read.error().code == ErrorCode::NotSupported);
}

TEST_CASE("invalid UTF-8 is repaired rather than rejected", "[fs][unicode]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write("mixed.txt", "good \xC3\x28" " text\n");

    auto read = fs::read_text(file);
    REQUIRE(read.ok());
    // The content must always be safe to put into JSON.
    CHECK(strings::is_valid_utf8(read->content));
}

TEST_CASE("a missing file is NotFound", "[fs]") {
    TempDirectory temp{"lc-fs"};
    auto read = fs::read_text(temp.path() / "nope.txt");
    REQUIRE_FALSE(read.ok());
    CHECK(read.error().code == ErrorCode::NotFound);
}

TEST_CASE("range reads return the requested lines", "[fs]") {
    TempDirectory temp{"lc-fs"};
    std::string content;
    for (int i = 1; i <= 100; ++i) content += "line" + std::to_string(i) + "\n";
    const auto file = temp.write("many.txt", content);

    auto range = fs::read_text_range(file, 10, 12);
    REQUIRE(range.ok());
    CHECK(range->line_count == 3);
    CHECK(range->content.find("line10") != std::string::npos);
    CHECK(range->content.find("line12") != std::string::npos);
    CHECK(range->content.find("line13") == std::string::npos);

    auto to_end = fs::read_text_range(file, 98, 0);
    REQUIRE(to_end.ok());
    CHECK(to_end->line_count == 3);
}

TEST_CASE("atomic write replaces the file completely", "[fs]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write("target.txt", "original content that is quite long\n");

    REQUIRE(fs::write_text_atomic(file, "short\n").ok());
    auto read = fs::read_text(file);
    REQUIRE(read.ok());
    CHECK(read->content == "short\n");

    // No temporary files may be left behind.
    int stray = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path())) {
        if (entry.path().filename().string().find(".lc-") != std::string::npos) ++stray;
    }
    CHECK(stray == 0);
}

TEST_CASE("writing creates missing parent directories", "[fs]") {
    TempDirectory temp{"lc-fs"};
    const auto nested = temp.path() / "a" / "b" / "c" / "file.txt";
    REQUIRE(fs::write_text_atomic(nested, "content\n").ok());
    CHECK(std::filesystem::exists(nested));
}

TEST_CASE("stat_file reports what a tool needs to decide", "[fs]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write("info.txt", "a\nb\nc\n");

    const auto info = fs::stat_file(file);
    CHECK(info.exists);
    CHECK(info.is_regular_file);
    CHECK_FALSE(info.is_directory);
    CHECK_FALSE(info.is_binary);
    CHECK(info.size == 6);
    CHECK(info.line_count == 3);

    const auto missing = fs::stat_file(temp.path() / "absent.txt");
    CHECK_FALSE(missing.exists);
}

TEST_CASE("ignore rules skip heavy directories by default", "[fs]") {
    fs::IgnoreRules rules;
    CHECK(rules.ignores_directory(".git"));
    CHECK(rules.ignores_directory("node_modules"));
    CHECK(rules.ignores_directory("build"));
    CHECK(rules.ignores_directory("BUILD"));      // case-insensitive
    CHECK_FALSE(rules.ignores_directory("src"));
    CHECK_FALSE(rules.ignores_directory("include"));
}

TEST_CASE("gitignore entries are honoured", "[fs]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write(".gitignore",
                                 "# a comment\n"
                                 "*.log\n"
                                 "temp/\n"
                                 "!keep.log\n");
    fs::IgnoreRules rules;
    rules.load_gitignore(file);

    CHECK(rules.ignores_directory("temp"));
    CHECK(rules.ignores_file("debug.log"));
    CHECK(rules.ignores_file("nested/deep/other.log"));
    CHECK_FALSE(rules.ignores_file("keep.log"));
    CHECK_FALSE(rules.ignores_file("main.cpp"));
}

TEST_CASE("walking a tree applies the ignore rules", "[fs]") {
    TempDirectory temp{"lc-fs"};
    temp.write("src/main.cpp", "x");
    temp.write("src/util.cpp", "x");
    temp.write("include/api.h", "x");
    temp.write("build/generated.cpp", "x");
    temp.write(".git/config", "x");

    fs::IgnoreRules rules;
    fs::WalkOptions options;
    options.ignore = &rules;

    std::vector<std::string> found;
    auto status = fs::walk(temp.path(), options,
                           [&](const std::filesystem::path& relative, const fs::DirEntry&) {
                               found.push_back(relative.generic_string());
                               return true;
                           });
    REQUIRE(status.ok());

    CHECK(found.size() == 3);
    for (const auto& path : found) {
        CHECK(path.find("build/") == std::string::npos);
        CHECK(path.find(".git/") == std::string::npos);
    }
}

TEST_CASE("the walk stops when the callback says so", "[fs]") {
    TempDirectory temp{"lc-fs"};
    for (int i = 0; i < 50; ++i) temp.write("f" + std::to_string(i) + ".txt", "x");

    fs::WalkOptions options;
    int seen = 0;
    fs::walk(temp.path(), options, [&](const std::filesystem::path&, const fs::DirEntry&) {
        return ++seen < 10;
    });
    CHECK(seen == 10);
}

TEST_CASE("unicode filenames are handled", "[fs][unicode]") {
    TempDirectory temp{"lc-fs"};
    const auto file = temp.write("caf\xC3\xA9-r\xC3\xA9sum\xC3\xA9.txt", "contenu\n");

    auto read = fs::read_text(file);
    REQUIRE(read.ok());
    CHECK(read->content == "contenu\n");

    auto listing = fs::list_directory(temp.path());
    REQUIRE(listing.ok());
    CHECK(listing->size() == 1);
}
