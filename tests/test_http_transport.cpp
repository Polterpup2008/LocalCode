#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <thread>

#include "inference/HttpClient.h"
#include "support/TempDirectory.h"
#include "util/Json.h"

using namespace localcode;
using namespace std::chrono_literals;

namespace {
std::string base_url() {
    const auto* value = std::getenv("LOCALCODE_TEST_HTTP_BASE_URL");
    return value ? value : "";
}

HttpRequest request(std::string_view path) {
    HttpRequest result;
    result.url = base_url() + std::string(path);
    result.connect_timeout = 2s;
    result.timeout = 5s;
    return result;
}
}   // namespace

TEST_CASE("configured native HTTP factory matches the selected transport", "[http-transport]") {
    auto client = HttpClient::create();
#if defined(LOCALCODE_HTTP_CURL)
    CHECK(client->backend_name() == "libcurl");
#elif defined(LOCALCODE_HTTP_WINHTTP)
    CHECK(client->backend_name() == "WinHTTP");
#else
    CHECK(client->backend_name() == "unavailable");
    auto response = client->send({});
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::NotSupported);
    response = client->stream({}, {});
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::NotSupported);
    test::TempDirectory scratch;
    auto result = client->download({}, scratch / "absent.bin", {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::NotSupported);
    CHECK_FALSE(std::filesystem::exists(scratch / "absent.bin"));
#endif
}

TEST_CASE("native HTTP validates TLS certificates", "[http-transport]") {
    auto client = HttpClient::create();
    if (client->backend_name() == "unavailable") SKIP("Build has no HTTP transport");
    const auto* untrusted = std::getenv("LOCALCODE_TEST_HTTP_UNTRUSTED_TLS_URL");
    const auto* trusted = std::getenv("LOCALCODE_TEST_HTTP_TRUSTED_TLS_URL");
    if (!untrusted && !trusted) SKIP("TLS fixtures are opt-in");
    if (untrusted) {
        auto value = request("/echo");
        value.url = untrusted;
        auto response = client->send(value);
        REQUIRE_FALSE(response);
        CHECK(response.error().code == ErrorCode::NetworkError);
    }
    if (trusted) {
        auto value = request("/echo");
        value.url = trusted;
        value.method = HttpMethod::Head;
        auto response = client->send(value);
        REQUIRE(response);
        CHECK(response->ok());
    }
    REQUIRE(client->send(request("/echo")));
}

TEST_CASE("native HTTP preserves requests and resets connection state", "[http-transport]") {
    if (base_url().empty()) SKIP("Run scripts/test-http-transport.py for the local HTTP fixture");
    auto client = HttpClient::create();
    if (client->backend_name() == "unavailable") SKIP("Build has no HTTP transport");
    auto post = request("/echo");
    post.method = HttpMethod::Post;
    post.body = "caf\xc3\xa9";
    post.headers = {{"Authorization", "Bearer fictional-alpha"}};
    auto response = client->send(post);
    REQUIRE(response);
    REQUIRE(response->ok());
    auto first = Json::parse(response->body);
    CHECK(first["body"] == post.body);
    CHECK(first["method"] == "POST");
    CHECK(first["authorization"] == "Bearer fictional-alpha");
    response = client->send(request("/echo"));
    REQUIRE(response);
    auto second = Json::parse(response->body);
    CHECK(second["method"] == "GET");
    CHECK(second["body"] == "");
    CHECK(second["authorization"] == "");
    if (client->backend_name() == "libcurl") CHECK(second["connection"] == first["connection"]);

    REQUIRE(client->send(request("/set-cookie")));
    response = client->send(request("/echo"));
    REQUIRE(response);
    CHECK(Json::parse(response->body)["cookie"] == "");
    auto head = request("/echo");
    head.method = HttpMethod::Head;
    response = client->send(head);
    REQUIRE(response);
    CHECK(response->body.empty());
    response = client->send(request("/echo"));
    REQUIRE(response);
    CHECK(Json::parse(response->body)["method"] == "GET");

    auto redirect = request("/redirect");
    response = client->send(redirect);
    REQUIRE(response);
    CHECK(response->status_code == 200);
    CHECK(Json::parse(response->body)["method"] == "GET");
    if (client->backend_name() == "libcurl") CHECK(response->final_url.ends_with("/echo"));
    redirect.follow_redirects = false;
    response = client->send(redirect);
    REQUIRE(response);
    CHECK(response->status_code == 302);
    CHECK(response->body == "intermediate");
    auto capped = request("/big");
    capped.max_response_bytes = 17;
    response = client->send(capped);
    REQUIRE(response);
    CHECK(response->truncated);
    CHECK(response->body.size() == 17);
    response = client->send(request("/echo"));
    REQUIRE(response);
    CHECK(Json::parse(response->body)["authorization"] == "");
    if (client->backend_name() == "libcurl") {
        redirect = request("/redirect-other");
        redirect.headers = {{"Authorization", "Bearer fictional-private-origin"}};
        response = client->send(redirect);
        REQUIRE(response);
        CHECK(Json::parse(response->body)["authorization"] == "");
        redirect = request("/loop");
        redirect.max_redirects = 1;
        response = client->send(redirect);
        REQUIRE_FALSE(response);
        auto invalid = request("/echo");
        invalid.url = "file:///not-an-http-request";
        response = client->send(invalid);
        REQUIRE_FALSE(response);
        CHECK(response.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("native HTTP reuse keeps explicit cookies and credentials isolated", "[http-transport]") {
    if (base_url().empty()) SKIP("Run scripts/test-http-transport.py for the local HTTP fixture");
    auto client = HttpClient::create();
    if (client->backend_name() == "unavailable") SKIP("Build has no HTTP transport");
    auto value = request("/echo");
    value.headers = {{"Authorization", "Bearer fictional-user-one"}};
    auto response = client->send(value);
    REQUIRE(response);
    const auto first = Json::parse(response->body);
    response = client->send(value);
    REQUIRE(response);
    const auto repeated = Json::parse(response->body);
    CHECK(repeated["connection"] == first["connection"]);
    CHECK(repeated["authorization"] == "Bearer fictional-user-one");
    value.headers = {{"Authorization", "Bearer fictional-user-two"}, {"Cookie", "explicit=allowed"}};
    response = client->send(value);
    REQUIRE(response);
    const auto changed = Json::parse(response->body);
    CHECK(changed["authorization"] == "Bearer fictional-user-two");
    CHECK(changed["cookie"] == "explicit=allowed");
    if (client->backend_name() == "WinHTTP") CHECK(changed["connection"] != first["connection"]);
    REQUIRE(client->send(request("/set-cookie")));
    response = client->send(request("/echo"));
    REQUIRE(response);
    const auto clean = Json::parse(response->body);
    CHECK(clean["authorization"] == "");
    CHECK(clean["cookie"] == "");
    value.headers = {{"Authorization", "Bearer fictional-user-one"}};
    response = client->send(value);
    REQUIRE(response);
    CHECK(Json::parse(response->body)["authorization"] == "Bearer fictional-user-one");
    CHECK(Json::parse(response->body)["cookie"] == "");
}

TEST_CASE("concurrent native HTTP deadlines do not overwrite an active request", "[http-transport]") {
    if (base_url().empty()) SKIP("Run scripts/test-http-transport.py for the local HTTP fixture");
    auto client = HttpClient::create();
    if (client->backend_name() == "unavailable") SKIP("Build has no HTTP transport");
    REQUIRE(client->send(request("/echo")));
    auto long_request = request("/stall");
    auto pending = std::async(std::launch::async, [&] { return client->send(long_request); });
    std::this_thread::sleep_for(150ms);
    auto short_request = request("/stall");
    short_request.timeout = 200ms;
    const auto start = std::chrono::steady_clock::now();
    auto response = client->send(short_request);
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::Timeout);
    CHECK(std::chrono::steady_clock::now() - start < 1500ms);
    response = pending.get();
    REQUIRE(response);
    CHECK(response->body == "late");
    response = client->send(request("/echo"));
    REQUIRE(response);
    CHECK(Json::parse(response->body)["method"] == "GET");
}

TEST_CASE("native HTTP streaming and cancellation preserve later requests", "[http-transport]") {
    if (base_url().empty()) SKIP("Run scripts/test-http-transport.py for the local HTTP fixture");
    auto client = HttpClient::create();
    if (client->backend_name() == "unavailable") SKIP("Build has no HTTP transport");
    SseParser parser;
    std::vector<std::string> events;
    auto response = client->stream(request("/sse"), [&](std::string_view chunk) {
        parser.feed(chunk, [&](const SseParser::Event& event) { events.push_back(event.data); });
        return events.empty() || events.back() != "[DONE]";
    });
    REQUIRE(response);
    CHECK(response->body.empty());
    CHECK(events == std::vector<std::string>{"caf\xc3\xa9", "[DONE]"});

    CancellationSource cancel;
    auto stalled = std::async(std::launch::async,
                              [&] { return client->send(request("/stall"), cancel.token()); });
    std::this_thread::sleep_for(150ms);
    const auto started = std::chrono::steady_clock::now();
    cancel.cancel();
    REQUIRE(stalled.wait_for(1500ms) == std::future_status::ready);
    response = stalled.get();
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::Cancelled);
    CHECK(std::chrono::steady_clock::now() - started < 1500ms);

    auto timeout = request("/stall");
    timeout.timeout = 200ms;
    response = client->send(timeout);
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::Timeout);
    std::string heartbeat;
    timeout = request("/heartbeat");
    timeout.timeout = 250ms;
    response = client->stream(timeout, [&](std::string_view chunk) {
        heartbeat += chunk;
        return true;
    });
    REQUIRE(response);
    CHECK(heartbeat == "abcdef");
    response = client->send(timeout);
    REQUIRE_FALSE(response);
    CHECK(response.error().code == ErrorCode::Timeout);
    if (client->backend_name() == "libcurl") {
        response = client->stream(request("/echo"), [](std::string_view) -> bool {
            throw std::runtime_error("fictional callback failure");
        });
        REQUIRE_FALSE(response);
        CHECK(response.error().code == ErrorCode::Internal);
    }
    REQUIRE(client->send(request("/echo")));
}

TEST_CASE("native HTTP downloads and concurrent requests stay isolated", "[http-transport]") {
    if (base_url().empty()) SKIP("Run scripts/test-http-transport.py for the local HTTP fixture");
    auto client = HttpClient::create();
    if (client->backend_name() == "unavailable") SKIP("Build has no HTTP transport");
    test::TempDirectory scratch;
    auto path = scratch.path() / std::filesystem::path(u8"caf\u00e9-\u65e5\u672c\u8a9e.bin");
    std::uint64_t progress = 0, total = 0;
    auto status = client->download(request("/big"), path, [&](auto bytes, auto length) {
        progress = bytes;
        total = length;
    });
    REQUIRE(status);
    std::ifstream file(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    REQUIRE(bytes.size() == 2048);
    CHECK(static_cast<unsigned char>(bytes[255]) == 255);
    CHECK(progress == 2048);
    CHECK(total == 2048);
    file.close();
    status = client->download(request("/missing"), scratch / "failed.bin", {});
    REQUIRE_FALSE(status);
    CHECK_FALSE(std::filesystem::exists(scratch / "failed.bin"));
    CHECK_FALSE(std::filesystem::exists(scratch / "failed.bin.part"));

    const auto blocked = scratch.path() / std::filesystem::path(u8"bloqu\u00e9-\u65e5\u672c\u8a9e");
    std::filesystem::create_directory(blocked);
    status = client->download(request("/big"), blocked, {});
    REQUIRE_FALSE(status);
    CHECK(status.error().code == ErrorCode::IoError);
    CHECK(std::filesystem::is_directory(blocked));
    auto partial = blocked;
    partial += ".part";
    CHECK_FALSE(std::filesystem::exists(partial));

    std::vector<std::future<Result<HttpResponse>>> pending;
    for (int i = 0; i < 8; ++i) {
        pending.push_back(std::async(std::launch::async, [&, i] {
            auto value = request("/echo");
            value.headers = {{"Authorization", "Bearer fictional-" + std::to_string(i)}};
            if (i % 2) {
                value.method = HttpMethod::Post;
                value.body = std::to_string(i);
            }
            return client->send(value);
        }));
    }
    for (int i = 0; i < 8; ++i) {
        auto response = pending[i].get();
        REQUIRE(response);
        const auto parsed = Json::parse(response->body);
        CHECK(parsed["authorization"] == "Bearer fictional-" + std::to_string(i));
        CHECK(parsed["method"] == (i % 2 ? "POST" : "GET"));
        CHECK(parsed["body"] == (i % 2 ? std::to_string(i) : ""));
    }
}

TEST_CASE("native HTTP client reuse is measured separately from model decoding",
          "[http-transport][.http-performance]") {
    const auto* output = std::getenv("LOCALCODE_TEST_HTTP_PERFORMANCE_OUTPUT");
    if (base_url().empty() || !output) SKIP("Performance report is opt-in");
    auto pooled = HttpClient::create();
    if (pooled->backend_name() == "unavailable") SKIP("Build has no HTTP transport");
    Json report{{"scope",
                 "Loopback HTTP overhead including fixture server; no model, GPU, external network "
                 "or Discord"},
                {"backend", pooled->backend_name()},
                {"phases", Json::array()}};
    for (const bool reuse : {false, true, true, false}) {
        std::vector<double> samples;
        for (int pass = -1; pass < 7; ++pass) {
            const auto start = std::chrono::steady_clock::now();
            for (int index = 0; index < 100; ++index) {
                auto client = reuse ? pooled : HttpClient::create();
                auto response = client->send(request("/echo"));
                REQUIRE(response);
                REQUIRE(response->ok());
                REQUIRE(Json::parse(response->body)["method"] == "GET");
            }
            if (pass >= 0)
                samples.push_back(std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
        }
        auto ordered = samples;
        std::sort(ordered.begin(), ordered.end());
        report["phases"].push_back({{"reuse", reuse},
                                    {"requests_per_sample", 100},
                                    {"samples_ms", samples},
                                    {"median_ms", ordered[3]}});
    }
    std::ofstream file(
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(output))),
        std::ios::binary);
    REQUIRE(file);
    file << report.dump(2) << '\n';
    REQUIRE(file);
}
