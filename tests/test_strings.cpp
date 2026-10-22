// String, UTF-8 and excerpt helpers - used by every tool result.
#include <catch2/catch_test_macros.hpp>

#include "util/Json.h"

#include <fstream>
#include "support/TempDirectory.h"
#include "util/Log.h"
#include "util/Strings.h"

using namespace localcode::strings;

TEST_CASE("trace redaction reaches secrets inside serialized message content", "[strings][audit]") {
    const localcode::Json trace{
        {"content",
         "API_KEY=synthetic-env-secret\nAuthorization: Bearer synthetic-header-secret\n"},
        {"arguments",
         localcode::Json{{"password", "synthetic-nested-secret"}, {"path", "src/main.cpp"}}.dump()},
        {"metrics", {{"prompt_tokens", 123}}}};
    const auto safe = redact_secrets(trace.dump());
    INFO(safe);
    CHECK(safe.find("synthetic-env-secret") == std::string::npos);
    CHECK(safe.find("synthetic-header-secret") == std::string::npos);
    CHECK(safe.find("synthetic-nested-secret") == std::string::npos);
    CHECK(safe.find("src/main.cpp") != std::string::npos);
    const auto restored = localcode::Json::parse(safe);
    CHECK(restored.at("metrics").at("prompt_tokens") == 123);
}

TEST_CASE("logger reconfiguration preserves output in the new directory", "[log]") {
    localcode::test::TempDirectory first;
    localcode::test::TempDirectory second;
    auto& logger = localcode::Logger::instance();
    const auto previous = logger.log_directory();
    const auto level = logger.min_level();

    struct RestoreLogger {
        std::filesystem::path directory;
        localcode::LogLevel level;

        ~RestoreLogger() { localcode::Logger::instance().configure(directory, level); }
    } restore{previous, level};

    logger.configure(first.path(), localcode::LogLevel::Info);
    localcode::log_info(localcode::LogChannel::App, "FIRST_DIRECTORY_MARKER");
    logger.configure(second.path(), localcode::LogLevel::Info);
    localcode::log_info(localcode::LogChannel::App, "SECOND_DIRECTORY_MARKER");
    std::ifstream first_file(first.path() / "app.log");
    std::string first_content{std::istreambuf_iterator<char>{first_file}, {}};
    std::ifstream second_file(second.path() / "app.log");
    std::string second_content{std::istreambuf_iterator<char>{second_file}, {}};
    CHECK(first_content.find("FIRST_DIRECTORY_MARKER") != std::string::npos);
    CHECK(first_content.find("SECOND_DIRECTORY_MARKER") == std::string::npos);
    CHECK(second_content.find("SECOND_DIRECTORY_MARKER") != std::string::npos);
}

TEST_CASE("trim, case and split", "[strings]") {
    CHECK(trim("  hello \t\n") == "hello");
    CHECK(trim("   ").empty());
    CHECK(to_lower("MiXeD") == "mixed");
    CHECK(to_upper("MiXeD") == "MIXED");
    CHECK(iequals("Hello", "hELLO"));
    CHECK_FALSE(iequals("Hello", "Hell"));
    CHECK(icontains("The Quick Brown", "quick"));
    CHECK_FALSE(icontains("abc", "abcd"));

    auto parts = split("a,b,,c", ',', true);
    REQUIRE(parts.size() == 4);
    CHECK(parts[2].empty());

    auto compact = split("a,b,,c", ',', false);
    CHECK(compact.size() == 3);
}

TEST_CASE("line splitting handles CRLF and a missing final newline", "[strings]") {
    auto lines = split_lines("one\r\ntwo\nthree");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "one");
    CHECK(lines[2] == "three");

    CHECK(split_lines("").empty());
    CHECK(split_lines("\n").size() == 1);
}

TEST_CASE("glob matching", "[strings]") {
    CHECK(glob_match("*.cpp", "main.cpp"));
    CHECK(glob_match("*.CPP", "main.cpp"));
    CHECK(glob_match("src/*.cpp", "src/main.cpp"));
    CHECK(glob_match("*", "anything"));
    CHECK(glob_match("a?c", "abc"));
    CHECK_FALSE(glob_match("*.cpp", "main.h"));
    CHECK_FALSE(glob_match("a?c", "ac"));

    // Pathological pattern: must terminate quickly, not blow up.
    CHECK_FALSE(glob_match("*a*a*a*a*a*a*b", std::string(64, 'a')));
}

TEST_CASE("UTF-8 validation rejects malformed sequences", "[strings][unicode]") {
    CHECK(is_valid_utf8("plain ascii"));
    CHECK(is_valid_utf8("caf\xC3\xA9"));
    CHECK(is_valid_utf8("\xE2\x82\xAC"));            // euro sign
    CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));        // emoji

    CHECK_FALSE(is_valid_utf8("\xC3"));              // truncated
    CHECK_FALSE(is_valid_utf8("\x80\x80"));          // stray continuation
    CHECK_FALSE(is_valid_utf8("\xC0\xAF"));          // overlong encoding of '/'
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));      // UTF-16 surrogate
    CHECK_FALSE(is_valid_utf8("\xF5\x80\x80\x80"));  // beyond U+10FFFF
}

TEST_CASE("sanitising replaces bad bytes and keeps good ones", "[strings][unicode]") {
    // Split so that "bad" is not absorbed into the hex escape.
    const auto cleaned = sanitize_utf8("ok\xFF\xFE" "bad");
    CHECK(is_valid_utf8(cleaned));
    CHECK(cleaned.find("ok") == 0);

    const std::string valid = "caf\xC3\xA9";
    CHECK(sanitize_utf8(valid) == valid);
}

TEST_CASE("control characters are made visible rather than printed", "[strings][unicode]") {
    // The failure this exists for: "build\\RelWithDebInfo\\fib.exe" in a JSON
    // string becomes build + RelWithDebInfo + form feed + ib.exe, and in a log
    // the form feed is invisible - the path simply looks wrong for no reason.
    CHECK(visible("build\fib.exe") == "build\\fib.exe");
    CHECK(visible("a\tb\nc") == "a\\tb\\nc");
    CHECK(visible(std::string("x\x01y")) == "x\\x01y");

    // Ordinary text, including backslashes that really are backslashes, is
    // left exactly as it is.
    CHECK(visible(R"(C:\Program Files\cmake.exe)") == R"(C:\Program Files\cmake.exe)");
    CHECK(visible("caf\xC3\xA9") == "caf\xC3\xA9");
}

TEST_CASE("every escape a JSON string can inject is made visible",
          "[strings][unicode][encoding]") {
    // The complete set that a single backslash in a JSON path turns into.
    // Each one is a real filename character that is not: a path carrying any
    // of them was damaged in transit, and a log that renders them invisibly
    // makes that undiagnosable.
    CHECK(visible("a\bb") == "a\\bb");     // \b  backspace
    CHECK(visible("a\fb") == "a\\fb");     // \f  form feed
    CHECK(visible("a\nb") == "a\\nb");     // \n  newline
    CHECK(visible("a\rb") == "a\\rb");     // \r  carriage return
    CHECK(visible("a\tb") == "a\\tb");     // \t  tab
    CHECK(visible("a\vb") == "a\\vb");     // \v  vertical tab
    CHECK(visible(std::string("a\0b", 3)) == "a\\x00b");   // \u0000
    CHECK(visible("a\x1B[0m") == "a\\e[0m");               // escape

    // Everything below 0x20 has a rendering, named or hex.
    for (int c = 0; c < 0x20; ++c) {
        const std::string one(1, static_cast<char>(c));
        INFO("control character " << c);
        const auto rendered = visible(one);
        CHECK(rendered.size() >= 2);
        CHECK(rendered.front() == '\\');
    }
    CHECK(visible(std::string(1, '\x7F')) == "\\x7f");

    // Characters that are legal in a filename pass through untouched, so the
    // rendering is not a source of false alarms.
    CHECK(visible("build/Debug/my app.exe") == "build/Debug/my app.exe");
    CHECK(visible(R"(build\Debug\app.exe)") == R"(build\Debug\app.exe)");
    // Unicode separators are real characters and are left alone.
    CHECK(visible("a\xE2\x80\xA8" "b") == "a\xE2\x80\xA8" "b");   // U+2028
}

TEST_CASE("secret-looking values are redacted from traces", "[strings][security]") {
    // Traces are written to disk and pasted into bug reports. A command that
    // legitimately carries a token in its environment should not put that
    // token in both places.
    const std::string trace =
        R"({"executable":"curl","environment":{"API_KEY":"sk-live-abc123","GITHUB_TOKEN":"ghp_zzz","PATH":"C:/bin"}})";
    const auto safe = redact_secrets(trace);
    INFO(safe);

    CHECK(safe.find("sk-live-abc123") == std::string::npos);
    CHECK(safe.find("ghp_zzz") == std::string::npos);
    CHECK(safe.find("<redacted>") != std::string::npos);

    // What is not a secret is left readable, or the trace stops being useful.
    CHECK(safe.find(R"("PATH":"C:/bin")") != std::string::npos);
    CHECK(safe.find(R"("executable":"curl")") != std::string::npos);
}

TEST_CASE("truncation never splits a multi-byte sequence", "[strings][unicode]") {
    const std::string text = "caf\xC3\xA9 latte";
    for (std::size_t limit = 0; limit <= text.size(); ++limit) {
        const auto cut = utf8_truncate(text, limit);
        INFO("limit " << limit);
        CHECK(is_valid_utf8(cut));
        CHECK(cut.size() <= limit);
    }
}

TEST_CASE("utf8_length counts code points", "[strings][unicode]") {
    CHECK(utf8_length("abc") == 3);
    CHECK(utf8_length("caf\xC3\xA9") == 4);
    CHECK(utf8_length("\xF0\x9F\x98\x80") == 1);
}

TEST_CASE("excerpt keeps the head and tail and says what was dropped", "[strings]") {
    std::string big;
    for (int i = 0; i < 500; ++i) big += "line " + std::to_string(i) + "\n";

    auto result = excerpt(big, 5, 5, 100000);
    CHECK(result.truncated);
    CHECK(result.original_lines >= 500);
    CHECK(result.text.find("line 0") != std::string::npos);
    CHECK(result.text.find("line 499") != std::string::npos);
    CHECK(result.text.find("lines omitted") != std::string::npos);
    CHECK(result.text.find("line 250") == std::string::npos);

    // Small input passes through untouched.
    auto small = excerpt("one\ntwo\n", 10, 10, 1000);
    CHECK_FALSE(small.truncated);
}

TEST_CASE("excerpt respects a byte cap even with few lines", "[strings]") {
    const std::string one_long_line(50000, 'x');
    auto result = excerpt(one_long_line, 5, 5, 1000);
    CHECK(result.truncated);
    CHECK(result.text.size() < 2000);
}

TEST_CASE("human formatting", "[strings]") {
    CHECK(human_bytes(0) == "0 B");
    CHECK(human_bytes(512) == "512 B");
    CHECK(human_bytes(1024).find("KB") != std::string::npos);
    CHECK(human_bytes(5ull * 1024 * 1024 * 1024).find("GB") != std::string::npos);

    CHECK(human_duration_ms(250) == "250ms");
    CHECK(human_duration_ms(1500).find("s") != std::string::npos);
    CHECK(human_duration_ms(90000).find("m") != std::string::npos);
}

TEST_CASE("runs of blank lines are collapsed", "[strings]") {
    // A degenerating model emits newlines. One reply in Run 10 carried several
    // hundred blank lines between two sentences, and the honest note about
    // what had not been verified ended up several screens below the answer.
    std::string many = "first\n";
    for (int i = 0; i < 400; ++i) many += "\n";
    many += "last";

    const auto collapsed = collapse_blank_lines(many);
    CHECK(collapsed == "first\n\n\nlast");

    // Two blank lines is a paragraph break, not noise.
    CHECK(collapse_blank_lines("a\n\nb") == "a\n\nb");
    CHECK(collapse_blank_lines("a\nb") == "a\nb");
    CHECK(collapse_blank_lines("").empty());
}

TEST_CASE("blank lines inside a code fence are the author's", "[strings]") {
    const std::string source =
        "before\n"
        "```cpp\n"
        "int a;\n"
        "\n"
        "\n"
        "\n"
        "int b;\n"
        "```\n"
        "after";
    CHECK(collapse_blank_lines(source) == source);
}
