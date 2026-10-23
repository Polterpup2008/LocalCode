# LocalCode runtime optimization and compatibility

LocalCode 1.2 fixes incomplete native-tool batches and replay of malformed calls
saved by older versions. The additional Vulkan pipeline-cache experiment failed
its preset performance gate and is **not enabled or shipped as a binary**.
See the [1.2 reliability and experiment report](../../docs/benchmarks/2026-10-23-tool-recovery.md)
and [isolated experiment source](experiments/README.md).

To reproduce the HTTP failure using synthetic history, export the actual C++
backend fixture payload into a new file, then launch the isolated CPU check
with an already installed small model and compatible llama.cpp server:

```powershell
$env:LOCALCODE_TEST_REPLAY_PAYLOAD = 'C:/work/replay-fixture.json'
./build/audit-release/bin/localcode_tests.exe 'malformed stored tool calls are omitted on replay without changing saved messages'
Remove-Item Env:LOCALCODE_TEST_REPLAY_PAYLOAD
python tools/llama.cpp.localcode/verify_tool_replay.py --server C:/work/runtime/llama-server.exe --model C:/models/model.gguf --payload C:/work/replay-fixture.json --output C:/work/replay-check
```

This starts and stops only its own server on a free loopback port. It sends no
tools for execution and does not read or modify an existing conversation store.

LocalCode 1.1 reduces unnecessary agent prompt work across inference backends.
Its Windows bundle also includes a Vulkan alternative, probes actual available
devices, fits GPU placement while retaining context and precision, and reports
the runtime's real layer counts. The primary runtime retains the qualified
Windows working-set patch; the Vulkan alternative uses the matching stock core.
See the [1.1 measurements and hardware coverage](../../docs/benchmarks/2026-10-23-low-end.md).

`benchmark_agent_prompt.py` compares the actual greeting requests made by two
LocalCode CLI builds in fresh homes and a synthetic project, using an existing
model. It measures application work reduction, not identical-prompt kernel speed.

## Earlier fixed-work runtime study

The first study targets Windows x64 CUDA inference on an RTX 4060 Laptop GPU.
It compares the shipped b10655 runtime, upstream b11829 (184f6536024668e8908ab8c86033d56708e62b40),
and a matched build with each candidate patch. Upstream changes and custom gains
are reported separately. No candidate is selected by this directory's existence.

All comparisons retain the exact GGUF weights, 32K server context, f16 K/V cache,
512-token batch/microbatch, GPU layer count, sampling settings, and output budgets.
Changing these settings does not count as a custom runtime optimization.
The short, long (about 15K tokens), and repeated-prefix workloads are synthetic.
Performance artifacts never include personal conversations.

Run the answer/API screen before admitting a candidate:

```powershell
python tools/llama.cpp.localcode/qualify_runtime.py --server PATH/llama-server.exe --model PATH/model.gguf --output build/qualification-new
```

Then alternate matched runtimes, with a warmup per process and multiple repetitions:

```powershell
python tools/llama.cpp.localcode/benchmark_runtime.py --runtime baseline=PATH/reference/llama-server.exe --runtime candidate=PATH/candidate/llama-server.exe --model PATH/model.gguf --output build/comparison-new
```

Python 3.11+ and psutil are required. NVIDIA observations use nvidia-smi when
available. Each output directory must be new. Commands, runtime/model hashes,
requests, responses, startup, streamed TTFT, runtime timings, sampled memory,
temperature, power, clocks, CPU load, and owned-process cleanup are retained.
Do not compare timings from different request hashes or token counts as equivalent
work. Separate cold-prefix and cached-prefix results.

The first CUDA candidate gate is: unchanged kernel arithmetic and context,
backend correctness tests passing, no loss of baseline API/answer checks,
at least 5% median decode improvement on the target workload with a positive
paired confidence interval, and no greater than 5% regression in the other
measured target workloads. Rejected candidates remain unselected.
Thermal throttling and background applications must be reported; profiled runs
are for bottleneck attribution and cannot substitute for uninstrumented timings.

Hardware/model coverage bounds every claim. This study cannot establish that one
runtime is faster than all AI servers on all hardware and workloads. Linux-only
servers require a separate supported Linux test environment.

The Windows memory candidate has a separate admission rule, fixed before its
long timing run: at least 20% median process working-set reduction across six
process pairs, identical mapped-file capacity and generated tokens, unchanged
API/answer verdicts, and no observed median timing regression over 5%. A memory
optimization does not require a decode speed gain. `--measure-residency` records
native `VirtualQueryEx`/`QueryWorkingSetEx` measurements outside timed requests.
These measure resident pages, not private commit, peak allocation or VRAM.

`windows-working-set.patch` applies only to upstream b11829, commit
`184f6536024668e8908ab8c86033d56708e62b40`. The new LocalCode additions use the project's
proprietary [LICENSE](../../LICENSE). Original upstream portions retain the MIT
terms in `LICENSE-llama.cpp`; the proprietary terms do not replace those rights. It trims only complete unused unlocked
Windows mapped-file pages after weight loading and repacking. Active tensor,
host-buffer and lazy ranges are retained; explicit locking bypasses reclamation.
The memory mapping and model bytes remain valid and refaultable.

To reproduce the core with Visual Studio 2022 C++ tools, CMake and Ninja, use a
dedicated clean checkout of that pinned source, then run:

```powershell
./tools/llama.cpp.localcode/build-windows-core.ps1 -SourceDirectory C:/work/llama-b11829 -BuildDirectory C:/work/localcode-core-build
```

The script applies the patch in that checkout, builds only the core DLL, and
compiles/runs the actual mapped-file class tests. It requires no CUDA SDK because
the CUDA backend is unchanged. Place the resulting `bin/llama.dll` in a separate
copy of the matching official Windows runtime, preserving its other DLLs and
notices. Then run qualification and matched benchmarks. Different compilers or build paths can
produce different binary hashes; the release receipt identifies the shipped one.

`benchmark_ollama.py` runs a portable Ollama build on a separate loopback port
with its own model store and profile. It disables cloud features, verifies the
imported GGUF hash and actual 32K loaded context, and retains native streaming
statistics. It never uses a user's existing daemon or model store. Different
engines may produce different tokens: report that limitation, actual cache work
and output lengths rather than passing those runs through the exact-output
custom-patch oracle.
