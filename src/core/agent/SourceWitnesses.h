#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../platform/FileSystem.h"
#include "../tools/Tool.h"
#include "ContextManager.h"

namespace localcode {

struct SourceObservation {
    bool new_evidence = false;
    std::string guidance;
};

// Task-local coverage of complete source lines actually returned by file tools.
// Size/mtime detects ordinary edits; it is not atomic filesystem confinement or
// proof that the model understood the source. Lexical RI leads never enter here.
class SourceWitnesses {
public:
    void reset(std::filesystem::path root);
    SourceObservation observe(const ToolResult& result, std::string_view actual_tool_name = {},
                              std::string_view tool_call_id = {});
    void invalidate(const std::string& path);
    [[nodiscard]] std::vector<std::string> citation_problems(const std::string& answer) const;
    [[nodiscard]] std::string render() const;
    [[nodiscard]] std::vector<SourceAnchor> anchors() const;
    [[nodiscard]] std::string navigation_checkpoint() const;
    // Called only for an already-authorized write path. New files need no
    // source observation. Existing files require complete current coverage.
    [[nodiscard]] ToolResult check_whole_file_write(const std::filesystem::path& path) const;

private:
    struct Witness {
        std::string path;
        std::string key;
        fs::SourceVersion version;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;

        // Bounded recent receipt slices for retention, separate from coverage.
        struct ReadWindow {
            std::size_t first;
            std::size_t last;
            std::string tool_call_id;
        };

        std::vector<ReadWindow> recent_windows;
        std::size_t previous_start = 0;
        std::size_t previous_complete_end = 0;
        std::string previous_hash;
        int adjacent_windows = 0;
        bool paging_advised = false;
        std::optional<std::size_t> eof_line;
        // Write eligibility excludes synthetic/search/model-provided receipts.
        std::vector<std::pair<std::size_t, std::size_t>> write_ranges;
        std::optional<std::size_t> write_eof_line;
    };

    struct NavigationCheckpoint {
        std::string key;
        fs::SourceVersion version;
        std::string instruction;
    };

    [[nodiscard]] std::string key_for(const std::string& path) const;
    std::filesystem::path root_;
    std::vector<Witness> files_;
    std::optional<NavigationCheckpoint> navigation_checkpoint_;
};

}   // namespace localcode
