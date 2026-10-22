// Windows' narrow CRT argv uses the system code page. llama.cpp file APIs
// expect UTF-8, so convert the original wide arguments before entering it.
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

int llama_bench(int argc, char** argv);

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int wmain(int argc, wchar_t** wide) {
    std::vector<std::string> storage;
    storage.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                              wide[index], -1, nullptr, 0, nullptr, nullptr);
        if (bytes == 0) {
            std::fputs("Invalid Unicode command-line argument.\n", stderr);
            return 2;
        }
        std::string text(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], -1,
                                text.data(), bytes, nullptr, nullptr) != bytes) {
            std::fputs("Cannot convert command-line argument to UTF-8.\n", stderr);
            return 2;
        }
        text.pop_back();
        storage.push_back(std::move(text));
    }
    std::vector<char*> args;
    args.reserve(storage.size() + 1);
    for (auto& text : storage) args.push_back(text.data());
    args.push_back(nullptr);
    // Upstream's --help exits from its parser, so print before entering it.
    if (argc == 2 && storage[1] == "--help") {
        std::fputs("LocalCode Windows UTF-8 argv v1\n", stdout);
        std::fflush(stdout);
    }
    return llama_bench(argc, args.data());
}
#else
int main(int argc, char** argv) {
    return llama_bench(argc, argv);
}
#endif
