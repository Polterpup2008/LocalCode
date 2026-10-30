# LocalCode 1.2.2 for Windows

Model setup and profiles now offer four runtime choices: the included LocalCode
optimized runtime, upstream llama.cpp, ik_llama.cpp, and PrismML llama.cpp for
Bonsai 2. ik_llama.cpp uses an existing build; its official releases currently
provide no Windows binary archive.

Select **Models → Set up a local model → PrismML llama.cpp (Bonsai 2)** to
download an official Windows CPU, CUDA, or Vulkan runtime and the compact
Bonsai 2 27B PTQ1_0 GGUF. Downloads display their source and size and verify
SHA256 before installation. CUDA packages include their matching libraries.
Older bundled-runtime profiles are migrated to the explicit LocalCode choice,
so upgrading preserves their included runtime.
The model profile retains its runtime; selecting PrismML preserves the other
runtime preferences. Existing PTQ1_0/PQ2_0 files are recognized during import.

The 27B weight file is about **5.95 GB**. Runtime buffers, context and caches need
additional memory. On the tested RTX 4060 Laptop GPU at 4K context, Bonsai ran
with all 65 layers on the GPU and generated a median **26.5 tokens/s**, compared
with **32.9 tokens/s** for MiMo 9B Q4_K_M. Thermal-slowdown events were recorded;
these are local observations, not a universal ranking. The small API/answer
screen scored 14/15 and 13/15 respectively and is not a broad coding benchmark.
See the [full comparison](benchmarks/2026-10-30-bonsai-vs-mimo.md).

The included CUDA/CPU and Vulkan runtimes retain the qualified 1.2.1 payload.
The optional Discord installer component and third-party notices are preserved.
Models, conversations, private settings and development artifacts are excluded.
The Windows binaries are unsigned.

See the [Bonsai setup guide](BONSAI.md), [previous release notes](RELEASE_NOTES_1.2.1.md),
and [release validation](benchmarks/2026-10-30-release-validation.json).
