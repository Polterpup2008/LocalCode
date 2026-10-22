// ---------------------------------------------------------------------------
// Telling the model what is actually installed.
//
// The point of this tool is to make "you need to install CMake" an answer that
// can only be given after looking. A small model asked to build a C++ project
// will otherwise guess, and guess wrong, on a machine where the toolchain is
// present but not on PATH.
// ---------------------------------------------------------------------------
#include "../platform/Environment.h"
#include "../util/Log.h"
#include "ToolRegistry.h"
#include "ToolSupport.h"

namespace localcode {
namespace {

using namespace tool_support;

class EnvironmentInfoTool final : public ITool {
public:
    std::string name() const override { return "environment_info"; }
    bool requires_workspace() const override { return false; }

    std::string description() const override {
        return "Reports the development environment of this machine: which build tools, "
               "compilers, language runtimes and SDKs are installed, where they are, and what "
               "version they report. On Windows this includes Visual Studio installations, their "
               "MSVC toolsets and the Windows SDKs. Call this before deciding how to build a "
               "project, and before ever telling the user that something is missing - say a tool "
               "is unavailable only when this tool says so.";
    }

    Json parameters_schema() const override {
        return schema::object(
            {{"refresh", schema::boolean_param(
                             "Re-scan instead of using the cached result. Use after the user "
                             "installs something. Defaults to false.",
                             false)},
             {"tool", schema::string_param(
                          "Ask about one tool only, e.g. 'cmake'. Omit for the full report.")}});
    }

    ToolResult execute(const Json& arguments, ToolContext& context) override {
        if (!context.environment) {
            return ToolResult::failure("environment service unavailable",
                                       "Environment detection is not available in this session.");
        }

        const bool refresh = arg_bool(arguments, "refresh", false);
        const auto single = arg_string(arguments, "tool");

        if (!single.empty()) {
            // Things the tool catalogue has no id for, but the report does know
            // about. Asked about "visual studio" this answered "Visual Studio is
            // not installed" on a machine with VS 2022 and a full MSVC
            // toolchain - a false statement about the environment, and enough to
            // send a small model off to write a Cargo.toml instead.
            const auto lowered = strings::to_lower(single);
            if (lowered.find("visual studio") != std::string::npos || lowered == "vs" ||
                lowered == "msvc" || lowered == "devenv") {
                const auto& full = context.environment->report();
                if (!full.visual_studio.empty()) {
                    std::string text;
                    for (const auto& vs : full.visual_studio) {
                        text += vs.display_name + " " + vs.version + "\n";
                        text += "  path:     " + vs.install_path.string() + "\n";
                        if (!vs.msbuild.empty()) {
                            text += "  msbuild:  " + vs.msbuild.string() + "\n";
                        }
                        if (!vs.vcvars64.empty()) {
                            text += "  vcvars64: " + vs.vcvars64.string() + "\n";
                        }
                        if (!vs.msvc_toolsets.empty()) {
                            text += "  MSVC toolsets: " +
                                    strings::join(vs.msvc_toolsets, ", ") + "\n";
                        }
                    }
                    auto found = ToolResult::success("Visual Studio is installed", text);
                    found.evidence.push_back(make_evidence(
                        EvidenceKind::EnvironmentChecked, "visual studio",
                        full.visual_studio.front().install_path.string(), true));
                    return found;
                }
            }

            const auto info = context.environment->detect_tool(single);
            std::string report;
            if (info.found) {
                report = info.display_name + " is installed.\n";
                report += "  path:    " + info.path.string() + "\n";
                if (!info.version.empty()) report += "  version: " + info.version + "\n";
                report += "  source:  " + info.source + "\n";
                if (!info.alternatives.empty()) {
                    report += "  other copies:\n";
                    for (const auto& other : info.alternatives) {
                        report += "    " + other.string() + "\n";
                    }
                }
            } else if (info.display_name.empty()) {
                // Unknown to the catalogue is not the same as absent from the
                // machine, and must never be reported as though it were.
                report = "LocalCode does not check for '" + single +
                         "', so this says nothing about whether it is installed. Look for it "
                         "with find_executable, or by searching its usual install directory.\n"
                         "Tools LocalCode does check: " +
                         strings::join(EnvironmentService::known_tool_ids(), ", ");
            } else {
                report = info.display_name +
                         " was not found on PATH or in the install locations LocalCode knows "
                         "about. " + info.note;
            }
            auto result = ToolResult::success(
                info.found                  ? single + " is available"
                : info.display_name.empty() ? single + " is not something LocalCode checks"
                                            : single + " was not found",
                report);
            result.evidence.push_back(make_evidence(EvidenceKind::EnvironmentChecked, single,
                                                    info.found ? info.path.string() : "not found",
                                                    info.found));
            return result;
        }

        const auto& report = context.environment->report(refresh);

        std::string text;
        text += "Operating system: " + report.os.name + " " + report.os.version + " (" +
                report.os.architecture + "), " + std::to_string(report.os.logical_cpus) +
                " logical CPUs, " + strings::human_bytes(report.os.total_memory_bytes) +
                " RAM\n\n";

        std::vector<std::string> installed;
        std::vector<std::string> missing;
        for (const auto& tool : report.tools) {
            if (tool.found) {
                std::string line = "  " + tool.id + ": " + tool.path.string();
                if (!tool.version.empty()) line += "  [" + tool.version + "]";
                if (tool.source != "PATH") line += "  (" + tool.source + ")";
                installed.push_back(line);
            } else {
                missing.push_back(tool.id);
            }
        }

        text += "Installed (" + std::to_string(installed.size()) + "):\n";
        text += strings::join(installed, "\n") + "\n";
        if (!missing.empty()) {
            text += "\nNot found: " + strings::join(missing, ", ") + "\n";
        }

        if (!report.visual_studio.empty()) {
            text += "\nVisual Studio:\n";
            for (const auto& vs : report.visual_studio) {
                text += "  " + vs.display_name + " " + vs.version + "\n";
                text += "    path: " + vs.install_path.string() + "\n";
                if (!vs.msvc_toolsets.empty()) {
                    text += "    MSVC toolsets: " + strings::join(vs.msvc_toolsets, ", ") + "\n";
                }
                if (!vs.msbuild.empty()) text += "    msbuild: " + vs.msbuild.string() + "\n";
                if (!vs.vcvars64.empty()) {
                    text += "    developer environment: " + vs.vcvars64.string() + "\n";
                }
            }
        }

        if (!report.windows_sdks.empty()) {
            std::vector<std::string> versions;
            for (const auto& sdk : report.windows_sdks) versions.push_back(sdk.version);
            text += "\nWindows SDKs: " + strings::join(versions, ", ") + "\n";
        }

        if (!report.notable_variables.empty()) {
            text += "\nRelevant environment variables:\n";
            for (const auto& [name, value] : report.notable_variables) {
                text += "  " + name + " = " + strings::utf8_truncate(value, 300) + "\n";
            }
        }

        auto result = ToolResult::success(
            "Environment: " + std::to_string(installed.size()) + " tools installed", text);
        result.evidence.push_back(make_evidence(
            EvidenceKind::EnvironmentChecked, "development environment",
            std::to_string(installed.size()) + " tools found", true));
        return result;
    }
};

}  // namespace

std::vector<ToolPtr> make_environment_tools() {
    return {std::make_shared<EnvironmentInfoTool>()};
}

}  // namespace localcode
