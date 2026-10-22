#pragma once
// A scratch directory that removes itself. Every test that touches the
// filesystem uses one, so a failing test cannot leave debris behind that makes
// the next run fail for a different reason.
#include <filesystem>
#include <string>

namespace localcode::test {

class TempDirectory {
public:
    explicit TempDirectory(std::string label = "lc-test");
    ~TempDirectory();

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path operator/(const std::string& relative) const {
        return path_ / relative;
    }

    // Creates a file with the given contents, making parent directories.
    std::filesystem::path write(const std::string& relative, std::string_view contents) const;
    std::filesystem::path make_directory(const std::string& relative) const;

private:
    std::filesystem::path path_;
};

}  // namespace localcode::test
