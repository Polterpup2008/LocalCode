#include "Database.h"
#include "../platform/PathEncoding.h"

#include <sqlite3.h>

#include <array>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

Error sqlite_error(sqlite3* db, std::string context) {
    const char* message = db ? sqlite3_errmsg(db) : "no connection";
    const int code = db ? sqlite3_extended_errcode(db) : 0;
    return make_error(ErrorCode::IoError, std::move(context),
                      std::string{message} + " (sqlite " + std::to_string(code) + ")");
}

// Ordered schema migrations. Each entry is applied once, in order, inside a
// transaction, and the user_version pragma records how far we got. Never edit a
// migration that has shipped: add a new one.
const std::array<const char*, 3>& migrations() {
    static const std::array<const char*, 3> kMigrations = {
        // --- 1: core tables -------------------------------------------------
        R"SQL(
        CREATE TABLE settings (
            key         TEXT PRIMARY KEY,
            value       TEXT NOT NULL,
            updated_at  INTEGER NOT NULL
        );

        CREATE TABLE workspaces (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            path          TEXT NOT NULL UNIQUE,
            display_name  TEXT NOT NULL,
            last_opened   INTEGER NOT NULL DEFAULT 0,
            open_count    INTEGER NOT NULL DEFAULT 0,
            settings_json TEXT NOT NULL DEFAULT '{}'
        );
        CREATE INDEX idx_workspaces_last_opened ON workspaces(last_opened DESC);

        CREATE TABLE model_profiles (
            id           TEXT PRIMARY KEY,
            display_name TEXT NOT NULL,
            model_path   TEXT NOT NULL,
            profile_json TEXT NOT NULL,
            created_at   INTEGER NOT NULL,
            last_used_at INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX idx_model_profiles_last_used ON model_profiles(last_used_at DESC);

        CREATE TABLE conversations (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            workspace_id  INTEGER REFERENCES workspaces(id) ON DELETE SET NULL,
            title         TEXT NOT NULL DEFAULT '',
            model_id      TEXT NOT NULL DEFAULT '',
            created_at    INTEGER NOT NULL,
            updated_at    INTEGER NOT NULL,
            archived      INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX idx_conversations_updated ON conversations(updated_at DESC);
        CREATE INDEX idx_conversations_workspace ON conversations(workspace_id);

        CREATE TABLE messages (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
            sequence        INTEGER NOT NULL,
            role            TEXT NOT NULL,
            content         TEXT NOT NULL DEFAULT '',
            reasoning       TEXT NOT NULL DEFAULT '',
            tool_calls_json TEXT NOT NULL DEFAULT '',
            tool_call_id    TEXT NOT NULL DEFAULT '',
            tool_name       TEXT NOT NULL DEFAULT '',
            created_at      INTEGER NOT NULL,
            token_count     INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX idx_messages_conversation ON messages(conversation_id, sequence);

        CREATE TABLE tool_history (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id INTEGER REFERENCES conversations(id) ON DELETE CASCADE,
            tool_name       TEXT NOT NULL,
            arguments_json  TEXT NOT NULL DEFAULT '{}',
            succeeded       INTEGER NOT NULL DEFAULT 0,
            summary         TEXT NOT NULL DEFAULT '',
            detail          TEXT NOT NULL DEFAULT '',
            duration_ms     INTEGER NOT NULL DEFAULT 0,
            created_at      INTEGER NOT NULL
        );
        CREATE INDEX idx_tool_history_conversation ON tool_history(conversation_id, id);

        CREATE TABLE tasks (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            conversation_id INTEGER REFERENCES conversations(id) ON DELETE CASCADE,
            objective       TEXT NOT NULL DEFAULT '',
            state_json      TEXT NOT NULL DEFAULT '{}',
            status          TEXT NOT NULL DEFAULT 'active',
            created_at      INTEGER NOT NULL,
            updated_at      INTEGER NOT NULL
        );
        CREATE INDEX idx_tasks_conversation ON tasks(conversation_id);

        CREATE TABLE skill_metadata (
            id          TEXT PRIMARY KEY,
            enabled     INTEGER NOT NULL DEFAULT 1,
            validated   INTEGER NOT NULL DEFAULT 1,
            use_count   INTEGER NOT NULL DEFAULT 0,
            last_used   INTEGER NOT NULL DEFAULT 0,
            source_path TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE app_state (
            key        TEXT PRIMARY KEY,
            value      TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        );
        )SQL",

        // --- 2: full-text search over messages -------------------------------
        R"SQL(
        CREATE VIRTUAL TABLE messages_fts USING fts5(
            content,
            content='messages',
            content_rowid='id',
            tokenize='unicode61'
        );

        CREATE TRIGGER messages_fts_insert AFTER INSERT ON messages BEGIN
            INSERT INTO messages_fts(rowid, content) VALUES (new.id, new.content);
        END;
        CREATE TRIGGER messages_fts_delete AFTER DELETE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, content)
                VALUES ('delete', old.id, old.content);
        END;
        CREATE TRIGGER messages_fts_update AFTER UPDATE ON messages BEGIN
            INSERT INTO messages_fts(messages_fts, rowid, content)
                VALUES ('delete', old.id, old.content);
            INSERT INTO messages_fts(rowid, content) VALUES (new.id, new.content);
        END;
        )SQL",

        // --- 3: recovery of unsent drafts ------------------------------------
        R"SQL(
        CREATE TABLE drafts (
            workspace_path TEXT PRIMARY KEY,
            text           TEXT NOT NULL DEFAULT '',
            updated_at     INTEGER NOT NULL
        );
        )SQL",
    };
    return kMigrations;
}

}  // namespace

// ---------------------------------------------------------------------------
Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        db_ = other.db_;
        stmt_ = other.stmt_;
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
    return *this;
}
Statement& Statement::bind(int index, int value) {
    sqlite3_bind_int(stmt_, index, value);
    return *this;
}
Statement& Statement::bind(int index, double value) {
    sqlite3_bind_double(stmt_, index, value);
    return *this;
}
Statement& Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT: SQLite copies the text, so callers may pass temporaries.
    sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
    return *this;
}
Statement& Statement::bind(int index, std::nullptr_t) {
    sqlite3_bind_null(stmt_, index);
    return *this;
}
Statement& Statement::bind(int index, bool value) {
    sqlite3_bind_int(stmt_, index, value ? 1 : 0);
    return *this;
}
Statement& Statement::bind_blob(int index, const void* data, std::size_t size) {
    sqlite3_bind_blob(stmt_, index, data, static_cast<int>(size), SQLITE_TRANSIENT);
    return *this;
}

Result<bool> Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return sqlite_error(db_, "statement failed");
}

Status Statement::execute() {
    for (;;) {
        auto row = step();
        if (!row) return row.error();
        if (!*row) return ok_status();
    }
}

std::int64_t Statement::column_int(int index) const { return sqlite3_column_int64(stmt_, index); }
double Statement::column_double(int index) const { return sqlite3_column_double(stmt_, index); }
bool Statement::column_bool(int index) const { return sqlite3_column_int(stmt_, index) != 0; }
bool Statement::column_is_null(int index) const {
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}
int Statement::column_count() const { return sqlite3_column_count(stmt_); }

std::string Statement::column_text(int index) const {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
    if (!text) return {};
    const int size = sqlite3_column_bytes(stmt_, index);
    return std::string{text, static_cast<std::size_t>(size)};
}

std::string Statement::column_name(int index) const {
    const char* name = sqlite3_column_name(stmt_, index);
    return name ? name : "";
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

// ---------------------------------------------------------------------------
Transaction::Transaction(Database& database) : database_(&database) {
    begin_status_ = database_->execute("BEGIN IMMEDIATE");
    active_ = begin_status_.ok();
}

Transaction::~Transaction() {
    if (active_) {
        // Nobody committed: undo everything rather than leaving partial state.
        auto rolled_back = database_->execute("ROLLBACK");
        if (!rolled_back) {
            log_error(LogChannel::App, "transaction rollback failed",
                      {{"error", rolled_back.error().message}});
        }
    }
}

Status Transaction::commit() {
    if (!active_) return make_error(ErrorCode::Internal, "transaction is not active");
    auto status = database_->execute("COMMIT");
    if (status) active_ = false;
    return status;
}

// ---------------------------------------------------------------------------
Database::Database() = default;

Database::~Database() { close(); }

bool Database::is_open() const {
    std::lock_guard lock(mutex_);
    return handle_ != nullptr;
}

std::filesystem::path Database::path() const {
    std::lock_guard lock(mutex_);
    return path_;
}

Status Database::configure_connection() {
    std::lock_guard lock(mutex_);
    // WAL keeps readers from blocking on the agent's writes. NORMAL synchronous
    // is the standard pairing: durable across application crashes, and a power
    // cut can lose at most the last transaction of conversation history.
    const char* pragmas =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA foreign_keys = ON;"
        "PRAGMA busy_timeout = 5000;"
        "PRAGMA temp_store = MEMORY;";
    char* error = nullptr;
    if (sqlite3_exec(handle_, pragmas, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "unknown";
        sqlite3_free(error);
        return make_error(ErrorCode::IoError, "cannot configure the database", message);
    }
    return ok_status();
}

Status Database::open(const std::filesystem::path& path) {
    close();

    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    // SQLite takes UTF-8 paths on every platform, including Windows.
    const auto utf8_path = path.generic_u8string();
    const std::string path_string{reinterpret_cast<const char*>(utf8_path.c_str()),
                                  utf8_path.size()};

    // The lock covers only the handle assignment. configure_connection() and
    // apply_migrations() call prepare()/execute(), which take this same mutex -
    // holding it across them would deadlock, and it is not needed: the
    // connection is not reachable from another thread until open() returns.
    {
        std::lock_guard lock(mutex_);
        const int rc = sqlite3_open_v2(path_string.c_str(), &handle_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                           SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
        if (rc != SQLITE_OK) {
            auto error = sqlite_error(handle_, "cannot open database " + path_to_utf8(path));
            if (handle_) {
                sqlite3_close(handle_);
                handle_ = nullptr;
            }
            return error;
        }
        path_ = path;
    }

    if (auto status = configure_connection(); !status) return status;
    if (auto status = apply_migrations(); !status) return status;

    log_info(LogChannel::App, "database ready", {{"path", path_to_utf8(path)}});
    return ok_status();
}

Status Database::open_in_memory() {
    close();
    {
        std::lock_guard lock(mutex_);
        if (sqlite3_open_v2(":memory:", &handle_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            auto error = sqlite_error(handle_, "cannot open in-memory database");
            if (handle_) {
                sqlite3_close(handle_);
                handle_ = nullptr;
            }
            return error;
        }
        path_ = ":memory:";
        // WAL is meaningless for :memory:, but the other pragmas still apply.
        sqlite3_exec(handle_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    }
    return apply_migrations();
}

void Database::close() {
    std::lock_guard lock(mutex_);
    if (handle_) {
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

Result<Statement> Database::prepare(std::string_view sql) {
    std::lock_guard lock(mutex_);
    if (!handle_) return make_error(ErrorCode::Internal, "database is not open");

    sqlite3_stmt* statement = nullptr;
    const int rc = sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()),
                                      &statement, nullptr);
    if (rc != SQLITE_OK) {
        return sqlite_error(handle_, "cannot prepare: " + strings::utf8_truncate(sql, 200));
    }
    return Statement{handle_, statement};
}

Status Database::execute(std::string_view sql) {
    std::lock_guard lock(mutex_);
    if (!handle_) return make_error(ErrorCode::Internal, "database is not open");

    const std::string statement{sql};
    char* error = nullptr;
    if (sqlite3_exec(handle_, statement.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "unknown";
        sqlite3_free(error);
        return make_error(ErrorCode::IoError,
                          "statement failed: " + strings::utf8_truncate(sql, 200), message);
    }
    return ok_status();
}

std::int64_t Database::last_insert_id() const {
    std::lock_guard lock(mutex_);
    return handle_ ? sqlite3_last_insert_rowid(handle_) : 0;
}

int Database::changes() const {
    std::lock_guard lock(mutex_);
    return handle_ ? sqlite3_changes(handle_) : 0;
}

Result<int> Database::schema_version() {
    auto statement = prepare("PRAGMA user_version");
    if (!statement) return statement.error();
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return 0;
    return static_cast<int>(statement->column_int(0));
}

Status Database::apply_migrations() {
    auto version = schema_version();
    if (!version) return version.error();

    const auto& all = migrations();
    if (static_cast<std::size_t>(*version) >= all.size()) return ok_status();

    for (std::size_t index = static_cast<std::size_t>(*version); index < all.size(); ++index) {
        const int target = static_cast<int>(index) + 1;

        if (auto status = execute("BEGIN IMMEDIATE"); !status) return status;
        if (auto status = execute(all[index]); !status) {
            execute("ROLLBACK");
            return make_error(ErrorCode::Internal,
                              "schema migration " + std::to_string(target) + " failed",
                              status.error().detail);
        }
        // PRAGMA user_version does not accept a bound parameter.
        if (auto status = execute("PRAGMA user_version = " + std::to_string(target)); !status) {
            execute("ROLLBACK");
            return status;
        }
        if (auto status = execute("COMMIT"); !status) {
            execute("ROLLBACK");
            return status;
        }
        log_info(LogChannel::App, "applied schema migration",
                 {{"version", std::to_string(target)}});
    }
    return ok_status();
}

Status Database::with_transaction(const std::function<Status()>& body) {
    Transaction transaction{*this};
    if (auto begun = transaction.status(); !begun) return begun;
    auto status = body();
    if (!status) return status;
    return transaction.commit();
}

Result<std::string> Database::integrity_check() {
    auto statement = prepare("PRAGMA integrity_check");
    if (!statement) return statement.error();

    std::vector<std::string> lines;
    for (;;) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        lines.push_back(statement->column_text(0));
    }
    return strings::join(lines, "; ");
}

}  // namespace localcode
