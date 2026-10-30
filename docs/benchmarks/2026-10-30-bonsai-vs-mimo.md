# Bonsai 2 27B versus MiMo 9B — 30 October 2026

Bonsai 2 27B PTQ1_0 loads and generates correctly through the PrismML runtime
and LocalCode's production agent/backend path on this RTX 4060 Laptop GPU.
Its median short-prompt generation was **26.5 tokens/s**;
MiMo 9B Q4_K_M with the bundled LocalCode runtime produced
**32.9 tokens/s**, about **24% more tokens/s** in this run.
GPU thermal-slowdown events were recorded. These are observations from this
laptop under load, with the observed variation retained below.

## Measured results

| Metric | Bonsai 2 27B PTQ1_0 | MiMo V2.6 Distill Qwen 9B Q4_K_M |
| --- | ---: | ---: |
| Weight file, decimal GB | 5.947 | 5.841 |
| Short-prompt decode, median tokens/s | 26.51 | 32.88 |
| Long-prompt decode, median tokens/s | 26.27 | 33.38 |
| Long-prompt processing, median tokens/s | 447.2 | 1337.5 |
| Short-prompt first token, median seconds | 0.407 | 0.189 |
| Long-prompt first token, median seconds | 3.137 | 1.070 |
| Observed peak board GPU memory, MiB | 6044 | 5388 |
| Peak process resident RAM including loading, GiB | 7.29 | 4.85 |
| Small API/answer screen | 14/15 | 13/15 |

The 5.95 GB figure describes the Bonsai weight file. It does not bound combined
system RAM, GPU memory, caches, or runtime buffers. GPU memory above is sampled
whole-board memory, rather than a per-process allocation guarantee. Peak process
RAM includes loading and is not a steady-state claim.

## Method and limits

- Intel Core i5-12450H, 32 GB installed RAM, RTX 4060 Laptop GPU with 8188 MiB VRAM.
- Bonsai: PrismML `prism-b10831-9d9efea`, Windows x64 CUDA 12.4.
  MiMo: the bundled LocalCode b11829 runtime, including its Windows memory patch.
  This compares the usable model/runtime combinations; it does not isolate a
  model-only effect or establish a general kernel ranking.
- 4096 context tokens, full GPU layer request, f16 K/V caches, 8 threads,
  batch 512, microbatch 128, one slot, temperature zero, seed 42.
- Forward/reverse order: Bonsai, MiMo, MiMo, Bonsai. One warm-up per process;
  three short and three long timed requests per process: six samples per model
  per workload, 24 timed requests altogether.
- Identical request hashes for each workload across both models, zero cached
  prompt tokens, 15 input tokens for the short workload, 1371 for the long
  workload, and exactly 128 generated tokens per sample. EOS is ignored only
  for the fixed-work throughput measurement. Those continuations are not used
  to judge answer quality.
- Short-prompt decode ranged from 25.57 to 29.01 tokens/s for Bonsai and
  25.94 to 34.40 tokens/s for MiMo. Thermal-slowdown markers occurred in all four
  phases; observed GPU temperatures covered 50–81 C. Power, clocks and cooling
  settings were not changed. Do not treat the percentage as a universal gain.
- The original MiMo run from the busy Seagate USB drive exceeded the harness's
  150-second startup limit. Both timed inputs were therefore copied to the
  same SATA SSD and checked against the verified source SHA256 values. The
  timed runs exclude the failed USB-drive attempt. SSD startup observations
  were 3.72–3.80 seconds for Bonsai and 4.44–6.93 seconds for MiMo; filesystem
  caches were not reset, so cold-start speed is not a controlled comparison.

## Functional checks

Both models passed Unicode token round-trip, JSON transport/schema handling,
three generated pure arithmetic functions evaluated against nine inputs, native
tool arguments, complete streaming token accounting, and post-request health.
These are small compatibility checks, not a broad coding-quality benchmark.
Bonsai missed the character-count question (`LocalCode` has 9 characters; it
answered 11). MiMo missed that question (answered 10) and the list sum (answered
19 instead of 20).

The actual LocalCode CLI agent, using `--backend prism`, loaded Bonsai with
**65/65 layers on CUDA0**, reported 5938 MiB of device buffers, and answered
`BONSAI_OK` in one step. That checks the production backend selection and
inference path separately from the Python HTTP harness. Every benchmark-owned
server process was stopped and its cleanup receipt reports no remaining PIDs.

## Implementation and distribution checks

- Four choices are exposed in setup and model profiles: LocalCode optimized,
  upstream llama.cpp, ik_llama.cpp, and PrismML llama.cpp.
- PrismML has its own saved path, discovery classification, backend identity,
  CLI/environment configuration, and SHA256-verified runtime downloads with
  matching CUDA libraries. `latest` resolves to an exact provider release.
- The official Bonsai repository is offered automatically and the compact
  PTQ1_0 file is selected ahead of F16. Existing Prism GGUF file types select
  PrismML even when renamed. Per-model runtime pins survive reopening setup;
  choosing PrismML preserves the bundled-runtime preference for other models.
- ik_llama.cpp remains an existing-build choice: the official project did not
  publish a Windows binary archive at the time of inspection.
- Original local-preview suites: 1029 core tests passed / 7 skipped, and 108 GUI tests passed /
  6 skipped. After the final changes, focused model/backend checks passed
  77 cases with 1144 assertions, and setup/settings checks passed 27 cases with
  1871 assertions; optional checks were reported as skipped.
- The real PrismML CPU download/install/probe test passed all 8 assertions.
  The benchmark helper's 10 protocol tests passed. Native setup was captured
  and visually inspected. The portable ZIP's executables match the tested
  build, it includes the Bonsai guide and both bundled runtime variants, and
  contains no model weights, user databases, or logs.

The verified PrismML runtime was also installed in the local managed-runtime
folder. Bonsai is registered in the local model library with the original
`D:/Ai/MODELS/LLM/Ternary-Bonsai-2-27B-PTQ1_0.gguf`, a pinned PrismML server,
4K context, full GPU layer selection, and f16 caches. Other configured runtime
paths were preserved.

The original local preview was subsequently prepared for release as LocalCode
1.2.2. See [the release notes](../RELEASE_NOTES_1.2.2.md) and
[release-source validation](2026-10-30-release-validation.json).
The installed app starts at `bin/LocalCode.exe`; the Bonsai runtime/model can be
selected in model setup. See [the setup guide](../BONSAI.md).

## Evidence and reproduction

[Machine-readable results, checks, hashes and provenance](2026-10-30-bonsai-vs-mimo.json)
contain the complete sampled results. Detailed local requests, responses,
server logs, observations and cleanup receipts are under
`build/bonsai-20261030/`. The comparison command is documented in
[the Bonsai setup guide](../BONSAI.md#reproduce-a-comparison).

Bonsai was downloaded from `prism-ml/Ternary-Bonsai-2-27B-gguf` revision
`b072e1d3b35a0a630cece372c2127528e0994386` into
`D:/Ai/MODELS/LLM/Ternary-Bonsai-2-27B-PTQ1_0.gguf`.

- Bonsai SHA256: `53107f530aa52eb00912263ab1ee29bd199261c87cd7b4ad4ca1318c1fe33ee3`
- Original requested MiMo SHA256: `4bca6f18c73f72270c7a20c2ea2bea581de8246e318714277120369d34048c81`
- Preview ZIP SHA256: `24b2b4752df24c10281ff39d0f8d26ce45a08c4ec60e316852a5bf697e7f2e29`

Primary references: [official model card](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf),
[PrismML releases](https://github.com/PrismML-Eng/llama.cpp/releases),
[known issues](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf/blob/main/KNOWN_ISSUES.md),
[ik_llama.cpp releases](https://github.com/ikawrakow/ik_llama.cpp/releases).
