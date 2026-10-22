// Process/format fixture only. Its invented rates are never performance evidence.
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#ifdef LOCALCODE_BENCH_UTF8_FIXTURE
int llama_bench(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    const auto* mode = std::getenv("LOCALCODE_BENCH_FIXTURE_MODE");
    const std::string scenario = mode ? mode : "normal";
    const bool repacking = scenario.rfind("repack", 0) == 0;
    const bool placement = scenario.rfind("placement", 0) == 0;
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args.front() == "--help") {
        std::cout << "--device --n-gpu-layers --no-kv-offload --threads --n-depth --output "
                     "--repetitions --prio --no-op-offload --offline\n";
        if (repacking)
            std::cout << (scenario == "repack-bare" ? "--repack --no-repack\n"
                                                    : "--repack <0|1>\n");
        if (placement && scenario != "placement-unsupported")
            std::cout << "--cpu-mask <hex> --cpu-strict <0|1>\n";
        return 0;
    }
    auto value = [&](const std::string& flag) {
        for (std::size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == flag) return args[i + 1];
        return std::string{};
    };
    if (value("--device") != "none" || value("--n-gpu-layers") != "0" ||
        value("--no-kv-offload") != "1" || value("--prio") != "0" || value("-o") != "json" ||
        !std::ifstream(std::filesystem::u8path(value("-m"))).good())
        return 9;
    if (scenario == "hang") std::this_thread::sleep_for(std::chrono::seconds(20));
    if (scenario == "delay") std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (scenario == "error") {
        std::cerr << "fixture runtime failure\n";
        return 7;
    }
    if (scenario == "bad-json") {
        std::cout << "not measurements";
        return 0;
    }
    if (scenario == "huge") {
        std::cout << std::string(5 * 1024 * 1024, 'x');
        return 0;
    }
    const int threads = std::stoi(value("--threads"));
    std::vector<unsigned int> allowed_cpus;
#if defined(_WIN32)
    DWORD_PTR allowed = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return 12;
    for (unsigned int cpu=0;cpu<sizeof(allowed)*8;++cpu)
        if (allowed & (DWORD_PTR{1} << cpu)) allowed_cpus.push_back(cpu);
#elif defined(__linux__)
    cpu_set_t allowed{};
    if (sched_getaffinity(0,sizeof(allowed),&allowed)!=0) return 12;
    for (unsigned int cpu=0;cpu<CPU_SETSIZE;++cpu)
        if (CPU_ISSET(cpu,&allowed)) allowed_cpus.push_back(cpu);
#endif
    std::string actual_allowance="[";
    for (const auto cpu:allowed_cpus) {
        if (actual_allowance.size()>1) actual_allowance+=',';
        actual_allowance+=std::to_string(cpu);
    }
    actual_allowance+=']';
    const auto repackValue = value("--repack");
    const auto cpuMask = value("--cpu-mask");
    if (!cpuMask.empty() && (!placement || value("--cpu-strict") != "1")) return 11;
    if (!repackValue.empty() && (!repacking || (repackValue != "0" && repackValue != "1")))
        return 10;
    std::string json = "[";
    for (int index = 0; index < 2; ++index) {
        if (index) json += ',';
        json += "{\"n_threads\":" + std::to_string(scenario == "mismatch" ? 999 : threads) +
                ",\"n_gpu_layers\":0,\"n_depth\":0,\"devices\":\"none\",\"no_kv_offload\":true,"
                "\"model_filename\":\"" +
                value("-m") + "\",\"n_prompt\":" + (index ? "0" : value("-p")) +
                ",\"n_gen\":" + (index ? value("-n") : "0") + ",\"avg_ts\":" +
                std::to_string(index ? 40.0 / threads
                                     : ((scenario == "split" || scenario == "repack-split")
                                            ? 100.0 * threads : 100.0 / threads)) +
                ",\"stddev_ts\":0.5,\"process_cpu_allowance\":"+actual_allowance;
        if (repacking && scenario != "repack-missing") {
            const bool enabled = repackValue != "0";
            json += ",\"repack\":";
            if (scenario == "repack-numeric") json += enabled ? "1" : "0";
            else json += (scenario == "repack-mismatch" ? !enabled : enabled) ? "true" : "false";
        }
        if (placement && scenario != "placement-missing") {
            json += ",\"cpu_mask\":\"" + (scenario == "placement-mask" ? std::string("0x0") : cpuMask) + "\",\"cpu_strict\":";
            json += scenario == "placement-numeric" ? "1" : scenario == "placement-false" ? "false" : "true";
        }
        json += '}';
    }
    json += ']';
    // Exercise fragmented delivery without conflating it with malformed output.
    for (const char letter : json) {
        std::cout << letter << std::flush;
    }
    return 0;
}
