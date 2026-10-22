#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "Application.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "support/TempDirectory.h"
#include "workspace/Workspace.h"

using namespace localcode;

namespace {
class HealthClient : public HttpClient {
public:
    bool fail = false;

    Result<HttpResponse> send(const HttpRequest& request, CancellationToken) override {
        if (fail) return make_error(ErrorCode::NetworkError, "simulated backend failure");
        HttpResponse response;
        response.status_code = 200;
        response.body = request.url.ends_with("/props")
                            ? R"({"model_path":"fixture.gguf","n_ctx":32768})"
                            : "{}";
        return response;
    }

    Result<HttpResponse> stream(const HttpRequest&, const HttpStreamCallback&,
                                CancellationToken) override {
        return make_error(ErrorCode::Internal, "stale backend was used");
    }

    Status download(const HttpRequest&, const std::filesystem::path&,
                    const std::function<void(std::uint64_t, std::uint64_t)>&,
                    CancellationToken) override {
        return make_error(ErrorCode::NotSupported, "not used");
    }

    std::string backend_name() const override { return "test health client"; }
};

struct IsolatedPaths {
    test::TempDirectory directory;
    std::filesystem::path data = app_paths().data_dir();
    std::filesystem::path cache = app_paths().cache_dir();
    std::filesystem::path logs = app_paths().log_dir();
    std::filesystem::path runtimes = app_paths().runtime_dir();

    IsolatedPaths() { app_paths().set_root(directory.path()); }

    ~IsolatedPaths() {
        app_paths().set_data_dir(data);
        app_paths().set_cache_dir(cache);
        app_paths().set_log_dir(logs);
        app_paths().set_runtime_dir(runtimes);
    }
};

struct ScopedVariable {
    std::string name;
    std::optional<std::string> previous;

    ScopedVariable(std::string key, const std::string& value) : name(std::move(key)) {
        if (const auto* old = std::getenv(name.c_str())) previous = old;
        set(value);
    }

    void set(const std::string& value) const {
#if defined(_WIN32)
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    ~ScopedVariable() {
        if (previous)
            set(*previous);
        else {
#if defined(_WIN32)
            _putenv_s(name.c_str(), "");
#else
            unsetenv(name.c_str());
#endif
        }
    }
};

std::string preflight_gguf() {
    std::string bytes;
    auto scalar = [&]<typename T>(T value) {
        bytes.append(reinterpret_cast<const char*>(&value), sizeof value);
    };
    auto text = [&](std::string_view value) {
        scalar(std::uint64_t(value.size()));
        bytes.append(value);
    };
    scalar(std::uint32_t(0x46554747));
    scalar(std::uint32_t(3));
    scalar(std::uint64_t(0));
    scalar(std::uint64_t(2));
    text("general.architecture");
    scalar(std::uint32_t(8));
    text("llama");
    text("llama.block_count");
    scalar(std::uint32_t(4));
    scalar(std::uint32_t(32));
    return bytes;
}

struct PreflightFixture {
    IsolatedPaths paths;
    std::filesystem::path marker = paths.directory.path() / "gpu-query.txt";
    ScopedVariable search{"PATH", path_to_utf8(paths.directory.path())};
    ScopedVariable markerVariable{"LOCALCODE_GPU_PROBE_MARKER", path_to_utf8(marker)};
    ScopedVariable delay{"LOCALCODE_GPU_FIXTURE_DELAY_MS", "200"};
    std::vector<std::string> advice;
    BackendManager manager;
    ModelProfile profile;

    PreflightFixture() {
#if defined(_WIN32)
        const auto gpu = paths.directory.path() / "nvidia-smi.exe";
#else
        const auto gpu = paths.directory.path() / "nvidia-smi";
#endif
        std::filesystem::copy_file(LOCALCODE_TEST_SERVER_FIXTURE, gpu);
        manager.set_http_client(std::make_shared<HealthClient>());
        manager.set_advisory_callback([this](const std::string& note) { advice.push_back(note); });
        profile.id = "preflight-fixture";
        profile.model_path = paths.directory.write("fixture.gguf", preflight_gguf());
        profile.server_binary = LOCALCODE_TEST_SERVER_FIXTURE;
    }

    std::size_t queries() const {
        std::ifstream input(marker);
        std::size_t count = 0;
        for (std::string line; std::getline(input, line);)
            ++count;
        return count;
    }

    double activate(int layers) {
        profile.gpu_layers = layers;
        const auto begin = std::chrono::steady_clock::now();
        const auto result = manager.activate(profile);
        REQUIRE(result);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        REQUIRE(manager.server_status().process_id != 0);
        manager.deactivate();
        CHECK(manager.server_status().process_id == 0);
        return ms;
    }
};
}   // namespace

TEST_CASE("activation queries GPU memory only when full-file advice can apply",
          "[model][preflight][integration]") {
    PreflightFixture fixture;
    for (int layers : {0, 1, 32}) {
        fixture.activate(layers);
        CHECK(fixture.queries() == 0);
        CHECK(fixture.advice.empty());
    }
    for (int layers : {33, -1}) {
        fixture.activate(layers);
        CHECK(fixture.queries() == fixture.advice.size());
    }
    CHECK(fixture.queries() == 2);
    CHECK(fixture.advice.size() == 2);
    fixture.activate(0);
    CHECK(fixture.queries() == 2);
    CHECK(fixture.advice.size() == 2);
}

TEST_CASE("CPU activation avoids an irrelevant driver timeout",
          "[model][preflight][.startup-measure]") {
    PreflightFixture fixture;
    fixture.delay.set("3500");
    const auto elapsed = fixture.activate(0);
    std::cout << "cpu_preflight_fixture_ms=" << elapsed << " gpu_queries=" << fixture.queries()
              << "\n";
    CHECK(fixture.queries() == 0);
    CHECK(fixture.advice.empty());
}

TEST_CASE("real command execution records repository provenance only in its directory",
          "[repo][integration]") {
    IsolatedPaths paths;
    test::TempDirectory work;
    work.write("CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.20)\nproject(provenance NONE)\n");
    std::filesystem::create_directory(work.path() / "other");
    Application app;
    REQUIRE(app.initialise());
    REQUIRE(app.open_workspace(work.path()));
    REQUIRE(app.repository().refresh(app.workspace().ignore_rules(), {}));
    repo::RepoCommand command;
    command.purpose = "run";
    command.command = "cmake --version";
    command.source = "test fixture command";
    REQUIRE(app.repository().store().put_command(command));
    ToolContext context;
    context.workspace = &app.workspace();
    context.environment = &app.environment();
    context.repo = &app.repository();
    context.confirm = [](const ConfirmationRequest&) { return true; };
    ToolInvocation invocation;
    invocation.tool_name = "run_command";
    invocation.arguments =
        Json{{"executable", "cmake"}, {"arguments", {"--version"}}, {"working_directory", "other"}};
    REQUIRE(app.tools().invoke(invocation, context).ok);
    auto commands = app.repository().store().commands();
    REQUIRE(commands);
    for (const auto& item : *commands)
        if (item.command == command.command) CHECK_FALSE(item.verified);
    invocation.arguments.erase("working_directory");
    const auto result = app.tools().invoke(invocation, context);
    REQUIRE(result.ok);
    CHECK(result.structured.at("requested_command") == command.command);
    commands = app.repository().store().commands();
    REQUIRE(commands);
    bool found = false;
    for (const auto& item : *commands) {
        if (item.command != command.command) continue;
        found = true;
        CHECK(item.verified);
        CHECK(item.evidence == repo::Evidence::Verified);
        CHECK(item.last_exit_code == 0);
    }
    CHECK(found);
}

TEST_CASE("failed settings persistence leaves active settings unchanged",
          "[settings][integration]") {
    IsolatedPaths paths;
    Application app;
    REQUIRE(app.initialise());
    const auto before = app.settings();
    Database fault;
    REQUIRE(fault.open(app_paths().database_path()));
    REQUIRE(
        fault.execute("CREATE TRIGGER reject_setting BEFORE INSERT ON settings "
                      "WHEN NEW.key='ui.theme' BEGIN SELECT RAISE(ABORT,'fixture failure'); END"));
    auto next = before;
    next.theme = "light";
    next.max_iterations = before.max_iterations + 1;
    CHECK_FALSE(app.apply_settings(next));
    CHECK(app.settings().theme == before.theme);
    CHECK(app.settings().max_iterations == before.max_iterations);
    CHECK(app.settings_repository().get_int("agent.max_iterations", 0) == before.max_iterations);
    REQUIRE(fault.execute("DROP TRIGGER reject_setting"));
    REQUIRE(app.apply_settings(next));
    CHECK(app.settings().max_iterations == next.max_iterations);
}

TEST_CASE("runtime settings retain Unicode across reload and reject malformed updates",
          "[settings][unicode][integration]") {
    IsolatedPaths paths;
    Application app;
    REQUIRE(app.initialise());
    const auto binary = std::filesystem::path(u8"L:/exécutables/日本語/llama-server.exe");
    auto next = app.settings();
    next.llama_server_path = path_to_utf8(binary);
    REQUIRE(app.apply_settings(next));
    REQUIRE(app.load_settings());
    CHECK(app.backends().configured_binary(BackendPreference::LlamaCpp) == binary);
    CHECK(app.settings_repository().get("backend.llama_server").value() == path_to_utf8(binary));
    next.llama_server_path = "bad-\xE9";
    CHECK_FALSE(app.apply_settings(next));
    CHECK(app.settings_repository().get("backend.llama_server").value() == path_to_utf8(binary));
#if defined(_WIN32)
    const auto legacy = std::filesystem::path(u8"L:/exécutables/llama-server.exe");
    REQUIRE(app.settings_repository().set("backend.llama_server", legacy.string()));
    REQUIRE(app.load_settings());
    CHECK(app.backends().configured_binary(BackendPreference::LlamaCpp) == legacy);
    CHECK(app.settings_repository().get("backend.llama_server").value() == path_to_utf8(legacy));
#endif
}

TEST_CASE("checkpoint and task persistence failures are reported and retryable",
          "[persistence][integration]") {
    IsolatedPaths paths;
    test::TempDirectory work;
    Application app;
    REQUIRE(app.initialise());
    REQUIRE(app.open_workspace(work.path()));
    REQUIRE(app.start_conversation("persistence fault fixture"));
    Database fault;
    REQUIRE(fault.open(app_paths().database_path()));
    REQUIRE(
        fault.execute("CREATE TRIGGER reject_draft BEFORE INSERT ON drafts "
                      "BEGIN SELECT RAISE(ABORT,'fixture draft failure'); END"));
    CHECK_FALSE(app.checkpoint_session({}, "keep my unsent prompt"));
    REQUIRE(fault.execute("DROP TRIGGER reject_draft"));
    REQUIRE(app.checkpoint_session({}, "keep my unsent prompt"));
    CHECK(app.draft_repository().load(work.path().generic_string()).value_or("") ==
          "keep my unsent prompt");
    TaskState task;
    task.set_objective("an interrupted task");
    app.agent().set_task_state(task);
    const std::vector<ChatMessage> history{ChatMessage::user("an interrupted task")};
    REQUIRE(
        fault.execute("CREATE TRIGGER reject_task BEFORE INSERT ON tasks "
                      "BEGIN SELECT RAISE(ABORT,'fixture task failure'); END"));
    CHECK_FALSE(app.persist_conversation(history));
    REQUIRE(fault.execute("DROP TRIGGER reject_task"));
    auto retried = app.persist_conversation(history);
    REQUIRE(retried);
    CHECK(*retried == 0);
    auto saved = app.task_repository().latest_for_conversation(app.current_conversation());
    REQUIRE(saved);
    REQUIRE(saved->has_value());
    CHECK((*saved)->objective == "an interrupted task");
    CHECK(app.conversation_repository().message_count(app.current_conversation()).value_or(0) == 1);
}

TEST_CASE("a failed switch clears the outgoing model from every owner", "[model][lifecycle]") {
    IsolatedPaths paths;
    Application app;
    REQUIRE(app.initialise());
    auto http = std::make_shared<HealthClient>();
    app.backends().set_http_client(http);
    ExternalEndpoint endpoint;
    endpoint.enabled = true;
    endpoint.base_url = "http://127.0.0.1:1";
    app.backends().set_external_endpoint(endpoint);
    ModelProfile first;
    first.id = "first";
    first.model_path = "fixture.gguf";
    first.display_name = "First";
    auto second = first;
    second.id = "second";
    REQUIRE(app.save_model_profile(first));
    REQUIRE(app.save_model_profile(second));
    REQUIRE(app.activate_model(first.id));
    CHECK(app.active_model_id() == first.id);
    REQUIRE(app.backends().current_backend());

    http->fail = true;
    CHECK_FALSE(app.activate_model(second.id));
    CHECK(app.active_model_id().empty());
    CHECK_FALSE(app.backends().current_backend());
    CHECK(app.backends().current_profile().id.empty());
    const auto run = app.agent().run("hello", {});
    CHECK(run.stop_reason.starts_with("No model is loaded"));

    http->fail = false;
    REQUIRE(app.activate_model(first.id));
    CHECK(app.active_model_id() == first.id);
    app.deactivate_model();
    CHECK(app.active_model_id().empty());
    CHECK(app.backends().current_profile().id.empty());
}

TEST_CASE("switching projects isolates the conversation while preserving the old one",
          "[persistence][integration]") {
    IsolatedPaths paths;
    test::TempDirectory first;
    test::TempDirectory second;
    Application app;
    REQUIRE(app.initialise());
    REQUIRE(app.open_workspace(first.path()));
    auto conversation = app.start_conversation("first project");
    REQUIRE(conversation);
    app.agent().set_history({ChatMessage::user("first project's request")});
    REQUIRE(app.open_workspace(second.path()));
    CHECK(app.current_conversation() == 0);
    CHECK(app.agent().history().empty());
    CHECK(app.conversation_repository().message_count(*conversation).value_or(0) == 1);
    auto session = app.recover_session();
    REQUIRE(session);
    CHECK(session->conversation_id == 0);
    CHECK(session->workspace_path == app.workspace().root().generic_string());
}

TEST_CASE("session recovery reports unreadable state and failed crash markers",
          "[persistence][integration]") {
    IsolatedPaths paths;
    Application app;
    REQUIRE(app.initialise());
    Database fault;
    REQUIRE(fault.open(app_paths().database_path()));
    REQUIRE(app.app_state_repository().set("session.clean_shutdown", "1"));
    REQUIRE(
        fault.execute("CREATE TRIGGER reject_crash_marker BEFORE INSERT ON app_state "
                      "WHEN NEW.key='session.clean_shutdown' BEGIN "
                      "SELECT RAISE(ABORT,'fixture marker failure'); END"));
    CHECK_FALSE(app.recover_session());
    CHECK(app.app_state_repository().get("session.clean_shutdown").value_or("") == "1");
    REQUIRE(fault.execute("DROP TRIGGER reject_crash_marker"));
    REQUIRE(app.app_state_repository().set("session.conversation", "12trailing"));
    CHECK_FALSE(app.recover_session());
    REQUIRE(app.app_state_repository().remove("session.conversation"));
    REQUIRE(app.app_state_repository().set("session.open_files", "[42]"));
    CHECK_FALSE(app.recover_session());
    REQUIRE(app.app_state_repository().set("session.open_files", "[]"));
    REQUIRE(app.recover_session());
    REQUIRE(fault.execute("ALTER TABLE app_state RENAME TO inaccessible_app_state"));
    CHECK_FALSE(app.recover_session());
    REQUIRE(fault.execute("ALTER TABLE inaccessible_app_state RENAME TO app_state"));
    REQUIRE(app.recover_session());
}

TEST_CASE("conversation metadata failure preserves the active session and rolls back creation",
          "[persistence][integration]") {
    IsolatedPaths paths;
    Application app;
    REQUIRE(app.initialise());
    const auto first = app.start_conversation("active");
    REQUIRE(first);
    app.agent().set_history({ChatMessage::user("preserve my history")});
    Database fault;
    REQUIRE(fault.open(app_paths().database_path()));
    REQUIRE(
        fault.execute("CREATE TRIGGER reject_conversation_marker BEFORE INSERT ON app_state "
                      "WHEN NEW.key='session.conversation' BEGIN "
                      "SELECT RAISE(ABORT,'fixture session failure'); END"));
    CHECK_FALSE(app.start_conversation("must roll back"));
    CHECK(app.current_conversation() == *first);
    CHECK(app.conversation_repository().recent(0).value_or({}).size() == 1);
    CHECK_FALSE(app.load_conversation(*first));
    REQUIRE(app.agent().history().size() == 1);
    CHECK(app.agent().history()[0].content == "preserve my history");
    REQUIRE(fault.execute("DROP TRIGGER reject_conversation_marker"));
    const auto next = app.start_conversation("next");
    REQUIRE(next);
    REQUIRE(app.load_conversation(*first));
    CHECK(app.current_conversation() == *first);
    CHECK_FALSE(app.load_conversation(999999));
    CHECK(app.current_conversation() == *first);
}
