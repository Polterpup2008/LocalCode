// ---------------------------------------------------------------------------
// Skill tools: reading the available procedures, and writing new ones.
//
// A skill the agent writes for itself starts unvalidated. It is saved, it shows
// up in the skills panel, and it is not selected for any task until a human has
// read it and marked it reviewed. That is the whole point of the flag: an agent
// that can silently install its own instructions is an agent that can silently
// change its own behaviour.
// ---------------------------------------------------------------------------
#include "../skills/SkillRegistry.h"
#include "../util/Log.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

// --- list_skills -----------------------------------------------------------
class ListSkillsTool final : public ITool {
public:
    std::string name() const override { return "list_skills"; }
    bool requires_workspace() const override { return false; }

    std::string description() const override {
        return "Lists the procedures available in this installation, with what each is for. The "
               "ones relevant to the current task are already included in your instructions; use "
               "this to check what exists before writing a new one with create_skill.";
    }

    Json parameters_schema() const override { return schema::object({}); }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (!context.skills) {
            return ToolResult::failure("skills unavailable",
                                       "The skill registry is not available in this session.");
        }
        const auto& skills = context.skills->skills();
        if (skills.empty()) {
            return ToolResult::success("No skills installed", "No skills are installed.");
        }

        std::string report = std::to_string(skills.size()) + " skill(s):\n";
        for (const auto& skill : skills) {
            report += "\n  " + skill.id;
            if (!skill.enabled) report += "  [disabled]";
            if (!skill.validated) report += "  [awaiting review]";
            report += "\n    " + skill.description + "\n";
            if (!skill.triggers.empty()) {
                report += "    triggers: " + strings::join(skill.triggers, ", ") + "\n";
            }
        }
        return ToolResult::success(std::to_string(skills.size()) + " skill(s) installed", report);
    }
};

// --- create_skill ----------------------------------------------------------
class CreateSkillTool final : public ITool {
public:
    std::string name() const override { return "create_skill"; }
    bool mutates() const override { return true; }
    bool requires_workspace() const override { return false; }

    std::string description() const override {
        return "Writes a new reusable procedure for a kind of work that recurs and that the "
               "existing skills do not already cover. Call list_skills first and do not duplicate "
               "one that exists. A good skill is narrow, concrete and about procedure rather than "
               "facts: when it applies, the exact steps in order, how to tell that it worked, and "
               "the ways it usually goes wrong. The new skill is saved but is NOT used until a "
               "person reviews it, so do not rely on it during this task.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"id", schema::string_param(
                        "Short kebab-case identifier, e.g. 'qt-signal-debug'. Letters, digits, "
                        "'-' and '_' only.")},
             {"name", schema::string_param("Human-readable title.")},
             {"description", schema::string_param(
                                 "One line saying when this procedure applies. This is what the "
                                 "selector matches against, so make it concrete.")},
             {"triggers", schema::array_of_strings(
                              "Words and phrases that indicate this procedure is relevant, e.g. "
                              "['linker error', 'unresolved external']. At least three characters "
                              "each.")},
             {"tags", schema::array_of_strings("Topic tags, e.g. ['cpp', 'cmake'].")},
             {"tools", schema::array_of_strings(
                           "Tools this procedure expects to use. Informational only.")},
             {"procedure", schema::string_param(
                               "The body, in Markdown. Include: when to use it, the numbered "
                               "steps, how to verify the result, and the common failure modes.")},
             {"priority", schema::integer_param("0-100, default 50.", 0, 100)}},
            {"id", "name", "description", "procedure"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (!context.skills) {
            return ToolResult::failure("skills unavailable",
                                       "The skill registry is not available in this session.");
        }

        Skill skill;
        skill.id = strings::to_lower(strings::trim(arg_string(arguments, "id")));
        skill.name = arg_string(arguments, "name");
        skill.description = arg_string(arguments, "description");
        skill.triggers = arg_strings(arguments, "triggers");
        skill.tags = arg_strings(arguments, "tags");
        skill.tools = arg_strings(arguments, "tools");
        skill.body = arg_string(arguments, "procedure");
        skill.priority = static_cast<int>(arg_int(arguments, "priority", 50));
        skill.enabled = true;
        // Deliberate: a skill the model wrote is not trusted until reviewed.
        skill.validated = false;

        if (const auto* existing = context.skills->find(skill.id)) {
            return ToolResult::failure(
                "a skill with that id already exists",
                "'" + skill.id + "' already exists: " + existing->description +
                    "\nEither pick a different id, or leave the existing procedure alone. Do not "
                    "duplicate skills.");
        }

        const auto problems = skill.validate();
        if (!problems.empty()) {
            return ToolResult::failure(
                "the skill did not validate",
                "The skill was not saved:\n  " + strings::join(problems, "\n  ") +
                    "\nFix these and call create_skill again.");
        }

        auto saved = context.skills->save_user_skill(skill);
        if (!saved) return ToolResult::failure("could not save the skill", saved.error().to_string());

        log_info(LogChannel::Agent, "agent created a skill",
                 {{"id", skill.id}, {"validated", "false"}});

        auto result = ToolResult::success(
            "Created skill '" + skill.id + "' (awaiting review)",
            "Saved the skill '" + skill.id + "'.\n"
            "It is marked as awaiting review and will NOT be used until a person enables it in "
            "the Skills panel, so do not rely on it for the current task.");
        result.structured = {{"id", skill.id}, {"validated", false}};
        return result;
    }
};

}  // namespace

std::vector<ToolPtr> make_skill_tools() {
    return {std::make_shared<ListSkillsTool>(), std::make_shared<CreateSkillTool>()};
}

}  // namespace localcode
