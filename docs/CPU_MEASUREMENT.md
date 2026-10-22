# Compare CPU settings with your chosen model

In the desktop application, open **Performance settings… → Compare CPU
performance…** from either a model profile or the Discord plugin. The model
path is prefilled from that page. Choose the matching `llama-bench` executable,
thread counts and workload, then start. Each count runs in forward and reverse
order. Prompt and generation tokens/s are displayed separately, including the
native generation standard deviation. **Stop** and closing the window terminate
the owned benchmark; completed trials remain available for export as JSON.
After a complete comparison, choose **Generation threads to try** and **Prompt
threads to try**, then **Try these CPU threads** to copy just those two values
into the open Performance dialog. The highest measured mean is initially
selected for each phase; the two trial means' range is available in the tooltip.
You can choose another tested count. Save Performance settings and reload the
model yourself; cancellation leaves the saved profile unchanged.

Transfer is available for a CPU-only profile using the same model and matching
runtime's sibling benchmark. Changed paths, file size/modification time, or
measurement inputs invalidate transfer; adjacent DLL/shared-library identities
are included. Changed files require reopening the
window. The export includes file identities and per-thread means/ranges. These
synthetic results suggest settings to try, not an optimal count for every
context, batch, background load or GPU configuration. The model, context, GPU
layers, KV precision, persona and other settings are not copied or changed.
For an agent profile, select its server file explicitly first. AUTO/runtime
discovery and external endpoints can use another runtime, so they retain
measurement and manual thread editing without offering this transfer.
An explicit **CPU weight repacking** choice is passed to a capable benchmark as
`--repack 0` or `--repack 1`. Its help must advertise `--repack <0|1>` and every
measurement must confirm the requested boolean mode. Otherwise comparison stops
before accepting results. Runtime default remains compatible with older tools.
The optional b10655 CPU Profile Benchmark archive supplies this extension while
retaining the original server/core/CPU libraries. Select it explicitly; never
copy its benchmark DLL into a different llama.cpp version. When comparing a
matching local CPU profile, thread transfer also works with an explicit layout.

This measures a separate CPU-only model copy, normal priority, fresh context,
warmup and runtime-default batches/KV. It does not compare your GPU configuration
or change your configured context. Unload other models first if RAM is tight.
Background work, thermals and file-cache state affect the results; compare both
directions. The window limits each trial to five minutes and the entire run to
thirty minutes. Invalid/mismatched results are rejected, and an interrupted run
is explicitly partial. Export contains runtime help, exact arguments, model path
and native samples, never bot credentials, persona or chats. Nothing is written
unless you choose **Save results…**. Windows checks the filename capabilities described
below. The desktop measurement is verified on Windows x64; other platforms remain
untested.

## Saved-prompt cache RAM

Native profiles also expose **Prompt cache RAM** in Performance settings. Runtime
default preserves the runtime's budget; zero disables saved prompt states and
positive values set MiB. The optional CPU starting profile selects128 MiB. Reload
the model after changing the value. Existing profiles are unchanged. A runtime
without this flag refuses an explicit choice; selecting Runtime default retains
its existing behavior. An advanced `--cache-ram` / `-cram` argument must be removed
before setting an explicit budget here, or left as the only source of the setting.

This host cache is separate from the active context/KV, weights and VRAM. A
smaller cache may require more prompt processing. The CPU comparison and script
below benchmark synthetic prompt/generation work; they do not exercise the
server's retained prompt-cache entries or measure this budget's RAM/latency
tradeoff. Compare real repeated/conversation requests for that purpose.

## CPU weight repacking

Performance settings expose **Runtime default**, **Enabled** and **Disabled**
for CPU weight repacking in native profiles and current Discord plugins. Default
does not add a runtime argument. Explicit choices require an advertised boolean
`--repack` or `--no-repack` switch and take effect after reloading. An unsupported
choice fails visibly. Native advanced arguments must not duplicate the choice.
The CPU starting profile preserves it; older plugins show the control disabled.

Disabling can reduce host RAM by avoiding a rearranged copy of supported CPU
weights. It can also slow prompt processing and change generated replies, so
compare memory, speed and answers for your own model before keeping the choice.
It does not compress the GGUF or promise VRAM savings. Existing profiles keep
their runtime's behavior.

## PowerShell alternative

This Windows tool measures an existing `llama-bench.exe` and GGUF that you
choose. It does not download models, start Discord, change a profile or apply
settings automatically. It keeps model weights, KV and operations on the CPU.

The installer includes this guide and `benchmark-cpu.ps1` in
`share/localcode/tools`. Open PowerShell in that directory and run:

```powershell
./benchmark-cpu.ps1 -Bench 'C:/path/to/llama-bench.exe' `
  -Model 'C:/path/to/your-model.gguf'
```

Use your actual paths. The benchmark executable usually accompanies
`llama-server.exe` in a complete upstream runtime package. A server-only
package may require acquiring the matching benchmark separately.
The script checks advertised runtime flags and refuses incompatible builds.
Non-ASCII Windows filenames require a benchmark advertising
`LocalCode Windows UTF-8 argv v1` in its help. The optional UTF-8 launcher passes
Windows' wide arguments correctly to the matching benchmark implementation.
Older Windows launchers corrupt these arguments, so both the desktop and script
reject unsupported filenames after checking capabilities, before loading a model.
With those older tools, use an ASCII filename such as `my-model.gguf`.
Containing directories, installation and results paths may contain Unicode.
The script launches from the model's directory with its relative filename and records the original path.
This diagnostic restriction does not change LocalCode's model selection.
Model filenames must also omit commas: the benchmark parses them as separators
between multiple models even inside a quoted argument. Spaces are supported.

The default comparison derives thread counts from the available logical and
physical CPUs. Specify `-Threads 2,4,8` to compare your own valid choices.
Comma lists also work when invoking Windows PowerShell with `-File`; they are
parsed explicitly instead of being converted to a single integer by its binder.
`-PromptTokens`, `-GeneratedTokens`, `-HistoryTokens` and `-Repetitions` control
the workload. Keep these values and the runtime/model unchanged when comparing
settings. More threads or larger batches do not always improve generation.

Each thread count runs in its own process with warmup. The combined
`samples.json` retains the native prompt/generation results and their standard
deviation; `threads-<n>/` preserves each process's raw results and log.
`environment.json` records a fresh system CPU sample before each process,
timestamped samples while it runs, and the actual arguments for that thread
count. The during-run samples include the benchmark and other work; they do
not isolate individual prompt/generation repetitions. Missing counter readings
remain `null`, not zero load. This makes changing machine load visible without
choosing a model or applying a profile.

The sampler uses Windows' English PDH counter API so the counter path does not
depend on the Windows display language. It closes its query after each process.
Each process loads the model again; OS file-cache state remains uncontrolled.

Results go into a new `cpu-<id>` directory under
`%LOCALAPPDATA%/LocalCode/data/performance`. `LOCALCODE_HOME` relocates the private
data root; `-EvidenceRoot 'C:/my/results'` selects a results parent explicitly.
Each run creates its own folder and preserves raw samples, runtime help,
arguments, checksums and observations. Nothing is written into the installation
directory. The tool contains no bot token, persona, chats or model weights.

Normal process and compute-worker priority is the default. Use
`-Priority BelowNormal` for an intentional reduced-priority diagnostic and compare
only runs at the same priority. The CPU-load samples provide context;
they do not certify that the machine stayed idle. Wait for unrelated compilation,
heavy applications and package compression to finish before interpreting small
differences. Repeat comparisons in reversed order. Warmup and OS file cache are
part of the recorded protocol; clocks, thermals and other work are uncontrolled.

Prompt throughput and generation throughput are separate. These synthetic
measurements do not verify answer quality, bot response latency or VRAM savings.
Select the settings you want in LocalCode's **Performance settings** dialog and
reload the model to apply them. Keep the model and context choices under your
control; the measured settings are not applied for you.

The sampling contract follows Microsoft's documentation for
[English counter paths](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)
and [validated formatted counter values](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcountervalue).
