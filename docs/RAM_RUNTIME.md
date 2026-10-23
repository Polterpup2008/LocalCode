# Windows RAM runtime

The RC4 default **llama.cpp.localcode** runtime uses upstream b11829 with a
LocalCode core change that releases unused unlocked mapped model pages after
loading. It keeps model weights, context capacity and cache precision unchanged.
It needs no extra setup checkbox. See the [benchmark study](https://github.com/Polterpup2008/LocalCode/blob/main/docs/benchmarks/2026-10-23-runtime.md)
for measured scope, tests and limitations. This is a resident working-set change;
no private commit, VRAM or universal throughput improvement is claimed.

## Separate legacy runtime-copy component

RC4 does not ship the older optional b10655 copy component described below.
Its setup control stays hidden unless a compatible package is explicitly supplied
at build time. Its historical results do not describe the RC4 default runtime.

In **Set up runtime and modelâ€¦**, select your existing `llama-server.exe` and
model. **Prepare a runtime copy with RAM optimizations** is unchecked by default.
Selecting it prepares a separate copy of a compatible runtime, then checks that
the copied server starts. **Use these files** returns that path to the open
settings dialog. Save the settings and reload the model when you want to use it.

The original runtime and model are preserved. Only runtime binaries are copied;
models, tokens, settings, persona and conversation history are excluded. The copy
includes its source patch, license and file digests. Keep any additional model,
adapter or auxiliary-file arguments as absolute paths, because the copied
runtime has a different working directory. The setup dialog resolves the chosen
model and projector paths for you.

The supplied package supports Windows x64 and an explicitly verified b10655
runtime ABI. Other runtimes remain usable with the option unchecked. This check
prevents mixing incompatible libraries; it does not require a particular model,
CPU or GPU. A future package can qualify a different runtime build.

The change asks Windows to trim unused, unlocked pages of the mapped model file
after loading, including interior ranges whose tensors have been copied or
repacked into other buffers. It preserves borrowed tensor spans, aliases, lazy
ranges and complete non-CPU host-buffer ranges. The mapping remains valid and
Windows can bring pages back if needed. Locked model mappings retain their pages.
It does not compress model
weights or the KV cache, change context length, or promise more tokens per
second or less VRAM.

The original outer-range version released about 298 MiB and 2.02 GiB of GPU-run
resident file pages on two diagnostic models. The extended version also released
160.66 MiB and 1.63 GiB in CPU runs relative to that version, retaining active
pages. CPU mean generation rates were about 0.43% lower in the recorded loaded
comparison, within the frozen 5% bound; this is no throughput gain claim. All 135
replies/3024 output IDs matched across CPU/GPU and locking controls. Repacking-off
and heap-loading controls also matched within each mode.

These are resident file-page measurements, not a reduction in committed private
memory. Results depend on how a runtime loads a model. Earlier snapshot-memory
and interior-range timing failures remain recorded; the distinct late-allocation
candidate passed the unchanged lifetime-peak/current-private 32 MiB and timing
bounds. This does not certify every model or machine. Actual split models,
non-CPU zero-copy buffers and lazy model architectures remain untested.

For builders, pass `-DLOCALCODE_MEMORY_RUNTIME_PACKAGE=<package-directory>` to
CMake, or `-MemoryRuntimePackage <package-directory>` to
`scripts/package-windows.ps1`. The directory contains a manifest, verified DLLs,
license, readme and source patch. Without it, the optional control is hidden.
The manifest binds the core, server, ggml and ggml-base binaries by SHA-256.
An optional bounded `compatible_core_sha256` list permits explicitly approved
previous RAM cores to be copied into a newer version. Server and dependency
digests must still match. Copies are checked before publication and again before
cached reuse; selecting an old copy never modifies that source directory.
