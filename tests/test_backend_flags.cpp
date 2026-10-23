// ---------------------------------------------------------------------------
// Server flag arity detection.
//
// This is small but load-bearing: the two runtimes notate a flag's value
// differently, and passing --flash-attn without a value to a build that wants
// one makes the server exit with a usage error, so the model never loads. The
// help excerpts below are taken verbatim from real builds.
// ---------------------------------------------------------------------------
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <tuple>

#include "inference/BackendManager.h"
#include "inference/HttpClient.h"
#include "inference/OpenAiCompatBackend.h"
#include "inference/RequestTrace.h"
#include "inference/ServerManager.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "support/TempDirectory.h"
#include "util/Strings.h"

namespace {
class TokenizerClient : public localcode::HttpClient {
public:
    int calls = 0;
    int token_count = 41;
    int failure_status = 0;
    bool malformed = false;

    localcode::Result<localcode::HttpResponse> send(const localcode::HttpRequest& request,
                                                    localcode::CancellationToken) override {
        localcode::HttpResponse response;
        response.status_code = 200;
        if (!request.url.ends_with("/tokenize")) {
            response.body = R"({"model_path":"public-fixture.gguf","n_ctx":32768})";
            return response;
        }
        const auto text = localcode::Json::parse(request.body).at("content").get<std::string>();
        if (text != "probe") ++calls;
        if (text != "probe" && failure_status) {
            response.status_code = failure_status;
            return response;
        }
        response.body = text != "probe" && malformed
                            ? "{}"
                            : localcode::Json{{"tokens", std::vector<int>(token_count, 17)}}.dump();
        return response;
    }

    localcode::Result<localcode::HttpResponse> stream(const localcode::HttpRequest&,
                                                      const localcode::HttpStreamCallback&,
                                                      localcode::CancellationToken) override {
        return localcode::make_error(localcode::ErrorCode::NotSupported, "not used");
    }

    localcode::Status download(const localcode::HttpRequest&, const std::filesystem::path&,
                               const std::function<void(std::uint64_t, std::uint64_t)>&,
                               localcode::CancellationToken) override {
        return localcode::make_error(localcode::ErrorCode::NotSupported, "not used");
    }

    std::string backend_name() const override { return "public tokenizer fixture"; }
};
}   // namespace

TEST_CASE("opt-in wire traces match captured HTTP payloads and redact secrets",
          "[backend][trace][wire]") {
    using namespace localcode;

    struct CaptureClient final : HttpClient {
        std::string body;

        Result<HttpResponse> send(const HttpRequest& request, CancellationToken) override {
            body = request.body;
            HttpResponse response;
            response.status_code = 200;
            response.body =
                R"({"choices":[{"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}]})";
            return response;
        }

        Result<HttpResponse> stream(const HttpRequest& request, const HttpStreamCallback& callback,
                                    CancellationToken) override {
            body = request.body;
            callback(
                "data: "
                "{\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n");
            callback("data: [DONE]\n\n");
            HttpResponse response;
            response.status_code = 200;
            return response;
        }

        Status download(const HttpRequest&, const std::filesystem::path&,
                        const std::function<void(std::uint64_t, std::uint64_t)>&,
                        CancellationToken) override {
            return make_error(ErrorCode::NotSupported, "unused fixture operation");
        }

        std::string backend_name() const override { return "captured wire fixture"; }
    };

    struct ResetTrace {
        ~ResetTrace() { RequestTrace::reset_for_testing(); }
    } reset;

    for (bool stream : {false, true}) {
        test::TempDirectory directory;
        RequestTrace::set_directory_for_testing(directory.path());
        RequestTrace::begin_run("wire-fixture");
        auto events_file = RequestTrace::current_file();
        events_file.replace_extension(".events.jsonl");
        auto client = std::make_shared<CaptureClient>();
        BackendEndpoint endpoint;
        endpoint.native_reasoning_budget = true;
        LlamaCppBackend backend{endpoint, client};
        ChatRequest request;
        request.stream = stream;
        request.thinking_override = false;
        request.messages = {ChatMessage::user("Inspect the public source fixture.")};
        request.tools = {{"read_file", "Read public source", Json{{"type", "object"}}}};
        REQUIRE(backend.chat(request, {}, {}));
        Json event;
        {
            std::ifstream input(events_file);
            input >> event;
        }
        REQUIRE(event.at("stage") == "http_chat_prepared");
        CHECK(event["fields"]["bytes"] == client->body.size());
        CHECK(event["fields"]["stream"] == stream);
        REQUIRE(event["fields"].contains("payload"));
        CHECK(event["fields"]["payload"] == Json::parse(client->body));
        CHECK(event["fields"]["payload"]["tools"][0]["function"]["name"] == "read_file");
        CHECK(event["fields"]["payload"]["reasoning_budget_tokens"] == 0);

        RequestTrace::end_run();
        RequestTrace::begin_run("wire-redaction-fixture");
        events_file = RequestTrace::current_file();
        events_file.replace_extension(".events.jsonl");
        request.messages = {ChatMessage::user("API_KEY=wire-fixture-secret-value")};
        REQUIRE(backend.chat(request, {}, {}));
        std::ifstream input(events_file);
        const std::string captured{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
        CHECK(captured.find("wire-fixture-secret-value") == std::string::npos);
        CHECK(captured.find("<redacted>") != std::string::npos);
        RequestTrace::end_run();
    }
}

TEST_CASE("repeated exact token counts avoid repeated HTTP exchanges", "[backend][token-cache]") {
    using namespace localcode;
    auto http = std::make_shared<TokenizerClient>();
    LlamaCppBackend backend{{}, http};
    REQUIRE(backend.info({}));
    const std::string text(12000, 'x');
    REQUIRE(backend.count_tokens(text, {}));
    CHECK(*backend.count_tokens(text, {}) == 41);
    CHECK(http->calls == 1);
    auto changed = text;
    changed.back() = 'y';
    CHECK(*backend.count_tokens(changed, {}) == 41);
    CHECK(http->calls == 2);
}

TEST_CASE("token counts are invalidated when endpoints or model probes change",
          "[backend][token-cache]") {
    using namespace localcode;
    auto http = std::make_shared<TokenizerClient>();
    LlamaCppBackend backend{{}, http};
    REQUIRE(backend.info({}));
    const std::string text(3000, 'x');
    CHECK(*backend.count_tokens(text, {}) == 41);
    http->token_count = 79;
    backend.set_endpoint({"http://127.0.0.1:43210"});
    CHECK(*backend.count_tokens(text, {}) == 79);
    http->token_count = 93;
    REQUIRE(backend.info({}));
    CHECK(*backend.count_tokens(text, {}) == 93);
    CHECK(http->calls == 3);
}

TEST_CASE("failed tokenizer responses are retried rather than cached as exact",
          "[backend][token-cache]") {
    using namespace localcode;
    auto http = std::make_shared<TokenizerClient>();
    LlamaCppBackend backend{{}, http};
    REQUIRE(backend.info({}));
    const std::string text(3000, 'x');
    http->failure_status = 503;
    CHECK(*backend.count_tokens(text, {}) == estimate_tokens(text));
    http->failure_status = 0;
    http->malformed = true;
    CHECK(*backend.count_tokens(text, {}) == estimate_tokens(text));
    http->malformed = false;
    CHECK(*backend.count_tokens(text, {}) == 41);
    CHECK(*backend.count_tokens(text, {}) == 41);
    CHECK(http->calls == 3);
}

TEST_CASE("token counting retains bounded text and evicts old entries", "[backend][token-cache]") {
    using namespace localcode;
    auto http = std::make_shared<TokenizerClient>();
    LlamaCppBackend backend{{}, http};
    REQUIRE(backend.info({}));
    const std::string oversized(300000, 'x');
    CHECK(*backend.count_tokens(oversized, {}) == 41);
    CHECK(*backend.count_tokens(oversized, {}) == 41);
    CHECK(http->calls == 2);
    const std::string first(16000, 'a');
    CHECK(*backend.count_tokens(first, {}) == 41);
    for (int i = 0; i < 18; ++i)
        REQUIRE(backend.count_tokens(std::string(16000, 'b') + std::to_string(i), {}));
    const int before = http->calls;
    CHECK(*backend.count_tokens(first, {}) == 41);
    CHECK(http->calls == before + 1);
    REQUIRE(backend.info({}));
    CHECK(*backend.count_tokens("first entry", {}) == 41);
    for (int i = 0; i < 32; ++i)
        REQUIRE(backend.count_tokens("short entry " + std::to_string(i), {}));
    const int after_entries = http->calls;
    CHECK(*backend.count_tokens("first entry", {}) == 41);
    CHECK(http->calls == after_entries + 1);
}

TEST_CASE("a throwing model state observer cannot escape shutdown", "[backend][observer]") {
    using namespace localcode;
    ModelServer server;
    int calls = 0;
    server.set_state_callback([&](const ServerStatus&) {
        ++calls;
        throw 7;
    });
    ServerConfig config;
    test::TempDirectory directory;
    config.binary = directory.path() / "missing-runtime.exe";
    config.startup_timeout = std::chrono::seconds(5);
    CHECK_FALSE(server.start(config, {}));
    CHECK_NOTHROW(server.stop());
    CHECK(calls == 1);
    CHECK(server.status().state == ServerState::Stopped);
    CHECK_NOTHROW(server.stop());
    CHECK(calls == 1);
}

namespace {
#if defined(_WIN32)
constexpr const char* kExeSuffixForTest = ".exe";
#else
constexpr const char* kExeSuffixForTest = "";
#endif
}   // namespace

using namespace localcode;

TEST_CASE("prompt cache arguments distinguish default disabled and bounded",
          "[backend][cache-ram]") {
    ServerConfig config;
    ServerBinaryFeatures features;
    features.cache_ram = true;
    auto args = ModelServer::build_arguments(config, features, 8080);
    CHECK(std::find(args.begin(), args.end(), "--cache-ram") == args.end());
    for (const auto value : {0, 64, 8192}) {
        config.profile.prompt_cache_mib = value;
        args = ModelServer::build_arguments(config, features, 8080);
        const auto flag = std::find(args.begin(), args.end(), "--cache-ram");
        REQUIRE(flag != args.end());
        REQUIRE(std::next(flag) != args.end());
        CHECK(*std::next(flag) == std::to_string(value));
        CHECK(std::count(args.begin(), args.end(), "--cache-ram") == 1);
    }
    config.profile.prompt_cache_mib.reset();
    config.profile.extra_server_arguments = {"--cache-ram", "256"};
    args = ModelServer::build_arguments(config, features, 8080);
    CHECK(std::count(args.begin(), args.end(), "--cache-ram") == 1);
}

TEST_CASE("explicit prompt cache limits reject unsupported runtimes and invalid overrides",
          "[backend][cache-ram]") {
    ServerConfig config;
    config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
    config.auto_restart = false;
    config.profile.prompt_cache_mib = 0;
    std::string expected = "--cache-ram";
    SECTION("unsupported explicit disable") {}
    SECTION("negative invalid value") {
        config.profile.prompt_cache_mib = -1;
        expected = "between 0";
    }
    SECTION("value exceeding native integer range") {
        config.profile.prompt_cache_mib = 2147483648LL;
        expected = "between 0";
    }
    SECTION("advanced profile flag") {
        config.profile.extra_server_arguments = {"--cache-ram", "64"};
        expected = "conflicts";
    }
    SECTION("advanced alias flag") {
        config.profile.extra_server_arguments = {"-cram", "64"};
        expected = "conflicts";
    }
    SECTION("advanced config equals form") {
        config.extra_arguments = {"--cache-ram=64"};
        expected = "conflicts";
    }
    bool healthCalled = false;
    ModelServer server;
    const auto started = server.start(config, [&](const std::string&, CancellationToken) {
        healthCalled = true;
        return Status{};
    });
    REQUIRE_FALSE(started);
    CHECK_FALSE(healthCalled);
    CHECK(started.error().to_string().find(expected) != std::string::npos);
    CHECK(server.status().process_id == 0);
}

TEST_CASE("native prompt cache budgets reach the server and preserve initial scores",
          "[backend][cache-ram][.runtime]") {
    const auto* runtime = std::getenv("LOCALCODE_CACHE_RAM_TEST_RUNTIME");
    const auto* model = std::getenv("LOCALCODE_CACHE_RAM_TEST_MODEL");
    if (!runtime || !model)
        SKIP("Set explicit runtime/model paths for native cache budget checks.");
    test::TempDirectory directory;
    const auto previous = app_paths().data_dir().parent_path();

    struct Restore {
        std::filesystem::path root;

        ~Restore() { app_paths().set_root(root); }
    } restore{previous};

    app_paths().set_root(directory.path());
    REQUIRE(app_paths().ensure_directories());
    const auto http = HttpClient::create();
    Json results = Json::array();
    Json reference;
    for (const auto budget : {std::optional<std::int64_t>{}, std::optional<std::int64_t>{0},
                              std::optional<std::int64_t>{64}}) {
        ServerConfig config;
        config.binary = path_from_utf8(runtime);
        config.profile.id = "owned-cache-budget";
        config.profile.model_path = path_from_utf8(model);
        config.profile.gpu_layers = 0;
        config.profile.context_size = 512;
        config.profile.threads = 2;
        config.profile.threads_batch = 2;
        config.profile.batch_size = 64;
        config.profile.ubatch_size = 64;
        config.profile.cache_type_k = "f16";
        config.profile.cache_type_v = "f16";
        config.profile.prompt_cache_mib = budget;
        config.extra_arguments = {"--log-verbosity", "4", "--no-warmup"};
        config.startup_timeout = std::chrono::seconds(120);
        config.auto_restart = false;
        ModelServer server;
        const auto started =
            server.start(config, [http](const std::string& url, CancellationToken cancel) {
                BackendEndpoint endpoint;
                endpoint.base_url = url;
                return OpenAiCompatBackend{endpoint, http}.health(cancel);
            });
        INFO(budget.value_or(-1));
        REQUIRE(started);
        const auto features = ModelServer::probe_binary(config.binary);
        REQUIRE(features);
        REQUIRE(features->cache_ram);
        const auto logs = strings::join(server.recent_log(3000), "\n");
        const auto expected =
            budget && *budget == 0
                ? "prompt cache is disabled"
                : "prompt cache is enabled, size limit: " + std::to_string(budget.value_or(8192)) +
                      " MiB";
        INFO(logs);
        REQUIRE(logs.find(expected) != std::string::npos);
        HttpRequest request;
        request.method = HttpMethod::Post;
        request.url = server.base_url() + "/completion";
        request.headers = {{"Content-Type", "application/json"}};
        request.timeout = std::chrono::seconds(30);
        Json prompt = Json::array();
        for (int token = 1; token <= 31; ++token)
            prompt.push_back(token);
        request.body =
            Json{{"prompt", prompt}, {"n_predict", 2},        {"temperature", 0}, {"seed", 42},
                 {"n_probs", 4},     {"cache_prompt", false}, {"stream", false}}
                .dump();
        const auto reply = http->send(request);
        REQUIRE(reply);
        REQUIRE(reply->ok());
        const auto parsed = Json::parse(reply->body);
        REQUIRE(parsed.value("tokens_predicted", 0) > 0);
        REQUIRE(parsed.contains("completion_probabilities"));
        const Json observed = {{"content", parsed.at("content")},
                               {"probabilities", parsed.at("completion_probabilities")},
                               {"predicted", parsed.at("tokens_predicted")}};
        if (reference.is_null()) reference = observed;
        CHECK(observed == reference);
        const auto args = ModelServer::build_arguments(config, *features, 8080);
        const auto pid = server.status().process_id;
        server.stop();
        REQUIRE(server.status().state == ServerState::Stopped);
        REQUIRE(server.status().process_id == 0);
        results.push_back({{"budget_mib", budget ? Json(*budget) : Json()},
                           {"arguments", args},
                           {"runtime_budget_log_observed", true},
                           {"pid", pid},
                           {"stopped", true},
                           {"scores", observed}});
    }
    if (const auto* output = std::getenv("LOCALCODE_CACHE_RAM_TEST_PROOF")) {
        std::ofstream file(path_from_utf8(output));
        REQUIRE(file.good());
        file << results.dump(2) << '\n';
    }
}

using localcode::test::TempDirectory;

TEST_CASE("weight repacking requires an exact advertised boolean switch",
          "[backend][flags][repacking]") {
    CHECK(ModelServer::help_advertises_switch(
        "  --repack, -nr, --no-repack    whether to repack weights\n", "--repack"));
    CHECK(ModelServer::help_advertises_switch(
        "  --repack, -nr, --no-repack    whether to repack weights\n", "--no-repack"));
    CHECK(ModelServer::help_advertises_switch("--no-repack\n", "--no-repack"));
    for (const auto& help :
         {"Use --repack to speed up loading\n", "--repack-cache  unrelated option\n",
          "--other  use --repack for weights\n",
          "--other  --repack applies only to another runtime\n",
          "--other\t--repack applies only to another runtime\n",
          "--repack [on|off]  value-taking form\n", "--repack BOOL  value-taking form\n",
          "--repack=BOOL  value-taking form\n"})
        CHECK_FALSE(ModelServer::help_advertises_switch(help, "--repack"));
}

TEST_CASE("weight repacking launch flags preserve an unset runtime default",
          "[backend][flags][repacking]") {
    ServerConfig config;
    ServerBinaryFeatures features;
    features.repack = features.no_repack = true;
    auto args = ModelServer::build_arguments(config, features, 8080);
    CHECK(std::find(args.begin(), args.end(), "--repack") == args.end());
    CHECK(std::find(args.begin(), args.end(), "--no-repack") == args.end());
    for (const auto mode : {WeightRepacking::Enabled, WeightRepacking::Disabled}) {
        config.profile.weight_repacking = mode;
        args = ModelServer::build_arguments(config, features, 8080);
        CHECK(std::count(args.begin(), args.end(), mode == WeightRepacking::Enabled ? "--repack" : "--no-repack") == 1);
        CHECK(std::find(args.begin(), args.end(), mode == WeightRepacking::Enabled ? "--no-repack" : "--repack") == args.end());
    }
    config.profile.weight_repacking = WeightRepacking::RuntimeDefault;
    config.profile.extra_server_arguments = {"--no-repack"};
    args = ModelServer::build_arguments(config, features, 8080);
    CHECK(std::count(args.begin(), args.end(), "--no-repack") == 1);
}

TEST_CASE("explicit weight repacking rejects unsupported runtimes and conflicting overrides",
          "[backend][flags][repacking]") {
    ServerConfig config;
    config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
    config.auto_restart = false;
    config.profile.weight_repacking = WeightRepacking::Disabled;
    std::string expected = "--no-repack";
    SECTION("unsupported disable") {}
    SECTION("unsupported enable") { config.profile.weight_repacking = WeightRepacking::Enabled; expected = "--repack"; }
    SECTION("invalid saved value") { config.profile.weight_repacking = WeightRepacking::Invalid; expected = "must be"; }
    SECTION("profile override") { config.profile.extra_server_arguments = {"--repack"}; expected = "conflicts"; }
    SECTION("runtime alias override") { config.extra_arguments = {"-nr"}; expected = "conflicts"; }
    SECTION("runtime equals override") { config.extra_arguments = {"--no-repack=false"}; expected = "conflicts"; }
    bool healthCalled = false;
    ModelServer server;
    const auto started = server.start(config, [&](const std::string&, CancellationToken) { healthCalled = true; return Status{}; });
    REQUIRE_FALSE(started);
    CHECK_FALSE(healthCalled);
    CHECK(started.error().to_string().find(expected) != std::string::npos);
    CHECK(server.status().process_id == 0);
}

TEST_CASE("physical placement refuses unsupported runtimes and conflicting CPU overrides",
          "[backend][flags][placement]") {
    using namespace localcode;
    ServerConfig config;
    config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
    config.auto_restart = false;
    config.profile.cpu_placement = CpuPlacement::PhysicalCores;
    std::string expected = "physical CPU core placement";
    SECTION("unsupported runtime") {}
    SECTION("invalid saved choice") { config.profile.cpu_placement = CpuPlacement::Invalid; expected = "must be"; }
    SECTION("profile CPU mask override") { config.profile.extra_server_arguments = {"--cpu-mask=0x1"}; expected = "conflicts"; }
    SECTION("config CPU range override") { config.extra_arguments = {"-Crb", "0-3"}; expected = "conflicts"; }
    SECTION("config CPU thread override") { config.extra_arguments = {"--threads=8"}; expected = "conflicts"; }
    bool health_called = false;
    ModelServer server;
    const auto started = server.start(config, [&](const std::string&,CancellationToken) { health_called = true; return Status{}; });
    REQUIRE_FALSE(started);
    CHECK_FALSE(health_called);
    CHECK(started.error().to_string().find(expected) != std::string::npos);
    CHECK(server.status().process_id == 0);
}

TEST_CASE("native weight repacking choices reach an owned CPU runtime",
          "[backend][repacking][.runtime]") {
    const auto* runtime = std::getenv("LOCALCODE_REPACK_TEST_RUNTIME");
    const auto* model = std::getenv("LOCALCODE_REPACK_TEST_MODEL");
    if (!runtime || !model) SKIP("Set an explicit CPU runtime and repack-compatible GGUF.");
    test::TempDirectory directory;
    const auto previous = app_paths().data_dir().parent_path();
    struct Restore { std::filesystem::path root; ~Restore() { app_paths().set_root(root); } } restore{previous};
    app_paths().set_root(directory.path());
    REQUIRE(app_paths().ensure_directories());
    const auto http = HttpClient::create();
    Json results = Json::array();
    for (const auto mode : {WeightRepacking::RuntimeDefault, WeightRepacking::Enabled, WeightRepacking::Disabled}) {
        ServerConfig config;
        config.binary = path_from_utf8(runtime);
        config.profile.id = "owned-repacking-check";
        config.profile.model_path = path_from_utf8(model);
        config.profile.gpu_layers = 0;
        config.profile.context_size = 512;
        config.profile.threads = config.profile.threads_batch = 2;
        config.profile.batch_size = config.profile.ubatch_size = 64;
        config.profile.cache_type_k = config.profile.cache_type_v = "f16";
        config.profile.prompt_cache_mib = 0;
        config.profile.weight_repacking = mode;
        config.extra_arguments = {"--log-verbosity", "4", "--no-warmup"};
        config.startup_timeout = std::chrono::seconds(90);
        config.auto_restart = false;
        ModelServer server;
        REQUIRE(server.start(config, [http](const std::string& url, CancellationToken cancel) {
            BackendEndpoint endpoint; endpoint.base_url = url;
            return OpenAiCompatBackend{endpoint, http}.health(cancel);
        }));
        const auto features = ModelServer::probe_binary(config.binary);
        REQUIRE(features);
        REQUIRE(features->repack);
        REQUIRE(features->no_repack);
        HttpRequest request;
        request.method = HttpMethod::Post;
        request.url = server.base_url() + "/completion";
        request.headers = {{"Content-Type", "application/json"}};
        request.timeout = std::chrono::seconds(30);
        Json prompt = Json::array(); for (int id = 1; id < 32; ++id) prompt.push_back(id);
        request.body = Json{{"prompt", prompt}, {"n_predict", 2}, {"temperature", 0}, {"seed", 42},
                            {"ignore_eos", true}, {"return_tokens", true}, {"cache_prompt", false}, {"stream", false}}.dump();
        const auto reply = http->send(request);
        REQUIRE(reply); REQUIRE(reply->ok());
        const auto value = Json::parse(reply->body);
        CHECK(value.at("tokens_predicted") == 2);
        CHECK(value.at("tokens").size() == 2);
        const auto pid = server.status().process_id;
        const auto logPath = server.log_file();
        const auto args = ModelServer::build_arguments(config, *features, 8080);
        server.stop();
        REQUIRE(server.status().process_id == 0);
        std::ifstream stream(logPath);
        const std::string logs((std::istreambuf_iterator<char>(stream)), {});
        const bool repacked = std::regex_search(logs, std::regex("CPU_REPACK[ \\t]+model buffer size"));
        if (mode == WeightRepacking::Enabled) CHECK(repacked);
        if (mode == WeightRepacking::Disabled) CHECK_FALSE(repacked);
        results.push_back({{"mode", mode == WeightRepacking::RuntimeDefault ? Json() : Json(mode == WeightRepacking::Enabled)},
                           {"arguments", args}, {"cpu_repack_buffer", repacked}, {"pid", pid}, {"stopped", true},
                           {"tokens", value.at("tokens")}});
    }
    if (const auto* path = std::getenv("LOCALCODE_REPACK_TEST_PROOF")) {
        std::ofstream file(path_from_utf8(path)); REQUIRE(file.good()); file << results.dump(2) << '\n';
    }
}

TEST_CASE("model launch arguments retain Unicode paths", "[backend][unicode]") {
    ServerConfig config;
    config.profile.model_path = std::filesystem::path(u8"L:/modèles/日本語/été.gguf");
    const auto args = ModelServer::build_arguments(config, {}, 8080);
    const auto model = std::find(args.begin(), args.end(), "--model");
    REQUIRE(model != args.end());
    REQUIRE(std::next(model) != args.end());
    CHECK(*std::next(model) == path_to_utf8(config.profile.model_path));
    CHECK(path_from_utf8(*std::next(model)) == config.profile.model_path);
}

TEST_CASE("CPU profiles select no GPU device when the runtime advertises that option",
          "[backend][flags][cpu]") {
    // Zero GPU layers alone still permits CUDA operation offload in llama.cpp
    // b10655. Exercise the help probe and launch arguments together: accepting
    // a CPU profile must select no device, while GPU and legacy builds retain
    // their existing arguments.
    const auto features = ModelServer::probe_binary(LOCALCODE_TEST_SERVER_FIXTURE);
    REQUIRE(features);
    ServerConfig config;
    config.profile.gpu_layers = 0;
    const auto cpu = ModelServer::build_arguments(config, *features, 8080);
    const auto device = std::find(cpu.begin(), cpu.end(), "--device");
    REQUIRE(device != cpu.end());
    REQUIRE(std::next(device) != cpu.end());
    CHECK(*std::next(device) == "none");

    for (const int layers : {-1, 1, 999}) {
        config.profile.gpu_layers = layers;
        const auto gpu = ModelServer::build_arguments(config, *features, 8080);
        CHECK(std::find(gpu.begin(), gpu.end(), "--device") == gpu.end());
    }
    config.profile.gpu_layers = 0;
    const auto legacy = ModelServer::build_arguments(config, {}, 8080);
    CHECK(std::find(legacy.begin(), legacy.end(), "--device") == legacy.end());
    const auto layers = std::find(legacy.begin(), legacy.end(), "--n-gpu-layers");
    REQUIRE(layers != legacy.end());
    REQUIRE(std::next(layers) != legacy.end());
    CHECK(*std::next(layers) == "0");
}

TEST_CASE("a real runtime keeps a CPU profile on CPU and serves a literal reply",
          "[backend][cpu][.runtime]") {
    const auto* binary = std::getenv("LOCALCODE_CPU_TEST_RUNTIME");
    const auto* model = std::getenv("LOCALCODE_CPU_TEST_MODEL");
    if (!binary || !model) SKIP("Set the explicit CPU runtime/model test paths.");

    ServerConfig config;
    config.binary = path_from_utf8(binary);
    config.profile.id = "cpu-device-smoke";
    config.profile.model_path = path_from_utf8(model);
    config.profile.gpu_layers = 0;
    config.profile.context_size = 4096;
    config.profile.threads = 8;
    config.profile.threads_batch = 2;
    config.profile.ubatch_size = 128;
    config.profile.reasoning_enabled = false;
    if (const auto* no_mmap = std::getenv("LOCALCODE_CPU_TEST_NO_MMAP")) {
        config.profile.mmap = std::string_view(no_mmap) != "1";
    }
    config.extra_arguments = {"--log-verbosity", "4", "--no-warmup"};
    config.startup_timeout = std::chrono::seconds(120);
    config.auto_restart = false;
    const auto http = HttpClient::create();
    ModelServer server;
    const auto started =
        server.start(config, [http](const std::string& url, CancellationToken cancel) -> Status {
            BackendEndpoint endpoint;
            endpoint.base_url = url;
            return OpenAiCompatBackend{endpoint, http}.health(cancel);
        });
    REQUIRE(started);
    const auto logs = strings::join(server.recent_log(2000), "\n");
    CHECK(logs.find("offloaded 0/") != std::string::npos);
    CHECK(std::regex_search(logs, std::regex{R"(CPU\s+compute buffer)"}));
    CHECK(std::regex_search(logs, std::regex{R"(n_threads_batch\s*=\s*2\b)"}));
    CHECK(std::regex_search(logs, std::regex{R"(n_ubatch\s*=\s*128\b)"}));
    CHECK_FALSE(std::regex_search(logs, std::regex{R"(CUDA\d+\s+(compute|KV|model) buffer)"}));
    if (!config.profile.mmap) {
        CHECK_FALSE(std::regex_search(logs, std::regex{R"(CPU_Mapped\s+model buffer)"}));
    }

    BackendEndpoint endpoint;
    endpoint.base_url = server.base_url();
    OpenAiCompatBackend backend{endpoint, http};
    ChatRequest request;
    request.messages = {ChatMessage::user("Réponds uniquement avec le mot banane.")};
    request.sampling.temperature = 0;
    request.sampling.max_tokens = 16;
    request.enable_reasoning = false;
    request.thinking_override = false;
    request.timeout = std::chrono::seconds(30);
    const auto answer = backend.chat(request, {}, {});
    REQUIRE(answer);
    INFO("finish=" << answer->finish_reason << ", tokens=" << answer->usage.completion_tokens
                   << ", reasoning characters=" << answer->message.reasoning_content.size());
    CHECK(strings::trim(answer->message.content) == "banane");
    server.stop();
    CHECK(server.status().state == ServerState::Stopped);
    CHECK(server.status().process_id == 0);
}

TEST_CASE("loading settings use modern modes with legacy flag fallback", "[backend][flags]") {
    ServerConfig config;
    ServerBinaryFeatures modern;
    modern.load_mode = true;
    modern.mlock = true;
    modern.no_mmap = true;
    for (const auto& [mapped, locked, mode] :
         std::vector<std::tuple<bool, bool, std::string>>{{true, false, ""},
                                                          {false, false, "none"},
                                                          {false, true, "mlock"},
                                                          {true, true, "mmap+mlock"}}) {
        config.profile.mmap = mapped;
        config.profile.mlock = locked;
        const auto args = ModelServer::build_arguments(config, modern, 8080);
        CHECK(std::find(args.begin(), args.end(), "--no-mmap") == args.end());
        CHECK(std::find(args.begin(), args.end(), "--mlock") == args.end());
        const auto flag = std::find(args.begin(), args.end(), "--load-mode");
        if (mode.empty()) {
            CHECK(flag == args.end());
        } else {
            REQUIRE(flag != args.end());
            REQUIRE(std::next(flag) != args.end());
            CHECK(*std::next(flag) == mode);
        }
    }
    modern.load_mode = false;
    config.profile.mmap = false;
    config.profile.mlock = true;
    const auto legacy = ModelServer::build_arguments(config, modern, 8080);
    CHECK(std::find(legacy.begin(), legacy.end(), "--no-mmap") != legacy.end());
    CHECK(std::find(legacy.begin(), legacy.end(), "--mlock") != legacy.end());
    CHECK(std::find(legacy.begin(), legacy.end(), "--load-mode") == legacy.end());
}

TEST_CASE("unsupported explicit loading settings fail before starting inference",
          "[backend][flags]") {
    for (const bool mapped : {false, true}) {
        ServerConfig config;
        config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
        config.profile.mmap = mapped;
        config.profile.mlock = mapped;
        config.startup_timeout = std::chrono::seconds(5);
        config.auto_restart = false;
        ModelServer server;
        bool health_called = false;
        const auto started = server.start(config, [&](const std::string&, CancellationToken) {
            health_called = true;
            return ok_status();
        });
        REQUIRE_FALSE(started);
        CHECK_FALSE(health_called);
        CHECK(server.status().process_id == 0);
        CHECK(started.error().to_string().find(mapped ? "--mlock" : "--no-mmap") !=
              std::string::npos);
    }
}

TEST_CASE("prompt threads are independent, opt-in and capability checked", "[backend][flags]") {
    ServerConfig config;
    config.profile.threads = 4;
    ServerBinaryFeatures features;
    features.threads = true;
    features.threads_batch = true;
    auto args = ModelServer::build_arguments(config, features, 8080);
    CHECK(std::find(args.begin(), args.end(), "--threads-batch") == args.end());
    config.profile.threads_batch = 8;
    args = ModelServer::build_arguments(config, features, 8080);
    CHECK(strings::join(args, " ").find("--threads 4") != std::string::npos);
    CHECK(strings::join(args, " ").find("--threads-batch 8") != std::string::npos);

    config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
    config.profile.threads = 0;
    config.auto_restart = false;
    config.startup_timeout = std::chrono::seconds(5);
    ModelServer server;
    bool health_called = false;
    const auto started = server.start(config, [&](const std::string&, CancellationToken) {
        health_called = true;
        return ok_status();
    });
    REQUIRE_FALSE(started);
    CHECK_FALSE(health_called);
    CHECK(server.status().process_id == 0);
    CHECK(started.error().to_string().find("--threads-batch") != std::string::npos);
}

TEST_CASE("tool-call reasoning is preserved in the serialized request", "[backend][reasoning]") {
    struct PayloadBackend : OpenAiCompatBackend {
        PayloadBackend() : OpenAiCompatBackend({}, nullptr) {}

        using OpenAiCompatBackend::build_payload;
    } backend;

    ChatRequest request;
    request.messages.push_back(ChatMessage::user("inspect this file"));
    auto assistant = ChatMessage::assistant("");
    assistant.reasoning_content = "The definition is found; inspect its callers next.";
    assistant.tool_calls = {{"call1", "read_file", "{\"path\":\"one.cpp\"}"}};
    request.messages.push_back(assistant);
    request.messages.push_back(ChatMessage::tool_result("call1", "read_file", "actual source"));
    const auto payload = backend.build_payload(request);
    REQUIRE(payload["messages"][1].contains("reasoning_content"));
    CHECK(payload["messages"][1]["reasoning_content"] == assistant.reasoning_content);
    CHECK(payload["messages"][1]["content"] == "");
    CHECK_FALSE(payload["messages"][2].contains("reasoning_content"));
}

TEST_CASE("native reasoning budget follows explicit off only on capable endpoints", "[backend][reasoning]") {
    struct PayloadBackend : OpenAiCompatBackend {
        using OpenAiCompatBackend::OpenAiCompatBackend;
        using OpenAiCompatBackend::build_payload;
    };
    for (const bool capable : {false, true}) {
        BackendEndpoint endpoint; endpoint.native_reasoning_budget = capable;
        PayloadBackend backend{endpoint, nullptr};
        for (const auto override : {std::optional<bool>{}, std::optional<bool>{false},
                                   std::optional<bool>{true}}) {
            ChatRequest request; request.thinking_override = override;
            request.messages = {ChatMessage::user("banane")};
            for (const bool stream : {false, true}) {
                request.stream = stream;
                const auto payload = backend.build_payload(request);
                CHECK(payload.contains("reasoning_budget_tokens") == (capable && override == false));
                if (payload.contains("reasoning_budget_tokens")) CHECK(payload["reasoning_budget_tokens"] == 0);
                if (override.has_value()) CHECK(payload["chat_template_kwargs"]["enable_thinking"] == *override);
                CHECK(payload["stream"] == stream);
            }
        }
    }
}

TEST_CASE("flag arity is read from the help text", "[backend][flags]") {
    SECTION("llama.cpp uses square brackets") {
        const std::string help =
            "-fa,   --flash-attn [on|off|auto]       set Flash Attention use ('on', 'off', or "
            "'auto', default: 'auto')\n"
            "       --jinja                          use jinja template for chat\n";
        CHECK(ModelServer::help_flag_takes_value(help, "--flash-attn"));
        CHECK_FALSE(ModelServer::help_flag_takes_value(help, "--jinja"));
    }

    SECTION("ik_llama.cpp uses parentheses") {
        const std::string help =
            "-no-fa, --no-flash-attn         disable Flash Attention (default: enabled)\n"
            "-fa, --flash-attn (auto|on|off|0|1)\n"
            "                                set Flash Attention (default: on)\n";
        CHECK(ModelServer::help_flag_takes_value(help, "--flash-attn"));
    }

    SECTION("a metavariable counts as a value") {
        const std::string help =
            "  -c,    --ctx-size N              size of the prompt context\n"
            "         --reasoning-format FORMAT controls whether thought tags are extracted\n"
            "  -sps,  --slot-prompt-similarity SIMILARITY\n";
        CHECK(ModelServer::help_flag_takes_value(help, "--ctx-size"));
        CHECK(ModelServer::help_flag_takes_value(help, "--reasoning-format"));
        CHECK(ModelServer::help_flag_takes_value(help, "--slot-prompt-similarity"));
    }

    SECTION("a description is not mistaken for a value") {
        const std::string help =
            "       --mlock                   Force system to keep model in RAM\n"
            "       --no-webui                Disable the Web UI\n"
            "       --log-disable             Disable trace logs\n";
        CHECK_FALSE(ModelServer::help_flag_takes_value(help, "--mlock"));
        CHECK_FALSE(ModelServer::help_flag_takes_value(help, "--no-webui"));
        CHECK_FALSE(ModelServer::help_flag_takes_value(help, "--log-disable"));
    }

    SECTION("an absent flag takes nothing") {
        CHECK_FALSE(ModelServer::help_flag_takes_value("nothing here", "--flash-attn"));
    }
}

TEST_CASE("prompt microbatch settings use supported flags and reject unsupported choices",
          "[backend][flags]") {
    ServerConfig config;
    ServerBinaryFeatures features;
    features.ubatch_size = true;
    auto args = ModelServer::build_arguments(config, features, 8080);
    CHECK(std::find(args.begin(), args.end(), "--ubatch-size") == args.end());
    config.profile.ubatch_size = 128;
    args = ModelServer::build_arguments(config, features, 8080);
    const auto flag = std::find(args.begin(), args.end(), "--ubatch-size");
    REQUIRE(flag != args.end());
    REQUIRE(std::next(flag) != args.end());
    CHECK(*std::next(flag) == "128");

    config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
    config.auto_restart = false;
    config.startup_timeout = std::chrono::seconds(5);
    ModelServer server;
    bool health_called = false;
    const auto started = server.start(config, [&](const std::string&, CancellationToken) {
        health_called = true;
        return Status{};
    });
    REQUIRE_FALSE(started);
    CHECK_FALSE(health_called);
    CHECK(started.error().to_string().find("--ubatch-size") != std::string::npos);
    CHECK(server.status().process_id == 0);
}

TEST_CASE("prompt microbatch cannot silently exceed an explicit prompt batch", "[backend][flags]") {
    ServerConfig config;
    config.binary = LOCALCODE_TEST_SERVER_FIXTURE;
    config.profile.batch_size = 64;
    config.profile.ubatch_size = 128;
    config.auto_restart = false;
    config.startup_timeout = std::chrono::seconds(5);
    ModelServer server;
    bool health_called = false;
    const auto started = server.start(config, [&](const std::string&, CancellationToken) {
        health_called = true;
        return Status{};
    });
    REQUIRE_FALSE(started);
    CHECK_FALSE(health_called);
    CHECK(started.error().to_string().find("microbatch") != std::string::npos);
    CHECK(server.status().process_id == 0);
}

TEST_CASE("server arguments only use flags the binary advertises", "[backend][flags]") {
    ServerConfig config;
    config.profile.model_path = "L:/models/test.gguf";
    config.profile.id = "test";
    config.profile.context_size = 16384;
    config.profile.gpu_layers = -1;
    config.profile.flash_attention = true;
    config.profile.cache_type_k = "q8_0";
    config.profile.capabilities.supports_native_reasoning = true;
    config.profile.reasoning_enabled = true;

    SECTION("a build that advertises nothing gets only the universal flags") {
        ServerBinaryFeatures bare;
        const auto args = ModelServer::build_arguments(config, bare, 8080);
        const std::string joined = strings::join(args, " ");

        CHECK(joined.find("--model") != std::string::npos);
        CHECK(joined.find("--ctx-size 16384") != std::string::npos);
        CHECK(joined.find("--port 8080") != std::string::npos);
        // None of the optional ones may appear.
        CHECK(joined.find("--jinja") == std::string::npos);
        CHECK(joined.find("--flash-attn") == std::string::npos);
        CHECK(joined.find("--reasoning-format") == std::string::npos);
        CHECK(joined.find("--cache-type-k") == std::string::npos);
    }

    SECTION("a value-taking flash-attn is given a value") {
        ServerBinaryFeatures features;
        features.flash_attention = true;
        features.flash_attention_takes_value = true;
        const auto args = ModelServer::build_arguments(config, features, 8080);

        auto it = std::find(args.begin(), args.end(), "--flash-attn");
        REQUIRE(it != args.end());
        REQUIRE(std::next(it) != args.end());
        CHECK(*std::next(it) == "on");
    }

    SECTION("zero batch uses the runtime default instead of an invalid zero flag") {
        config.profile.batch_size = 0;
        ServerBinaryFeatures features;
        features.batch_size = true;
        CHECK(strings::join(ModelServer::build_arguments(config, features, 8080), " ")
                  .find("--batch-size") == std::string::npos);
    }

    SECTION("a valueless flash-attn is given none") {
        ServerBinaryFeatures features;
        features.flash_attention = true;
        features.flash_attention_takes_value = false;
        const auto args = ModelServer::build_arguments(config, features, 8080);

        auto it = std::find(args.begin(), args.end(), "--flash-attn");
        REQUIRE(it != args.end());
        // Either it is last, or the next token is another flag.
        if (std::next(it) != args.end()) {
            CHECK(std::next(it)->rfind("--", 0) == 0);
        }
    }

    SECTION("disabling flash attention is explicit for a value-taking runtime") {
        config.profile.flash_attention = false;
        ServerBinaryFeatures features;
        features.flash_attention = true;
        features.flash_attention_takes_value = true;
        CHECK(strings::join(ModelServer::build_arguments(config, features, 8080), " ")
                  .find("--flash-attn off") != std::string::npos);
    }

    SECTION("reasoning is not requested from a model that has none") {
        ServerBinaryFeatures features;
        features.reasoning_format = true;

        auto without = config;
        without.profile.capabilities.supports_native_reasoning = false;
        const auto args = ModelServer::build_arguments(without, features, 8080);
        CHECK(strings::join(args, " ").find("--reasoning-format") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Reading a start-up failure out of the server's own output.
//
// The text below is verbatim from this machine on the day a run failed: another
// process held the GPU, the 8B model would not fit, and LocalCode reported
// "server exited with code -1" followed by twelve lines of allocation dots. The
// reason was in the log the whole time, eight lines further up.
// ---------------------------------------------------------------------------

TEST_CASE("insufficient device memory is explained, not swallowed", "[backend][server]") {
    const std::vector<std::string> output{
        "  Layer 36:    486.86,    224.75,    711.61 MiB (output layer)",
        "  --------------------------------------------------------------------------",
        "  Total   :   4679.47,   1376.75,   6056.22 MiB",
        "  Memory required for model tensors + cache: 6543 MiB",
        "  Memory available on all devices - compute: 5440 MiB",
        "llm_load_tensors: ggml ctx size =    0.40 MiB",
        "llm_load_tensors: offloading 36 repeating layers to GPU",
        "llm_load_tensors:      CUDA0 buffer size =  5166.34 MiB",
        "..................................................................",
    };

    const auto advice = ModelServer::explain_startup_failure(output);
    REQUIRE_FALSE(advice.empty());
    CHECK(strings::icontains(advice, "do not fit"));
    CHECK(strings::icontains(advice, "context size"));
}

TEST_CASE("a rejected argument is explained", "[backend][server]") {
    const std::vector<std::string> output{
        "main: loading model",
        "error while handling argument \"--flash-attn\": expected value",
        "usage: llama-server [options]",
    };

    const auto advice = ModelServer::explain_startup_failure(output);
    REQUIRE_FALSE(advice.empty());
    CHECK(strings::icontains(advice, "rejected an argument"));
}

TEST_CASE("an unrecognised failure gets no invented explanation", "[backend][server]") {
    // Guessing would be worse than the raw tail the caller falls back to.
    const std::vector<std::string> output{
        "main: server listening",
        "something went sideways in a way nobody anticipated",
        "goodbye",
    };

    CHECK(ModelServer::explain_startup_failure(output).empty());
}

// ---------------------------------------------------------------------------
// Finding runtimes in folders named the way people actually name them.
//
// Both projects install a binary called llama-server.exe, and the old discovery
// only looked inside folders named exactly "llama.cpp", "llama", "ik_llama.cpp"
// and a couple of variants. Nobody names them that. This machine had two working
// runtimes in a folder called runtimes/ - ik-b5197-cuda13.3-alderlake beside
// llama-b10655-cuda13.3 - and LocalCode reported that no runtime was found.
// ---------------------------------------------------------------------------

TEST_CASE("runtimes are found in version-stamped folders", "[backend][discovery]") {
    TempDirectory temp;
    const auto root = temp.path() / "runtimes";

    auto make_server = [&](const std::string& folder) {
        const auto dir = root / folder;
        std::filesystem::create_directories(dir);
        std::ofstream out(dir / ("llama-server" + std::string{kExeSuffixForTest}));
        out << "not a real binary";
    };

    make_server("ik-b5197-cuda13.3-alderlake");
    make_server("llama-b10655-cuda13.3");

    const auto found = BackendManager::scan_runtime_root(root);
    REQUIRE(found.size() == 2);

    int ik = 0;
    int mainline = 0;
    for (const auto& [path, is_ik] : found) {
        INFO(path.string());
        if (is_ik)
            ++ik;
        else
            ++mainline;
    }
    CHECK(ik == 1);
    CHECK(mainline == 1);
}

TEST_CASE("a folder that merely starts with the letters ik is not an ik build",
          "[backend][discovery]") {
    // "Nikola" contains ik; "ikigai" starts with it but is a word, not the
    // flavour tag. Only a whole leading token counts.
    TempDirectory temp;
    const auto root = temp.path() / "runtimes";

    const auto dir = root / "Nikola" / "llama-b10655";
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / ("llama-server" + std::string{kExeSuffixForTest}));
    out << "not a real binary";
    out.close();

    const auto found = BackendManager::scan_runtime_root(root);
    REQUIRE(found.size() == 1);
    CHECK_FALSE(found.front().second);
}

TEST_CASE("the scan stops at the depth limit", "[backend][discovery]") {
    TempDirectory temp;
    const auto root = temp.path() / "runtimes";

    const auto deep = root / "a" / "b" / "c" / "d" / "e";
    std::filesystem::create_directories(deep);
    std::ofstream out(deep / ("llama-server" + std::string{kExeSuffixForTest}));
    out << "not a real binary";
    out.close();

    CHECK(BackendManager::scan_runtime_root(root, 2).empty());
    CHECK_FALSE(BackendManager::scan_runtime_root(root, 8).empty());
}

TEST_CASE("the server's own explanation survives a failed chat", "[backend][errors]") {
    // llama.cpp answered a runaway tool call with a complete diagnosis:
    // {"error":{"code":500,"message":"Failed to parse tool call arguments as
    // JSON: ... missing closing quote"}}. LocalCode reported "chat completion
    // failed with HTTP 500" and dropped the rest - the one place the reason
    // existed.
    const std::string body =
        R"({"error":{"code":500,"message":"Failed to parse tool call arguments as JSON: )"
        R"([json.exception.parse_error.101] parse error at line 1, column 33214: syntax )"
        R"(error while parsing value - invalid string: missing closing quote"}})";

    const auto error = OpenAiCompatBackend::chat_http_error(500, body);
    INFO(error.to_string());

    // Named as the model's doing, because it is, and because that is what makes
    // it recoverable rather than a dead server.
    CHECK(error.message.find("could not parse") != std::string::npos);
    CHECK(error.detail.find("missing closing quote") != std::string::npos);
}

TEST_CASE("an ordinary server error still says what the server said", "[backend][errors]") {
    const auto error = OpenAiCompatBackend::chat_http_error(
        503, R"({"error":{"message":"the model is still loading"}})");
    CHECK(error.message.find("HTTP 503") != std::string::npos);
    CHECK(error.detail.find("still loading") != std::string::npos);
}

TEST_CASE("malformed stored tool calls are omitted on replay without changing saved messages",
          "[backend][recovery][tool-output-limit]") {
    using namespace localcode;
    struct InspectBackend : OpenAiCompatBackend {
        using OpenAiCompatBackend::OpenAiCompatBackend;
        using OpenAiCompatBackend::build_payload;
    } backend{{}, nullptr};
    ChatRequest request;
    auto previous = ChatMessage::assistant("");
    previous.tool_calls = {{"valid", "read_file", R"({"path":"README.md"})"},
                          {"broken", "list_directory", "{\"path\":\"" + std::string(20376, 'x')}};
    request.messages = {ChatMessage::user("Make a Python chess game."), previous,
        ChatMessage::tool_result("valid", "read_file", "Existing verified source evidence"),
        ChatMessage::tool_result("broken", "list_directory", "ERROR: malformed arguments"),
        ChatMessage::user("Continue.")};
    const auto payload = backend.build_payload(request);
    REQUIRE(payload["messages"].size() == 4);
    if (const auto* capture = std::getenv("LOCALCODE_TEST_REPLAY_PAYLOAD")) {
        const auto path = path_from_utf8(capture);
        REQUIRE_FALSE(std::filesystem::exists(path));
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.good());
        output << payload.dump(2);
    }
    CHECK(payload["messages"][1]["tool_calls"].size() == 1);
    CHECK(payload["messages"][1]["tool_calls"][0]["id"] == "valid");
    CHECK(payload["messages"][1]["content"].get<std::string>().find("omitted malformed") != std::string::npos);
    CHECK(payload["messages"][2]["content"] == "Existing verified source evidence");
    CHECK(request.messages[1].tool_calls.size() == 2);
    CHECK_FALSE(request.messages[1].tool_calls[1].parsed_arguments());
    request.parallel_tool_calls = false;
    request.tools = {{"read_file", "Read a file", Json{{"type", "object"}}}};
    CHECK(backend.build_payload(request)["parallel_tool_calls"] == false);
}

TEST_CASE("SSE tool-parser errors receive the same recoverable classification as HTTP errors",
          "[backend][recovery][tool-output-limit]") {
    using namespace localcode;
    struct ErrorClient : TokenizerClient {
        Result<HttpResponse> stream(const HttpRequest&, const HttpStreamCallback& callback,
                                    CancellationToken) override {
            callback("data: {\"error\":{\"message\":\"Failed to parse tool call arguments as JSON: missing closing quote\"}}\n\n");
            HttpResponse response;
            response.status_code = 200;
            return response;
        }
    };
    LlamaCppBackend backend{{}, std::make_shared<ErrorClient>()};
    ChatRequest request;
    request.messages = {ChatMessage::user("Make a Python chess game.")};
    const auto result = backend.chat(request, {}, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::InvalidToolCall);
    const auto http = OpenAiCompatBackend::chat_http_error(500,
        R"({"error":{"message":"Failed to parse tool call arguments as JSON: missing closing quote"}})");
    CHECK(http.code == ErrorCode::InvalidToolCall);
}

TEST_CASE("replay repair preserves valid results when another turn reused the call ID",
          "[backend][recovery][tool-output-limit]") {
    using namespace localcode;
    struct InspectBackend : OpenAiCompatBackend {
        using OpenAiCompatBackend::OpenAiCompatBackend;
        using OpenAiCompatBackend::build_payload;
    } backend{{}, nullptr};
    auto valid = ChatMessage::assistant("");
    valid.tool_calls = {{"call_0", "read_file", R"({"path":"README.md"})"}};
    auto invalid = ChatMessage::assistant("");
    invalid.tool_calls = {{"call_0", "write_file", "{\"path\":\"unfinished"}};
    ChatRequest request;
    request.messages = {ChatMessage::user("Inspect the file."), valid,
        ChatMessage::tool_result("call_0", "read_file", "EARLIER_SOURCE_EVIDENCE"), invalid,
        ChatMessage::tool_result("call_0", "write_file", "ERROR: rejected"), valid,
        ChatMessage::tool_result("call_0", "read_file", "LATER_SOURCE_EVIDENCE")};
    const auto payload = backend.build_payload(request);
    const auto wire = payload.dump();
    CHECK(wire.find("EARLIER_SOURCE_EVIDENCE") != std::string::npos);
    CHECK(wire.find("LATER_SOURCE_EVIDENCE") != std::string::npos);
    CHECK(wire.find("ERROR: rejected") == std::string::npos);
    CHECK(payload["messages"].size() == 6);
}

TEST_CASE("a body that is not JSON is passed through rather than lost", "[backend][errors]") {
    const auto error = OpenAiCompatBackend::chat_http_error(502, "upstream connect error");
    CHECK(error.message.find("HTTP 502") != std::string::npos);
    CHECK(error.detail.find("upstream connect error") != std::string::npos);
}

TEST_CASE("adaptive ngrams are refused after answer integrity failures even on a capable runtime",
          "[backend][flags][ngram]") {
    TempDirectory temp;
    const auto binary = temp.path() / (std::string("ngram-fixture") + kExeSuffixForTest);
    std::filesystem::copy_file(LOCALCODE_TEST_SERVER_FIXTURE, binary);
    const auto capable = ModelServer::probe_binary(binary);
    const auto legacy = ModelServer::probe_binary(LOCALCODE_TEST_SERVER_FIXTURE);
    REQUIRE(capable);
    REQUIRE(legacy);
    CHECK(capable->ngram_mod);
    CHECK_FALSE(legacy->ngram_mod);
    ServerConfig config;
    auto args = ModelServer::build_arguments(config, *capable, 8080);
    CHECK(std::find(args.begin(), args.end(), "--spec-type") == args.end());
    config.profile.ngram_speculation = true;
    args = ModelServer::build_arguments(config, *capable, 8080);
    CHECK(std::find(args.begin(), args.end(), "--spec-type") == args.end());
    CHECK(std::find(args.begin(), args.end(), "--model-draft") == args.end());
    config.auto_restart = false;
    config.startup_timeout = std::chrono::seconds(5);
    ModelServer server;
    bool health_called = false;
    for (const auto& runtime : {binary, std::filesystem::path(LOCALCODE_TEST_SERVER_FIXTURE),
                                temp.path() / "missing-runtime"}) {
        config.binary = runtime;
        const auto started = server.start(config, [&](const std::string&, CancellationToken) {
            health_called = true;
            return Status{};
        });
        REQUIRE_FALSE(started);
        CHECK_FALSE(health_called);
        CHECK(server.status().process_id == 0);
        CHECK(started.error().to_string().find("answer-integrity") != std::string::npos);
        CHECK(started.error().to_string().find("Disabled") != std::string::npos);
    }
}

TEST_CASE("runtime capability cache follows shared library changes and missing binaries",
          "[backend][flags][probe-cache]") {
    TempDirectory temp;
    const auto binary = temp.path() / (std::string("ngram-fixture") + kExeSuffixForTest);
    const auto library = temp.path() / "fixture-capabilities.dll";
    std::filesystem::copy_file(LOCALCODE_TEST_SERVER_FIXTURE, binary);
    std::ofstream(library) << "enabled";
    const auto first = ModelServer::probe_binary(binary);
    REQUIRE(first);
    CHECK(first->ngram_mod);
    REQUIRE(ModelServer::probe_binary(binary));
    auto count = [&] {
        std::ifstream stream(temp.path() / "help-count.txt");
        return std::count(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>(),
                          '\n');
    };
    CHECK(count() == 1);   // An unchanged runtime still benefits from caching.
    const auto stamp = std::filesystem::last_write_time(library);
    std::ofstream(library) << "disabled";
    std::filesystem::last_write_time(library, stamp + std::chrono::seconds(2));
    const auto updated = ModelServer::probe_binary(binary);
    REQUIRE(updated);
    CHECK_FALSE(updated->ngram_mod);
    CHECK(count() == 2);
    std::filesystem::remove(binary);   // Exactly the fixture copy owned by this test.
    const auto missing = ModelServer::probe_binary(binary);
    CHECK_FALSE(missing);
    if (!missing) CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("failed or truncated runtime help cannot establish capabilities",
          "[backend][flags][probe-integrity]") {
    for (const auto* name : {"failed-help-fixture", "oversized-help-fixture"}) {
        TempDirectory temp;
        const auto binary = temp.path() / (std::string(name) + kExeSuffixForTest);
        std::filesystem::copy_file(LOCALCODE_TEST_SERVER_FIXTURE, binary);
        const auto result = ModelServer::probe_binary(binary);
        CHECK_FALSE(result);
        // A failed probe must not populate the capability cache either.
        CHECK_FALSE(ModelServer::probe_binary(binary));
    }
}

TEST_CASE("runtime capability probing requires exact option declarations",
          "[backend][flags][probe-integrity][option-boundaries]") {
    TempDirectory temp;
    for (const auto* name : {"prefix-only-help-fixture", "exact-options-help-fixture"}) {
        const auto binary = temp.path() / (std::string(name) + kExeSuffixForTest);
        std::filesystem::copy_file(LOCALCODE_TEST_SERVER_FIXTURE, binary);
        const auto features = ModelServer::probe_binary(binary);
        REQUIRE(features);
        const bool exact = std::string_view(name).starts_with("exact-");
        INFO(name);
        CHECK(features->threads_batch);
        CHECK(features->threads == exact);
        CHECK(features->cpu_placement == exact);
        CHECK(features->device == exact);
        CHECK(features->parallel == exact);
        CHECK(features->flash_attention == exact);
        CHECK(features->flash_attention_takes_value == exact);
        CHECK(features->cache_ram == exact);
        CHECK(features->mlock == exact);
        CHECK_FALSE(features->ngram_mod);
    }
}

TEST_CASE("runtime flag arity ignores prefix options and prose",
          "[backend][flags][option-boundaries]") {
    const std::string help =
        "  --flash-attn-mode MODE  unrelated option\n"
        "  --other N  use --flash-attn [on|off] elsewhere\n"
        "  --flash-attn  enable attention\n"
        "  --cache-ram-extra N  unrelated option\n"
        "Example: --reasoning-budget N\n"
        "  -cr, --cache-ram=MiB  maximum cache memory\n";
    CHECK_FALSE(ModelServer::help_flag_takes_value(help, "--flash-attn"));
    CHECK_FALSE(ModelServer::help_flag_takes_value(help, "--reasoning-budget"));
    CHECK(ModelServer::help_flag_takes_value(help, "--cache-ram"));
    CHECK(ModelServer::help_flag_takes_value(
        "  --flash-attn-mode, --flash-attn [on|off]  attention\n", "--flash-attn"));
    CHECK_FALSE(ModelServer::help_flag_takes_value(
        "  --other --flash-attn [on|off] in another runtime\n", "--flash-attn"));
}

TEST_CASE("previously saved adaptive ngram profile is refused with an actual runtime",
          "[backend][ngram][integration]") {
    const auto* runtime = std::getenv("LOCALCODE_TEST_NGRAM_RUNTIME");
    const auto* model = std::getenv("LOCALCODE_TEST_NGRAM_MODEL");
    if (!runtime || !model)
        SKIP("Set LOCALCODE_TEST_NGRAM_RUNTIME/MODEL for an owned CPU runtime test");
    ServerConfig config;
    config.binary = path_from_utf8(runtime);
    config.profile.id = "ngram-integration";
    config.profile.display_name = "Owned ngram integration model";
    config.profile.model_path = path_from_utf8(model);
    config.profile.gpu_layers = 0;
    config.profile.context_size = 4096;
    config.profile.threads = 4;
    config.profile.threads_batch = 4;
    config.profile.batch_size = 256;
    config.profile.ubatch_size = 128;
    config.profile.ngram_speculation = true;
    config.auto_restart = false;
    config.startup_timeout = std::chrono::seconds(120);
    ModelServer server;
    bool health_called = false;
    const auto started = server.start(config, [&](const std::string&, CancellationToken) -> Status {
        health_called = true;
        return {};
    });
    REQUIRE_FALSE(started);
    CHECK_FALSE(health_called);
    CHECK(started.error().to_string().find("answer-integrity") != std::string::npos);
    CHECK(server.status().process_id == 0);
}
