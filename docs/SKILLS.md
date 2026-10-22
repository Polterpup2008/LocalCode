# Skills

A skill is procedural knowledge as Markdown: how to read a CMake error, how to
prove a change actually works, how to touch git without destroying someone's
uncommitted edits.

They live on disk so a user can read, edit and add them without rebuilding
anything.

## Format

```markdown
---
id: cmake-build-fix
name: Diagnose a CMake build
description: How to configure, build and repair a CMake project.
tags: [cmake, build, cpp, windows]
triggers: [cmake, cmakelists, configure failed, generator]
tools: [run_command, read_file, environment_info]
priority: 85
always: false
validated: true
---

# Diagnose a CMake build

## When this applies
...

## Procedure
1. ...

## Failure modes
- ...
```

| Field | Meaning |
|---|---|
| `id` | stable key; matches the directory name |
| `description` | one line — this is what the selector matches on, so make it concrete |
| `triggers` | phrases that make the skill relevant; matched case-insensitively |
| `tags` | topic words |
| `priority` | 0–100, breaks ties |
| `always` | included regardless of the query — reserve for the few that apply to every task |
| `validated` | false means awaiting human review; unvalidated skills are never selected |

## Selection

Skills are **selected, never bulk-injected**. Pushing all of them into every
request would swamp the context of exactly the small models this exists to serve.

Scoring:

- a trigger phrase appearing verbatim — strongest signal
- a tag matching a word in the request
- words shared with the description — weak
- priority as a gentle tie-break

Matching is against the objective plus the last few turns, so a task that has
drifted into a linker problem picks up the linker skill.

The budget is three *relevant* skills. Always-on skills are additional — they do
not consume the budget, because crowding out the skill the task actually called
for is the opposite of the intent.

## The bundled library

26 skills, chosen to be few and good rather than many and thin.

| | |
|---|---|
| `verify-before-done` * | how to establish that a change actually works before reporting it, and how to report honestly when it does not |
| `cmake-build-fix` | how to configure, build and repair a CMake project, including the generator and toolchain problems specific to Windows |
| `code-review` | how to review a diff for correctness, safety and fit before it is accepted |
| `compile-test-fix-loop` | how to work through a list of compiler errors efficiently without thrashing or introducing new ones |
| `concurrency-bugs` | how to find a threading bug from the evidence, given that it will not reproduce on demand and adding a print can hide it |
| `cpp-debug` | how to find the cause of a crash, a wrong result or a hang in C++ from the evidence available |
| `cpp-undefined-behavior` | how to recognise and confirm undefined behaviour in C++, especially bugs that appear only in release builds or on one machine |
| `cross-platform-portability` | the differences that actually break builds when code written on Windows is compiled on Linux or macOS, and how to avoid them without ifdef sprawl |
| `dependency-bootstrap` | what to do when a build needs something that is not installed, before concluding a human has to intervene |
| `git-safe-workflow` | which git operations are safe to run unprompted, which need a human, and how to inspect changes without risking someone's uncommitted work |
| `linker-errors` | how to read an unresolved-symbol or duplicate-symbol error and fix the actual cause, which is almost never the file the error names |
| `memory-safety-tools` | how to make a leak, an overrun or a use-after-free reproduce and point at its own cause, instead of guessing from a crash address |
| `minimal-patch` | how to scope an edit so it fixes the problem without collateral damage to code nobody asked you to touch |
| `node-project` | how to find the right package manager and scripts, and how to read the errors these toolchains produce |
| `performance-profiling` | how to find where time actually goes before optimising anything |
| `powershell-safe` | how to run commands on Windows so that quoting, exit codes and encoding behave the way you expect |
| `python-project` | how to run, test and install a Python project without corrupting the interpreter the user depends on |
| `refactor-safely` | how to move, rename or restructure code so that the result provably does the same thing as before |
| `regression-hunting` | how to locate the change that introduced a regression, using history rather than guesswork |
| `repo-map` | how to build an accurate picture of a project you have not seen before, cheaply and without guessing |
| `rust-cargo` | how to build, test and read errors in a Cargo project, where the compiler usually tells you the fix if you read to the end |
| `secrets-and-credentials` | what to do when a task involves API keys, tokens or passwords, and how to avoid writing one into a file, a log or a commit |
| `shader-debug` | how to isolate a graphics failure when the symptom is a black screen or wrong pixels rather than an error message |
| `test-generation` | how to add a test that actually catches the bug and fits the project's existing suite |
| `web-documentation-grounding` | how to use web search and fetch to confirm an API or an error rather than recalling it approximately |
| `windows-native-development` | the toolchain facts that make MSVC, the Windows SDK and Windows paths behave, and the mistakes that waste the most time |

`*` always on.

## Skills the agent writes

`create_skill` lets the agent record a procedure it worked out. Such a skill:

- is validated structurally — required fields, a non-trivial body, triggers long
  enough to carry signal, no duplicate id;
- is saved to the user skills directory;
- appears in the Skills panel marked **awaiting review**;
- **is not selected for any task** until a person reads it and marks it reviewed.

That last point is the whole reason the flag exists. An agent that can silently
install its own instructions is an agent that can silently change its own
behaviour.

## Locations

| | |
|---|---|
| Built-in | next to the executable, in `skills/` |
| User | `%LOCALAPPDATA%\LocalCode\data\skills\` |
| Override | `LOCALCODE_SKILLS_DIR` |

A user skill with the same id shadows the built-in one, so a shipped procedure
can be overridden without editing the installation.

## Writing a good one

- **Narrow.** "Diagnose a CMake build" is a skill. "Write good C++" is not.
- **Procedural, not factual.** Steps in order, not a reference manual.
- **Include the failure modes.** The mistakes are often more valuable than the
  happy path.
- **Say how to verify.** A procedure that cannot be checked is a suggestion.
- **Concrete triggers.** `linker error` is a trigger. `code` is noise.

Check them with:

```bash
localcode-doctor skills
```

which loads every skill, validates it, and reports what would be rejected.
