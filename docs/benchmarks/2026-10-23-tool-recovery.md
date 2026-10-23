# LocalCode 1.2: tool-call recovery and a rejected runtime candidate

## Release decision

Ship the tool-call reliability fix and reply-token control. Keep the qualified
1.1 runtime: the explicit Vulkan pipeline-cache candidate failed its preset
performance gate. It is not enabled or included as a binary in 1.2.
No new raw inference speedup is claimed for this release.

## Reproducing the reported failure

An incomplete native tool call was stored before argument validation. When the
next request replayed it, llama.cpp parsed the historical JSON during template
rendering and returned HTTP 500 before generating a replacement. Repeating a
shorter-call instruction could not repair that request.

The synthetic regression has 20,385 malformed argument bytes. The real server
reported the missing closing quote at column 20,386, matching the reported
failure. The payload exported by the actual C++ backend fixture, with the
invalid historical call omitted, received HTTP 200 from the **same process**:
99 prompt tokens and 16 output tokens. This used an installed Qwen3.5-0.8B
Q4_K_M model on CPU with 32,768 context capacity and f16 K/V. No model tool was
executed in this HTTP proof; no personal chat content was used or published.

New batches are validated before persistence or dispatch. An incomplete batch
executes none of its calls, including earlier valid calls in that batch.
Recovery disables parallel calls for the next request, asks for one short valid
JSON object, and stops after two retries. Previously saved malformed calls and
their paired rejected results are omitted from outgoing history before budgeting;
saved transcripts, valid results and task evidence are retained. Call-ID reuse
in a later turn does not discard earlier valid results.

This does not establish that a small model can implement a correct chess game.
It establishes that the demonstrated parser failure no longer poisons retries,
that rejected batches have no file effects, and that recovery is bounded.

## Additional package acceptance findings

A real Qwen3.5-9B coding check edited a chess-side helper and ran its three
tests successfully, then exposed a separate Windows bug: the full command-log
link used the system code page for an accented path and broke JSON serialization
on the next request. Version 1.2 now converts that path, command-log headers and
working-directory errors through the shared UTF-8 path helper. An actual command
in a folder containing accented, CJK and emoji characters passed the regression:
its spooled header, model-visible log link and outgoing JSON remained valid.

A separate MiMo V2.6 Distill Qwen 9B check at 16K context, f16 K/V, 4,096 reply
tokens and reasoning enabled did not complete a valid tool call before the
action deadline. It made no edit and its independent helper tests failed.
There was no parser crash, but this is **not a successful coding screen**.
The cause of that model's prolonged generation is not established by this test.
The final package validation receipt records the post-fix coding result and
does not turn a model capability failure into an application success claim.

## Runtime experiment

The candidate adds an opt-in Vulkan driver pipeline cache to pinned llama.cpp
b11829 (`184f6536024668e8908ab8c86033d56708e62b40`). It does not change shader
arithmetic, weights, model capacity, context or K/V precision. Files are keyed
by build/device/driver/UUID, checked for bounds and corruption, replaced
atomically, and checkpointed without relying on process destructors. Missing
or rejected cache files are benign misses.

The implementation uses a pipeline cache with flags zero, retaining Vulkan's
internal synchronization for concurrent pipeline creation. This follows the
[Vulkan pipeline-creation reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateComputePipelines.html)
and [cache-data reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html).
The [official cache sample](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html)
explains the mechanism; it does not predict a gain on this workload.

The same candidate backend DLL was used in both timing arms: variable unset
versus an explicit cache directory. One driver warmup and one cache-first-use
run were retained separately, followed by six alternating process pairs. Model:
Qwen3.5-9B Q4_K_M; 33/33 GPU layers, 32K context, f16 K/V, eight threads,
512 batch/microbatch, temperature zero and seed 42. The two prompts used 31 and
2,219 tokens; all paired outputs and output-token counts matched exactly
(18 and 26 tokens). Output allowance was 64 in both arms.

The primary measure was process start to the first content token for the short
request. The gate was fixed before the campaign: at least 5% median paired
improvement, paired 95% bootstrap interval excluding zero, exact work
equivalence, and no median prefill/decode regression over 5%.

| Pair | Cache disabled | Cache enabled | Paired improvement |
|---|---:|---:|---:|
| 1 | 10.638 s | 10.553 s | +0.80% |
| 2 | 10.465 s | 10.583 s | -1.13% |
| 3 | 10.403 s | 10.347 s | +0.54% |
| 4 | 10.701 s | 10.396 s | +2.85% |
| 5 | 10.450 s | 10.295 s | +1.48% |
| 6 | 10.604 s | 10.443 s | +1.52% |

**Result: 1.14% median improvement; 95% paired bootstrap interval −0.29% to
+2.18%. Not admitted.** No measured prefill/decode median regressed over the 5%
guardrail. The study does not establish a meaningful startup benefit. No
additional timing campaign was run to search for a favorable result.

Measurements were on one Windows laptop with an RTX 4060 Laptop GPU, warm OS
and driver caches, and background desktop activity. GPU temperature, utilization,
clocks and free memory were retained for each run. No user server was resident
at campaign start; no user apps were closed. Driver caches, drivers and power
settings were not changed. This is not a cold-boot test, a universal decode
claim, or evidence about AMD/Intel/Apple hardware.

## Correctness and coverage

- The pure cache helper passed 592 checks, including corruption, truncation,
  incompatible keys/headers, Unicode paths, failed writes and concurrent file
  replacement.
- 134 selected Vulkan matrix-multiply cases passed against the CPU reference.
  This covers selected quantized operations, not the entire GPU backend. Two
  queue-acquisition validation messages also occurred in the unmodified control;
  no new cache-related message was observed. The run is not claimed to be
  validation-clean.
- The 9B candidate passed all 15 API/answer checks. Both 0.8B reference and
  candidate passed 8/15 with the same verdict pattern; the smaller model's
  failed answer checks are retained, not relabeled as successes.
- The GUI reply-limit check saved 8,192 output tokens while preserving the
  32,768 context and f16 precision. The narrow scrolling form was inspected
  through a Qt-rendered image without desktop input. The full GUI suite passed
  105 cases and 4,263 assertions; six opt-in cases were skipped.

The release validation receipt records the final core, package and GitHub
checks. The [machine-readable results](2026-10-23-tool-recovery.json) retain all
paired measurements, warmups, model/backend hashes and the admission decision.
The [isolated experiment source and reproduction steps](../../tools/llama.cpp.localcode/experiments/README.md)
remain available; they do not select or qualify a release runtime.
