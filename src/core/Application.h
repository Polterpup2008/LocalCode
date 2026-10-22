#pragma once
// ---------------------------------------------------------------------------
// The application core.
//
// Everything the GUI needs, owned in one place and wired together once: storage,
// the workspace, the model backend, the tools, the skills and the agent. The Qt
// layer holds a pointer to this and nothing else; there is no other path from
// the interface into the engine.
//
// Keeping it Qt-free is what makes the headless tests and the CLI possible, and
// it is the reason a future front end would not require rewriting any of this.
// ---------------------------------------------------------------------------
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <atomic>
#include <thread>

#include "agent/Agent.h"
#include "inference/BackendManager.h"
#include "platform/Environment.h"
#include "repo/RepoIndex.h"
#include "skills/SkillRegistry.h"
#include "storage/Repositories.h"
#include "tools/ToolRegistry.h"
#include "web/WebService.h"
#include "workspace/Workspace.h"

namespace localcode {

// Everything the user can configure, in one struct so the settings UI has a
// single thing to bind to and one place to persist.
struct AppSettings {
    // Paths
    std::string data_directory;
    std::string cache_directory;
    std::string runtime_directory;

    // Backends
#if defined(_WIN32)
    bool use_bundled_llama = true;
#else
    bool use_bundled_llama = false;
#endif
    std::string llama_server_path;
    std::string ik_llama_server_path;
    BackendPreference default_backend = BackendPreference::LlamaCpp;
    bool use_external_server = false;
    std::string external_server_url = "http://127.0.0.1:8080";
    std::string external_server_api_key;

    // Agent
    AutonomyLevel autonomy = AutonomyLevel::Standard;
    int max_iterations = 40;
    int agent_time_budget_minutes = 45;
    bool read_only = false;
    bool stream_responses = true;

    // Web
    bool web_enabled = true;
    std::string search_provider = "duckduckgo";
    std::string search_endpoint;
    std::string search_api_key;
    std::vector<std::string> extra_trusted_hosts;

    // Interface
    std::string theme = "dark";
    int editor_font_size = 12;
    bool show_reasoning = true;
    std::string log_level = "info";
};

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Creates the data directories, opens the database, loads settings and
    // skills, and starts environment detection. Call once at start-up.
    Status initialise();
    void shutdown();

    // True between a successful initialise() and shutdown(). Every subsystem
    // accessor below is valid only while this holds: shutdown() destroys what
    // they return, and they dereference the owning pointer without checking, so
    // calling one afterwards is undefined rather than merely empty.
    //
    // This is not hypothetical. The GUI used to call shutdown() while the main
    // window was still alive, and the window's destructor then unregistered its
    // event sink through agent() - a null dereference that took the process
    // down with an access violation on every single exit.
    [[nodiscard]] bool is_initialised() const noexcept { return initialised_; }

    // --- Subsystems -------------------------------------------------------
    // Valid only while is_initialised(). Anything holding a reference to the
    // Application must be destroyed before shutdown() is called.
    [[nodiscard]] Database& database() { return *database_; }

    [[nodiscard]] Workspace& workspace() { return *workspace_; }

    [[nodiscard]] EnvironmentService& environment() { return *environment_; }

    [[nodiscard]] SkillRegistry& skills() { return *skills_; }

    [[nodiscard]] WebService& web() { return *web_; }

    [[nodiscard]] BackendManager& backends() { return *backends_; }

    [[nodiscard]] ToolRegistry& tools() { return *tools_; }

    [[nodiscard]] Agent& agent() { return *agent_; }

    [[nodiscard]] repo::RepoIndex& repository() { return *repo_index_; }

    [[nodiscard]] const repo::RepoIndex& repository() const { return *repo_index_; }

    // Brings the index into line with the tree, on a thread of its own. Safe to
    // call again while one is running: the second call is ignored rather than
    // queued, because two scans of the same tree help nobody.
    void refresh_repository_index(bool full = false);
    void cancel_repository_index();
    [[nodiscard]] bool repository_index_running() const;

    // Called on the indexing thread as it goes, and once when it finishes.
    void set_repository_index_callback(std::function<void(const repo::IndexProgress&)> callback);

    [[nodiscard]] SettingsRepository& settings_repository() { return *settings_repo_; }

    [[nodiscard]] WorkspaceRepository& workspace_repository() { return *workspace_repo_; }

    [[nodiscard]] ModelProfileRepository& model_repository() { return *model_repo_; }

    [[nodiscard]] ConversationRepository& conversation_repository() { return *conversation_repo_; }

    [[nodiscard]] ToolHistoryRepository& tool_history_repository() { return *tool_history_repo_; }

    [[nodiscard]] TaskRepository& task_repository() { return *task_repo_; }

    [[nodiscard]] DraftRepository& draft_repository() { return *draft_repo_; }

    [[nodiscard]] AppStateRepository& app_state_repository() { return *app_state_repo_; }

    // --- Settings ---------------------------------------------------------
    [[nodiscard]] AppSettings settings() const;
    Status apply_settings(const AppSettings& settings);
    Status load_settings();

    // --- Workspace --------------------------------------------------------
    Status open_workspace(const std::filesystem::path& path);
    [[nodiscard]] Result<std::vector<WorkspaceRecord>> recent_workspaces() const;

    // --- Models -----------------------------------------------------------
    // Reads a GGUF file, builds a profile from its metadata and stores it.
    Result<ModelProfile> import_model(const std::filesystem::path& gguf_path);
    Result<std::vector<ModelProfile>> model_profiles() const;
    Status save_model_profile(const ModelProfile& profile);
    Status remove_model_profile(const std::string& id);

    // Starts the server for a profile and points the agent at it.
    Status activate_model(const std::string& profile_id, CancellationToken cancel = {});
    void deactivate_model();
    [[nodiscard]] std::string active_model_id() const;

    // --- Conversations ----------------------------------------------------
    Result<std::int64_t> start_conversation(const std::string& title);
    Status load_conversation(std::int64_t id);
    // Deletes only this project's stored chat. The agent must be idle; deleting
    // the active chat also clears its in-memory history and recovery marker.
    Status delete_conversation(std::int64_t id);
    Status persist_message(const ChatMessage& message);

    // Stores whatever part of `history` is not already stored, and returns how
    // many messages were written.
    //
    // Callers hold the whole conversation and want it durable; the obvious
    // implementation - append every message every time - stored the history
    // again after each turn. Six real messages became fourteen rows, growing
    // quadratically, and a restored conversation showed every early turn several
    // times over. This compares against what is already there.
    Result<int> persist_conversation(const std::vector<ChatMessage>& history);
    [[nodiscard]] std::int64_t current_conversation() const;

    // --- Crash recovery ---------------------------------------------------
    struct RecoveredSession {
        std::string workspace_path;
        std::int64_t conversation_id = 0;
        std::string model_id;
        std::string draft;
        std::vector<std::string> open_files;
        bool had_unclean_shutdown = false;
    };

    // Reads what the last session was doing. Call before opening anything.
    [[nodiscard]] Result<RecoveredSession> recover_session();
    // Records the current state so the next start can restore it.
    Status checkpoint_session(const std::vector<std::string>& open_files, const std::string& draft);
    // Marks a clean exit, so the next start knows the difference.
    Status mark_clean_shutdown();

private:
    Status persist_task_state(std::int64_t conversation);
    Status configure_paths();
    void wire_agent();
    [[nodiscard]] std::function<void(const repo::IndexProgress&)> repository_callback() const;
    void register_tool_observer();

    std::unique_ptr<Database> database_;
    std::unique_ptr<SettingsRepository> settings_repo_;
    std::unique_ptr<WorkspaceRepository> workspace_repo_;
    std::unique_ptr<ModelProfileRepository> model_repo_;
    std::unique_ptr<ConversationRepository> conversation_repo_;
    std::unique_ptr<ToolHistoryRepository> tool_history_repo_;
    std::unique_ptr<TaskRepository> task_repo_;
    std::unique_ptr<DraftRepository> draft_repo_;
    std::unique_ptr<AppStateRepository> app_state_repo_;
    std::unique_ptr<SkillMetadataRepository> skill_meta_repo_;

    std::unique_ptr<Workspace> workspace_;
    std::unique_ptr<EnvironmentService> environment_;
    std::unique_ptr<SkillRegistry> skills_;
    std::shared_ptr<HttpClient> http_;
    std::unique_ptr<WebService> web_;
    std::unique_ptr<BackendManager> backends_;
    std::unique_ptr<ToolRegistry> tools_;
    std::unique_ptr<Agent> agent_;
    std::unique_ptr<repo::RepoIndex> repo_index_;
    std::thread repo_thread_;
    CancellationSource repo_cancel_;
    std::atomic<bool> repo_running_{false};
    std::function<void(const repo::IndexProgress&)> repo_callback_;

    mutable std::mutex mutex_;
    std::recursive_mutex model_transition_mutex_;
    AppSettings settings_;
    std::int64_t conversation_id_ = 0;
    std::int64_t workspace_id_ = 0;
    std::string active_model_;
    bool initialised_ = false;
};

}   // namespace localcode
