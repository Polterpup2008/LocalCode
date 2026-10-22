# The agent

## The loop

```
user message
  │
  ├─ snapshot which files git already reports as modified
  │
  ▼
┌──────────────────────────────────────────────────────┐
│ select 1–3 relevant skills                           │
│ assemble the context within the token budget         │
│ call the model, streaming                            │
│                                                      │
│ tool calls?                                          │
│   yes → execute each, append results, check progress │◄─┐
│   no  → this is the final answer                     │  │
└──────────────────────────────────────────────────────┘  │
        │                                                 │
        └─────────────────────────────────────────────────┘

final answer → check every claim against the evidence
```

It keeps going. One tool call is not a turn. The loop ends when the model stops
asking for tools, the iteration budget runs out, the wall-clock budget runs out,
the user cancels, or the progress monitor concludes nothing is changing.

## Task state

The model should not have to reconstruct what it is doing by re-reading the
transcript every turn. That is expensive, and small models are bad at it — they
lose the objective around the point where the transcript stops fitting and start
solving a different problem.

So the mission lives outside the conversation, as data:

```cpp
class TaskState {
    std::string              objective;
    std::vector<std::string> requirements;
    std::vector<TaskStep>    steps;        // pending / active / done / failed
    std::vector<std::string> blockers;
    std::vector<FileRecord>    files_changed;
    std::vector<CommandRecord> commands_run;
    std::vector<Evidence>      evidence;
};
```

A compact rendering goes into every request and survives context trimming
untouched. It is also persisted alongside the conversation, so a resumed session
knows what it was doing rather than starting the mission over.

## Evidence and verification

This is the part that distinguishes LocalCode from a chat window with tools.

Every tool returns `Evidence` alongside its result: what kind of thing happened
(a file read, a build, a test run), what it was about, whether it succeeded.
The agent accumulates these.

**Verification is derived, not asserted.** The question is never "did a build
ever succeed" but "did one succeed *after the last change was made*":

| State | Meaning |
|---|---|
| `Verified` | a build **and** a test run succeeded after the final edit |
| `Partial` | one of the two succeeded; the other was not run |
| `Failed` | the most recent build or test after the final edit failed |
| `NotAttempted` | nothing was built or tested |

**Final answers are checked.** When the model writes "the tests pass" and no
successful test run is in the evidence, the discrepancy is appended to the
message the user sees:

> ---
> **LocalCode could not confirm part of this answer.**
> - you say the tests pass, but no successful test run happened
>
> NOT VERIFIED — nothing was built or tested, so the change is unproven.

The answer is not suppressed — the model may have done something useful — but
the claim does not stand unchallenged. This is enforced in code, not requested
in a prompt, because asking a model not to overclaim does not stop it.

**Succeeding is not the same as being right.** Everything above is about
whether an operation happened. It says nothing about whether the result was
correct — and a run that built a program, ran it to exit code 0 and reported
`1 3 6 12 24 48 96 192 384 768` as the first ten Fibonacci numbers had all of
these signals green.

So a second, stricter model sits on top of this one: stages that never imply
each other, staleness measured against the source revision, an acceptance plan
committed to before the work, and a gate that consults the record when the
model says it is finished. It is documented separately in
[VERIFICATION_MODEL.md](VERIFICATION_MODEL.md), and it is the one to read if
you only read one.

## Context management

Priority order, highest first:

1. the system prompt and the selected skills — fixed for the whole run
2. the task state block — never evicted; carried on the last message
3. the user's most recent message
4. recent conversation and tool results, newest first
5. older turns, condensed or dropped

Filling happens **backwards from the newest turn**, which is what makes eviction
sensible: the model always gets the freshest state and it is the stale turns that
fall off.

- Tool results older than six turns are condensed to their head and tail with an
  explicit marker saying how many lines were removed. Numbered source results
  matching current repository anchors can retain bounded windows around those
  anchors; available spare capacity can also carry earlier source excerpts.
  These excerpts remain incomplete source receipts, not build or test evidence.
- Reasoning from old turns is dropped, except for assistant tool-call turns in
  the current request's tool chain.
- A tool message whose originating assistant turn was evicted is dropped too: an
  orphaned tool reply is rejected by the server.
- When whole history messages are dropped, the outgoing request tells the model
  that earlier source/tool results were omitted and missing evidence needs to
  be re-read. The receipt is budgeted only when eviction occurs. It cannot
  guarantee retention or rediscovery of every dependency in a long task.
- **The user's prompt is never silently truncated.** If it does not fit, the run
  stops and says so, with the numbers.

### Why the system message never changes during a run

Every runtime caches KV state keyed on the longest matching prefix of the prompt.
A system message that differs between turns throws that cache away, and on a
machine where prefill is slow the re-processing costs far more than anything the
changed bytes could buy.

So the system message holds only what is stable for a whole run — the rules and
the skills, selected once at the start and pinned. The task state, which changes
by construction on every iteration, is appended to the **last** message instead.
That placement is also the one every chat template accepts: some reject a system
message that is not first, and an injected user turn makes small models answer it
as though the operator had spoken.

This is not a micro-optimisation. Before the change, an 8B model at 8k
re-prefilled roughly 4,400 tokens on six of ten turns; the numbers are in
`docs/FINAL_AUDIT.md`.

Token counting can use the runtime's `/tokenize` endpoint for individual text
blocks, with character-based estimates for short blocks and unsupported calls.
Message/template overhead and tool-schema costs are also estimated, so the
complete context budget is marked with `~` even when an endpoint is available.
Server-reported usage remains a separate measurement of an actual request.

## No-progress detection

The characteristic failure of a small local model in an agent loop is not a wrong
answer. It is running the same failing command twelve times while convinced it is
making progress.

The monitor watches for:

| Pattern | Detected by |
|---|---|
| Repeated identical call | same tool, same normalised arguments, same normalised result, 3× in a row |
| Repeated failure | the same failure signature 3 times |
| Oscillation | A, B, A, B — undoing its own work |
| Stall | many turns with no file changed and nothing verified |

Arguments are normalised (keys sorted, whitespace collapsed) and results are
normalised (digit runs replaced) before comparison, so a cosmetic difference does
not hide a genuine repeat.

When something fires, a short factual note is injected into the conversation —

> You have now called `run_command` with exactly the same arguments 3 times and
> received the same result each time. Repeating it again will not change
> anything. Stop and reconsider: what does that result actually tell you?

— and after repeated warnings the run stops and hands back what was learned.

## Tool calling

**Native** when the model's chat template declares tool support: standard
OpenAI-style `tool_calls`, produced by the runtime with `--jinja`.

**Text fallback** otherwise. This is the only emulation in the codebase and it is
explicit — the profile says `text-fallback`, the model panel shows it, and the
system prompt documents the format:

```
<tool_call>
{"name": "read_file", "arguments": {"path": "src/main.cpp"}}
</tool_call>
```

The parser also accepts the delimiters models reach for anyway
(`<function_call>`, `[TOOL_CALL]`, fenced blocks), the key names they substitute
(`tool`, `parameters`, `args`), and arguments emitted as a JSON string rather
than an object. Being liberal here costs nothing and saves a wasted turn every
time a model uses the convention it was trained on rather than the one it was
told. A block that cannot be parsed is reported back with the exact syntax
required, rather than being silently treated as a final answer.

## Reasoning

Reasoning is model-native or absent. There are no invented tiers, no synthetic
effort levels, no anti-shortcut layers.

- If the chat template contains a reasoning section, the capability is set and
  `--reasoning-format` is passed so the runtime returns reasoning in its own
  field.
- The chat view shows it in a separate, dimmed block that can be turned off.
- If a model has no native reasoning, there is no thinking section, and the
  reasoning controls in the model panel are disabled with an explanation.
- `native_reasoning_effort` is passed through verbatim when the user sets it and
  the runtime supports it. LocalCode never invents a value for it.

## Language

The agent replies in the language the user writes in. The system prompt states
this explicitly, because tool descriptions and internal instructions are in
English and a model will otherwise drift into English regardless of what it was
asked in. Code, identifiers, paths and command output stay as they are.

## Configuration

| Setting | Default | Effect |
|---|---|---|
| `max_iterations` | 40 | tool-calling steps per request |
| `wall_clock_budget` | 45 min | total time per request |
| `max_tool_calls_per_turn` | 8 | calls accepted from a single reply |
| `stream` | on | stream tokens as they arrive |
| `include_reasoning_in_history` | off | replay reasoning to the model |
