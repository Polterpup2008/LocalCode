// Explicitly opt-in real-model lifecycle measurements. No CI timing thresholds.
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "inference/BackendManager.h"
#include "model/GgufMetadata.h"
#include "platform/Paths.h"
#include "util/Strings.h"

namespace {
using namespace localcode;
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

ModelProfile profile(const std::filesystem::path& path, const std::filesystem::path& server,
                     std::uint32_t context) {
    auto metadata = read_gguf_metadata(path);
    if (!metadata) throw std::runtime_error(metadata.error().message);
    auto result = ModelProfile::from_gguf(*metadata);
    result.context_size = context;
    result.backend = BackendPreference::LlamaCpp;
    result.server_binary = server;
    return result;
}

Json question(const BackendPtr& backend, const ModelProfile& model) {
    ChatRequest request;
    request.messages = {ChatMessage::user("What is 17 multiplied by 23? Give only the number.")};
    request.sampling = model.sampling;
    request.sampling.max_tokens = 1024;
    request.enable_reasoning = model.reasoning_enabled;
    request.stream = true;
    auto first = Clock::time_point{};
    const auto started = Clock::now();
    StreamHandlers handlers;
    handlers.on_first_token = [&] { first = Clock::now(); };
    CancellationSource cancel;
    std::mutex deadline_mutex;
    std::condition_variable_any deadline_condition;
    std::jthread deadline([&](std::stop_token stop) {
        std::unique_lock lock(deadline_mutex);
        deadline_condition.wait_for(lock, stop, std::chrono::seconds(120), [] { return false; });
        if (!stop.stop_requested()) cancel.cancel();
    });
    auto result = backend->chat(request, handlers, cancel.token());
    if (!result)
        return {{"wall_ms", elapsed(started)},
                {"correct", false},
                {"error", result.error().to_string()}};
    return {{"wall_ms", elapsed(started)},
            {"first_token_ms",
             first == Clock::time_point{}
                 ? Json{}
                 : Json(std::chrono::duration<double, std::milli>(first - started).count())},
            {"answer", strings::trim(result->message.content)},
            {"correct", strings::trim(result->message.content) == "391"},
            {"completion_limit", request.sampling.max_tokens},
            {"reasoning_enabled", request.enable_reasoning},
            {"reasoning_characters", result->message.reasoning_content.size()},
            {"finish_reason", result->finish_reason},
            {"reasoning_excerpt", strings::utf8_truncate(result->message.reasoning_content, 600)},
            {"prompt_tokens", result->usage.prompt_tokens},
            {"completion_tokens", result->usage.completion_tokens},
            {"cached_prompt_tokens", result->usage.cached_prompt_tokens},
            {"timings_from_server", result->timings.from_server},
            {"prompt_tok_s", result->timings.prompt_tokens_per_second},
            {"decode_tok_s", result->timings.decode_tokens_per_second}};
}
}   // namespace

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::cerr << "Usage: localcode_model_performance output.json qwen.gguf ling.gguf "
                     "server.exe [context]\n";
        return 2;
    }
    try {
        const auto output = std::filesystem::absolute(argv[1]);
        app_paths().set_root(output.parent_path() / "model-lifecycle-home");
        auto directories = app_paths().ensure_directories();
        if (!directories) throw std::runtime_error(directories.error().message);
        const auto context = argc == 6 ? static_cast<std::uint32_t>(std::stoul(argv[5])) : 32768u;
        if (context < 4096 || context > 32768)
            throw std::runtime_error("test context must be 4096 to 32768");
        const auto qwen = profile(argv[2], argv[4], context);
        const auto ling = profile(argv[3], argv[4], context);
        BackendManager manager;
        manager.set_http_client(HttpClient::create());
        Json report{{"schema", 1},
                    {"context", context},
                    {"question_deadline_seconds", 120},
                    {"starts", Json::array()},
                    {"switches", Json::array()},
                    {"notes",
                     {"First start includes binary discovery; no OS cache purge.",
                      "Uses production BackendManager, metadata-derived profiles.",
                      "Other GPU applications may contend; timings are observational."}}};
        auto save = [&] {
            std::ofstream file(output, std::ios::binary | std::ios::trunc);
            file << report.dump(2) << '\n';
            if (!file) throw std::runtime_error("cannot save lifecycle measurements");
        };
        auto activate = [&](const ModelProfile& model) {
            auto ready = manager.activate(model);
            if (!ready) throw std::runtime_error(ready.error().message);
            if (manager.server_status().state != ServerState::Ready)
                throw std::runtime_error("activation returned without a Ready server");
            return *ready;
        };
        for (const auto& model : {qwen, ling}) {
            for (int repeat = 0; repeat < 3; ++repeat) {
                const auto started = Clock::now();
                auto backend = activate(model);
                const auto ready = manager.server_status();
                Json row{{"model", model.display_name},    {"repeat", repeat},
                         {"startup_ms", elapsed(started)}, {"managed_pid", ready.process_id},
                         {"endpoint", ready.base_url},     {"running_model_id", ready.model_id},
                         {"questions", Json::array()}};
                report["starts"].push_back(std::move(row));
                save();
                auto& recorded = report["starts"].back();
                for (int prompt = 0; prompt < 3; ++prompt) {
                    recorded["questions"].push_back(question(backend, model));
                    save();
                    if (recorded["questions"].back().contains("error"))
                        throw std::runtime_error(
                            "model question failed; partial measurements preserved");
                }
                const auto stopped = Clock::now();
                manager.deactivate();
                recorded["shutdown_ms"] = elapsed(stopped);
                recorded["stopped"] = manager.server_status().state == ServerState::Stopped;
                recorded["stopped_pid"] = manager.server_status().process_id;
                save();
                std::cout << "Measured startup/questions/shutdown: " << model.display_name
                          << " repeat " << repeat << std::endl;
            }
        }
        activate(qwen);
        for (int repeat = 0; repeat < 3; ++repeat) {
            for (const auto& model : {ling, qwen}) {
                const auto started = Clock::now();
                activate(model);
                const auto ready = manager.server_status();
                report["switches"].push_back({{"to", model.display_name},
                                              {"repeat", repeat},
                                              {"switch_ms", elapsed(started)},
                                              {"managed_pid", ready.process_id},
                                              {"endpoint", ready.base_url},
                                              {"running_model_id", ready.model_id}});
                save();
                std::cout << "Measured switch to " << model.display_name << std::endl;
            }
        }
        manager.deactivate();
        report["final_stopped"] = manager.server_status().state == ServerState::Stopped;
        save();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
