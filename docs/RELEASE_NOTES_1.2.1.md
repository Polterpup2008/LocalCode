# LocalCode 1.2.1 for Windows

The bundled Vulkan alternative now includes LocalCode's qualified Windows
mapped-file working-set optimization. The primary CUDA/CPU runtime retains its
previously qualified core.

In the matched Qwen3.5-9B test, ready-process resident working set fell from
**5.04 GiB to 0.29 GiB (94.17% lower)** across twelve process
pairs, keeping the same 32K context, f16 cache, model weights and generated tokens.
This is not a reduction in private commit, startup peak or VRAM, and no faster
token generation is claimed.

There is a startup tradeoff: measured overall medians were
7.77s before and
8.19s after. The separate startup
guard failed; this release retains that known cost for the RAM saving with
the user's explicit acceptance. See the full paired data in the report.

CPU, partial offload, locked-memory, no-mmap and Intel integrated Vulkan controls
returned identical tokens. A prompt near 32K capacity and separate API checks
also passed. See the [measurements and hardware limits](benchmarks/2026-10-24-vulkan-memory.md).
The full-GPU result was measured on an RTX 4060 Laptop GPU under background load;
it is not a universal hardware score.

The tool-call recovery and Unicode command-history fixes from 1.2 remain.
The optional Discord installer component and proprietary LocalCode license
remain. Third-party licenses are included. The Windows application is unsigned.
Models, conversations, private settings and development outputs are excluded.
See [1.2 release notes](RELEASE_NOTES_1.2.md) and [limitations](KNOWN_LIMITATIONS_1.2.md).
