#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

// A real, harmless owned process for lifecycle tests. It is not an HTTP/model
// substitute: tests supply a health probe which checks this process is alive.
int main(int argc, char** argv) {
    if (argc > 1 && (std::string_view(argv[1]) == "--fixture-cpu-allowance" ||
                     std::string_view(argv[1]) == "--fixture-cpu-allowance-wait")) {
        std::vector<unsigned int> cpus;
#if defined(_WIN32)
        DWORD_PTR allowed = 0, system = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return 12;
        for (unsigned int cpu = 0; cpu < sizeof(allowed) * 8; ++cpu) {
            if (allowed & (DWORD_PTR{1} << cpu)) cpus.push_back(cpu);
        }
#elif defined(__linux__)
        cpu_set_t allowed{};
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return 12;
        for (unsigned int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
        }
#else
        return 13;
#endif
        std::cout << "cpus=";
        bool first = true;
        for (const auto cpu : cpus) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << cpu;
        }
        std::cout << '\n' << std::flush;
        if (std::string_view(argv[1]) == "--fixture-cpu-allowance") return 0;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg.starts_with("--query-gpu=")) {
            if (const auto* marker = std::getenv("LOCALCODE_GPU_PROBE_MARKER")) {
                const auto text = std::string_view(marker);
                const std::u8string path(reinterpret_cast<const char8_t*>(text.data()),
                                         text.size());
                std::ofstream(std::filesystem::path(path), std::ios::app) << "query\n";
            }
            const auto* delay = std::getenv("LOCALCODE_GPU_FIXTURE_DELAY_MS");
            std::this_thread::sleep_for(std::chrono::milliseconds(delay ? std::atoi(delay) : 200));
            std::cout << "Fixture GPU, 1024, 0\n";
            return 0;
        }
        if (arg == "--help") {
            const auto stem = std::filesystem::path(argv[0]).stem().string();
            if (stem.starts_with("failed-help-fixture")) {
                std::cout << "  --model FNAME --host HOST --port N --jinja --threads N\n";
                return 7;
            }
            if (stem.starts_with("oversized-help-fixture")) {
                std::cout << "  --model FNAME --host HOST --port N --jinja --threads N\n"
                          << std::string(600 * 1024, 'x');
                return 0;
            }
            if (stem.starts_with("prefix-only-help-fixture")) {
                std::cout << "  --threads-batch N  batch threads only\n"
                             "  --cpu-mask-batch M  batch affinity only\n"
                             "  --cpu-strict-batch N  batch affinity strictness only\n"
                             "  --device-list  list devices\n"
                             "  --parallel-test N  unrelated option\n"
                             "  --flash-attn-mode N  unrelated option\n"
                             "  --other N  use --mlock only elsewhere\n"
                             "Example: --spec-type ngram-mod\n";
                return 0;
            }
            if (stem.starts_with("exact-options-help-fixture")) {
                std::cout << "  --threads-batch N  batch threads\n"
                             "  -t, --threads=N  generation threads\n"
                             "  --cpu-mask-batch M  batch affinity\n"
                             "  --cpu-mask M  generation affinity\n"
                             "  --cpu-strict-batch N  batch strictness\n"
                             "  --cpu-strict N  generation strictness\n"
                             "  -d, --device=DEVICES  devices\n"
                             "  --parallel N  parallel slots\n"
                             "  -fa, --flash-attn=BOOL  attention\n"
                             "  --cache-ram=MiB  cache memory\n"
                             "  --mlock  Lock model memory\n";
                return 0;
            }
            std::cout << "LocalCode process fixture\n  --version  print the build version\n";
            std::cout << "  --device DEVICES  comma-separated devices to use, or none for CPU\n";
            std::cout << "  --spec-type none,draft-simple";
            const auto binary = std::filesystem::path(argv[0]);
            if (binary.stem().string().starts_with("ngram-fixture")) {
                // A data-only stand-in for capability definitions supplied by a
                // shared library beside a thin runtime launcher. Never loaded as code.
                std::string mode;
                std::ifstream(binary.parent_path() / "fixture-capabilities.dll") >> mode;
                if (mode != "disabled") std::cout << ",ngram-mod";
                std::ofstream(binary.parent_path() / "help-count.txt", std::ios::app) << "help\n";
            }
            std::cout << "\n";
            return 0;
        }
        if (arg == "--version") {
            std::cout << "LocalCode lifecycle fixture 1\n";
            return 0;
        }
        if (arg == "--fixture-exit") {
            std::cerr << "fixture startup refused\n";
            return 7;
        }
        if (arg == "--fixture-long-log") {
            for (int line = 0; line < 3000; ++line)
                std::cout << "fixture line\n";
            std::cout << std::string(80 * 1024, 'x') << std::flush;
        }
    }
    std::cout << "owned lifecycle fixture running\n" << std::flush;
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
