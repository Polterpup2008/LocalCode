#include "Repositories.h"
#include "../platform/PathEncoding.h"

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

Json tool_calls_to_json(const std::vector<ToolCall>& calls) {
    Json array = Json::array();
    for (const auto& call : calls) {
        array.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments_json}});
    }
    return array;
}

std::vector<ToolCall> tool_calls_from_json(std::string_view text) {
    std::vector<ToolCall> calls;
    if (strings::trim(text).empty()) return calls;
    auto parsed = parse_json(text, "stored tool calls");
    if (!parsed || !parsed->is_array()) return calls;
    for (const auto& entry : *parsed) {
        ToolCall call;
        call.id = json_string(entry, "id");
        call.name = json_string(entry, "name");
        call.arguments_json = json_string(entry, "arguments");
        if (!call.name.empty()) calls.push_back(std::move(call));
    }
    return calls;
}

StoredMessage read_message(const Statement& statement) {
    StoredMessage stored;
    stored.id = statement.column_int(0);
    stored.conversation_id = statement.column_int(1);
    stored.sequence = statement.column_int(2);
    stored.message.role = role_from_string(statement.column_text(3));
    stored.message.content = statement.column_text(4);
    stored.message.reasoning_content = statement.column_text(5);
    stored.message.tool_calls = tool_calls_from_json(statement.column_text(6));
    stored.message.tool_call_id = statement.column_text(7);
    stored.message.name = statement.column_text(8);
    stored.created_at = statement.column_int(9);
    stored.token_count = static_cast<int>(statement.column_int(10));
    return stored;
}

constexpr const char* kMessageColumns =
    "id, conversation_id, sequence, role, content, reasoning, tool_calls_json, "
    "tool_call_id, tool_name, created_at, token_count";

}  // namespace

// ===========================================================================
// Settings
// ===========================================================================
Result<std::string> SettingsRepository::get(std::string_view key, std::string fallback) const {
    auto statement = db_.prepare("SELECT value FROM settings WHERE key = ?1");
    if (!statement) return statement.error();
    statement->bind(1, key);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return fallback;
    return statement->column_text(0);
}

Status SettingsRepository::set(std::string_view key, std::string_view value) {
    auto statement = db_.prepare(
        "INSERT INTO settings(key, value, updated_at) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at");
    if (!statement) return statement.error();
    statement->bind(1, key).bind(2, value).bind(3, now_unix_ms());
    return statement->execute();
}

Status SettingsRepository::set_all(const std::vector<std::pair<std::string, std::string>>& values) {
    if (values.empty()) return ok_status();
    std::string sql = "INSERT INTO settings(key, value, updated_at) VALUES ";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) sql += ',';
        sql += "(?,?,?)";
    }
    sql += " ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at";
    auto statement = db_.prepare(sql);
    if (!statement) return statement.error();
    const auto timestamp = now_unix_ms();
    int index = 1;
    for (const auto& [key, value] : values) {
        statement->bind(index++, key);
        statement->bind(index++, value);
        statement->bind(index++, timestamp);
    }
    return statement->execute();
}

bool SettingsRepository::get_bool(std::string_view key, bool fallback) const {
    auto value = get(key);
    if (!value || value->empty()) return fallback;
    const auto lowered = strings::to_lower(*value);
    return lowered == "1" || lowered == "true" || lowered == "yes";
}

long long SettingsRepository::get_int(std::string_view key, long long fallback) const {
    auto value = get(key);
    if (!value || value->empty()) return fallback;
    try {
        return std::stoll(*value);
    } catch (...) {
        return fallback;
    }
}

double SettingsRepository::get_double(std::string_view key, double fallback) const {
    auto value = get(key);
    if (!value || value->empty()) return fallback;
    try {
        return std::stod(*value);
    } catch (...) {
        return fallback;
    }
}

Status SettingsRepository::set_bool(std::string_view key, bool value) {
    return set(key, value ? "true" : "false");
}
Status SettingsRepository::set_int(std::string_view key, long long value) {
    return set(key, std::to_string(value));
}
Status SettingsRepository::set_double(std::string_view key, double value) {
    return set(key, std::to_string(value));
}

Result<std::vector<std::pair<std::string, std::string>>> SettingsRepository::all() const {
    auto statement = db_.prepare("SELECT key, value FROM settings ORDER BY key");
    if (!statement) return statement.error();
    std::vector<std::pair<std::string, std::string>> rows;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        rows.emplace_back(statement->column_text(0), statement->column_text(1));
    }
    return rows;
}

Status SettingsRepository::remove(std::string_view key) {
    auto statement = db_.prepare("DELETE FROM settings WHERE key = ?1");
    if (!statement) return statement.error();
    statement->bind(1, key);
    return statement->execute();
}

// ===========================================================================
// App state
// ===========================================================================
Result<std::string> AppStateRepository::get(std::string_view key, std::string fallback) const {
    auto statement = db_.prepare("SELECT value FROM app_state WHERE key = ?1");
    if (!statement) return statement.error();
    statement->bind(1, key);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return fallback;
    return statement->column_text(0);
}

Status AppStateRepository::set(std::string_view key, std::string_view value) {
    auto statement = db_.prepare(
        "INSERT INTO app_state(key, value, updated_at) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at");
    if (!statement) return statement.error();
    statement->bind(1, key).bind(2, value).bind(3, now_unix_ms());
    return statement->execute();
}

Status AppStateRepository::remove(std::string_view key) {
    auto statement = db_.prepare("DELETE FROM app_state WHERE key = ?1");
    if (!statement) return statement.error();
    statement->bind(1, key);
    return statement->execute();
}

// ===========================================================================
// Workspaces
// ===========================================================================
Result<WorkspaceRecord> WorkspaceRepository::touch(const std::string& path,
                                                   const std::string& display_name) {
    auto statement = db_.prepare(
        "INSERT INTO workspaces(path, display_name, last_opened, open_count) "
        "VALUES(?1, ?2, ?3, 1) "
        "ON CONFLICT(path) DO UPDATE SET display_name = excluded.display_name, "
        "last_opened = excluded.last_opened, open_count = workspaces.open_count + 1");
    if (!statement) return statement.error();
    statement->bind(1, path).bind(2, display_name).bind(3, now_unix_ms());
    if (auto status = statement->execute(); !status) return status.error();

    auto found = find_by_path(path);
    if (!found) return found.error();
    if (!found->has_value()) {
        return make_error(ErrorCode::Internal, "workspace row vanished after insert");
    }
    return **found;
}

Result<std::optional<WorkspaceRecord>> WorkspaceRepository::find_by_path(
    const std::string& path) const {
    auto statement = db_.prepare(
        "SELECT id, path, display_name, last_opened, open_count, settings_json "
        "FROM workspaces WHERE path = ?1");
    if (!statement) return statement.error();
    statement->bind(1, path);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::optional<WorkspaceRecord>{};

    WorkspaceRecord record;
    record.id = statement->column_int(0);
    record.path = statement->column_text(1);
    record.display_name = statement->column_text(2);
    record.last_opened = statement->column_int(3);
    record.open_count = statement->column_int(4);
    record.settings_json = statement->column_text(5);
    return std::optional<WorkspaceRecord>{std::move(record)};
}

Result<std::vector<WorkspaceRecord>> WorkspaceRepository::recent(std::size_t limit) const {
    auto statement = db_.prepare(
        "SELECT id, path, display_name, last_opened, open_count, settings_json "
        "FROM workspaces ORDER BY last_opened DESC LIMIT ?1");
    if (!statement) return statement.error();
    statement->bind(1, static_cast<std::int64_t>(limit));

    std::vector<WorkspaceRecord> records;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        WorkspaceRecord record;
        record.id = statement->column_int(0);
        record.path = statement->column_text(1);
        record.display_name = statement->column_text(2);
        record.last_opened = statement->column_int(3);
        record.open_count = statement->column_int(4);
        record.settings_json = statement->column_text(5);
        records.push_back(std::move(record));
    }
    return records;
}

Status WorkspaceRepository::save_settings(std::int64_t id, const std::string& settings_json) {
    auto statement = db_.prepare("UPDATE workspaces SET settings_json = ?2 WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, settings_json);
    return statement->execute();
}

Status WorkspaceRepository::remove(std::int64_t id) {
    auto statement = db_.prepare("DELETE FROM workspaces WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id);
    return statement->execute();
}

// ===========================================================================
// Model profiles
// ===========================================================================
Status ModelProfileRepository::save(const ModelProfile& profile) {
    if (!profile.valid()) {
        return make_error(ErrorCode::InvalidArgument, "model profile has no id or path");
    }
    auto statement = db_.prepare(
        "INSERT INTO model_profiles(id, display_name, model_path, profile_json, created_at, "
        "last_used_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(id) DO UPDATE SET display_name = excluded.display_name, "
        "model_path = excluded.model_path, profile_json = excluded.profile_json");
    if (!statement) return statement.error();

    const auto payload = to_json(profile).dump();
    statement->bind(1, profile.id)
        .bind(2, profile.display_name)
        .bind(3, path_to_utf8(profile.model_path))
        .bind(4, payload)
        .bind(5, profile.created_unix_ms ? profile.created_unix_ms : now_unix_ms())
        .bind(6, profile.last_used_unix_ms);
    return statement->execute();
}

Result<std::optional<ModelProfile>> ModelProfileRepository::find(const std::string& id) const {
    auto statement = db_.prepare("SELECT profile_json FROM model_profiles WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::optional<ModelProfile>{};

    auto parsed = parse_json(statement->column_text(0), "stored model profile");
    if (!parsed) return parsed.error();
    return std::optional<ModelProfile>{model_profile_from_json(*parsed)};
}

Result<std::vector<ModelProfile>> ModelProfileRepository::all() const {
    auto statement = db_.prepare(
        "SELECT profile_json FROM model_profiles ORDER BY last_used_at DESC, display_name");
    if (!statement) return statement.error();

    std::vector<ModelProfile> profiles;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        auto parsed = parse_json(statement->column_text(0), "stored model profile");
        if (!parsed) {
            log_warn(LogChannel::App, "skipping unreadable model profile",
                     {{"error", parsed.error().message}});
            continue;
        }
        profiles.push_back(model_profile_from_json(*parsed));
    }
    return profiles;
}

Status ModelProfileRepository::mark_used(const std::string& id) {
    auto statement = db_.prepare("UPDATE model_profiles SET last_used_at = ?2 WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, now_unix_ms());
    return statement->execute();
}

Status ModelProfileRepository::remove(const std::string& id) {
    auto statement = db_.prepare("DELETE FROM model_profiles WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id);
    return statement->execute();
}

// ===========================================================================
// Conversations
// ===========================================================================
Result<std::int64_t> ConversationRepository::create(std::int64_t workspace_id,
                                                    const std::string& title,
                                                    const std::string& model_id) {
    auto statement = db_.prepare(
        "INSERT INTO conversations(workspace_id, title, model_id, created_at, updated_at) "
        "VALUES(?1, ?2, ?3, ?4, ?4)");
    if (!statement) return statement.error();
    const auto now = now_unix_ms();
    if (workspace_id > 0) statement->bind(1, workspace_id);
    else statement->bind(1, nullptr);
    statement->bind(2, title).bind(3, model_id).bind(4, now);
    if (auto status = statement->execute(); !status) return status.error();
    return db_.last_insert_id();
}

Status ConversationRepository::rename(std::int64_t id, const std::string& title) {
    auto statement =
        db_.prepare("UPDATE conversations SET title = ?2, updated_at = ?3 WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, title).bind(3, now_unix_ms());
    return statement->execute();
}

Status ConversationRepository::set_model(std::int64_t id, const std::string& model_id) {
    auto statement =
        db_.prepare("UPDATE conversations SET model_id = ?2, updated_at = ?3 WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, model_id).bind(3, now_unix_ms());
    return statement->execute();
}

Status ConversationRepository::archive(std::int64_t id, bool archived) {
    auto statement =
        db_.prepare("UPDATE conversations SET archived = ?2, updated_at = ?3 WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, archived).bind(3, now_unix_ms());
    return statement->execute();
}

Status ConversationRepository::remove(std::int64_t id) {
    auto statement = db_.prepare("DELETE FROM conversations WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id);
    return statement->execute();
}

Result<std::optional<ConversationRecord>> ConversationRepository::find(std::int64_t id) const {
    auto statement = db_.prepare(
        "SELECT id, COALESCE(workspace_id, 0), title, model_id, created_at, updated_at, archived "
        "FROM conversations WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::optional<ConversationRecord>{};

    ConversationRecord record;
    record.id = statement->column_int(0);
    record.workspace_id = statement->column_int(1);
    record.title = statement->column_text(2);
    record.model_id = statement->column_text(3);
    record.created_at = statement->column_int(4);
    record.updated_at = statement->column_int(5);
    record.archived = statement->column_bool(6);
    return std::optional<ConversationRecord>{std::move(record)};
}

Result<std::vector<ConversationRecord>> ConversationRepository::recent(std::int64_t workspace_id,
                                                                       std::size_t limit) const {
    // workspace_id == 0 means "every workspace".
    auto statement = db_.prepare(
        "SELECT id, COALESCE(workspace_id, 0), title, model_id, created_at, updated_at, archived "
        "FROM conversations WHERE (?1 = 0 OR workspace_id = ?1) AND archived = 0 "
        "ORDER BY updated_at DESC LIMIT ?2");
    if (!statement) return statement.error();
    statement->bind(1, workspace_id).bind(2, static_cast<std::int64_t>(limit));

    std::vector<ConversationRecord> records;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        ConversationRecord record;
        record.id = statement->column_int(0);
        record.workspace_id = statement->column_int(1);
        record.title = statement->column_text(2);
        record.model_id = statement->column_text(3);
        record.created_at = statement->column_int(4);
        record.updated_at = statement->column_int(5);
        record.archived = statement->column_bool(6);
        records.push_back(std::move(record));
    }
    return records;
}

Result<std::int64_t> ConversationRepository::append_message(std::int64_t conversation_id,
                                                             const ChatMessage& message,
                                                             int token_count) {
    auto sequence_statement = db_.prepare(
        "SELECT COALESCE(MAX(sequence), 0) + 1 FROM messages WHERE conversation_id = ?1");
    if (!sequence_statement) return sequence_statement.error();
    sequence_statement->bind(1, conversation_id);
    auto row = sequence_statement->step();
    if (!row) return row.error();
    const std::int64_t sequence = *row ? sequence_statement->column_int(0) : 1;

    auto statement = db_.prepare(
        "INSERT INTO messages(conversation_id, sequence, role, content, reasoning, "
        "tool_calls_json, tool_call_id, tool_name, created_at, token_count) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
    if (!statement) return statement.error();

    const auto calls = message.tool_calls.empty() ? std::string{}
                                                   : tool_calls_to_json(message.tool_calls).dump();
    statement->bind(1, conversation_id)
        .bind(2, sequence)
        .bind(3, to_string(message.role))
        .bind(4, message.content)
        .bind(5, message.reasoning_content)
        .bind(6, calls)
        .bind(7, message.tool_call_id)
        .bind(8, message.name)
        .bind(9, now_unix_ms())
        .bind(10, token_count);
    if (auto status = statement->execute(); !status) return status.error();

    const auto id = db_.last_insert_id();

    auto touch = db_.prepare("UPDATE conversations SET updated_at = ?2 WHERE id = ?1");
    if (touch) {
        touch->bind(1, conversation_id).bind(2, now_unix_ms());
        touch->execute();
    }
    return id;
}

Result<std::vector<StoredMessage>> ConversationRepository::messages(std::int64_t conversation_id,
                                                                     std::size_t limit,
                                                                     std::size_t offset) const {
    std::string sql = std::string{"SELECT "} + kMessageColumns +
                      " FROM messages WHERE conversation_id = ?1 ORDER BY sequence";
    if (limit > 0) sql += " LIMIT ?2 OFFSET ?3";

    auto statement = db_.prepare(sql);
    if (!statement) return statement.error();
    statement->bind(1, conversation_id);
    if (limit > 0) {
        statement->bind(2, static_cast<std::int64_t>(limit))
            .bind(3, static_cast<std::int64_t>(offset));
    }

    std::vector<StoredMessage> messages;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        messages.push_back(read_message(*statement));
    }
    return messages;
}

Result<std::int64_t> ConversationRepository::message_count(std::int64_t conversation_id) const {
    auto statement = db_.prepare("SELECT COUNT(*) FROM messages WHERE conversation_id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, conversation_id);
    auto row = statement->step();
    if (!row) return row.error();
    return *row ? statement->column_int(0) : 0;
}

Result<std::vector<StoredMessage>> ConversationRepository::search(const std::string& query,
                                                                   std::size_t limit) const {
    // FTS5 treats several characters as operators; quoting the whole term makes
    // a user's literal search behave the way they expect.
    const std::string quoted = "\"" + strings::replace_all(query, "\"", "\"\"") + "\"";

    std::string sql = std::string{"SELECT "};
    for (const auto& column : strings::split(kMessageColumns, ',', false)) {
        sql += "m." + strings::trim(column) + ", ";
    }
    sql.erase(sql.size() - 2);
    sql += " FROM messages_fts f JOIN messages m ON m.id = f.rowid "
           "WHERE messages_fts MATCH ?1 ORDER BY rank LIMIT ?2";

    auto statement = db_.prepare(sql);
    if (!statement) return statement.error();
    statement->bind(1, quoted).bind(2, static_cast<std::int64_t>(limit));

    std::vector<StoredMessage> messages;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        messages.push_back(read_message(*statement));
    }
    return messages;
}

// ===========================================================================
// Tool history
// ===========================================================================
Status ToolHistoryRepository::append(const ToolHistoryRecord& record) {
    auto statement = db_.prepare(
        "INSERT INTO tool_history(conversation_id, tool_name, arguments_json, succeeded, "
        "summary, detail, duration_ms, created_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    if (!statement) return statement.error();
    if (record.conversation_id > 0) statement->bind(1, record.conversation_id);
    else statement->bind(1, nullptr);
    statement->bind(2, record.tool_name)
        .bind(3, record.arguments_json)
        .bind(4, record.succeeded)
        .bind(5, record.summary)
        .bind(6, record.detail)
        .bind(7, record.duration_ms)
        .bind(8, record.created_at ? record.created_at : now_unix_ms());
    return statement->execute();
}

namespace {
Result<std::vector<ToolHistoryRecord>> read_tool_history(Result<Statement>& statement) {
    if (!statement) return statement.error();
    std::vector<ToolHistoryRecord> records;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        ToolHistoryRecord record;
        record.id = statement->column_int(0);
        record.conversation_id = statement->column_int(1);
        record.tool_name = statement->column_text(2);
        record.arguments_json = statement->column_text(3);
        record.succeeded = statement->column_bool(4);
        record.summary = statement->column_text(5);
        record.detail = statement->column_text(6);
        record.duration_ms = statement->column_int(7);
        record.created_at = statement->column_int(8);
        records.push_back(std::move(record));
    }
    return records;
}
}  // namespace

Result<std::vector<ToolHistoryRecord>> ToolHistoryRepository::for_conversation(
    std::int64_t conversation_id, std::size_t limit) const {
    auto statement = db_.prepare(
        "SELECT id, COALESCE(conversation_id, 0), tool_name, arguments_json, succeeded, summary, "
        "detail, duration_ms, created_at FROM tool_history WHERE conversation_id = ?1 "
        "ORDER BY id DESC LIMIT ?2");
    if (statement) statement->bind(1, conversation_id).bind(2, static_cast<std::int64_t>(limit));
    return read_tool_history(statement);
}

Result<std::vector<ToolHistoryRecord>> ToolHistoryRepository::recent(std::size_t limit) const {
    auto statement = db_.prepare(
        "SELECT id, COALESCE(conversation_id, 0), tool_name, arguments_json, succeeded, summary, "
        "detail, duration_ms, created_at FROM tool_history ORDER BY id DESC LIMIT ?1");
    if (statement) statement->bind(1, static_cast<std::int64_t>(limit));
    return read_tool_history(statement);
}

// ===========================================================================
// Tasks
// ===========================================================================
Result<std::int64_t> TaskRepository::create(std::int64_t conversation_id,
                                             const std::string& objective,
                                             const std::string& state_json) {
    auto statement = db_.prepare(
        "INSERT INTO tasks(conversation_id, objective, state_json, status, created_at, updated_at) "
        "VALUES(?1, ?2, ?3, 'active', ?4, ?4)");
    if (!statement) return statement.error();
    if (conversation_id > 0) statement->bind(1, conversation_id);
    else statement->bind(1, nullptr);
    statement->bind(2, objective).bind(3, state_json).bind(4, now_unix_ms());
    if (auto status = statement->execute(); !status) return status.error();
    return db_.last_insert_id();
}

Status TaskRepository::update(std::int64_t id, const std::string& state_json,
                              const std::string& status) {
    auto statement = db_.prepare(
        "UPDATE tasks SET state_json = ?2, status = ?3, updated_at = ?4 WHERE id = ?1");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, state_json).bind(3, status).bind(4, now_unix_ms());
    return statement->execute();
}

Result<std::optional<TaskRecord>> TaskRepository::latest_for_conversation(
    std::int64_t conversation_id) const {
    auto statement = db_.prepare(
        "SELECT id, COALESCE(conversation_id, 0), objective, state_json, status, created_at, "
        "updated_at FROM tasks WHERE conversation_id = ?1 ORDER BY id DESC LIMIT 1");
    if (!statement) return statement.error();
    statement->bind(1, conversation_id);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::optional<TaskRecord>{};

    TaskRecord record;
    record.id = statement->column_int(0);
    record.conversation_id = statement->column_int(1);
    record.objective = statement->column_text(2);
    record.state_json = statement->column_text(3);
    record.status = statement->column_text(4);
    record.created_at = statement->column_int(5);
    record.updated_at = statement->column_int(6);
    return std::optional<TaskRecord>{std::move(record)};
}

Result<std::vector<TaskRecord>> TaskRepository::history(std::size_t limit) const {
    auto statement = db_.prepare(
        "SELECT id, COALESCE(conversation_id, 0), objective, state_json, status, created_at, "
        "updated_at FROM tasks ORDER BY id DESC LIMIT ?1");
    if (!statement) return statement.error();
    statement->bind(1, static_cast<std::int64_t>(limit));

    std::vector<TaskRecord> records;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        TaskRecord record;
        record.id = statement->column_int(0);
        record.conversation_id = statement->column_int(1);
        record.objective = statement->column_text(2);
        record.state_json = statement->column_text(3);
        record.status = statement->column_text(4);
        record.created_at = statement->column_int(5);
        record.updated_at = statement->column_int(6);
        records.push_back(std::move(record));
    }
    return records;
}

// ===========================================================================
// Drafts
// ===========================================================================
Status DraftRepository::save(const std::string& workspace_path, const std::string& text) {
    auto statement = db_.prepare(
        "INSERT INTO drafts(workspace_path, text, updated_at) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(workspace_path) DO UPDATE SET text = excluded.text, "
        "updated_at = excluded.updated_at");
    if (!statement) return statement.error();
    statement->bind(1, workspace_path).bind(2, text).bind(3, now_unix_ms());
    return statement->execute();
}

Result<std::string> DraftRepository::load(const std::string& workspace_path) const {
    auto statement = db_.prepare("SELECT text FROM drafts WHERE workspace_path = ?1");
    if (!statement) return statement.error();
    statement->bind(1, workspace_path);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::string{};
    return statement->column_text(0);
}

Status DraftRepository::clear(const std::string& workspace_path) {
    auto statement = db_.prepare("DELETE FROM drafts WHERE workspace_path = ?1");
    if (!statement) return statement.error();
    statement->bind(1, workspace_path);
    return statement->execute();
}

// ===========================================================================
// Skill metadata
// ===========================================================================
Status SkillMetadataRepository::upsert(const std::string& id, bool enabled, bool validated,
                                       const std::string& source_path) {
    auto statement = db_.prepare(
        "INSERT INTO skill_metadata(id, enabled, validated, source_path) VALUES(?1, ?2, ?3, ?4) "
        "ON CONFLICT(id) DO UPDATE SET enabled = excluded.enabled, "
        "validated = excluded.validated, source_path = excluded.source_path");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, enabled).bind(3, validated).bind(4, source_path);
    return statement->execute();
}

Status SkillMetadataRepository::record_use(const std::string& id) {
    auto statement = db_.prepare(
        "INSERT INTO skill_metadata(id, use_count, last_used) VALUES(?1, 1, ?2) "
        "ON CONFLICT(id) DO UPDATE SET use_count = skill_metadata.use_count + 1, "
        "last_used = excluded.last_used");
    if (!statement) return statement.error();
    statement->bind(1, id).bind(2, now_unix_ms());
    return statement->execute();
}

Result<std::vector<std::tuple<std::string, bool, bool>>> SkillMetadataRepository::all() const {
    auto statement = db_.prepare("SELECT id, enabled, validated FROM skill_metadata");
    if (!statement) return statement.error();

    std::vector<std::tuple<std::string, bool, bool>> rows;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        rows.emplace_back(statement->column_text(0), statement->column_bool(1),
                          statement->column_bool(2));
    }
    return rows;
}

}  // namespace localcode
