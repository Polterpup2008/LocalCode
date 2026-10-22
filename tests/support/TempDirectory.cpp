#include "TempDirectory.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>

namespace localcode::test {
namespace {

std::string unique_suffix() {
    static std::atomic<unsigned> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(now % 1000000) + "-" + std::to_string(counter.fetch_add(1));
}

}  // namespace

TempDirectory::TempDirectory(std::string label) {
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    path_ = base / (label + "-" + unique_suffix());
    std::filesystem::create_directories(path_, ec);
}

TempDirectory::~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::filesystem::path TempDirectory::write(const std::string& relative,
                                           std::string_view contents) const {
    const auto full = path_ / relative;
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return full;
}

std::filesystem::path TempDirectory::make_directory(const std::string& relative) const {
    const auto full = path_ / relative;
    std::error_code ec;
    std::filesystem::create_directories(full, ec);
    return full;
}

}  // namespace localcode::test
