// ---------------------------------------------------------------------------
// Saying what "done" means, before doing it.
//
// A definition of done written after the work is finished is not a definition;
// it is a description of whatever happened. Run 9 built a program, ran it, and
// then decided that having run it was what the task required.
//
// So the agent states its acceptance criteria up front and LocalCode holds
// them. The tool itself is deliberately inert - it records a commitment and
// returns it in the structured result, and the agent folds it into the
// verification ledger. Whether each criterion is met is decided later, by
// evidence, not here.
// ---------------------------------------------------------------------------
#include "../util/Log.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

class SetAcceptanceCriteriaTool final : public ITool {
public:
    std::string name() const override { return "set_acceptance_criteria"; }
    bool requires_workspace() const override { return false; }

    std::string description() const override {
        return "States what has to be true for the user's request to be satisfied, before you "
               "start working on it. Call this once, early, on any task that changes code. "
               "Write each criterion as something that can be checked by running something or "
               "by a read_file content expectation - "
               "\"the program prints the first ten Fibonacci numbers\", not \"the code is "
               "correct\". LocalCode holds these criteria and will not let the run be reported "
               "as finished until each one has been settled by actual evidence, so vague "
               "criteria only make the run harder to finish.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"criteria",
              schema::array_of_strings(
                  "The checkable conditions for this task being done. Two to five of them is "
                  "usually right. Each must describe an observable outcome, not an action you "
                  "intend to take.")}},
            {"criteria"});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        (void)context;
        auto criteria = arg_strings(arguments, "criteria");

        // Empty and whitespace-only entries removed rather than accepted: a
        // blank criterion would be settled by nothing and block the run
        // forever, which reads as a LocalCode bug rather than a model mistake.
        std::vector<std::string> kept;
        for (auto& criterion : criteria) {
            auto trimmed = strings::trim(criterion);
            if (trimmed.empty()) continue;
            kept.push_back(strings::utf8_truncate(trimmed, 300));
        }
        if (kept.empty()) {
            return ToolResult::failure(
                "no criteria given",
                "Pass 'criteria' as a list of checkable statements, for example "
                "[\"the project builds with no errors\", \"running the program prints the "
                "first ten Fibonacci numbers\"].");
        }

        std::string report = "Acceptance criteria recorded. This run is not finished until each "
                             "of these has been settled by evidence:\n";
        for (std::size_t i = 0; i < kept.size(); ++i) {
            report += "  " + std::to_string(i + 1) + ". " + kept[i] + "\n";
        }
        report +=
            "\nTo settle one that is about what the program does, run the program with "
            "run_command and pass 'expect' describing the output the criterion requires. "
            "LocalCode does the comparison. For source or documentation contents, use read_file "
            "with expect.contains_all (literal snippets) and expect.criterion (the number). "
            "That settles a content criterion without claiming the program was run.";

        ToolResult result = ToolResult::success(
            "Acceptance criteria set (" + std::to_string(kept.size()) + ")", report);
        result.structured = {{"acceptance_criteria", kept}};
        return result;
    }
};

}  // namespace

std::vector<ToolPtr> make_verification_tools() {
    std::vector<ToolPtr> tools;
    tools.push_back(std::make_shared<SetAcceptanceCriteriaTool>());
    return tools;
}

}  // namespace localcode
