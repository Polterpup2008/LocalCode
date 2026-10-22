// Web helpers: trust policy, HTML extraction, SSE parsing, result parsing.
#include <catch2/catch_test_macros.hpp>

#include "inference/HttpClient.h"
#include "web/WebService.h"

using namespace localcode;

TEST_CASE("host extraction handles the shapes URLs come in", "[web]") {
    CHECK(TrustPolicy::host_of("https://github.com/owner/repo") == "github.com");
    CHECK(TrustPolicy::host_of("http://example.com:8080/path") == "example.com");
    CHECK(TrustPolicy::host_of("https://user:pass@example.com/x") == "example.com");
    CHECK(TrustPolicy::host_of("https://EXAMPLE.COM/") == "example.com");
    CHECK(TrustPolicy::host_of("https://sub.domain.example.com") == "sub.domain.example.com");
    CHECK(TrustPolicy::host_of("not a url").empty() == false);
}

TEST_CASE("the trusted list covers official sources and nothing else", "[web][security]") {
    TrustPolicy trust;

    CHECK(trust.is_trusted("https://github.com/x/y/releases/download/v1/tool.zip"));
    CHECK(trust.is_trusted("https://files.pythonhosted.org/packages/x.whl"));
    CHECK(trust.is_trusted("https://static.crates.io/crates/serde/serde-1.0.crate"));
    // Subdomains of a trusted host are trusted.
    CHECK(trust.is_trusted("https://objects.githubusercontent.com/x"));

    CHECK_FALSE(trust.is_trusted("https://random-file-host.example/tool.exe"));
    // The classic near-miss: a lookalike domain must not pass.
    CHECK_FALSE(trust.is_trusted("https://notgithub.com/x"));
    CHECK_FALSE(trust.is_trusted("https://github.com.evil.example/x"));
}

TEST_CASE("the trusted list is configurable", "[web][security]") {
    TrustPolicy trust;
    CHECK_FALSE(trust.is_trusted("https://internal.corp.example/artifact.zip"));

    trust.add_trusted_host("internal.corp.example");
    CHECK(trust.is_trusted("https://internal.corp.example/artifact.zip"));

    trust.remove_trusted_host("internal.corp.example");
    CHECK_FALSE(trust.is_trusted("https://internal.corp.example/artifact.zip"));
}

TEST_CASE("executable payloads are recognised by extension", "[web][security]") {
    CHECK(TrustPolicy::is_executable_payload("https://x/setup.exe"));
    CHECK(TrustPolicy::is_executable_payload("https://x/installer.MSI"));
    CHECK(TrustPolicy::is_executable_payload("https://x/script.ps1"));
    CHECK(TrustPolicy::is_executable_payload("https://x/archive.zip"));
    CHECK(TrustPolicy::is_executable_payload("https://x/source.tar.gz"));
    CHECK(TrustPolicy::is_executable_payload("https://x/tool.exe?version=2"));

    CHECK_FALSE(TrustPolicy::is_executable_payload("https://x/docs/page.html"));
    CHECK_FALSE(TrustPolicy::is_executable_payload("https://x/header.h"));
    CHECK_FALSE(TrustPolicy::is_executable_payload("https://x/data.json"));
}

TEST_CASE("HTML is reduced to readable text", "[web]") {
    const std::string html =
        "<html><head><title>Page Title</title>"
        "<style>body { color: red; }</style>"
        "<script>alert('should not appear');</script></head>"
        "<body><h1>Heading</h1><p>First paragraph.</p>"
        "<p>Second &amp; final.</p></body></html>";

    const auto text = WebService::html_to_text(html, 10000);

    CHECK(text.find("Heading") != std::string::npos);
    CHECK(text.find("First paragraph.") != std::string::npos);
    CHECK(text.find("Second & final.") != std::string::npos);
    // Script and style contents must be gone entirely.
    CHECK(text.find("should not appear") == std::string::npos);
    CHECK(text.find("color: red") == std::string::npos);
    CHECK(text.find("<p>") == std::string::npos);
}

TEST_CASE("the title is extracted", "[web]") {
    CHECK(WebService::extract_title("<html><head><title>My Page</title></head></html>") ==
          "My Page");
    CHECK(WebService::extract_title("<html><body>no title</body></html>").empty());
}

TEST_CASE("HTML entities are decoded", "[web]") {
    CHECK(WebService::decode_entities("a &amp; b") == "a & b");
    CHECK(WebService::decode_entities("&lt;tag&gt;") == "<tag>");
    CHECK(WebService::decode_entities("&quot;quoted&quot;") == "\"quoted\"");
    CHECK(WebService::decode_entities("&#65;&#66;") == "AB");
    CHECK(WebService::decode_entities("&#x41;") == "A");
    // A malformed entity is left as written rather than eaten.
    CHECK(WebService::decode_entities("&notanentity") == "&notanentity");
}

TEST_CASE("URL encoding escapes what it must", "[web]") {
    CHECK(WebService::url_encode("hello world") == "hello+world");
    CHECK(WebService::url_encode("a/b?c=d") == "a%2Fb%3Fc%3Dd");
    CHECK(WebService::url_encode("safe-._~") == "safe-._~");
}

TEST_CASE("a truncated page is marked", "[web]") {
    const std::string html = "<p>" + std::string(50000, 'x') + "</p>";
    const auto text = WebService::html_to_text(html, 1000);
    CHECK(text.size() < 2000);
    CHECK(text.find("truncated") != std::string::npos);
}

TEST_CASE("SearXNG results are parsed", "[web]") {
    const std::string body = R"({"results":[
        {"title":"First","url":"https://a.example/1","content":"snippet one"},
        {"title":"Second","url":"https://b.example/2","content":"snippet two"}]})";

    const auto results = WebService::parse_searxng_json(body, 10);
    REQUIRE(results.size() == 2);
    CHECK(results[0].title == "First");
    CHECK(results[1].url == "https://b.example/2");
}

TEST_CASE("Brave results are parsed", "[web]") {
    const std::string body =
        R"({"web":{"results":[{"title":"T","url":"https://x.example","description":"D"}]}})";
    const auto results = WebService::parse_brave_json(body, 10);
    REQUIRE(results.size() == 1);
    CHECK(results[0].snippet == "D");
}

TEST_CASE("malformed search responses yield nothing rather than throwing", "[web]") {
    CHECK(WebService::parse_searxng_json("not json", 10).empty());
    CHECK(WebService::parse_brave_json("{}", 10).empty());
    CHECK(WebService::parse_duckduckgo_html("<html></html>", 10).empty());
}

TEST_CASE("SSE events are assembled across chunk boundaries", "[sse]") {
    SseParser parser;
    std::vector<std::string> events;
    auto collect = [&events](const SseParser::Event& event) { events.push_back(event.data); };

    // A single event delivered in three pieces, split mid-field.
    parser.feed("data: {\"a\":", collect);
    CHECK(events.empty());
    parser.feed("1}\n", collect);
    CHECK(events.empty());   // not terminated yet
    parser.feed("\n", collect);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == "{\"a\":1}");
}

TEST_CASE("multiple SSE events in one chunk are all delivered", "[sse]") {
    SseParser parser;
    std::vector<std::string> events;
    parser.feed("data: one\n\ndata: two\n\ndata: [DONE]\n\n",
                [&events](const SseParser::Event& e) { events.push_back(e.data); });

    REQUIRE(events.size() == 3);
    CHECK(events[0] == "one");
    CHECK(events[2] == "[DONE]");
}

TEST_CASE("SSE comments and multi-line data are handled", "[sse]") {
    SseParser parser;
    std::vector<std::string> events;
    auto collect = [&events](const SseParser::Event& e) { events.push_back(e.data); };

    parser.feed(": this is a keep-alive comment\n\n", collect);
    CHECK(events.empty());

    parser.feed("data: line one\ndata: line two\n\n", collect);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == "line one\nline two");
}

TEST_CASE("CRLF line endings in an SSE stream work", "[sse]") {
    SseParser parser;
    std::vector<std::string> events;
    parser.feed("data: value\r\n\r\n",
                [&events](const SseParser::Event& e) { events.push_back(e.data); });
    REQUIRE(events.size() == 1);
    CHECK(events[0] == "value");
}

TEST_CASE("finish flushes an unterminated event", "[sse]") {
    SseParser parser;
    std::vector<std::string> events;
    auto collect = [&events](const SseParser::Event& e) { events.push_back(e.data); };

    parser.feed("data: trailing", collect);
    CHECK(events.empty());
    parser.finish(collect);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == "trailing");
}

TEST_CASE("SSE event bytes survive arbitrary transport fragmentation", "[sse]") {
    const std::string wire =
        ": keep-alive\r\nevent: update\r\ndata: {\"text\":\"été 🍌\"}\r\n"
        "data: suffix\r\n\r\ndata:\r\n\r\nevent: trailing\ndata: unfinished";
    for (std::size_t chunk = 1; chunk <= wire.size(); ++chunk) {
        CAPTURE(chunk);
        SseParser parser;
        std::vector<SseParser::Event> events;
        const auto collect = [&](const auto& event) { events.push_back(event); };
        for (std::size_t offset = 0; offset < wire.size(); offset += chunk) {
            parser.feed(std::string_view(wire).substr(offset, chunk), collect);
            parser.feed({}, collect);
        }
        parser.finish(collect);
        REQUIRE(events.size() == 3);
        CHECK(events[0].event == "update");
        CHECK(events[0].data == "{\"text\":\"été 🍌\"}\nsuffix");
        CHECK(events[1].data.empty());
        CHECK(events[2].event == "trailing");
        CHECK(events[2].data == "unfinished");
    }
}

TEST_CASE("SSE reset and finish allow a fresh stream after a partial line", "[sse]") {
    SseParser parser;
    std::vector<std::string> events;
    const auto collect = [&](const auto& event) { events.push_back(event.data); };
    parser.feed("event: abandoned\ndata: " + std::string(4096, 'x'), collect);
    parser.reset();
    parser.feed("data: one", collect);
    parser.finish(collect);
    parser.finish(collect);
    parser.feed("data: two\n\ndata: th", collect);
    parser.feed("ree\n\n", collect);
    REQUIRE(events.size() == 3);
    CHECK(events[0] == "one");
    CHECK(events[1] == "two");
    CHECK(events[2] == "three");
}

TEST_CASE("a large fragmented SSE field retains its exact payload", "[sse]") {
    SseParser parser;
    const std::string body = std::string(65536, 'x') + " fin 🍌";
    const std::string wire = "data: " + body + "\n\n";
    std::vector<std::string> events;
    const auto collect = [&](const auto& event) { events.push_back(event.data); };
    for (std::size_t offset = 0; offset < wire.size(); offset += 7) {
        parser.feed(std::string_view(wire).substr(offset, 7), collect);
    }
    parser.finish(collect);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == body);
}
