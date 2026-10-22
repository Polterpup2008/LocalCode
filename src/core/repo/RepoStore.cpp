#include "RepoStore.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <system_error>

#include "Hashing.h"
#include "platform/PathEncoding.h"
#include "util/Json.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace localcode::repo {
namespace {

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS repo_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS repo_files (
    path          TEXT PRIMARY KEY,
    language      TEXT NOT NULL DEFAULT '',
    role          TEXT NOT NULL DEFAULT 'unknown',
    size_bytes    INTEGER NOT NULL DEFAULT 0,
    modified_ms   INTEGER NOT NULL DEFAULT 0,
    content_hash  TEXT NOT NULL DEFAULT '',
    line_count    INTEGER NOT NULL DEFAULT 0,
    skipped       INTEGER NOT NULL DEFAULT 0,
    skip_reason   TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS repo_symbols (
    name      TEXT NOT NULL,
    kind      TEXT NOT NULL,
    file      TEXT NOT NULL,
    line      INTEGER NOT NULL,
    end_line  INTEGER NOT NULL,
    language  TEXT NOT NULL DEFAULT '',
    container TEXT NOT NULL DEFAULT '',
    signature TEXT NOT NULL DEFAULT '',
    evidence  TEXT NOT NULL DEFAULT 'lexical'
);
CREATE INDEX IF NOT EXISTS repo_symbols_name ON repo_symbols(name);
CREATE INDEX IF NOT EXISTS repo_symbols_file ON repo_symbols(file);
CREATE TABLE IF NOT EXISTS repo_references (
    from_file TEXT NOT NULL,
    to_file   TEXT NOT NULL DEFAULT '',
    to_symbol TEXT NOT NULL DEFAULT '',
    kind      TEXT NOT NULL,
    line      INTEGER NOT NULL DEFAULT 0,
    evidence  TEXT NOT NULL DEFAULT 'lexical'
);
CREATE INDEX IF NOT EXISTS repo_refs_from ON repo_references(from_file);
CREATE INDEX IF NOT EXISTS repo_refs_to   ON repo_references(to_file);
CREATE TABLE IF NOT EXISTS repo_commands (
    purpose     TEXT NOT NULL,
    command     TEXT NOT NULL,
    directory   TEXT NOT NULL DEFAULT '',
    source      TEXT NOT NULL DEFAULT '',
    evidence    TEXT NOT NULL DEFAULT 'structural',
    discovery_evidence TEXT NOT NULL DEFAULT 'structural',
    verified    INTEGER NOT NULL DEFAULT 0,
    verified_ms INTEGER NOT NULL DEFAULT 0,
    exit_code   INTEGER NOT NULL DEFAULT -1,
    PRIMARY KEY (purpose, command)
);
CREATE TABLE IF NOT EXISTS repo_facts (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL,
    source      TEXT NOT NULL DEFAULT '',
    evidence    TEXT NOT NULL DEFAULT 'heuristic',
    recorded_ms INTEGER NOT NULL DEFAULT 0,
    depends_on  TEXT NOT NULL DEFAULT '',
    generation  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS repo_overview (
    id   INTEGER PRIMARY KEY CHECK (id = 1),
    json TEXT NOT NULL
);
)SQL";

std::string join(const std::vector<std::string>& items, char separator) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out.push_back(separator);
        out += item;
    }
    return out;
}

std::vector<std::string> split_nonempty(const std::string& text, char separator) {
    std::vector<std::string> out;
    for (auto& piece : strings::split(text, separator, false)) {
        if (!piece.empty()) out.push_back(std::move(piece));
    }
    return out;
}

FileRole role_from_string(std::string_view text) {
    if (text == "source")        return FileRole::Source;
    if (text == "test")          return FileRole::Test;
    if (text == "build")         return FileRole::Build;
    if (text == "config")        return FileRole::Config;
    if (text == "documentation") return FileRole::Documentation;
    if (text == "asset")         return FileRole::Asset;
    if (text == "generated")     return FileRole::Generated;
    if (text == "vendored")      return FileRole::Vendored;
    return FileRole::Unknown;
}

ReferenceKind reference_kind_from_string(std::string_view text) {
    if (text == "include")      return ReferenceKind::Include;
    if (text == "test-subject") return ReferenceKind::TestSubject;
    if (text == "build-input")  return ReferenceKind::BuildInput;
    return ReferenceKind::Mention;
}

RepoSymbol read_symbol(const Statement& row) {
    RepoSymbol symbol;
    symbol.name = row.column_text(0);
    symbol.kind = symbol_kind_from_string(row.column_text(1)).value_or(SymbolKind::Unknown);
    symbol.file = row.column_text(2);
    symbol.line = static_cast<std::size_t>(row.column_int(3));
    symbol.end_line = static_cast<std::size_t>(row.column_int(4));
    symbol.language = row.column_text(5);
    symbol.container = row.column_text(6);
    symbol.signature = row.column_text(7);
    symbol.evidence = evidence_from_string(row.column_text(8)).value_or(Evidence::Lexical);
    return symbol;
}

constexpr const char* kSymbolColumns =
    "name, kind, file, line, end_line, language, container, signature, evidence";

}  // namespace

struct RepoStore::Impl {
    Database db;
};

RepoStore::RepoStore() : impl_(std::make_unique<Impl>()) {}
RepoStore::~RepoStore() = default;
RepoStore::RepoStore(RepoStore&&) noexcept = default;
RepoStore& RepoStore::operator=(RepoStore&&) noexcept = default;

std::filesystem::path RepoStore::index_path(const std::filesystem::path& directory,
                                            const std::filesystem::path& workspace_root) {
    // Named after the workspace so two projects never share an index, and
    // hashed so a path with awkward characters cannot escape the directory.
    const auto canonical = path_to_utf8(workspace_root.lexically_normal());
    // The digest keeps two workspaces with the same folder name apart; the
    // readable half is there so a person looking in the directory can tell
    // which index belongs to what.
    return directory / ("repo-" + slug(path_to_utf8(workspace_root.filename())) + "-" +
                        fnv1a_hex(canonical) + ".db");
}

bool RepoStore::is_open() const { return impl_ && impl_->db.is_open(); }

Status RepoStore::open(const std::filesystem::path& directory,
                       const std::filesystem::path& workspace_root) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return make_error(ErrorCode::IoError, "could not create index directory", ec.message());
    path_ = index_path(directory, workspace_root);
    reset_reason_.clear();

    auto discard_and_retry = [&](std::string reason) -> Status {
        impl_->db.close();
        // The write-ahead log and shared-memory files have to go with it.
        // Removing only the database leaves an orphaned -wal, and SQLite
        // recovers the old contents from it on the next open - so the index
        // that was supposed to have been discarded answers queries again.
        for (const char* suffix : {"", "-wal", "-shm"}) {
            auto companion = path_;
            companion += suffix;
            std::error_code remove_ec;
            std::filesystem::remove(companion, remove_ec);
            if (remove_ec) {
                return make_error(ErrorCode::IoError, "could not discard repository index",
                                  path_to_utf8(companion) + ": " + remove_ec.message());
            }
        }
        reset_reason_ = std::move(reason);
        log_warn(LogChannel::App, "repository index discarded",
                 {{"path", path_to_utf8(path_)}, {"reason", reset_reason_}});
        if (auto status = impl_->db.open(path_); !status) return status;
        return ensure_schema();
    };

    if (auto status = impl_->db.open(path_); !status) {
        return discard_and_retry("the index file could not be opened: " + status.error().message);
    }
    if (auto status = ensure_schema(); !status) {
        return discard_and_retry("the index schema could not be created: " +
                                 status.error().message);
    }

    // A file written by a different indexer is not reinterpreted. Its rows were
    // produced by rules that no longer apply, and guessing which of them still
    // hold is exactly the kind of stale phantom this must not have.
    //
    // The read is scoped so the statement is finalized before anything tries to
    // delete the file: sqlite3_close_v2 defers the real close while a statement
    // is alive, which leaves the file locked and makes the removal fail without
    // saying so.
    int found = 0;
    bool readable = true;
    bool has_format = false;
    bool valid_format = true;
    {
        auto statement = impl_->db.prepare("SELECT value FROM repo_meta WHERE key = 'format'");
        if (!statement) {
            readable = false;
        } else {
            auto row = statement->step();
            if (!row) {
                readable = false;
            } else if (*row) {
                has_format = true;
                const auto value = statement->column_text(0);
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), found);
                valid_format =
                    parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
            }
        }
    }
    if (!readable) return discard_and_retry("the index could not be read");
    if (!valid_format) return discard_and_retry("the index format is malformed");

    if (found != kIndexFormatVersion) {
        if (has_format) {
            return discard_and_retry("it was written by index format " + std::to_string(found) +
                                     ", and this build writes " +
                                     std::to_string(kIndexFormatVersion));
        }
        auto write = impl_->db.prepare(
            "INSERT OR REPLACE INTO repo_meta(key, value) VALUES('format', ?1)");
        if (!write) return write.error();
        write->bind(1, std::to_string(kIndexFormatVersion));
        if (auto status = write->execute(); !status) return status;
    }
    return ok_status();
}

Status RepoStore::open_in_memory() {
    if (auto status = impl_->db.open_in_memory(); !status) return status;
    return ensure_schema();
}

void RepoStore::close() {
    if (impl_) impl_->db.close();
}

Status RepoStore::ensure_schema() { return impl_->db.execute(kSchema); }

Status RepoStore::clear() {
    return impl_->db.execute(
        "DELETE FROM repo_files; DELETE FROM repo_symbols; DELETE FROM repo_references;"
        "DELETE FROM repo_commands; DELETE FROM repo_facts; DELETE FROM repo_overview;");
}

// --- Files -----------------------------------------------------------------

Status RepoStore::put_file(const RepoFile& file) {
    auto statement = impl_->db.prepare(
        "INSERT OR REPLACE INTO repo_files(path, language, role, size_bytes, modified_ms,"
        " content_hash, line_count, skipped, skip_reason)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    if (!statement) return statement.error();
    statement->bind(1, file.path)
        .bind(2, file.language)
        .bind(3, std::string{to_string(file.role)})
        .bind(4, static_cast<std::int64_t>(file.size_bytes))
        .bind(5, file.modified_unix_ms)
        .bind(6, file.content_hash)
        .bind(7, static_cast<std::int64_t>(file.line_count))
        .bind(8, file.skipped)
        .bind(9, file.skip_reason);
    return statement->execute();
}

Status RepoStore::remove_file(std::string_view path) {
    return impl_->db.with_transaction([&]() -> Status {
        for (const char* sql : {"DELETE FROM repo_files WHERE path = ?1",
                                "DELETE FROM repo_symbols WHERE file = ?1",
                                "DELETE FROM repo_references WHERE from_file = ?1"}) {
            auto statement = impl_->db.prepare(sql);
            if (!statement) return statement.error();
            statement->bind(1, path);
            if (auto status = statement->execute(); !status) return status;
        }
        return ok_status();
    });
}

Result<std::vector<RepoFile>> RepoStore::files() const {
    auto statement = impl_->db.prepare(
        "SELECT path, language, role, size_bytes, modified_ms, content_hash, line_count,"
        " skipped, skip_reason FROM repo_files ORDER BY path");
    if (!statement) return statement.error();

    std::vector<RepoFile> out;
    while (true) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        RepoFile file;
        file.path = statement->column_text(0);
        file.language = statement->column_text(1);
        file.role = role_from_string(statement->column_text(2));
        file.size_bytes = static_cast<std::uint64_t>(statement->column_int(3));
        file.modified_unix_ms = statement->column_int(4);
        file.content_hash = statement->column_text(5);
        file.line_count = static_cast<std::size_t>(statement->column_int(6));
        file.skipped = statement->column_bool(7);
        file.skip_reason = statement->column_text(8);
        out.push_back(std::move(file));
    }
    return out;
}

Result<std::optional<RepoFile>> RepoStore::file(std::string_view path) const {
    auto statement = impl_->db.prepare(
        "SELECT path, language, role, size_bytes, modified_ms, content_hash, line_count,"
        " skipped, skip_reason FROM repo_files WHERE path = ?1");
    if (!statement) return statement.error();
    statement->bind(1, path);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::optional<RepoFile>{};

    RepoFile file;
    file.path = statement->column_text(0);
    file.language = statement->column_text(1);
    file.role = role_from_string(statement->column_text(2));
    file.size_bytes = static_cast<std::uint64_t>(statement->column_int(3));
    file.modified_unix_ms = statement->column_int(4);
    file.content_hash = statement->column_text(5);
    file.line_count = static_cast<std::size_t>(statement->column_int(6));
    file.skipped = statement->column_bool(7);
    file.skip_reason = statement->column_text(8);
    return std::optional<RepoFile>{std::move(file)};
}

// --- Symbols ---------------------------------------------------------------

Status RepoStore::replace_symbols(std::string_view path, const std::vector<RepoSymbol>& symbols) {
    return impl_->db.with_transaction([&]() -> Status {
        auto wipe = impl_->db.prepare("DELETE FROM repo_symbols WHERE file = ?1");
        if (!wipe) return wipe.error();
        wipe->bind(1, path);
        if (auto status = wipe->execute(); !status) return status;

        // Prepared once, not once per row. Compiling this statement seventy
        // thousand times is what a full reindex of llama.cpp was doing.
        auto statement = impl_->db.prepare(
            "INSERT INTO repo_symbols(name, kind, file, line, end_line, language, container,"
            " signature, evidence) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
        if (!statement) return statement.error();

        for (const auto& symbol : symbols) {
            statement->reset();
            statement->bind(1, symbol.name)
                .bind(2, std::string{to_string(symbol.kind)})
                .bind(3, symbol.file)
                .bind(4, static_cast<std::int64_t>(symbol.line))
                .bind(5, static_cast<std::int64_t>(symbol.end_line))
                .bind(6, symbol.language)
                .bind(7, symbol.container)
                .bind(8, symbol.signature)
                .bind(9, std::string{to_string(symbol.evidence)});
            if (auto status = statement->execute(); !status) return status;
        }
        return ok_status();
    });
}

Result<std::vector<RepoSymbol>> RepoStore::symbols_named(std::string_view name,
                                                         std::size_t limit) const {
    auto statement = impl_->db.prepare(std::string{"SELECT "} + kSymbolColumns +
                                       " FROM repo_symbols WHERE name = ?1 ORDER BY file, line"
                                       " LIMIT ?2");
    if (!statement) return statement.error();
    statement->bind(1, name).bind(2, static_cast<std::int64_t>(limit));

    std::vector<RepoSymbol> out;
    while (true) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        out.push_back(read_symbol(*statement));
    }
    return out;
}

Result<std::vector<RepoSymbol>> RepoStore::symbols_like(std::string_view fragment,
                                                        std::size_t limit) const {
    // Exact matches first, then prefix, then anywhere: the thing somebody typed
    // in full is almost always the thing they meant.
    auto statement = impl_->db.prepare(
        std::string{"SELECT "} + kSymbolColumns +
        " FROM repo_symbols WHERE name LIKE ?1 ESCAPE '\\'"
        " ORDER BY CASE WHEN name = ?2 THEN 0 WHEN name LIKE ?3 ESCAPE '\\' THEN 1 ELSE 2 END,"
        " length(name), file, line LIMIT ?4");
    if (!statement) return statement.error();

    // LIKE wildcards inside what was typed must match literally.
    auto escaped = strings::replace_all(fragment, "\\", "\\\\");
    escaped = strings::replace_all(escaped, "%", "\\%");
    escaped = strings::replace_all(escaped, "_", "\\_");

    statement->bind(1, "%" + escaped + "%")
        .bind(2, fragment)
        .bind(3, escaped + "%")
        .bind(4, static_cast<std::int64_t>(limit));

    std::vector<RepoSymbol> out;
    while (true) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        out.push_back(read_symbol(*statement));
    }
    return out;
}

Result<std::vector<RepoSymbol>> RepoStore::symbols_in(std::string_view path) const {
    auto statement = impl_->db.prepare(std::string{"SELECT "} + kSymbolColumns +
                                       " FROM repo_symbols WHERE file = ?1 ORDER BY line");
    if (!statement) return statement.error();
    statement->bind(1, path);

    std::vector<RepoSymbol> out;
    while (true) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        out.push_back(read_symbol(*statement));
    }
    return out;
}

Result<std::size_t> RepoStore::symbol_count() const {
    auto statement = impl_->db.prepare("SELECT COUNT(*) FROM repo_symbols");
    if (!statement) return statement.error();
    auto row = statement->step();
    if (!row) return row.error();
    return static_cast<std::size_t>(*row ? statement->column_int(0) : 0);
}

// --- References ------------------------------------------------------------

Status RepoStore::replace_references(std::string_view path,
                                     const std::vector<RepoReference>& references) {
    return impl_->db.with_transaction([&]() -> Status {
        auto wipe = impl_->db.prepare("DELETE FROM repo_references WHERE from_file = ?1");
        if (!wipe) return wipe.error();
        wipe->bind(1, path);
        if (auto status = wipe->execute(); !status) return status;

        auto statement = impl_->db.prepare(
            "INSERT INTO repo_references(from_file, to_file, to_symbol, kind, line, evidence)"
            " VALUES(?1,?2,?3,?4,?5,?6)");
        if (!statement) return statement.error();

        for (const auto& reference : references) {
            statement->reset();
            statement->bind(1, reference.from_file)
                .bind(2, reference.to_file)
                .bind(3, reference.to_symbol)
                .bind(4, std::string{to_string(reference.kind)})
                .bind(5, static_cast<std::int64_t>(reference.line))
                .bind(6, std::string{to_string(reference.evidence)});
            if (auto status = statement->execute(); !status) return status;
        }
        return ok_status();
    });
}

namespace {
Result<std::vector<RepoReference>> read_references(Result<Statement>& statement) {
    if (!statement) return statement.error();
    std::vector<RepoReference> out;
    while (true) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        RepoReference reference;
        reference.from_file = statement->column_text(0);
        reference.to_file = statement->column_text(1);
        reference.to_symbol = statement->column_text(2);
        reference.kind = reference_kind_from_string(statement->column_text(3));
        reference.line = static_cast<std::size_t>(statement->column_int(4));
        reference.evidence = evidence_from_string(statement->column_text(5)).value_or(
            Evidence::Lexical);
        out.push_back(std::move(reference));
    }
    return out;
}
}  // namespace

Result<std::vector<RepoReference>> RepoStore::references_to_file(std::string_view path,
                                                                 std::size_t limit) const {
    auto statement = impl_->db.prepare(
        "SELECT from_file, to_file, to_symbol, kind, line, evidence FROM repo_references"
        " WHERE to_file = ?1"
        " ORDER BY from_file, line LIMIT ?2");
    if (!statement) return statement.error();
    // Resolved targets must match exactly: basename fallback mixes unrelated
    // headers. Refresh re-resolves include edges when the tree changes.
    statement->bind(1, path).bind(2, static_cast<std::int64_t>(limit));
    return read_references(statement);
}

Result<std::vector<RepoReference>> RepoStore::references_from_file(std::string_view path) const {
    auto statement = impl_->db.prepare(
        "SELECT from_file, to_file, to_symbol, kind, line, evidence FROM repo_references"
        " WHERE from_file = ?1 ORDER BY line");
    if (!statement) return statement.error();
    statement->bind(1, path);
    return read_references(statement);
}

// --- Commands --------------------------------------------------------------

Status RepoStore::put_command(const RepoCommand& command) {
    auto statement = impl_->db.prepare(
        "INSERT OR REPLACE INTO repo_commands(purpose, command, directory, source, evidence,"
        " discovery_evidence, verified, verified_ms, exit_code) VALUES(?1,?2,?3,?4,"
        " CASE WHEN COALESCE((SELECT verified FROM repo_commands WHERE purpose=?1 AND command=?2), "
        "?6)"
        " THEN 'verified' ELSE ?5 END, ?5,"
        " COALESCE((SELECT verified FROM repo_commands WHERE purpose=?1 AND command=?2), ?6),"
        " COALESCE((SELECT verified_ms FROM repo_commands WHERE purpose=?1 AND command=?2), ?7),"
        " COALESCE((SELECT exit_code FROM repo_commands WHERE purpose=?1 AND command=?2), ?8))");
    if (!statement) return statement.error();
    statement->bind(1, command.purpose)
        .bind(2, command.command)
        .bind(3, command.working_directory)
        .bind(4, command.source)
        .bind(5, std::string{to_string(command.evidence)})
        .bind(6, command.verified)
        .bind(7, command.verified_at_unix_ms)
        .bind(8, command.last_exit_code);
    return statement->execute();
}

Status RepoStore::replace_commands(const std::vector<RepoCommand>& discovered,
                                   bool invalidate_verified) {
    auto previous = commands();
    if (!previous) return previous.error();
    return impl_->db.with_transaction([&]() -> Status {
        if (auto status = impl_->db.execute("DELETE FROM repo_commands"); !status) return status;
        for (const auto& command : discovered) {
            if (auto status = put_command(command); !status) return status;
            if (invalidate_verified) continue;
            const auto old =
                std::find_if(previous->begin(), previous->end(), [&](const RepoCommand& c) {
                    return c.purpose == command.purpose && c.command == command.command &&
                           c.source == command.source &&
                           c.working_directory == command.working_directory;
                });
            if (old != previous->end() && old->verified_at_unix_ms > 0) {
                if (auto status =
                        mark_command_verified(command.purpose, command.command, old->last_exit_code,
                                              old->verified_at_unix_ms);
                    !status)
                    return status;
            }
        }
        return ok_status();
    });
}

Result<std::vector<RepoCommand>> RepoStore::commands() const {
    auto statement = impl_->db.prepare(
        "SELECT purpose, command, directory, source, evidence, verified, verified_ms, exit_code"
        " FROM repo_commands ORDER BY verified DESC, purpose, command");
    if (!statement) return statement.error();

    std::vector<RepoCommand> out;
    while (true) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        RepoCommand command;
        command.purpose = statement->column_text(0);
        command.command = statement->column_text(1);
        command.working_directory = statement->column_text(2);
        command.source = statement->column_text(3);
        command.evidence = evidence_from_string(statement->column_text(4)).value_or(
            Evidence::Structural);
        command.verified = statement->column_bool(5);
        command.verified_at_unix_ms = statement->column_int(6);
        command.last_exit_code = static_cast<int>(statement->column_int(7));
        out.push_back(std::move(command));
    }
    return out;
}

Status RepoStore::mark_command_verified(std::string_view purpose, std::string_view command,
                                        int exit_code, std::int64_t at_unix_ms) {
    auto statement = impl_->db.prepare(
        "UPDATE repo_commands SET verified = ?3, verified_ms = ?4, exit_code = ?5,"
        " evidence = CASE WHEN ?3 THEN 'verified' ELSE discovery_evidence END"
        " WHERE purpose = ?1 AND command = ?2");
    if (!statement) return statement.error();
    statement->bind(1, purpose)
        .bind(2, command)
        .bind(3, exit_code == 0)
        .bind(4, at_unix_ms)
        .bind(5, exit_code);
    return statement->execute();
}

// --- Facts -----------------------------------------------------------------

Status RepoStore::put_fact(const RepoFact& fact) {
    auto statement = impl_->db.prepare(
        "INSERT OR REPLACE INTO repo_facts(key, value, source, evidence, recorded_ms, depends_on,"
        " generation) VALUES(?1,?2,?3,?4,?5,?6,?7)");
    if (!statement) return statement.error();
    statement->bind(1, fact.key)
        .bind(2, fact.value)
        .bind(3, fact.source)
        .bind(4, std::string{to_string(fact.evidence)})
        .bind(5, fact.recorded_at_unix_ms)
        .bind(6, join(fact.depends_on, '\n'))
        .bind(7, fact.generation);
    return statement->execute();
}

namespace {
RepoFact read_fact(const Statement& row) {
    RepoFact fact;
    fact.key = row.column_text(0);
    fact.value = row.column_text(1);
    fact.source = row.column_text(2);
    fact.evidence = evidence_from_string(row.column_text(3)).value_or(Evidence::Heuristic);
    fact.recorded_at_unix_ms = row.column_int(4);
    fact.depends_on = split_nonempty(row.column_text(5), '\n');
    fact.generation = row.column_int(6);
    return fact;
}
constexpr const char* kFactColumns =
    "key, value, source, evidence, recorded_ms, depends_on, generation";
}  // namespace

Result<std::vector<RepoFact>> RepoStore::facts() const {
    auto statement = impl_->db.prepare(std::string{"SELECT "} + kFactColumns +
                                       " FROM repo_facts ORDER BY key");
    if (!statement) return statement.error();
    std::vector<RepoFact> out;
    while (true) {
        auto row = statement->step();
        if (!row) return row.error();
        if (!*row) break;
        out.push_back(read_fact(*statement));
    }
    return out;
}

Result<std::optional<RepoFact>> RepoStore::fact(std::string_view key) const {
    auto statement = impl_->db.prepare(std::string{"SELECT "} + kFactColumns +
                                       " FROM repo_facts WHERE key = ?1");
    if (!statement) return statement.error();
    statement->bind(1, key);
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::optional<RepoFact>{};
    return std::optional<RepoFact>{read_fact(*statement)};
}

Status RepoStore::remove_fact(std::string_view key) {
    auto statement = impl_->db.prepare("DELETE FROM repo_facts WHERE key = ?1");
    if (!statement) return statement.error();
    statement->bind(1, key);
    return statement->execute();
}

// --- Overview --------------------------------------------------------------

namespace {

Json overview_to_json(const RepoOverview& overview) {
    Json languages = Json::array();
    for (const auto& language : overview.languages) {
        languages.push_back({{"language", language.language},
                             {"files", language.files},
                             {"lines", language.lines}});
    }
    return Json{{"root", overview.root},
                {"git_root", overview.git_root},
                {"is_git_repository", overview.is_git_repository},
                {"languages", languages},
                {"build_systems", overview.build_systems},
                {"package_managers", overview.package_managers},
                {"source_roots", overview.source_roots},
                {"test_roots", overview.test_roots},
                {"manifests", overview.manifests},
                {"entry_points", overview.entry_points},
                {"documentation", overview.documentation},
                {"generated_directories", overview.generated_directories},
                {"vendored_directories", overview.vendored_directories},
                {"indexed_files", overview.indexed_files},
                {"skipped_files", overview.skipped_files},
                {"total_lines", overview.total_lines},
                {"indexed_bytes", overview.indexed_bytes},
                {"symbol_count", overview.symbol_count},
                {"truncated", overview.truncated},
                {"indexed_at_unix_ms", overview.indexed_at_unix_ms},
                {"generation", overview.generation}};
}

std::vector<std::string> string_array(const Json& json, const char* key) {
    std::vector<std::string> out;
    if (!json.contains(key) || !json[key].is_array()) return out;
    for (const auto& item : json[key]) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

}  // namespace

Status RepoStore::put_overview(const RepoOverview& overview) {
    auto statement = impl_->db.prepare(
        "INSERT OR REPLACE INTO repo_overview(id, json) VALUES(1, ?1)");
    if (!statement) return statement.error();
    statement->bind(1, overview_to_json(overview).dump());
    return statement->execute();
}

Result<std::optional<RepoOverview>> RepoStore::overview() const {
    auto statement = impl_->db.prepare("SELECT json FROM repo_overview WHERE id = 1");
    if (!statement) return statement.error();
    auto row = statement->step();
    if (!row) return row.error();
    if (!*row) return std::optional<RepoOverview>{};

    const auto parsed = Json::parse(statement->column_text(0), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        // A row that will not parse is treated as absent rather than as a
        // reason to fail: the caller can always index again.
        return std::optional<RepoOverview>{};
    }

    RepoOverview overview;
    overview.root = parsed.value("root", std::string{});
    overview.git_root = parsed.value("git_root", std::string{});
    overview.is_git_repository = parsed.value("is_git_repository", false);
    if (parsed.contains("languages") && parsed["languages"].is_array()) {
        for (const auto& item : parsed["languages"]) {
            LanguageSummary summary;
            summary.language = item.value("language", std::string{});
            summary.files = item.value("files", std::size_t{0});
            summary.lines = item.value("lines", std::size_t{0});
            overview.languages.push_back(std::move(summary));
        }
    }
    overview.build_systems = string_array(parsed, "build_systems");
    overview.package_managers = string_array(parsed, "package_managers");
    overview.source_roots = string_array(parsed, "source_roots");
    overview.test_roots = string_array(parsed, "test_roots");
    overview.manifests = string_array(parsed, "manifests");
    overview.entry_points = string_array(parsed, "entry_points");
    overview.documentation = string_array(parsed, "documentation");
    overview.generated_directories = string_array(parsed, "generated_directories");
    overview.vendored_directories = string_array(parsed, "vendored_directories");
    overview.indexed_files = parsed.value("indexed_files", std::size_t{0});
    overview.skipped_files = parsed.value("skipped_files", std::size_t{0});
    overview.total_lines = parsed.value("total_lines", std::size_t{0});
    overview.indexed_bytes = parsed.value("indexed_bytes", std::uint64_t{0});
    overview.symbol_count = parsed.value("symbol_count", std::size_t{0});
    overview.truncated = parsed.value("truncated", false);
    overview.indexed_at_unix_ms = parsed.value("indexed_at_unix_ms", std::int64_t{0});
    overview.generation = parsed.value("generation", std::int64_t{0});
    return std::optional<RepoOverview>{std::move(overview)};
}

Result<std::uint64_t> RepoStore::size_on_disk() const {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) return std::uint64_t{0};
    return static_cast<std::uint64_t>(size);
}

}  // namespace localcode::repo
