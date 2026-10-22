#include "Application.h"

#include <algorithm>
#include <charconv>

#include "model/GgufMetadata.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace localcode {
namespace {

constexpr const char* kCleanShutdownKey = "session.clean_shutdown";
constexpr const char* kLastWorkspaceKey = "session.workspace";
constexpr const char* kLastConversationKey = "session.conversation";
constexpr const char* kLastModelKey = "session.model";
constexpr const char* kOpenFilesKey = "session.open_files";

LogLevel parse_log_level(const std::string& text) {
    const auto lowered = strings::to_lower(text);
    if (lowered == "trace") return LogLevel::Trace;
    if (lowered == "debug") return LogLevel::Debug;
    if (lowered == "warn" || lowered == "warning") return LogLevel::Warn;
    if (lowered == "error") return LogLevel::Error;
    return LogLevel::Info;
}

AutonomyLevel parse_autonomy(const std::string& text) {
    const auto lowered = strings::to_lower(text);
    if (lowered == "restricted") return AutonomyLevel::Restricted;
    if (lowered == "full") return AutonomyLevel::Full;
    return AutonomyLevel::Standard;
}

}   // namespace

Application::Application() = default;

Application::~Application() {
    shutdown();
}

Status Application::configure_paths() {
    // Settings may relocate the data directories, but they live *in* the
    // database, which lives in the data directory - so the default location is
    // used to open it, and a relocation takes effect on the next start.
    if (auto status = app_paths().ensure_directories(); !status) return status;

    Logger::instance().configure(app_paths().log_dir(), LogLevel::Info);
    log_info(LogChannel::App, "LocalCode starting",
             {{"version", LOCALCODE_VERSION},
              {"data", path_to_utf8(app_paths().data_dir())},
              {"cache", path_to_utf8(app_paths().cache_dir())}});
    return ok_status();
}

Status Application::initialise() {
    if (initialised_) return ok_status();

    if (auto status = configure_paths(); !status) return status;

    // --- Storage ---------------------------------------------------------
    database_ = std::make_unique<Database>();
    if (auto status = database_->open(app_paths().database_path()); !status) return status;

    auto integrity = database_->integrity_check();
    if (integrity && *integrity != "ok") {
        log_error(LogChannel::App, "database integrity check failed", {{"result", *integrity}});
    }

    settings_repo_ = std::make_unique<SettingsRepository>(*database_);
    workspace_repo_ = std::make_unique<WorkspaceRepository>(*database_);
    model_repo_ = std::make_unique<ModelProfileRepository>(*database_);
    conversation_repo_ = std::make_unique<ConversationRepository>(*database_);
    tool_history_repo_ = std::make_unique<ToolHistoryRepository>(*database_);
    task_repo_ = std::make_unique<TaskRepository>(*database_);
    draft_repo_ = std::make_unique<DraftRepository>(*database_);
    app_state_repo_ = std::make_unique<AppStateRepository>(*database_);
    skill_meta_repo_ = std::make_unique<SkillMetadataRepository>(*database_);

    // --- Subsystems -------------------------------------------------------
    workspace_ = std::make_unique<Workspace>();
    environment_ = std::make_unique<EnvironmentService>();
    skills_ = std::make_unique<SkillRegistry>();
    http_ = HttpClient::create();
    web_ = std::make_unique<WebService>(http_);
    backends_ = std::make_unique<BackendManager>();
    backends_->set_http_client(http_);
    tools_ = std::make_unique<ToolRegistry>();
    tools_->register_builtin_tools();
    agent_ = std::make_unique<Agent>();
    repo_index_ = std::make_unique<repo::RepoIndex>();

    // Skills: built-in first, then the user's, then whatever the database says
    // about which are enabled and which are still awaiting review.
    auto skills_loaded = skills_->load_default_locations();
    if (!skills_loaded) {
        log_warn(LogChannel::App, "no built-in skills were loaded",
                 {{"reason", skills_loaded.error().message}});
    }
    if (auto stored = skill_meta_repo_->all()) {
        for (const auto& [id, enabled, validated] : *stored) {
            skills_->set_enabled(id, enabled);
        }
    }
    for (const auto& skill : skills_->skills()) {
        skill_meta_repo_->upsert(skill.id, skill.enabled, skill.validated,
                                 path_to_utf8(skill.source_path));
    }

    if (auto status = load_settings(); !status) {
        log_warn(LogChannel::App, "settings could not be loaded; using defaults",
                 {{"reason", status.error().message}});
    }

    wire_agent();
    register_tool_observer();

    initialised_ = true;

    // An empty skill library is a broken installation, not a normal state: the
    // agent still runs, but without any of the procedures it is supposed to
    // follow, and every run is quietly worse. It was reported as an INFO line
    // reading "skills=0" and went unnoticed through several runs, so it now says
    // what is wrong and where the files were expected.
    if (skills_->skills().empty()) {
        log_warn(LogChannel::App,
                 "no skills were loaded - the agent will run without any procedures",
                 {{"expected_in", path_to_utf8(app_paths().builtin_skills_dir())},
                  {"errors", strings::join(skills_->load_errors(), "; ")}});
    }

    log_info(LogChannel::App, "initialised",
             {{"tools", std::to_string(tools_->names().size())},
              {"skills", std::to_string(skills_->skills().size())}});
    return ok_status();
}

void Application::shutdown() {
    if (!initialised_) return;

    // The indexing thread reaches into the workspace and the store, both of
    // which are about to be destroyed. Stop it first, and wait: letting it run
    // on into the teardown is the same mistake the GUI made with its git scan.
    cancel_repository_index();

    // Order matters: stop the model server before tearing down the objects the
    // health probe closes over.
    if (backends_) backends_->deactivate();
    agent_.reset();
    repo_index_.reset();
    tools_.reset();
    backends_.reset();
    web_.reset();
    http_.reset();
    skills_.reset();
    environment_.reset();
    workspace_.reset();

    skill_meta_repo_.reset();
    app_state_repo_.reset();
    draft_repo_.reset();
    task_repo_.reset();
    tool_history_repo_.reset();
    conversation_repo_.reset();
    model_repo_.reset();
    workspace_repo_.reset();
    settings_repo_.reset();
    if (database_) database_->close();
    database_.reset();

    initialised_ = false;
    log_info(LogChannel::App, "shut down");
}

void Application::wire_agent() {
    agent_->set_tools(tools_.get());
    agent_->set_workspace(workspace_.get());
    agent_->set_skills(skills_.get());
    agent_->set_environment(environment_.get());
    agent_->set_web(web_.get());
    agent_->set_repo(repo_index_.get());
}

void Application::register_tool_observer() {
    // Every tool invocation is recorded, so the activity timeline survives a
    // restart and the developer pane has something to show.
    tools_->set_observer([this](const ToolInvocation& invocation, const ToolResult& result) {
        ToolHistoryRecord record;
        {
            std::lock_guard lock(mutex_);
            record.conversation_id = conversation_id_;
        }
        record.tool_name = invocation.tool_name;
        record.arguments_json = invocation.arguments.dump();
        record.succeeded = result.ok;
        if (json_bool(result.structured, "expectation_checked") &&
            !json_bool(result.structured, "expectation_satisfied"))
            record.succeeded = false;
        if (result.structured.is_object() && result.structured.contains("exit_code")) {
            record.succeeded = record.succeeded && result.structured.value("exit_code", -1) == 0;
        }
        record.summary = strings::utf8_truncate(result.summary, 500);
        record.detail = strings::utf8_truncate(result.ok ? result.content : result.error, 4000);
        record.duration_ms = result.duration_ms;
        record.created_at = now_unix_ms();
        if (tool_history_repo_) {
            if (auto saved = tool_history_repo_->append(record); !saved)
                log_error(LogChannel::App, "tool history could not be saved",
                          {{"error", saved.error().message}});
        }

        if (repo_index_ && std::any_of(result.evidence.begin(), result.evidence.end(),
                                       [](const Evidence& evidence) {
                                           return evidence.kind == EvidenceKind::FileWritten &&
                                                  evidence.positive;
                                       })) {
            repo_index_->invalidate();
            refresh_repository_index(false);
        }

        // Discovery is not execution. Only this production observer can
        // promote a matching command after seeing the real process result.
        if (repo_index_ && repo_index_->state() == repo::IndexState::Ready &&
            invocation.tool_name == "run_command" && result.structured.is_object() &&
            result.structured.contains("exit_code") &&
            !result.structured.value("cancelled", false) &&
            !result.structured.value("timed_out", false)) {
            const auto command = result.structured.value("requested_command", std::string{});
            const std::filesystem::path cwd{result.structured.value("cwd", std::string{})};
            if (auto commands = repo_index_->store().commands()) {
                for (const auto& known : *commands) {
                    std::error_code error;
                    const bool same_directory = std::filesystem::equivalent(
                        workspace_->root() / known.working_directory, cwd, error);
                    if (command != known.command || !same_directory || error) continue;
                    auto status = repo_index_->store().mark_command_verified(
                        known.purpose, known.command, result.structured.value("exit_code", -1),
                        record.created_at);
                    if (!status)
                        log_warn(LogChannel::App, "command provenance could not be recorded",
                                 {{"error", status.error().message}});
                }
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
AppSettings Application::settings() const {
    std::lock_guard lock(mutex_);
    return settings_;
}

Status Application::load_settings() {
    if (!settings_repo_) return make_error(ErrorCode::Internal, "storage is not open");

    AppSettings loaded;
    auto& repo = *settings_repo_;

    loaded.data_directory = repo.get("paths.data").value_or(path_to_utf8(app_paths().data_dir()));
    loaded.cache_directory =
        repo.get("paths.cache").value_or(path_to_utf8(app_paths().cache_dir()));
    loaded.runtime_directory =
        repo.get("paths.runtime").value_or(path_to_utf8(app_paths().runtime_dir()));

    loaded.llama_server_path = repo.get("backend.llama_server").value_or("");
    // Preserve an explicitly configured runtime when upgrading an older profile.
    loaded.use_bundled_llama = repo.get_bool(
        "backend.bundled_llama", loaded.use_bundled_llama && loaded.llama_server_path.empty());
    loaded.ik_llama_server_path = repo.get("backend.ik_llama_server").value_or("");
#if defined(_WIN32)
    // Earlier versions persisted native narrow paths with the system code page.
    // Preserve those paths when upgrading an existing Windows installation.
    try {
        for (auto* value :
             {&loaded.data_directory, &loaded.cache_directory, &loaded.runtime_directory,
              &loaded.llama_server_path, &loaded.ik_llama_server_path}) {
            if (!strings::is_valid_utf8(*value))
                *value = path_to_utf8(std::filesystem::path(*value));
        }
    } catch (const std::filesystem::filesystem_error& e) {
        return make_error(ErrorCode::InvalidArgument, "Cannot migrate saved Windows paths.",
                          e.what());
    }
#endif
    loaded.default_backend =
        backend_preference_from_string(repo.get("backend.default").value_or("llama.cpp"));
    loaded.use_external_server = repo.get_bool("backend.external_enabled", false);
    loaded.external_server_url = repo.get("backend.external_url").value_or("http://127.0.0.1:8080");
    loaded.external_server_api_key = repo.get("backend.external_api_key").value_or("");

    loaded.autonomy = parse_autonomy(repo.get("agent.autonomy").value_or("standard"));
    loaded.max_iterations = static_cast<int>(repo.get_int("agent.max_iterations", 40));
    loaded.agent_time_budget_minutes =
        static_cast<int>(repo.get_int("agent.time_budget_minutes", 45));
    loaded.read_only = repo.get_bool("agent.read_only", false);
    loaded.stream_responses = repo.get_bool("agent.stream", true);

    loaded.web_enabled = repo.get_bool("web.enabled", true);
    loaded.search_provider = repo.get("web.provider").value_or("duckduckgo");
    loaded.search_endpoint = repo.get("web.endpoint").value_or("");
    loaded.search_api_key = repo.get("web.api_key").value_or("");
    const auto trusted = repo.get("web.trusted_hosts").value_or("");
    if (!trusted.empty()) loaded.extra_trusted_hosts = strings::split(trusted, ',', false);

    loaded.theme = repo.get("ui.theme").value_or("dark");
    loaded.editor_font_size = static_cast<int>(repo.get_int("ui.editor_font_size", 12));
    loaded.show_reasoning = repo.get_bool("ui.show_reasoning", true);
    loaded.log_level = repo.get("app.log_level").value_or("info");

    return apply_settings(loaded);
}

Status Application::apply_settings(const AppSettings& settings) {
    // Decode paths before persistence, so malformed UTF-8 cannot leave saved
    // settings ahead of the native backend configuration.
    std::filesystem::path runtime_directory, llama_binary, ik_binary;
    try {
        for (const auto* value : {&settings.runtime_directory, &settings.llama_server_path,
                                  &settings.ik_llama_server_path}) {
            if (!strings::is_valid_utf8(*value))
                return make_error(ErrorCode::InvalidArgument,
                                  "Settings paths must be valid UTF-8.");
        }
        runtime_directory = path_from_utf8(settings.runtime_directory);
        llama_binary = path_from_utf8(settings.llama_server_path);
        ik_binary = path_from_utf8(settings.ik_llama_server_path);
    } catch (const std::filesystem::filesystem_error& e) {
        return make_error(ErrorCode::InvalidArgument, "Cannot decode settings paths.", e.what());
    }
    // Persist before changing runtime state. A failed write must not leave the
    // dialog claiming success or a half-updated configuration after restart.
    if (settings_repo_) {
        const auto boolean = [](bool value) { return value ? "true" : "false"; };
        const auto status = settings_repo_->set_all({
            {"paths.data", settings.data_directory},
            {"paths.cache", settings.cache_directory},
            {"paths.runtime", settings.runtime_directory},
            {"backend.llama_server", settings.llama_server_path},
            {"backend.bundled_llama", boolean(settings.use_bundled_llama)},
            {"backend.ik_llama_server", settings.ik_llama_server_path},
            {"backend.default", std::string(to_string(settings.default_backend))},
            {"backend.external_enabled", boolean(settings.use_external_server)},
            {"backend.external_url", settings.external_server_url},
            {"backend.external_api_key", settings.external_server_api_key},
            {"agent.autonomy", std::string(to_string(settings.autonomy))},
            {"agent.max_iterations", std::to_string(settings.max_iterations)},
            {"agent.time_budget_minutes", std::to_string(settings.agent_time_budget_minutes)},
            {"agent.read_only", boolean(settings.read_only)},
            {"agent.stream", boolean(settings.stream_responses)},
            {"web.enabled", boolean(settings.web_enabled)},
            {"web.provider", settings.search_provider},
            {"web.endpoint", settings.search_endpoint},
            {"web.api_key", settings.search_api_key},
            {"web.trusted_hosts", strings::join(settings.extra_trusted_hosts, ",")},
            {"ui.theme", settings.theme},
            {"ui.editor_font_size", std::to_string(settings.editor_font_size)},
            {"ui.show_reasoning", boolean(settings.show_reasoning)},
            {"app.log_level", settings.log_level},
        });
        if (!status) return status;
    }
    {
        std::lock_guard lock(mutex_);
        settings_ = settings;
    }

    Logger::instance().set_min_level(parse_log_level(settings.log_level));

    if (!settings.runtime_directory.empty()) {
        app_paths().set_runtime_dir(runtime_directory);
    }

    if (backends_) {
        backends_->set_use_bundled_llama(settings.use_bundled_llama);
        backends_->set_configured_binary(BackendPreference::LlamaCpp, llama_binary);
        backends_->set_configured_binary(BackendPreference::IkLlama, ik_binary);
        backends_->set_default_preference(settings.default_backend);

        ExternalEndpoint external;
        external.enabled = settings.use_external_server;
        external.base_url = settings.external_server_url;
        external.api_key = settings.external_server_api_key;
        external.kind = settings.default_backend == BackendPreference::IkLlama
                            ? BackendPreference::IkLlama
                            : BackendPreference::LlamaCpp;
        backends_->set_external_endpoint(external);
    }

    if (workspace_) workspace_->commands().set_autonomy(settings.autonomy);
    if (tools_) tools_->set_read_only(settings.read_only);

    if (web_) {
        web_->set_enabled(settings.web_enabled);
        if (!settings.search_endpoint.empty()) {
            SearchProvider provider;
            provider.id = settings.search_provider;
            provider.display_name = settings.search_provider;
            provider.endpoint = settings.search_endpoint;
            provider.api_key = settings.search_api_key;
            provider.kind = settings.search_provider == "searxng" ? SearchProviderKind::SearxngJson
                            : settings.search_provider == "brave" ? SearchProviderKind::BraveJson
                                                                  : SearchProviderKind::Custom;
            auto providers = web_->providers();
            providers.erase(
                std::remove_if(providers.begin(), providers.end(),
                               [&](const SearchProvider& p) { return p.id == provider.id; }),
                providers.end());
            providers.push_back(provider);
            web_->set_providers(providers);
        }
        web_->set_active_provider(settings.search_provider);
        for (const auto& host : settings.extra_trusted_hosts) {
            web_->trust().add_trusted_host(strings::trim(host));
        }
    }

    if (agent_) {
        AgentConfig config = agent_->config();
        config.max_iterations = std::max(1, settings.max_iterations);
        config.wall_clock_budget =
            std::chrono::minutes(std::max(1, settings.agent_time_budget_minutes));
        config.stream = settings.stream_responses;
        agent_->set_config(config);
    }

    return ok_status();
}

// ---------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------
Status Application::open_workspace(const std::filesystem::path& path) {
    if (!workspace_) return make_error(ErrorCode::Internal, "not initialised");

    std::error_code path_error;
    const bool changed_project =
        workspace_->is_open() &&
        !std::filesystem::equivalent(workspace_->root(), path, path_error) && !path_error;
    if (changed_project) {
        auto saved = persist_conversation(agent_->history());
        if (!saved) return saved.error();
        if (auto cleared = app_state_repo_->remove(kLastConversationKey); !cleared) return cleared;
    }
    cancel_repository_index();

    if (auto status = workspace_->open(path); !status) return status;
    if (changed_project) {
        agent_->clear_history();
        std::lock_guard lock(mutex_);
        conversation_id_ = 0;
    }
    workspace_->commands().set_autonomy(settings().autonomy);

    auto record =
        workspace_repo_->touch(path_to_utf8(workspace_->root()), workspace_->display_name());
    if (record) {
        std::lock_guard lock(mutex_);
        workspace_id_ = record->id;
    } else {
        std::lock_guard lock(mutex_);
        workspace_id_ = 0;
        log_error(LogChannel::App, "workspace metadata could not be saved",
                  {{"error", record.error().message}});
    }
    if (app_state_repo_) {
        if (auto saved = app_state_repo_->set(kLastWorkspaceKey, path_to_utf8(workspace_->root()));
            !saved)
            log_error(LogChannel::App, "session workspace could not be saved",
                      {{"error", saved.error().message}});
    }

    // Opening the stored index is a database open, not a scan, so whatever was
    // learned last time is available immediately. Bringing it up to date is a
    // separate, cancellable job the caller starts when it suits them.
    cancel_repository_index();
    const auto index_directory = AppPaths::instance().data_dir() / "repo-index";
    if (auto status = repo_index_->open(index_directory, workspace_->root()); !status) {
        // An index that will not open is a degraded experience, not a failure to
        // open the project.
        log_warn(LogChannel::App, "repository index unavailable",
                 LogFields{{"workspace", path_to_utf8(workspace_->root())},
                           {"error", status.error().message}});
    }
    return ok_status();
}

void Application::refresh_repository_index(bool full) {
    if (!repo_index_ || !repo_index_->is_open() || !workspace_ || !workspace_->is_open()) return;
    if (repo_running_.exchange(true)) return;   // one at a time

    if (repo_thread_.joinable()) repo_thread_.join();
    repo_cancel_.reset();

    // The ignore rules are read here, on the calling thread, so the worker does
    // not touch the workspace object while the rest of the application is using
    // it.
    auto ignore = workspace_->ignore_rules();
    auto token = repo_cancel_.token();
    auto callback = repository_callback();

    repo_thread_ = std::thread(
        [this, ignore = std::move(ignore), token, full, callback = std::move(callback)]() mutable {
            auto stats = repo_index_->refresh(ignore, token, full, callback);
            if (!stats) {
                log_warn(LogChannel::App, "repository index refresh failed",
                         LogFields{{"error", stats.error().message}});
            }
            repo_running_.store(false);
            if (callback) {
                repo::IndexProgress done;
                done.state = repo_index_->state();
                try {
                    callback(done);
                } catch (...) {
                    log_error(LogChannel::App, "repository completion observer threw");
                }
            }
        });
}

void Application::cancel_repository_index() {
    repo_cancel_.cancel();
    if (repo_thread_.joinable()) repo_thread_.join();
    repo_running_.store(false);
}

bool Application::repository_index_running() const {
    return repo_running_.load();
}

void Application::set_repository_index_callback(
    std::function<void(const repo::IndexProgress&)> callback) {
    std::lock_guard lock(mutex_);
    repo_callback_ = std::move(callback);
}

std::function<void(const repo::IndexProgress&)> Application::repository_callback() const {
    std::lock_guard lock(mutex_);
    return repo_callback_;
}

Result<std::vector<WorkspaceRecord>> Application::recent_workspaces() const {
    if (!workspace_repo_) return make_error(ErrorCode::Internal, "not initialised");
    return workspace_repo_->recent(20);
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------
Result<ModelProfile> Application::import_model(const std::filesystem::path& gguf_path) {
    auto metadata = read_gguf_metadata(gguf_path);
    if (!metadata) return metadata.error();

    auto profile = ModelProfile::from_gguf(*metadata);

    // Preserve the user's settings if this model was imported before.
    if (auto existing = model_repo_->find(profile.id); existing && existing->has_value()) {
        auto merged = **existing;
        merged.capabilities = profile.capabilities;
        merged.architecture = profile.architecture;
        merged.quantization = profile.quantization;
        merged.file_size = profile.file_size;
        merged.trained_context = profile.trained_context;
        profile = merged;
    }

    if (auto status = model_repo_->save(profile); !status) return status.error();
    log_info(LogChannel::App, "model imported",
             {{"id", profile.id}, {"path", path_to_utf8(gguf_path)}});
    return profile;
}

Result<std::vector<ModelProfile>> Application::model_profiles() const {
    if (!model_repo_) return make_error(ErrorCode::Internal, "not initialised");
    return model_repo_->all();
}

Status Application::save_model_profile(const ModelProfile& profile) {
    if (!model_repo_) return make_error(ErrorCode::Internal, "not initialised");
    return model_repo_->save(profile);
}

Status Application::remove_model_profile(const std::string& id) {
    if (!model_repo_) return make_error(ErrorCode::Internal, "not initialised");

    bool is_active = false;
    {
        std::lock_guard lock(mutex_);
        is_active = active_model_ == id;
    }
    // Unload outside the lock: deactivate_model() takes the same mutex.
    if (is_active) deactivate_model();

    return model_repo_->remove(id);
}

Status Application::activate_model(const std::string& profile_id, CancellationToken cancel) {
    std::lock_guard transition(model_transition_mutex_);
    if (!backends_ || !model_repo_) return make_error(ErrorCode::Internal, "not initialised");

    auto found = model_repo_->find(profile_id);
    if (!found) return found.error();
    if (!found->has_value()) {
        return make_error(ErrorCode::NotFound, "no model profile with id " + profile_id);
    }
    const auto profile = **found;

    // Clear every owner of the outgoing endpoint before a switch can stop it.
    // On failure the agent and selector must agree that no model is loaded.
    deactivate_model();
    auto backend = backends_->activate(profile, cancel);
    if (!backend) return backend.error();

    agent_->set_backend(*backend);
    agent_->set_profile(profile);

    // The server may have been started with a different context size than the
    // profile asks for; believe the server, and tell the user through the log.
    const auto info = backends_->current_info();
    if (info.context_size > 0 && info.context_size != profile.context_size) {
        auto adjusted = profile;
        adjusted.context_size = info.context_size;
        agent_->set_profile(adjusted);
        log_info(LogChannel::Backend, "context size taken from the running server",
                 {{"profile", std::to_string(profile.context_size)},
                  {"server", std::to_string(info.context_size)}});
    }

    if (auto used = model_repo_->mark_used(profile_id); !used)
        log_error(LogChannel::App, "model usage metadata could not be saved",
                  {{"error", used.error().message}});
    {
        std::lock_guard lock(mutex_);
        active_model_ = profile_id;
    }
    if (app_state_repo_) {
        if (auto saved = app_state_repo_->set(kLastModelKey, profile_id); !saved)
            log_error(LogChannel::App, "session model could not be saved",
                      {{"error", saved.error().message}});
    }
    if (conversation_repo_ && current_conversation() > 0) {
        if (auto saved = conversation_repo_->set_model(current_conversation(), profile_id); !saved)
            log_error(LogChannel::App, "conversation model could not be saved",
                      {{"error", saved.error().message}});
    }
    return ok_status();
}

void Application::deactivate_model() {
    std::lock_guard transition(model_transition_mutex_);
    if (backends_) backends_->deactivate();
    if (agent_) agent_->set_backend(nullptr);
    std::lock_guard lock(mutex_);
    active_model_.clear();
}

std::string Application::active_model_id() const {
    if (!backends_ || !backends_->current_backend()) return {};
    std::lock_guard lock(mutex_);
    return active_model_;
}

// ---------------------------------------------------------------------------
// Conversations
// ---------------------------------------------------------------------------
Result<std::int64_t> Application::start_conversation(const std::string& title) {
    if (!conversation_repo_) return make_error(ErrorCode::Internal, "not initialised");

    std::int64_t workspace_id = 0;
    std::string model;
    {
        std::lock_guard lock(mutex_);
        workspace_id = workspace_id_;
        model = active_model_;
    }

    std::int64_t id = 0;
    const auto saved = database_->with_transaction([&]() -> Status {
        auto created = conversation_repo_->create(workspace_id, title, model);
        if (!created) return created.error();
        id = *created;
        return app_state_repo_->set(kLastConversationKey, std::to_string(id));
    });
    if (!saved) return saved.error();

    {
        std::lock_guard lock(mutex_);
        conversation_id_ = id;
    }
    if (agent_) agent_->clear_history();
    return id;
}

Status Application::load_conversation(std::int64_t id) {
    if (!conversation_repo_ || !agent_) return make_error(ErrorCode::Internal, "not initialised");

    auto found = conversation_repo_->find(id);
    if (!found) return found.error();
    if (!found->has_value()) return make_error(ErrorCode::NotFound, "conversation not found");
    {
        std::lock_guard lock(mutex_);
        if ((*found)->workspace_id != workspace_id_)
            return make_error(ErrorCode::PermissionDenied,
                              "conversation belongs to a different project");
    }
    auto messages = conversation_repo_->messages(id);
    if (!messages) return messages.error();

    std::vector<ChatMessage> history;
    history.reserve(messages->size());
    for (const auto& stored : *messages)
        history.push_back(stored.message);
    std::optional<TaskState> state;
    std::optional<VerificationLedger> ledger;

    // The task state is stored beside the conversation, so a resumed session
    // knows what it was doing rather than starting the mission over.
    if (task_repo_) {
        auto task = task_repo_->latest_for_conversation(id);
        if (!task) return task.error();
        if (task->has_value()) {
            auto parsed = parse_json((*task)->state_json, "stored task state");
            if (!parsed) return parsed.error();
            if (!parsed->is_object())
                return make_error(ErrorCode::ParseError, "stored task state must be an object");
            try {
                state = TaskState::from_json(*parsed);
                state->invalidate_checks();
                if (parsed->contains("verification_ledger")) {
                    ledger = VerificationLedger::from_json((*parsed)["verification_ledger"]);
                    ledger->invalidate_for_resume();
                }
            } catch (const Json::exception&) {
                return make_error(ErrorCode::ParseError, "invalid stored task state");
            }
        }
    }

    if (auto saved = app_state_repo_->set(kLastConversationKey, std::to_string(id)); !saved)
        return saved;
    agent_->clear_history();
    agent_->set_history(std::move(history));
    if (state) agent_->set_task_state(std::move(*state));
    if (ledger) agent_->set_verification_ledger(std::move(*ledger));
    {
        std::lock_guard lock(mutex_);
        conversation_id_ = id;
    }
    return ok_status();
}

Status Application::delete_conversation(std::int64_t id) {
    if (!conversation_repo_ || !agent_) return make_error(ErrorCode::Internal, "not initialised");
    if (agent_->running())
        return make_error(ErrorCode::InvalidArgument,
                          "stop the agent before deleting a conversation");
    auto found = conversation_repo_->find(id);
    if (!found) return found.error();
    if (!found->has_value()) return make_error(ErrorCode::NotFound, "conversation not found");
    {
        std::lock_guard lock(mutex_);
        if ((*found)->workspace_id != workspace_id_)
            return make_error(ErrorCode::PermissionDenied,
                              "conversation belongs to a different project");
    }
    const bool active = id == current_conversation();
    const auto removed = database_->with_transaction([&]() -> Status {
        if (auto deleted = conversation_repo_->remove(id); !deleted) return deleted;
        if (auto cleared = app_state_repo_->remove("conversation.terminal." + std::to_string(id));
            !cleared)
            return cleared;
        auto remembered = app_state_repo_->get(kLastConversationKey);
        if (!remembered) return remembered.error();
        if (*remembered == std::to_string(id)) return app_state_repo_->remove(kLastConversationKey);
        return ok_status();
    });
    if (!removed) return removed;
    if (active) {
        agent_->clear_history();
        std::lock_guard lock(mutex_);
        conversation_id_ = 0;
    }
    return ok_status();
}

Result<int> Application::persist_conversation(const std::vector<ChatMessage>& history) {
    if (!conversation_repo_) return make_error(ErrorCode::Internal, "not initialised");
    if (history.empty()) return 0;

    const std::int64_t conversation = current_conversation();
    std::int64_t already = 0;
    if (conversation != 0) {
        auto counted = conversation_repo_->message_count(conversation);
        if (!counted) return counted.error();
        already = *counted;
    }

    // The stored prefix is assumed to be the same prefix: the agent only ever
    // appends to its history within a conversation, and starting a new one
    // resets the conversation id.
    int written = 0;
    for (std::size_t i = static_cast<std::size_t>(already); i < history.size(); ++i) {
        if (auto status = persist_message(history[i]); !status) return status.error();
        ++written;
    }
    if (auto status = persist_task_state(conversation ? conversation : current_conversation());
        !status)
        return status.error();
    return written;
}

Status Application::persist_message(const ChatMessage& message) {
    if (!conversation_repo_) return make_error(ErrorCode::Internal, "not initialised");

    std::int64_t conversation = current_conversation();
    if (conversation == 0) {
        auto created = start_conversation(strings::utf8_truncate(message.content, 80));
        if (!created) return created.error();
        conversation = *created;
    }

    auto appended =
        conversation_repo_->append_message(conversation, message, estimate_tokens(message.content));
    if (!appended) return appended.error();

    return ok_status();
}

Status Application::persist_task_state(std::int64_t conversation) {
    // Refresh the task even if no new message arrived, e.g. cancellation after
    // tools ran. A retry must also repair an earlier task-state write failure.
    if (task_repo_ && agent_) {
        const auto state = agent_->task_state();
        if (!state.objective().empty()) {
            auto existing = task_repo_->latest_for_conversation(conversation);
            if (!existing) return existing.error();
            auto task_json = state.to_json();
            task_json["verification_ledger"] = agent_->verification_ledger().to_json();
            const auto payload = task_json.dump();
            const auto status_text =
                state.verification() == VerificationState::Verified ? "verified" : "active";
            if (existing && existing->has_value()) {
                if (auto status = task_repo_->update((*existing)->id, payload, status_text);
                    !status)
                    return status;
            } else {
                auto created = task_repo_->create(conversation, state.objective(), payload);
                if (!created) return created.error();
            }
        }
    }
    return ok_status();
}

std::int64_t Application::current_conversation() const {
    std::lock_guard lock(mutex_);
    return conversation_id_;
}

// ---------------------------------------------------------------------------
// Crash recovery
// ---------------------------------------------------------------------------
Result<Application::RecoveredSession> Application::recover_session() {
    RecoveredSession session;
    if (!app_state_repo_) return make_error(ErrorCode::Internal, "not initialised");

    auto clean = app_state_repo_->get(kCleanShutdownKey);
    auto workspace = app_state_repo_->get(kLastWorkspaceKey);
    auto model = app_state_repo_->get(kLastModelKey);
    auto conversation = app_state_repo_->get(kLastConversationKey);
    auto files = app_state_repo_->get(kOpenFilesKey);
    if (!clean) return clean.error();
    if (!workspace) return workspace.error();
    if (!model) return model.error();
    if (!conversation) return conversation.error();
    if (!files) return files.error();
    session.had_unclean_shutdown = *clean != "1";

    session.workspace_path = *workspace;
    session.model_id = *model;

    if (!conversation->empty()) {
        const auto parsed =
            std::from_chars(conversation->data(), conversation->data() + conversation->size(),
                            session.conversation_id);
        if (parsed.ec != std::errc{} || parsed.ptr != conversation->data() + conversation->size() ||
            session.conversation_id < 0)
            return make_error(ErrorCode::ParseError, "invalid stored conversation identifier");
    }

    if (!files->empty()) {
        auto parsed = parse_json(*files, "open files");
        if (!parsed) return parsed.error();
        if (!parsed->is_array())
            return make_error(ErrorCode::ParseError, "stored open files must be an array");
        for (const auto& entry : *parsed) {
            if (!entry.is_string())
                return make_error(ErrorCode::ParseError, "stored open file must be a string");
            session.open_files.push_back(entry.get<std::string>());
        }
    }

    if (draft_repo_ && !session.workspace_path.empty()) {
        auto draft = draft_repo_->load(session.workspace_path);
        if (!draft) return draft.error();
        session.draft = *draft;
    }

    // The next unclean exit must be detectable, so clear the flag now.
    if (auto status = app_state_repo_->set(kCleanShutdownKey, "0"); !status) return status.error();

    if (session.had_unclean_shutdown) {
        log_warn(LogChannel::App, "previous session did not shut down cleanly",
                 {{"workspace", session.workspace_path},
                  {"conversation", std::to_string(session.conversation_id)}});
    }
    return session;
}

Status Application::checkpoint_session(const std::vector<std::string>& open_files,
                                       const std::string& draft) {
    if (!app_state_repo_) return make_error(ErrorCode::Internal, "not initialised");

    Json files = Json::array();
    for (const auto& file : open_files)
        files.push_back(file);
    if (auto status = app_state_repo_->set(kOpenFilesKey, files.dump()); !status) return status;

    if (draft_repo_ && workspace_ && workspace_->is_open()) {
        const auto key = path_to_utf8(workspace_->root());
        auto status = draft.empty() ? draft_repo_->clear(key) : draft_repo_->save(key, draft);
        if (!status) return status;
    }
    return ok_status();
}

Status Application::mark_clean_shutdown() {
    if (!app_state_repo_) return make_error(ErrorCode::Internal, "not initialised");
    return app_state_repo_->set(kCleanShutdownKey, "1");
}

}   // namespace localcode
