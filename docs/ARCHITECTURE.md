# Architecture

## The one structural rule

**`localcode_core` never links Qt.**

Everything the agent does — policy, tools, process execution, inference,
storage, skills — lives in the core library. The Qt layer is presentation: it
holds a pointer to `Application` and nothing else reaches into the engine.

That rule buys three things:

- the whole engine is testable headlessly, which is why 230 tests can run in CI
  with no display and no model;
- `localcode-doctor` can expose the same functionality from a terminal;
- a different front end — a TUI, a server, an editor plugin — would not require
  rewriting any of it.

It is a rule, not an aspiration. If a core header ever needs `QString`, the
design has gone wrong.

## Layout

```
src/
├── core/                     localcode_core — portable, Qt-free
│   ├── util/                 Result, logging, strings/UTF-8, JSON, cancellation
│   ├── platform/             paths, filesystem, process execution, env discovery
│   ├── security/             PathPolicy, CommandPolicy
│   ├── storage/              SQLite wrapper and repositories
│   ├── model/                GGUF reader, capabilities, profiles
│   ├── inference/            HTTP, chat types, backends, server management
│   ├── tools/                the tool registry and every tool
│   ├── git/                  git subprocess + porcelain parsers
│   ├── web/                  search, fetch, download, trust policy
│   ├── skills/               skill parsing, registry, selection
│   ├── agent/                task state, verification, context, progress, the loop
│   ├── workspace/            the selected project and change tracking
│   └── Application.*         wiring: owns everything, exposes it once
│
├── gui/                      the Qt 6 desktop application
│   ├── Theme.*               palette, fonts, stylesheet
│   ├── AgentController.*     the thread bridge
│   ├── MainWindow.*
│   ├── widgets/              chat, composer, explorer, timeline, editor, diff…
│   └── dialogs/              settings, model import, confirmation
│
└── cli/                      localcode-doctor
```

## How a request flows

```
  PromptComposer                         (GUI thread)
        │  submitted(text)
        ▼
  MainWindow::submitPrompt
        │
        ▼
  AgentController::send                  spawns a worker thread
        │
        ▼
  Agent::run                             (worker thread) ─────────────┐
        │                                                             │
        │  1. snapshot which files were already dirty                 │
        │  2. select 1–3 relevant skills                              │
        │  3. assemble context within the token budget                │
        │  4. call the backend, streaming                             │
        │  5. tool calls?  ── yes ──► ToolRegistry::invoke ───────────┤
        │        │                          │                         │
        │        │                    PathPolicy / CommandPolicy      │
        │        │                          │                         │
        │        │                    the tool runs, returns          │
        │        │                    a small result + evidence       │
        │        │                          │                         │
        │        └──────── append result, go to 3 ◄──────────────────┘
        │
        │  6. no tool calls → check the final answer against evidence
        ▼
  AgentEvent sink  ──queued signals──►  MainWindow slots (GUI thread)
```

Every arrow crossing into the GUI is a queued Qt signal. The one call that goes
the other way is confirmation: a tool thread that needs a human answer blocks on
a `BlockingQueuedConnection` into the GUI thread. That cannot deadlock because
the GUI thread never waits on the agent thread.

## Threading

| Runs on | What |
|---|---|
| GUI thread | every widget, the confirmation dialog, all painting |
| Agent worker | the agent loop, backend HTTP, tool execution |
| Process reader threads | one per pipe, draining child stdout/stderr |
| `QtConcurrent` pool | git scans, model loading, repository walks |
| Model server process | out of process entirely |

Nothing long-running happens on the GUI thread. A build that emits 40 MB of
output, a repository with 200,000 files, a model that takes four minutes to
load — none of them touch the interface except through signals.

Cancellation is a single `CancellationToken` threaded from the Stop button
through the agent loop, the HTTP stream, the running child process and any web
request. Process execution additionally registers a callback so the child's Job
Object is terminated immediately rather than at the next poll.

## Key decisions

### HTTP to the model server, not linked libllama

v1 talks to `llama-server` over its OpenAI-compatible API rather than linking
`libllama`. This is deliberate:

- it works with whatever build the user already has, including one compiled for
  their exact GPU, without LocalCode having to ship or match a runtime;
- both llama.cpp and ik_llama.cpp are supported by the same code path;
- a model server that crashes or exhausts VRAM cannot take the editor with it.

The cost is a local socket round trip, which is irrelevant next to inference
time. `IInferenceBackend` is the seam: a `DirectLlamaBackend` linking libllama
would implement that interface and change nothing above it.

### Structured commands, never shell strings

`ProcessRequest` carries an executable, an argument vector, a working directory
and an environment. Nothing concatenates a command line. Model-authored
arguments with spaces, quotes or semicolons cannot become injection, and Windows
quoting is applied once, correctly, in one place.

Where a shell genuinely is needed, `run_command` accepts `shell_command`, splits
it on the operators and classifies every segment — so `cmake --build . && format
C:` is blocked as a whole.

### Job Objects for process trees

On Windows every child is created suspended, assigned to a Job Object with
`KILL_ON_JOB_CLOSE`, and then resumed. A build that spawns `cl.exe` which spawns
`link.exe` is killed as a tree on timeout or cancellation. No orphaned compilers,
no leaked `llama-server` holding VRAM.

### Canonical paths, not lexical ones

Containment is decided on canonical paths only. On Windows canonicalisation goes
through `GetFinalPathNameByHandleW`, which resolves symlinks, NTFS junctions and
8.3 short names. A lexical check alone is escapable by planting a junction inside
the workspace — the test suite plants one and proves the escape is refused.

### SQLite with a real schema

Conversations grow to thousands of messages. A single JSON blob would have to be
rewritten on every turn and read whole to show anything. The schema is normalised,
migrations are ordered and versioned, WAL is on so the agent's writes never block
the interface's reads, and FTS5 indexes message bodies for search.

### Results are small and carry evidence

A compiler producing 100 MB of output must not produce 100 MB of context. Every
tool returns a short summary, the extracted diagnostics, and a path to the full
log on disk. Alongside that it returns `Evidence` entries recording what actually
happened, which is what the final-answer check is built on.

## Dependencies

| | Why | How |
|---|---|---|
| nlohmann/json | tool schemas, the wire format, config | FetchContent, pinned + hashed |
| SQLite | persistence | official amalgamation, FetchContent |
| Catch2 | tests only | FetchContent |
| Qt 6 | the GUI only | `find_package` |
| WinHTTP | HTTP on Windows | ships with the OS |
| libcurl | HTTP elsewhere | optional, `find_package` |

No vendored third-party source tree. Every fetched dependency is pinned by
version and verified by SHA-256; set `FETCHCONTENT_SOURCE_DIR_<NAME>` to build
offline.
