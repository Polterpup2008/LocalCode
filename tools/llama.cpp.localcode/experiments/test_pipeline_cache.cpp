#include "localcode-pipeline-cache.h"
#include <iostream>
#include <thread>
#include <atomic>

#ifdef _WIN32
int wmain(int argc, wchar_t ** argv) {
#else
int main(int argc, char ** argv) {
#endif
    using namespace localcode_vk_cache;
    if (argc != 2) return 2;
    int checks = 0;
    auto require = [&](bool ok) { ++checks; if (!ok) { std::cerr << "check failed " << checks << '\n'; std::exit(1); } };
    key k{0x1234, 0x5678, 345, sizeof(void *), {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}};
    bytes raw;
    put(raw, 32, 4); put(raw, 1, 4); put(raw, k.vendor, 4); put(raw, k.device, 4);
    raw.insert(raw.end(), k.uuid.begin(), k.uuid.end());
    raw.insert(raw.end(), 200, 42);
    const auto encoded = encode(raw, k);
    require(decode(encoded, k) == raw);
    for (size_t i = 0; i < encoded.size(); ++i) {
        auto bad = encoded; bad[i] ^= 1; require(decode(bad, k).empty());
    }
    for (size_t i = 0; i < encoded.size(); ++i)
        require(decode(bytes(encoded.begin(), encoded.begin() + i), k).empty());
    auto other = k; ++other.driver; require(decode(encoded, other).empty());
    other = k; ++other.vendor; require(decode(encoded, other).empty());
    other = k; ++other.device; require(decode(encoded, other).empty());
    other = k; other.pointer_size = 4; require(decode(encoded, other).empty());
    other = k; ++other.uuid[0]; require(decode(encoded, other).empty());
    auto bad_raw = raw; bad_raw[0] = 0; require(encode(bad_raw, k).empty());
    const auto folder = std::filesystem::path(argv[1]);
    const auto file = folder / std::filesystem::u8path("cache é.bin");
    require(load(file, k).empty());
    require(save(file, raw, k, "initial"));
    require(load(file, k) == raw);
    require(!save(file, {}, k, "invalid"));
    require(load(file, k) == raw);
    require(!save(file / "not-a-directory", raw, k, "blocked"));
    require(load(file, k) == raw);
    auto alternate = raw; alternate.back() = 77;
    std::atomic<int> misses{0};
    std::thread a([&] { for (int i = 0; i < 10; ++i) if (!save(file, raw, k, "one")) ++misses; });
    // Each writer uses its own temporary name; readers must see a complete
    // compatible file, even if concurrent processes replace the shared cache.
    for (int i = 0; i < 10; ++i) {
        if (!save(file, alternate, k, "two")) ++misses;
        const auto loaded = load(file, k);
        if (!loaded.empty() && loaded != raw && loaded != alternate) return 4;
    }
    a.join();
    require(!load(file, k).empty());
    {
        std::ofstream too_large(folder / "oversized.bin", std::ios::binary);
        too_large.seekp(max_bytes + header_bytes); too_large.put('x');
    }
    require(load(folder / "oversized.bin", k).empty());
    std::cout << checks << " cache format/persistence checks passed\n";
}
