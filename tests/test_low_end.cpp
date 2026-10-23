#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "agent/Agent.h"
#include "inference/BackendManager.h"
#include "inference/RuntimeDiagnostics.h"
#include "inference/ServerManager.h"
#include "platform/GpuProbe.h"
#include "support/MockBackend.h"
#include "support/TempDirectory.h"
#include "workspace/Workspace.h"

using namespace localcode;

TEST_CASE("automatic conversation routing never swallows a coding request",
          "[low-end][conversation]") {
    for (const auto* message :
         {"hello how are you", "Hello, how are you?", "HI!", "bonjour", "thanks"})
        CHECK(use_conversation_path(message, RequestMode::Auto));
    for (const auto* message : {"hello, fix calc.py", "thanks, now run the tests", "hello.cpp",
                                "hello\nread my files", "continue", "what is in this project?"})
        CHECK_FALSE(use_conversation_path(message, RequestMode::Auto));
    CHECK_FALSE(use_conversation_path("hello", RequestMode::Code));
    CHECK(use_conversation_path("Explain recursion", RequestMode::Chat));
}

TEST_CASE("greetings omit agent overhead and switching to code restores tools",
          "[low-end][conversation]") {
    test::TempDirectory temp{"lc-light-chat"};
    Workspace workspace;
    REQUIRE(workspace.open(temp.path()).ok());
    ToolRegistry tools;
    tools.register_builtin_tools();
    auto backend = std::make_shared<test::MockBackend>();
    backend->then_say("Hello! How can I help?");
    backend->then_say("I have not inspected the source yet.");
    Agent agent;
    ModelProfile profile;
    profile.context_size = 32768;
    profile.tool_protocol = ToolProtocol::Native;
    agent.set_profile(profile);
    agent.set_backend(backend);
    agent.set_workspace(&workspace);
    agent.set_tools(&tools);
    std::vector<AgentEventType> events;
    agent.set_event_sink([&](const AgentEvent& event) { events.push_back(event.type); });
    const auto chat = agent.run("hello how are you", {});
    REQUIRE(chat.completed);
    CHECK(chat.conversational);
    REQUIRE(backend->requests().size() == 1);
    const auto& light = backend->requests().front();
    CHECK(light.tools.empty());
    CHECK(chat.context.total == 32768);
    CHECK(chat.context.skills == 0);
    CHECK(chat.context.tool_definitions == 0);
    CHECK(chat.context.task_state == 0);
    CHECK(chat.context.used < 400);
    CHECK(light.messages.front().content.find("Workspace:") == std::string::npos);
    CHECK(std::find(events.begin(), events.end(), AgentEventType::GenerationStats) != events.end());
    CHECK(agent.task_state().objective().empty());
    const auto coding = agent.run("Hello, inspect calc.py and explain its behavior.", {});
    CHECK_FALSE(coding.conversational);
    REQUIRE(backend->requests().size() >= 2);
    CHECK_FALSE(backend->requests()[1].tools.empty());
}

TEST_CASE("chat mode never executes native or serialized tool calls", "[low-end][conversation]") {
    for (const bool native : {false, true}) {
        test::TempDirectory temp{"lc-chat-no-tools"};
        Workspace workspace;
        REQUIRE(workspace.open(temp.path()).ok());
        ToolRegistry tools;
        tools.register_builtin_tools();
        auto backend = std::make_shared<test::MockBackend>();
        if (native)
            backend->then_call("write_file", {{"path", "unexpected.txt"}, {"content", "bad"}});
        else
            backend->then_say(
                "<tool_call>{\"name\":\"write_file\",\"arguments\":{\"path\":\"unexpected.txt\","
                "\"content\":\"bad\"}}</tool_call>");
        Agent agent;
        agent.set_backend(backend);
        agent.set_workspace(&workspace);
        agent.set_tools(&tools);
        auto config = agent.config();
        config.request_mode = RequestMode::Chat;
        agent.set_config(config);
        const auto summary = agent.run("Please write a file", {});
        CHECK_FALSE(summary.completed);
        CHECK(summary.tool_calls == 0);
        CHECK_FALSE(std::filesystem::exists(temp.path() / "unexpected.txt"));
    }
}

TEST_CASE("chat keeps clean role order and does not rewrite coding evidence",
          "[low-end][conversation]") {
    Agent agent;
    auto backend = std::make_shared<test::MockBackend>();
    backend->then_say("Hello again.");
    agent.set_backend(backend);
    ChatMessage call;
    call.role = Role::Assistant;
    call.tool_calls.push_back({"prior-call", "read_file", "{}"});
    agent.set_history({ChatMessage::user("Read the file"), call,
                       ChatMessage::tool_result("prior-call", "read_file", "SECRET_TOOL_OUTPUT")});
    TaskState task;
    task.begin("Previous coding task");
    agent.set_task_state(task);
    const auto result = agent.run("hello", {});
    REQUIRE(result.completed);
    const auto& messages = backend->last_request().messages;
    REQUIRE(messages.size() == 2);
    CHECK(messages.back().role == Role::User);
    CHECK(messages.back().content.find("SECRET_TOOL_OUTPUT") == std::string::npos);
    CHECK(messages.back().content == "hello");
    CHECK(agent.task_state().objective() == "Previous coding task");
    CHECK(result.ledger.evidence().empty());
}

TEST_CASE("new profile recommendations fit low memory without rewriting saved settings",
          "[low-end][model]") {
    constexpr std::uint64_t gib = 1024ull * 1024 * 1024;
    GgufMetadata metadata;
    metadata.path = "existing-small-model.gguf";
    metadata.context_length = 131072;
    CHECK(ModelProfile::from_gguf(metadata, 16 * gib, 4 * gib).context_size == 4096);
    CHECK(ModelProfile::from_gguf(metadata, 16 * gib, 4 * gib).prompt_cache_mib == 128);
    CHECK(ModelProfile::from_gguf(metadata, 32 * gib, 8 * gib).context_size == 16384);
    CHECK(ModelProfile::from_gguf(metadata).context_size == 8192);
    metadata.context_length = 2048;
    CHECK(ModelProfile::from_gguf(metadata, 16 * gib, 4 * gib).context_size == 2048);
    auto profile = ModelProfile::from_gguf(metadata);
    CHECK(profile.auto_gpu_layers);
    profile.context_size = 32768;
    profile.auto_gpu_layers = false;
    profile.gpu_layers = 12;
    const auto restored = model_profile_from_json(to_json(profile));
    CHECK(restored.context_size == 32768);
    CHECK_FALSE(restored.auto_gpu_layers);
    CHECK(restored.gpu_layers == 12);
    auto legacy = to_json(profile);
    legacy.erase("auto_gpu_layers");
    CHECK_FALSE(model_profile_from_json(legacy).auto_gpu_layers);
    legacy["gpu_layers"] = -1;
    CHECK(model_profile_from_json(legacy).auto_gpu_layers);
    legacy["auto_gpu_layers"] = false;
    CHECK_FALSE(model_profile_from_json(legacy).auto_gpu_layers);
}

TEST_CASE("cancelled conversation keeps partial text without completing or invoking tools",
          "[low-end][conversation]") {
    Agent agent;
    auto backend = std::make_shared<test::MockBackend>();
    backend->then_say("Partial answer");
    agent.set_backend(backend);
    CancellationSource source;
    agent.set_event_sink([&](const AgentEvent& event) {
        if (event.type == AgentEventType::ContentDelta && !event.text.empty()) source.cancel();
    });
    const auto result = agent.run("hello", source.token());
    CHECK(result.cancelled);
    CHECK_FALSE(result.completed);
    CHECK(result.tool_calls == 0);
    REQUIRE(agent.history().size() == 2);
    CHECK(agent.history().back().content == "Partial answer");
}

TEST_CASE("automatic GPU fitting preserves context and single agent slot", "[low-end][backend]") {
    ServerConfig config;
    config.profile.context_size = 32768;
    config.profile.auto_gpu_layers = true;
    ServerBinaryFeatures features;
    features.fit = true;
    features.parallel = true;
    features.log_verbosity = true;
    auto args = ModelServer::build_arguments(config, features, 12345);
    const auto value = [&](const char* flag) {
        auto it = std::find(args.begin(), args.end(), flag);
        return it != args.end() && ++it != args.end() ? *it : "absent";
    };
    CHECK(value("--n-gpu-layers") == "auto");
    CHECK(value("--ctx-size") == "32768");
    CHECK(value("--fit-ctx") == "32768");
    CHECK(value("--parallel") == "1");
    CHECK(value("--log-verbosity") == "4");
    config.profile.auto_gpu_layers = false;
    config.profile.gpu_layers = 12;
    args = ModelServer::build_arguments(config, features, 12345);
    CHECK(value("--n-gpu-layers") == "12");
    CHECK(value("--fit-ctx") == "absent");
}

TEST_CASE("GPU diagnostics distinguish devices from actual layer offload", "[low-end][backend]") {
    const auto devices = parse_device_listing(
        "CUDA initialization failed\nAvailable devices:\n  Vulkan0: Example GPU (4096 MiB, 3000 "
        "MiB free)\n");
    REQUIRE(devices.size() == 1);
    CHECK(devices.front().starts_with("Vulkan0:"));
    CHECK(parse_device_listing("Available devices:\nCPU: Generic CPU\n").empty());
    CHECK(parse_device_listing("Available devices:\n  ROCm0: AMD GPU (8192 MiB, 7000 MiB free)\n")
              .size() == 1);
    CHECK(
        parse_device_listing("Available devices:\n  RPC[127.0.0.1:50052]: Remote GPU (8192 MiB)\n")
            .size() == 1);
    CHECK(parse_device_listing("error: failed to initialize CUDA\n").empty());
    const std::vector<std::string> mixed = {
        "Vulkan0: Integrated Graphics (16384 MiB, 15000 MiB free)",
        "Vulkan1: Discrete GPU (4096 MiB, 3000 MiB free)"};
    CHECK(prefer_dedicated_device(mixed, {{"Integrated Graphics", 128ull * 1024 * 1024},
                                          {"Discrete GPU", 4ull * 1024 * 1024 * 1024}}) ==
          "Vulkan1");
    CHECK(prefer_dedicated_device(mixed, {}).empty());
    AccelerationInfo info;
    CHECK(info.describe().find("unconfirmed") != std::string::npos);
    info.observe("load_tensors: offloaded 0/33 layers to GPU");
    CHECK(info.describe().starts_with("CPU inference"));
    info.observe("load_tensors: offloaded 12/33 layers to GPU");
    info.observe("load_tensors: CUDA_Host model buffer size = 2000.00 MiB");
    info.observe("load_tensors: Vulkan0 model buffer size = 1500.50 MiB");
    CHECK(info.device_buffer_mib == 1500.50);
    info.observe("llama_memory_recurrent: Vulkan0 RS buffer size = 1.25 MiB");
    CHECK(info.device_buffer_mib == 1501.75);
    CHECK(info.describe().starts_with("Partial GPU offload"));
    info.observe("load_tensors: offloaded 33/33 layers to GPU");
    CHECK(info.describe().starts_with("Full GPU offload"));
    info.observe("offloaded 999999999999999999999/1 layers to GPU");
    CHECK(info.offloaded_layers == 33);
}

TEST_CASE("runtime selection probes actual devices and exposes CPU fallback",
          "[low-end][backend]") {
    for (const auto* variant : {"device-gpu", "device-empty", "device-failed", "device-odd"}) {
        test::TempDirectory temp{"lc-device-probe"};
        const auto binary = temp.path() / (std::string(variant) +
#if defined(_WIN32)
                                           ".exe"
#else
                                           ""
#endif
                                          );
        std::filesystem::copy_file(LOCALCODE_TEST_SERVER_FIXTURE, binary);
#if !defined(_WIN32)
        std::filesystem::permissions(binary, std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::add);
#endif
        ModelProfile profile;
        profile.server_binary = binary;
        BackendManager manager;
        const auto selection = manager.select(profile);
        REQUIRE(selection);
        CHECK(selection->install.server_binary == binary);
        CHECK(selection->force_cpu == (std::string_view(variant) == "device-empty" ||
                                       std::string_view(variant) == "device-failed"));
        profile.gpu_layers = 0;
        const auto cpu = manager.select(profile);
        REQUIRE(cpu);
        CHECK_FALSE(cpu->force_cpu);
    }
}
