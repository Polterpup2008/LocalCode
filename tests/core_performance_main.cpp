#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "agent/ContextManager.h"
#include "inference/RequestTrace.h"
#include "platform/Environment.h"
#include "platform/Process.h"
#include "repo/RepoIndex.h"
#include "storage/Repositories.h"
#include "util/Json.h"

namespace {
using namespace localcode;
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Json summary(const std::vector<double>& samples) {
    auto ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    auto percentile = [&](double fraction) {
        return ordered.at(static_cast<std::size_t>(std::ceil(fraction * ordered.size())) - 1);
    };
    return {{"unit", "ms"},
            {"n", samples.size()},
            {"samples", samples},
            {"p50", percentile(.5)},
            {"p95", percentile(.95)}};
}

template <typename T>
void require(const T& status) {
    if (!status) throw std::runtime_error(status.error().to_string());
}

Json persistence(const std::filesystem::path& root, int messages, int repeats) {
    Database database;
    require(database.open(root / ("conversation-" + std::to_string(messages) + ".sqlite3")));
    WorkspaceRepository workspaces(database);
    ConversationRepository conversations(database);
    std::vector<double> save, restore, workspace_save, workspace_restore;
    const std::string body(2048, 'x');
    for (int run = 0; run < repeats; ++run) {
        auto start = Clock::now();
        auto workspace = workspaces.touch(root.string(), "Synthetic performance fixture");
        require(workspace);
        require(workspaces.save_settings(workspace->id, "{\"fixture\":true}"));
        workspace_save.push_back(elapsed(start));
        start = Clock::now();
        auto found = workspaces.find_by_path(root.string());
        require(found);
        if (!found->has_value()) throw std::runtime_error("saved workspace missing");
        workspace_restore.push_back(elapsed(start));
        auto conversation = conversations.create(workspace->id, "Synthetic large chat", "fixture");
        require(conversation);
        start = Clock::now();
        for (int index = 0; index < messages; ++index) {
            require(conversations.append_message(
                *conversation,
                index % 2 == 0 ? ChatMessage::user(body) : ChatMessage::assistant(body)));
        }
        save.push_back(elapsed(start));
        start = Clock::now();
        auto restored = conversations.messages(*conversation);
        require(restored);
        if (restored->size() != static_cast<std::size_t>(messages))
            throw std::runtime_error("incomplete conversation restore");
        restore.push_back(elapsed(start));
    }
    return {{"messages", messages},
            {"bytes_per_message", body.size()},
            {"save", summary(save)},
            {"restore", summary(restore)},
            {"workspace_save", summary(workspace_save)},
            {"workspace_restore", summary(workspace_restore)}};
}

Json repository(const std::filesystem::path& root, int repeats) {
    const auto workspace = root / "repository";
    std::filesystem::create_directories(workspace / "src");
    for (int index = 0; index < 1000; ++index) {
        std::ofstream file(workspace / "src" / ("fixture" + std::to_string(index) + ".cpp"));
        file << "// Deterministic synthetic repository, not user code.\n";
        file << "int fixture_symbol_" << index << "(int value) { return value + " << index
             << "; }\n";
    }
    fs::IgnoreRules ignore;
    std::vector<double> cold, warm, invalidation, query;
    std::uint64_t cache_bytes = 0;
    std::size_t scanned = 0, unchanged = 0, reindexed = 0;
    for (int run = 0; run < repeats; ++run) {
        repo::RepoIndex index;
        require(index.open(root / ("index-" + std::to_string(run)), workspace));
        auto start = Clock::now();
        auto stats = index.refresh(ignore, {});
        require(stats);
        cold.push_back(elapsed(start));
        start = Clock::now();
        stats = index.refresh(ignore, {});
        require(stats);
        warm.push_back(elapsed(start));
        scanned = stats->files_scanned;
        unchanged = stats->files_unchanged;
        {
            std::ofstream file(workspace / "src" / "fixture0.cpp", std::ios::app);
            file << "int edited_symbol_" << run << "() { return 1; }\n";
        }
        start = Clock::now();
        stats = index.refresh(ignore, {});
        require(stats);
        invalidation.push_back(elapsed(start));
        reindexed = stats->files_reindexed;
        for (int sample = 0; sample < 30; ++sample) {
            start = Clock::now();
            auto hits = index.store().symbols_named("fixture_symbol_500");
            require(hits);
            if (hits->empty()) throw std::runtime_error("fixture query missed source");
            query.push_back(elapsed(start));
        }
        auto size = index.store().size_on_disk();
        require(size);
        cache_bytes = *size;
    }
    return {{"fixture_files", 1000},
            {"cold_new_index", summary(cold)},
            {"warm_refresh", summary(warm)},
            {"one_file_edit", summary(invalidation)},
            {"exact_symbol_query", summary(query)},
            {"cache_bytes", cache_bytes},
            {"last_warm_scanned", scanned},
            {"last_warm_unchanged", unchanged},
            {"last_edit_reindexed", reindexed}};
}

Json process(int repeats) {
    ProcessRequest request;
#ifdef _WIN32
    const auto executable = find_executable("cmd");
    request.arguments = {"/d", "/s", "/c", "echo localcode-performance-fixture"};
#else
    const auto executable = find_executable("sh");
    request.arguments = {"-c", "echo localcode-performance-fixture"};
#endif
    if (!executable) throw std::runtime_error("system shell unavailable");
    request.executable = *executable;
    request.timeout = std::chrono::seconds(10);
    std::vector<double> samples;
    for (int run = 0; run < repeats * 10; ++run) {
        const auto start = Clock::now();
        auto result = ProcessRunner::run(request);
        require(result);
        if (!result->success() ||
            result->stdout_text.find("localcode-performance-fixture") == std::string::npos)
            throw std::runtime_error("owned process fixture failed");
        samples.push_back(elapsed(start));
    }
    return summary(samples);
}

Json environment(int repeats) {
    std::vector<double> cold, cached;
    std::size_t tool_count = 0, found = 0;
    for (int run = 0; run < repeats; ++run) {
        RequestTrace::begin_run("environment");
        EnvironmentService environment;
        const auto start = Clock::now();
        const auto& report = environment.report();
        cold.push_back(elapsed(start));
        tool_count = report.tools.size();
        found = static_cast<std::size_t>(
            std::count_if(report.tools.begin(), report.tools.end(),
                          [](const ToolInfo& tool) { return tool.found; }));
        const auto cached_start = Clock::now();
        const auto& warm = environment.report();
        cached.push_back(elapsed(cached_start));
        if (&warm != &report) throw std::runtime_error("environment cache was not reused");
        RequestTrace::end_run();
    }
    return {{"first_discovery", summary(cold)},
            {"cached", summary(cached)},
            {"tools", tool_count},
            {"found", found}};
}
}   // namespace

int main(int argc, char** argv) {
    try {
        const auto output =
            std::filesystem::absolute(argc > 1 ? argv[1] : "build/performance/core-baseline.json");
        const int repeats = argc > 2 ? std::clamp(std::stoi(argv[2]), 1, 20) : 3;
        const auto root =
            output.parent_path() /
            ("fixture-" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                             Clock::now().time_since_epoch())
                                             .count()));
        std::filesystem::create_directories(root);
        Json report{{"schema", 1}, {"fixture_root", root.string()}};
        const bool repository_only = argc > 3 && std::string_view(argv[3]) == "repository";
        if (argc <= 3 || std::string_view(argv[3]) != "environment") {
            report["repository"] = repository(root, repeats);
            if (!repository_only) {
                report["process_round_trip"] = process(repeats);
                report["persistence"] =
                    Json::array({persistence(root, 100, repeats), persistence(root, 500, repeats)});
            }
        }
        if (!repository_only) report["environment"] = environment(repeats);
        std::ofstream file(output, std::ios::binary);
        file << report.dump(2) << '\n';
        if (!file) throw std::runtime_error("cannot write benchmark artifact");
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
