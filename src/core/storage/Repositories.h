#pragma once
// ---------------------------------------------------------------------------
// Typed access to the database.
//
// Each repository owns one area of the schema. They take a Database& rather
// than owning it so a single connection (and therefore a single WAL) serves the
// whole application.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../inference/ChatTypes.h"
#include "../model/ModelProfile.h"
#include "Database.h"

namespace localcode {

// --- Settings and app state -----------------------------------------------
class SettingsRepository {
public:
    explicit SettingsRepository(Database& database) : db_(database) {}

    Result<std::string> get(std::string_view key, std::string fallback = {}) const;
    Status set(std::string_view key, std::string_view value);
    // One SQLite statement: failure leaves every existing setting unchanged.
    Status set_all(const std::vector<std::pair<std::string, std::string>>& values);

    [[nodiscard]] bool  get_bool(std::string_view key, bool fallback) const;
    [[nodiscard]] long long get_int(std::string_view key, long long fallback) const;
    [[nodiscard]] double get_double(std::string_view key, double fallback) const;
    Status set_bool(std::string_view key, bool value);
    Status set_int(std::string_view key, long long value);
    Status set_double(std::string_view key, double value);

    Result<std::vector<std::pair<std::string, std::string>>> all() const;
    Status remove(std::string_view key);

private:
    Database& db_;
};

// Separate from settings: this is volatile UI state (window geometry, open
// files, selected model) that should be restorable but is not configuration.
class AppStateRepository {
public:
    explicit AppStateRepository(Database& database) : db_(database) {}

    Result<std::string> get(std::string_view key, std::string fallback = {}) const;
    Status set(std::string_view key, std::string_view value);
    Status remove(std::string_view key);

private:
    Database& db_;
};

// --- Workspaces ------------------------------------------------------------
struct WorkspaceRecord {
    std::int64_t id = 0;
    std::string  path;
    std::string  display_name;
    std::int64_t last_opened = 0;
    std::int64_t open_count = 0;
    std::string  settings_json = "{}";
};

class WorkspaceRepository {
public:
    explicit WorkspaceRepository(Database& database) : db_(database) {}

    // Inserts or updates by path, bumping the open counters.
    Result<WorkspaceRecord> touch(const std::string& path, const std::string& display_name);
    Result<std::optional<WorkspaceRecord>> find_by_path(const std::string& path) const;
    Result<std::vector<WorkspaceRecord>> recent(std::size_t limit = 20) const;
    Status save_settings(std::int64_t id, const std::string& settings_json);
    Status remove(std::int64_t id);

private:
    Database& db_;
};

// --- Model profiles --------------------------------------------------------
class ModelProfileRepository {
public:
    explicit ModelProfileRepository(Database& database) : db_(database) {}

    Status save(const ModelProfile& profile);
    Result<std::optional<ModelProfile>> find(const std::string& id) const;
    Result<std::vector<ModelProfile>> all() const;
    Status mark_used(const std::string& id);
    Status remove(const std::string& id);

private:
    Database& db_;
};

// --- Conversations ---------------------------------------------------------
struct ConversationRecord {
    std::int64_t id = 0;
    std::int64_t workspace_id = 0;
    std::string  title;
    std::string  model_id;
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
    bool         archived = false;
};

struct StoredMessage {
    std::int64_t id = 0;
    std::int64_t conversation_id = 0;
    std::int64_t sequence = 0;
    ChatMessage  message;
    std::int64_t created_at = 0;
    int          token_count = 0;
};

class ConversationRepository {
public:
    explicit ConversationRepository(Database& database) : db_(database) {}

    Result<std::int64_t> create(std::int64_t workspace_id, const std::string& title,
                                const std::string& model_id);
    Status rename(std::int64_t id, const std::string& title);
    Status set_model(std::int64_t id, const std::string& model_id);
    Status archive(std::int64_t id, bool archived);
    Status remove(std::int64_t id);

    Result<std::optional<ConversationRecord>> find(std::int64_t id) const;
    Result<std::vector<ConversationRecord>> recent(std::int64_t workspace_id,
                                                   std::size_t limit = 50) const;

    Result<std::int64_t> append_message(std::int64_t conversation_id, const ChatMessage& message,
                                        int token_count = 0);
    Result<std::vector<StoredMessage>> messages(std::int64_t conversation_id,
                                                std::size_t limit = 0,
                                                std::size_t offset = 0) const;
    Result<std::int64_t> message_count(std::int64_t conversation_id) const;

    // Full-text search across message bodies.
    Result<std::vector<StoredMessage>> search(const std::string& query,
                                              std::size_t limit = 50) const;

private:
    Database& db_;
};

// --- Tool history ----------------------------------------------------------
struct ToolHistoryRecord {
    std::int64_t id = 0;
    std::int64_t conversation_id = 0;
    std::string  tool_name;
    std::string  arguments_json;
    bool         succeeded = false;
    std::string  summary;
    std::string  detail;
    std::int64_t duration_ms = 0;
    std::int64_t created_at = 0;
};

class ToolHistoryRepository {
public:
    explicit ToolHistoryRepository(Database& database) : db_(database) {}

    Status append(const ToolHistoryRecord& record);
    Result<std::vector<ToolHistoryRecord>> for_conversation(std::int64_t conversation_id,
                                                            std::size_t limit = 200) const;
    Result<std::vector<ToolHistoryRecord>> recent(std::size_t limit = 200) const;

private:
    Database& db_;
};

// --- Tasks -----------------------------------------------------------------
struct TaskRecord {
    std::int64_t id = 0;
    std::int64_t conversation_id = 0;
    std::string  objective;
    std::string  state_json = "{}";
    std::string  status = "active";
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
};

class TaskRepository {
public:
    explicit TaskRepository(Database& database) : db_(database) {}

    Result<std::int64_t> create(std::int64_t conversation_id, const std::string& objective,
                                const std::string& state_json);
    Status update(std::int64_t id, const std::string& state_json, const std::string& status);
    Result<std::optional<TaskRecord>> latest_for_conversation(std::int64_t conversation_id) const;
    Result<std::vector<TaskRecord>> history(std::size_t limit = 50) const;

private:
    Database& db_;
};

// --- Prompt drafts ---------------------------------------------------------
// An unsent prompt is the thing users are angriest to lose after a crash, so it
// is persisted on a debounce rather than only at shutdown.
class DraftRepository {
public:
    explicit DraftRepository(Database& database) : db_(database) {}

    Status save(const std::string& workspace_path, const std::string& text);
    Result<std::string> load(const std::string& workspace_path) const;
    Status clear(const std::string& workspace_path);

private:
    Database& db_;
};

// --- Skill metadata --------------------------------------------------------
class SkillMetadataRepository {
public:
    explicit SkillMetadataRepository(Database& database) : db_(database) {}

    Status upsert(const std::string& id, bool enabled, bool validated,
                  const std::string& source_path);
    Status record_use(const std::string& id);
    Result<std::vector<std::tuple<std::string, bool, bool>>> all() const;

private:
    Database& db_;
};

}  // namespace localcode
