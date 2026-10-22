#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "inference/HttpClient.h"

namespace {
using Clock = std::chrono::steady_clock;
using localcode::SseParser;

struct Workload {
    std::string name;
    std::size_t body_bytes;
    std::size_t chunk_bytes;
    int events;
};

double measure(const Workload& workload, int passes) {
    const std::string body(workload.body_bytes, 'x');
    const std::string event = "event: completion\ndata: " + body + "\n\n";
    std::string wire;
    wire.reserve(event.size() * workload.events);
    for (int i = 0; i < workload.events; ++i)
        wire += event;

    SseParser parser;
    int received = 0;
    bool correct = true;
    const std::function<void(const SseParser::Event&)> collect = [&](const auto& value) {
        ++received;
        correct = correct && value.event == "completion" && value.data == body;
    };
    const auto started = Clock::now();
    for (int pass = 0; pass < passes; ++pass) {
        for (std::size_t offset = 0; offset < wire.size(); offset += workload.chunk_bytes) {
            parser.feed(std::string_view(wire).substr(offset, workload.chunk_bytes), collect);
        }
    }
    parser.finish(collect);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    if (!correct || received != workload.events * passes)
        throw std::runtime_error("SSE event mismatch");
    return ms;
}
}   // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 5)
            throw std::runtime_error(
                "usage: stream_performance OUTPUT [REPEATS [PASSES [all|short]]]");
        const int repeats = argc > 2 ? std::clamp(std::stoi(argv[2]), 3, 20) : 7;
        const int passes = argc > 3 ? std::clamp(std::stoi(argv[3]), 1, 64) : 16;
        const std::string_view group = argc > 4 ? argv[4] : "all";
        if (group != "all" && group != "short") throw std::runtime_error("unknown workload group");
        const std::vector<Workload> workloads = {
            {"short_fragmented", 256, 16, 4096},
            {"short_transport_chunks", 256, 4096, 4096},
            {"short_whole_stream", 256, 2 * 1024 * 1024, 4096},
            {"large_fragmented", 65536, 16, 16},
            {"large_medium_chunks", 65536, 128, 16},
            {"large_transport_chunks", 65536, 4096, 16},
            {"large_whole_stream", 65536, 2 * 1024 * 1024, 16},
        };
        const auto output = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(output.parent_path());
        std::ofstream report(output, std::ios::binary);
        if (!report) throw std::runtime_error("cannot open result file");
        report << "{\"schema\":1,\"scope\":\"SSE parser only; no model decoding\",\"passes\":"
               << passes << ",\"cases\":[";
        bool first_case = true;
        int checked = 0;
        for (const auto& workload : workloads) {
            if (group == "short" && !workload.name.starts_with("short_")) continue;
            measure(workload, passes);   // Untimed warm-up; checks the complete payload.
            std::vector<double> samples;
            for (int run = 0; run < repeats; ++run)
                samples.push_back(measure(workload, passes));
            auto ordered = samples;
            std::sort(ordered.begin(), ordered.end());
            const double median =
                (ordered[(ordered.size() - 1) / 2] + ordered[ordered.size() / 2]) / 2;
            if (!first_case) report << ',';
            first_case = false;
            report << "{\"name\":\"" << workload.name << "\",\"body_bytes\":" << workload.body_bytes
                   << ",\"chunk_bytes\":" << workload.chunk_bytes
                   << ",\"events\":" << workload.events
                   << ",\"correct\":true,\"median_ms\":" << median << ",\"samples_ms\":[";
            for (std::size_t i = 0; i < samples.size(); ++i) {
                if (i) report << ',';
                report << samples[i];
            }
            report << "]}";
            ++checked;
        }
        report << "]}\n";
        if (!report) throw std::runtime_error("cannot save results");
        std::cout << checked << " SSE workloads checked; results saved.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
