# LocalCode 1.2.1 — Vulkan resident-memory qualification

The Windows mapped-file working-set patch is now qualified for the bundled
Vulkan alternative. On this 9B workload, median ready-process working set fell
from **5.044 GiB to 0.294 GiB (94.17% lower)** across twelve fresh
process pairs. The complete GGUF mapping, 32,768-token context, f16 K/V cache,
weight quantization, batch sizes and output tokens were retained.

| Snapshot | Unmodified core | Modified core |
| --- | ---: | ---: |
| Ready, twelve process pairs | 5.044 GiB | 0.294 GiB |
| After long and repeated cached workloads, six follow-up pairs | 5.384 GiB | 0.637 GiB |

The post-workload paired median reduction was
88.17%. Both snapshots are
process working set, measured outside timed requests.

This extends the existing primary-runtime change to another backend. It does
not introduce a new GPU kernel or claim faster generation. The Vulkan backend
DLL and every other runtime binary are byte-identical between the matched arms;
only `llama.dll` differs. Both cores were rebuilt with the same MSVC 14.44.35207
Release configuration and `GGML_NATIVE=OFF`, from llama.cpp b11829 commit
`184f6536024668e8908ab8c86033d56708e62b40`.

## Preset admission and result

The admission rule was fixed before measurement: six alternating process pairs,
at least 20% median working-set reduction, exact request hashes, generated tokens
and prompt-work counts, equal native mapped-file capacity, no observed median
request timing regression greater than 5%, and no loss of API/answer checks.
The initial six-pair block **failed**: cached first-text ratio was 9.53% worse,
despite exact work and the resident-memory reduction. A single near-capacity
pair also showed a 24.39% lower cached first-text ratio. The shipped-runtime
ABBA control did not reproduce a consistent slowdown. These failures and all
samples are retained in the complete record; their cause is not established.

Before further measurement, one additional six-pair block was specified with
five cached repetitions per process. The model, output budget, context,
precision and binaries remained identical. All twelve independent pairs were
then combined, averaging repeated requests within each process pair and
retaining every original sample. The unchanged memory/timing gate **passed on
that combined dataset**. Startup was retained separately and its median
baseline/candidate ratio changed by -7.14%.
The separate 5% startup guard **failed**. Overall startup medians were
7.77 seconds versus
8.19 seconds. The user explicitly
accepted this startup tradeoff for the resident-RAM saving before publication.
The failure remains recorded and is not reported as a passing startup check.

| Workload | Median decode ratio change | Wall-time ratio change | First-text ratio change |
| --- | ---: | ---: | ---: |
| cached | -0.10% | +0.19% | -3.23% |
| long | -0.20% | +1.34% | +1.11% |
| short | -0.21% | +0.00% | +0.29% |

Positive ratios favor the candidate. These are observations under background
load, not speed claims. All raw pair ratios and bootstrap intervals, including
the slower samples, remain in the [analysis](data/vulkan-memory-9b/analysis.json).

The native page measurement uses `VirtualQueryEx` and `QueryWorkingSetEx`, outside
timed requests. It distinguishes virtual mapping size from resident pages. The
mapping remains valid and can refault from Windows' file cache or disk. This is
a reduction in resident process working set, **not private committed memory,
startup peak memory or VRAM**. It does not make an oversized model fit in VRAM.

## Correctness and coverage

- Shipped stock Vulkan, matched unpatched core and patched core each passed
  **15/15** synthetic API/answer checks, including Unicode, arithmetic,
  generated-function AST checks, native tool arguments and streamed tokens.
- All **283 named exports** match in all three cores.
- Native tests passed 20,000 interval cases, 5,000 grouped-metadata cases,
  nine real file mappings, page-boundary retention, byte-exact refaults and
  actual locked-page preservation, with no unexpected reclaim warning.
- Five matched controls passed: CPU-only, 20-layer mixed offload with the 9B
  model, `mmap+mlock`, no mmap, and Intel integrated Vulkan with the installed
  0.8B model. Initial and repeated request tokens match in each arm. The locked
  control retained its actually locked pages; it is not a claim that Windows
  locked every model page.
- A separate ABBA comparison against the actual shipped stock Vulkan runtime
  matched tokens and prompt work. Its two pairs are a product compatibility
  control, not a separate six-pair performance admission.
- The capacity check processed **31,325 prompt tokens**
  and generated 128 tokens, with unchanged 32K capacity, no context shifting,
  no truncation, and identical cold/cached output tokens.

The full-GPU admission was measured on an RTX 4060 Laptop GPU. Intel UHD Vulkan
and CPU checks establish only the stated compatibility observations. The patch
uses Windows mapping APIs and contains no adapter-name or RTX-specific branch.
AMD and Apple inference performance were not measured. The primary CUDA/CPU
runtime retains its previously qualified core unchanged.

An application-level transport check executed Python, spooled the actual output
`323`, and replayed command history through a Unicode folder with both cores.
Both model-task verification summaries were failing, so these checks are not
counted as fully verified agent-task successes. Their results remain in the
complete record. This update does not claim to improve model task accuracy.

## Environment and limits

The laptop remained plugged in with user applications open. Godot and Blender
were active; one native engine sample observed Godot using about 31% of the GPU
while the owned test server was active. The GPU reached
88°C and software thermal slowdown was
observed.
No user app, power setting, driver or GPU clock was changed. The initial faster
baseline sample and all later loaded samples are retained. These measurements
do not establish idle-machine throughput or universal superiority.

The [complete sanitized qualification record](2026-10-24-vulkan-memory.json)
includes native tests, ABI, compatibility controls, shipped comparison,
capacity check and exact binary hashes. The [formal rows](data/vulkan-memory-9b/)
contain only synthetic content, with no user conversations or absolute paths.

## Reproduction

Use the [core patch and build recipe](../../tools/llama.cpp.localcode/README.md)
with two dedicated checkouts of the pinned commit. Build the reference before
applying the patch, using the same flags listed by `build-windows-core.ps1`.
Replace only the core DLL in otherwise byte-identical Vulkan runtime folders.
Use your installed GGUF and the device returned by `llama-server --list-devices`.

```powershell
python tools/llama.cpp.localcode/qualify_runtime.py --server candidate/llama-server.exe --model model.gguf --device Vulkan1 --output build/api-new
python tools/llama.cpp.localcode/benchmark_runtime.py --runtime baseline=reference/llama-server.exe --runtime candidate=candidate/llama-server.exe --model model.gguf --device Vulkan1 --order baseline,candidate,candidate,baseline,baseline,candidate,candidate,baseline,baseline,candidate,candidate,baseline --repetitions 1 --cached-repetitions 1 --measure-residency --output build/comparison-new
python tools/llama.cpp.localcode/analyze_runtime.py build/comparison-new
```

The follow-up used the same command in a new output directory with
`--cached-repetitions 5`. Its six process pairs supplement the original six;
the combined manifest records both blocks and the repetition count for each
phase. The published combined rows retain every initial request. Recompute
the exact published statistics with:

```powershell
python tools/llama.cpp.localcode/analyze_runtime.py docs/benchmarks/data/vulkan-memory-9b
```

Original llama.cpp portions retain MIT rights. LocalCode additions retain the
project's proprietary terms; both notices accompany the modified runtime.
