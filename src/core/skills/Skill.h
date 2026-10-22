#pragma once
// ---------------------------------------------------------------------------
// Skills: procedural knowledge as Markdown.
//
// A skill is a short, narrow procedure the agent should follow for a particular
// kind of work - how to read a CMake error, how to prove a change actually
// works, how to touch git without destroying someone's uncommitted edits. They
// live on disk as SKILL.md files so a user can read, edit and add them without
// rebuilding anything.
//
// Skills are selected, never bulk-injected. Pushing every skill into every
// request would swamp the context of exactly the small local models this
// application exists to serve. One to three relevant skills is the budget.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <string>
#include <vector>

#include "../util/Result.h"

namespace localcode {

struct Skill {
    std::string id;                       // stable key, matches the directory name
    std::string name;
    std::string description;              // one line, used for selection
    std::vector<std::string> tags;
    // Words and phrases that make this skill relevant. Matched case-insensitively
    // against the objective and the recent conversation.
    std::vector<std::string> triggers;
    // Tools this skill expects to be available. Purely informational: a skill
    // never grants access to anything.
    std::vector<std::string> tools;

    int  priority = 50;                   // ties break toward the higher number
    bool enabled = true;
    // Always-on skills are included regardless of the query. Reserve this for
    // the handful that apply to every coding task.
    bool always = false;
    // Skills authored at runtime by create_skill start untrusted and are not
    // selected until a human marks them reviewed.
    bool validated = true;

    std::string body;                     // the Markdown procedure itself
    std::filesystem::path source_path;

    // Renders the skill for inclusion in a prompt.
    [[nodiscard]] std::string to_prompt_section() const;

    // Structural checks: required fields, a non-trivial body, sane triggers.
    // Returns the list of problems; empty means valid.
    [[nodiscard]] std::vector<std::string> validate() const;
};

// Parses a SKILL.md file: a YAML-subset frontmatter block delimited by '---'
// followed by the Markdown body.
Result<Skill> parse_skill(std::string_view text, const std::filesystem::path& source);

// Serialises a skill back to the SKILL.md format, used by create_skill.
[[nodiscard]] std::string serialize_skill(const Skill& skill);

}  // namespace localcode
