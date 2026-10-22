# Backends

LocalCode supports **llama.cpp** and **ik_llama.cpp**, and is not welded to
either. Adding a third — or linking `libllama` directly — means implementing one
interface.

## The interface

```cpp
class IInferenceBackend {
    virtual Result<ChatResult> chat(const ChatRequest&, const StreamHandlers&,
                                    CancellationToken) = 0;
    virtual Result<int>  count_tokens(std::string_view, CancellationToken) = 0;
    virtual Status       health(CancellationToken) = 0;
    virtual Result<BackendInfo> info(CancellationToken) = 0;
};
```

Everything above this line — the agent, the tools, the GUI, storage — knows only
these four calls. `OpenAiCompatBackend` implements them over the HTTP API that
both runtimes expose; `LlamaCppBackend` and `IkLlamaBackend` derive from it so
that runtime-specific request fields and server arguments have somewhere to live
as the fork diverges.

## Why HTTP in v1

Talking to `llama-server` rather than linking `libllama` is deliberate:

- it supports the included runtime and compatible custom builds, including builds
  selected for the user's GPU;
- one code path serves both runtimes;
- a model server that crashes or runs out of VRAM cannot take the editor down.

The cost is a loopback round trip, which is nothing next to inference time.

A future `DirectLlamaBackend` would implement `IInferenceBackend` and require no
changes to the agent, the tools, storage, the workspace or the interface. That is
the whole point of the seam.

## Bundled default and discovery

The Windows distribution includes **llama.cpp.localcode**, selected by default
in **Settings > Models**. LocalCode resolves it beside its executable at
`bin/llama.cpp.localcode/llama-server.exe`; it does not need a developer checkout
or an environment variable. RC2 includes upstream build b10655 without a custom
optimization claim. If that bundle is missing or incompatible, reinstall the
package or explicitly select a custom runtime.

Choose **Custom llama.cpp** in Settings to use your own build. Explicit paths
already saved by earlier versions remain custom choices after an update.

In custom mode, runtimes are looked for in this order:

1. an explicit path in Settings
2. `LOCALCODE_LLAMA_SERVER` / `LOCALCODE_IK_LLAMA_SERVER`
3. the managed runtime directory —
   `%LOCALAPPDATA%\LocalCode\runtimes\llama.cpp\`, `…/ik_llama.cpp/`, each
   optionally with a `bin/` or `build/bin/` inside
4. `PATH`

`localcode-doctor backends` prints what was found, where it came from, its
version and which flags it advertises.

## Capability probing

**Server arguments are never guessed.** Each binary is run once with `--help`,
the output is parsed, and only flags that build actually advertises are passed.
The result is cached per path.

This matters more than it sounds. The two runtimes disagree about notation:

```
llama.cpp      -fa,   --flash-attn [on|off|auto]
ik_llama.cpp   -fa,   --flash-attn (auto|on|off|0|1)
```

and a build that wants a value but is given none exits with a usage error — so
the model never loads at all. Arity is therefore decided by inspecting what
follows the flag on its help line: a bracketed group of any kind, or a bare
metavariable such as `N` or `FORMAT`, means it takes a value. Description text,
which contains lower-case letters, does not.

That detection has its own regression tests, built from help excerpts taken
verbatim from both runtimes.

## AUTO selection

AUTO does not mean guess. It means look at what is installed, at what the profile
asks for, and at what the model is — then pick one and say why. The reason is
shown in the model panel and written to the log.

1. A profile that pins a server binary wins outright.
2. A profile that names a backend gets it, or a clear error if it is not
   installed.
3. With the bundled default selected, AUTO uses the included llama.cpp runtime.
4. In custom mode, only one runtime installed → that one.
5. Both installed → the configured default preference, with the reason stating
   plainly that neither is faster in general and inviting the user to compare.
6. If the model uses native tool calls and the chosen build has no `--jinja`, a
   build that does have it is preferred, because tool calling needs the model's
   own chat template.

**LocalCode never claims one runtime is faster than the other.** Which one wins
depends on the model, the quantization and the hardware. Switch on a model's
profile and measure.

## Server lifecycle

`ModelServer` starts the process, waits for `/health`, watches it, and shuts it
down cleanly.

- **Loading unloads first.** Two large models will not fit in VRAM together, and
  a half-freed one makes the next load fail in ways that look like a corrupt
  file.
- **Ports are chosen, not assumed.** A free loopback port is obtained from the
  OS rather than defaulting to 8080 and colliding with whatever is already there.
- **Start-up is asynchronous with a generous timeout.** A large model on a slow
  disk takes minutes; the server's own output is streamed into the output panel
  so a slow start is visible rather than mysterious.
- **The process tree dies with us.** On Windows the server is in a Job Object
  with `KILL_ON_JOB_CLOSE`, so no orphaned `llama-server` is left holding VRAM
  even if LocalCode is killed.
- **A crash is detected and reported** with the tail of the server log, not
  swallowed.

## Attaching to a server you started

Settings → Advanced → *Use a server I started myself*. Leave it unchecked for
normal use: LocalCode manages its bundled or selected custom runtime. When
enabled, LocalCode will talk to the existing server
and will not try to start or stop it. Useful when you want unusual flags, a
remote host, or a server shared with something else.

## Streaming

Chat completions are consumed as Server-Sent Events. The parser buffers across
chunk boundaries, because chunks arrive split at arbitrary byte positions and
getting that wrong shows up as tokens silently vanishing under load.

Streamed `tool_calls` are accumulated by index: the wire format sends a call's
arguments in fragments and emits the id and name only on the first one.

Reasoning is read from `reasoning_content`, `reasoning` or `thinking` — the field
name has changed across llama.cpp releases, and accepting all three means not
requiring a particular build.

Cancellation closes the HTTP request handle, which makes an in-flight read return
immediately rather than waiting for the timeout. A cancelled generation still
returns what arrived, so the conversation stays consistent.
