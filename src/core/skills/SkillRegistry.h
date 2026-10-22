#pragma once
// ---------------------------------------------------------------------------
// Skill discovery and selection.
//
// Two directories are scanned: the skills shipped with the application and the
// user's own under the data directory. A user skill with the same id shadows the
// built-in one, so anyone can override a shipped procedure without editing the
// installation.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "Skill.h"

namespace localcode {

struct SkillMatch {
    const Skill* skill = nullptr;
    double       score = 0.0;
    std::string  reason;   // which trigger or tag matched, for the activity log
};

class SkillRegistry {
public:
    SkillRegistry() = default;

    // Loads every SKILL.md under `directory` (one level of subdirectories, plus
    // a SKILL.md directly in the directory itself). Later loads shadow earlier
    // ones by id.
    Status load_directory(const std::filesystem::path& directory);

    // Loads built-in skills followed by user skills.
    Status load_default_locations();

    void clear();

    [[nodiscard]] const std::vector<Skill>& skills() const { return skills_; }
    [[nodiscard]] const std::vector<std::string>& load_errors() const { return errors_; }
    [[nodiscard]] const Skill* find(std::string_view id) const;

    void set_enabled(std::string_view id, bool enabled);

    // Adds or replaces a skill in memory and writes it to the user directory.
    Status save_user_skill(const Skill& skill);

    // Scores every enabled, validated skill against the text and returns the
    // best `max_results`. `always` skills are always included and sort first.
    [[nodiscard]] std::vector<SkillMatch> select(std::string_view objective,
                                                 std::string_view recent_context,
                                                 std::size_t max_results = 3) const;

    // Renders the selected skills as one prompt block.
    //
    // `max_tokens` bounds the result. Skills are the second-largest fixed cost
    // in a request after the tool schemas, and on a small context window an
    // unbounded block crowds out the conversation entirely - which showed up in
    // testing as an 8k model that could not accept any message at all. Lower
    // scoring skills are dropped first; a skill that still does not fit is
    // included as its opening section rather than omitted, because knowing that
    // a procedure exists is most of its value.
    [[nodiscard]] static std::string render(const std::vector<SkillMatch>& matches,
                                            int max_tokens = 0);

private:
    void insert(Skill skill);

    mutable std::mutex       mutex_;
    std::vector<Skill>       skills_;
    std::vector<std::string> errors_;
};

}  // namespace localcode
