#pragma once
// ---------------------------------------------------------------------------
// Where the index lives between sessions.
//
// One SQLite file per workspace, under LocalCode's own data directory rather
// than inside the user's source tree - an index is LocalCode's working memory,
// not a project artefact, and writing one into somebody's repository is a way
// of making enemies.
//
// The schema carries its own version. When the indexer changes in a way that
// makes older rows wrong, the version goes up and the old rows are dropped
// rather than reinterpreted: a symbol extracted by a previous set of rules is
// not something to guess about.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "RepoTypes.h"
#include "storage/Database.h"
#include "util/Result.h"

namespace localcode::repo {

// Raised when the indexer's output changes - the shape of the rows, or what
// goes in them. Rows written by an older version are discarded on open.
//
// It has to move for a change in meaning, not only a change in schema. A
// refresh reprocesses the files that changed, so a file nobody has touched
// keeps whatever the previous build wrote about it: without a bump, an index
// built before this version would go on answering with call sites presented as
// definitions and with include edges pointing at unresolved text, and no amount
// of refreshing would clear them.
//
//   4 - inline control-flow and spaced/qualified C++ calls excluded from definitions.
//   3 - command discovery evidence retained separately from execution evidence.
//   2 - includes resolved to repository paths; calls no longer indexed as
//       definitions; a function in a namespace is a function, not a method.
inline constexpr int kIndexFormatVersion = 4;

class RepoStore {
public:
    RepoStore();
    ~RepoStore();
    RepoStore(RepoStore&&) noexcept;
    RepoStore& operator=(RepoStore&&) noexcept;

    // Opens (creating if needed) the index for `workspace_root`. `directory` is
    // where index files live. A file that cannot be read - corrupt, truncated,
    // written by a future version - is replaced rather than refused, because an
    // unusable cache must never stop the application starting.
    Status open(const std::filesystem::path& directory, const std::filesystem::path& workspace_root);
    Status open_in_memory();
    void   close();
    [[nodiscard]] bool is_open() const;

    // True when the store was discarded and rebuilt on open, and why.
    [[nodiscard]] const std::string& reset_reason() const { return reset_reason_; }

    // --- Files ------------------------------------------------------------
    [[nodiscard]] Result<std::vector<RepoFile>> files() const;
    [[nodiscard]] Result<std::optional<RepoFile>> file(std::string_view path) const;
    Status put_file(const RepoFile& file);
    Status remove_file(std::string_view path);

    // --- Symbols ----------------------------------------------------------
    Status replace_symbols(std::string_view path, const std::vector<RepoSymbol>& symbols);
    [[nodiscard]] Result<std::vector<RepoSymbol>> symbols_named(std::string_view name,
                                                                std::size_t limit = 50) const;
    // Prefix or substring match, for "where is anything called ...".
    [[nodiscard]] Result<std::vector<RepoSymbol>> symbols_like(std::string_view fragment,
                                                               std::size_t limit = 50) const;
    [[nodiscard]] Result<std::vector<RepoSymbol>> symbols_in(std::string_view path) const;
    [[nodiscard]] Result<std::size_t> symbol_count() const;

    // --- References -------------------------------------------------------
    Status replace_references(std::string_view path, const std::vector<RepoReference>& references);
    [[nodiscard]] Result<std::vector<RepoReference>> references_to_file(std::string_view path,
                                                                       std::size_t limit = 100) const;
    [[nodiscard]] Result<std::vector<RepoReference>> references_from_file(std::string_view path) const;

    // --- Commands and facts ----------------------------------------------
    Status put_command(const RepoCommand& command);
    Status replace_commands(const std::vector<RepoCommand>& commands, bool invalidate_verified);
    [[nodiscard]] Result<std::vector<RepoCommand>> commands() const;
    Status mark_command_verified(std::string_view purpose, std::string_view command,
                                 int exit_code, std::int64_t at_unix_ms);

    Status put_fact(const RepoFact& fact);
    [[nodiscard]] Result<std::vector<RepoFact>> facts() const;
    [[nodiscard]] Result<std::optional<RepoFact>> fact(std::string_view key) const;
    Status remove_fact(std::string_view key);

    // --- Overview ---------------------------------------------------------
    Status put_overview(const RepoOverview& overview);
    [[nodiscard]] Result<std::optional<RepoOverview>> overview() const;

    // Wipes every indexed row, keeping the file itself. Used when the format
    // changes or the caller asks for a full reindex.
    Status clear();

    // Where the index file for a workspace lives. Exposed so the GUI can say.
    [[nodiscard]] static std::filesystem::path index_path(
        const std::filesystem::path& directory, const std::filesystem::path& workspace_root);

    [[nodiscard]] Result<std::uint64_t> size_on_disk() const;

private:
    Status ensure_schema();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string           reset_reason_;
    std::filesystem::path path_;
};

}  // namespace localcode::repo
