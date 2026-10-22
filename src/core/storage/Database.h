#pragma once
// ---------------------------------------------------------------------------
// SQLite wrapper.
//
// A real schema with real tables, not one giant JSON blob: conversations grow
// to thousands of messages and the GUI needs to page through them without
// reading everything. Write-ahead logging is on so a long agent run writing tool
// history never blocks the UI reading it.
//
// The connection is guarded by a mutex and shared across threads. SQLite is
// compiled with SQLITE_THREADSAFE=1, but serialising at this level keeps
// transaction boundaries meaningful when several subsystems write at once.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../util/Result.h"

struct sqlite3;
struct sqlite3_stmt;

namespace localcode {

class Database;

// A prepared statement. Bind indices are 1-based, column indices 0-based, as in
// the SQLite C API.
class Statement {
public:
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, int value);
    Statement& bind(int index, double value);
    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, std::nullptr_t);
    Statement& bind(int index, bool value);
    Statement& bind_blob(int index, const void* data, std::size_t size);

    // Advances one row. Returns true when a row is available.
    Result<bool> step();

    // Runs to completion, discarding rows.
    Status execute();

    [[nodiscard]] std::int64_t  column_int(int index) const;
    [[nodiscard]] double        column_double(int index) const;
    [[nodiscard]] std::string   column_text(int index) const;
    [[nodiscard]] bool          column_bool(int index) const;
    [[nodiscard]] bool          column_is_null(int index) const;
    [[nodiscard]] int           column_count() const;
    [[nodiscard]] std::string   column_name(int index) const;

    void reset();

private:
    friend class Database;
    Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}

    sqlite3*      db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// RAII transaction. Rolls back unless commit() is called, so an early return in
// the middle of a multi-table write cannot leave a half-saved conversation.
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Status commit();

    [[nodiscard]] Status status() const { return begin_status_; }

private:
    Database* database_;
    bool      active_ = false;
    Status begin_status_;
};

class Database {
public:
    Database();
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (creating if needed) and brings the schema up to date.
    Status open(const std::filesystem::path& path);
    // In-memory database, used by the tests.
    Status open_in_memory();
    void   close();

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::filesystem::path path() const;

    Result<Statement> prepare(std::string_view sql);
    Status            execute(std::string_view sql);

    [[nodiscard]] std::int64_t last_insert_id() const;
    [[nodiscard]] int          changes() const;

    // Current schema version, for diagnostics.
    [[nodiscard]] Result<int> schema_version();

    // Runs the callback inside a transaction, rolling back if it returns an error.
    Status with_transaction(const std::function<Status()>& body);

    std::mutex& mutex() { return mutex_; }

    // Verifies database integrity. Run at start-up after an unclean shutdown.
    Result<std::string> integrity_check();

private:
    Status apply_migrations();
    Status configure_connection();

    sqlite3*              handle_ = nullptr;
    std::filesystem::path path_;
    mutable std::mutex    mutex_;
};

}  // namespace localcode
