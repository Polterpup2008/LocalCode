# Tools

A compact set — twenty-three tools, not fifty variations of the same three.
Small local models choose badly from a long list.

## The set

### Reading

| Tool | Purpose |
|---|---|
| `read_file` | full contents with line numbers |
| `read_file_range` | a line range — for large files and compiler diagnostics |
| `list_directory` | one level, or the whole subtree |
| `file_info` | exists, size, binary, encoding, line count, modified |

### Searching

| Tool | Purpose |
|---|---|
| `search_files` | find files by glob |
| `search_text` | find text in files, with context lines |

### Writing

| Tool | Purpose |
|---|---|
| `write_file` | whole file — for new files |
| `replace_in_file` | exact block replacement — preferred for edits |
| `apply_patch` | a unified diff across one or more files |
| `create_directory`, `move_file`, `copy_file` | filesystem structure |

### Running

| Tool | Purpose |
|---|---|
| `run_command` | build, test, run — the tool that makes it an agent |
| `find_executable` | resolve a program and show every copy installed |
| `environment_info` | what is actually installed on this machine |

### Repository

| Tool | Purpose |
|---|---|
| `git_status`, `git_diff`, `git_log` | read-only |

There is deliberately no commit, reset, clean or push tool. Those change history
or destroy work; that decision belongs to the user. They remain reachable through
`run_command`, where the policy requires confirmation for each.

### Web

| Tool | Purpose |
|---|---|
| `web_search`, `web_fetch` | look things up |
| `download_file` | fetch a dependency, under the trust policy |

### Skills

| Tool | Purpose |
|---|---|
| `list_skills` | what procedures exist |
| `create_skill` | write a new one — saved unvalidated, unused until reviewed |

## Descriptions are written for small models

A tool description is a prompt. It says what the tool does and, where it matters,
says explicitly not to ask the user to do it by hand:

> **read_file** — Reads the actual contents of a text or source file from disk
> and returns them with line numbers. Use this whenever answering a question or
> making a change requires knowing what is in a file. Never ask the user to paste
> a file that this tool can read, and never guess at a file's contents.

> **run_command** — Runs a development command with the workspace as the working
> directory, and returns its exit code, timing and output. Use this to configure
> and build the project, run its tests, run the compiler, and run the program
> itself. Do this rather than telling the user to run something by hand — if the
> command can be run here, run it. The executable does not have to live inside
> the workspace: system tools such as cmake, git and compilers are found on PATH.

## Results are small

A compiler producing 100 MB of output must not produce 100 MB of context.
`run_command` returns:

```
command:  cmake --build build
cwd:      L:\projects\game
exit:     1
duration: 4.21s

Errors (2 shown):
  src\render.cpp(112,17): error C2065: 'contex' : undeclared identifier
  src\render.cpp(118,5): error C2664: cannot convert argument 1

stdout (487 lines total, abridged):
  … head and tail, with an explicit omitted marker …

[full output: %LOCALAPPDATA%\LocalCode\cache\commands\cmd-1788275529.log]
```

- Diagnostics are extracted by pattern — MSVC, GCC/Clang, linkers, CMake, Ninja,
  make, test runners, and runtime failures like segfaults and panics.
- On failure the **tail** is shown, where the summary is. On success the **head**,
  where configure output matters.
- Truncation is always marked. Nothing is silently cut.
- The complete log is on disk and its path is returned.

## Every result carries evidence

```cpp
struct Evidence {
    EvidenceKind kind;      // FileRead, BuildAttempted, TestsRun, …
    std::string  subject;   // the file, command or query
    std::string  detail;    // exit code, match count
    bool         positive;  // did it succeed?
};
```

This is what the verification check is built on. `run_command` classifies what a
command was trying to do — a `ctest` invocation records `TestsRun`, a `cmake
--build` records `BuildAttempted` — so "the tests pass" can be checked against
whether a test run actually succeeded.

## Failures teach

A tool that fails explains how to succeed:

- `replace_in_file` with text that is not found checks whether the *first line*
  of the search text appears, and says whether the mismatch is likely in the
  indentation or somewhere else entirely.
- Text that appears more than once is refused with the count and a suggestion to
  add context or pass `replace_all`.
- `apply_patch` rejects the whole patch if any hunk cannot be placed, writes
  nothing, and lists each problem.
- An unknown tool name comes back with the list of real ones.
- A missing required parameter comes back with the schema.
- A missing executable comes back with the list of development tools that *are*
  installed.

## Safety in the tools themselves

- Every path goes through `PathPolicy` before anything is opened.
- Every command goes through `CommandPolicy` before anything is run.
- Writes preserve the file's original encoding and line endings, so an edit does
  not show up in the diff as "every line changed".
- Writes are atomic: a temporary file in the same directory, renamed over the
  target, so an interrupted write cannot leave a half-written source file.
- A file that already had uncommitted changes is not overwritten wholesale
  without an explicit flag.
- `apply_patch` stages everything in memory and writes only if every hunk
  applied, so a partial failure cannot leave the tree half-modified.
- Read-only mode drops the mutating tools from the advertised set entirely,
  rather than letting the model call them and be refused.

## Adding a tool

Implement `ITool`, add it to a factory in `ToolRegistry`. Keep the description
written for a small model, keep the result small, and return evidence for
anything a later claim might rest on.
