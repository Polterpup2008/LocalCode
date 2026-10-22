#pragma once
// ---------------------------------------------------------------------------
// System prompt construction.
//
// The prompt is assembled from facts, not boilerplate: the real workspace path,
// the build system that was actually detected, the tools that are actually
// installed. A small model told "cmake is at C:\Program Files\CMake\bin\cmake.exe"
// behaves very differently from one left to guess.
//
// Note what is *not* here: no invented reasoning levels, no persona, no
// exhortations to "think step by step". Behaviour is shaped by the structure of
// the loop and by the evidence rules the application enforces, because a prompt
// that merely asks a model not to hallucinate does not stop it.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include "../inference/ChatTypes.h"
#include "../model/ModelProfile.h"

namespace localcode {

class Workspace;
class EnvironmentService;
struct RepositoryMap;

// What the repository index currently holds, as much of it as the prompt needs.
// A summary rather than the index itself, so the prompt builder stays testable
// and does not depend on the store.
struct RepositoryIndexSummary {
    bool available = false;   // open, and with something in it
    std::size_t files = 0;
    std::size_t symbols = 0;
    bool truncated = false;   // the scan hit its limit
};

struct PromptContext {
    Workspace* workspace = nullptr;
    EnvironmentService* environment = nullptr;
    const ModelProfile* profile = nullptr;
    const RepositoryMap* repository_map = nullptr;
    // Null when there is no index, which is also how the section stays out of
    // the prompt for a workspace that has not been indexed.
    const RepositoryIndexSummary* repository_index = nullptr;

    bool web_enabled = true;
    bool read_only = false;
    bool file_tools_only = false;
    bool compact_instructions = false;
    // Set when the model has no native tool calling and the text protocol is in
    // use; the protocol description is then appended.
    bool text_tool_protocol = false;
    std::vector<ToolDefinition> tools;

    // How many bytes of the project's own instruction file to include. Small
    // windows cannot afford a long one, and a truncated convention is better
    // than no conversation at all.
    std::size_t project_instructions_budget = 2000;
};

class SystemPrompt {
public:
    // The complete system message.
    [[nodiscard]] static std::string build(const PromptContext& context);

    // The pieces, exposed so the developer pane can show what was sent and why.
    [[nodiscard]] static std::string role_section(const PromptContext& context);
    [[nodiscard]] static std::string workspace_section(const PromptContext& context);
    [[nodiscard]] static std::string environment_section(const PromptContext& context);
    [[nodiscard]] static std::string conduct_section(const PromptContext& context);
    [[nodiscard]] static std::string evidence_section();
    [[nodiscard]] static std::string compact_evidence_section();
    // The difference between an operation succeeding and the user's request
    // being satisfied, and how the verification stages express it.
    [[nodiscard]] static std::string proof_section();
    // The project's own conventions, read from AGENTS.md or CLAUDE.md.
    //
    // Conventions, not permissions: nothing in a file that came out of a
    // repository relaxes the security policy, and the section says so where
    // the model reads it.
    // Present only when there is an index with something in it. Tools the model
    // never reaches for are worth nothing, and it will not reach for these
    // unless it is told they exist and which question each one answers.
    [[nodiscard]] static std::string repository_section(const PromptContext& context);
    [[nodiscard]] static std::string project_section(const PromptContext& context);
    [[nodiscard]] static std::string language_section();
};

}   // namespace localcode
