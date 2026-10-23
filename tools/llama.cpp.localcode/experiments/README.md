# Rejected runtime experiments

## Earlier CUDA experiment

`rejected-ada-two-row.patch` targets llama.cpp b11829 / 184f6536024668e8908ab8c86033d56708e62b40.
It is retained for inspection. New LocalCode additions use the project proprietary LICENSE;
original upstream portions retain the MIT terms in ../LICENSE-llama.cpp.
It passed correctness but did not demonstrate the required 5% speed gain on the
RTX 4060 Laptop GPU. It is excluded from the default runtime and build script.
See [the earlier runtime study](../../../docs/benchmarks/2026-10-23-runtime.md).

## Vulkan pipeline-cache experiment

This candidate is **not selected by LocalCode 1.2**. It is retained for
reproduction and further work. Do not substitute it into the release runtime
without repeating correctness checks and the performance admission gate.

The patch applies only to upstream llama.cpp b11829, commit
`184f6536024668e8908ab8c86033d56708e62b40`. LocalCode additions use the
project's proprietary [LICENSE](../../../LICENSE); original upstream portions
retain their MIT terms. Keep both notices with any permitted test build.

The opt-in `GGML_VK_PIPELINE_CACHE_DIR` stores driver pipeline data, keyed by
build, vendor, device, driver, pointer width and Vulkan cache UUID. Corrupt,
incompatible, unavailable or unwritable files are treated as cache misses.
It does not store prompts, weights or model KV state. A checkpoint after graph
execution survives owned-process termination that skips destructors. Unset the
variable for the matched control; arithmetic and model settings are unchanged.

On a dedicated clean checkout of that exact commit, apply the patch and build
with an existing Vulkan SDK, Visual Studio C++ tools, CMake and Ninja:

```powershell
git -C C:/work/llama-b11829 apply C:/work/LocalCode/tools/llama.cpp.localcode/experiments/vulkan-pipeline-cache.patch
cmake -S C:/work/llama-b11829 -B C:/work/vulkan-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON -DGGML_CUDA=OFF -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=ON -DLLAMA_CURL=OFF
cmake --build C:/work/vulkan-build --target ggml-vulkan test-backend-ops --parallel 2
```

Place the built backend in a separate copy of the matching official b11829
Vulkan package. Keep its core, CPU backends and licenses. Compile the pure cache
format test with C++17, `/utf-8`, `/DNOMINMAX`, the SDK include directory and
`ggml/src/ggml-vulkan` on the include path. Pass a new temporary folder as its
only argument. Run `test-backend-ops` against the available Vulkan device, then
`qualify_runtime.py` with an already installed model.

The fixed-work process-start study uses the same candidate DLL in both arms,
with the environment variable unset versus set. It requires Python 3.11+,
psutil and NVIDIA telemetry (`nvidia-smi`) on the measurement machine:

```powershell
python benchmark_pipeline_cache.py --server C:/work/runtime/llama-server.exe --model C:/models/model.gguf --device Vulkan0 --output C:/work/cache-study --pairs 6
python analyze_pipeline_cache.py C:/work/cache-study
```

One driver warmup and one cache first-use run are retained separately. Six
alternating process pairs retain 32K context, f16 K/V, 512 batch/microbatch,
full GPU placement, sampling, prompts and output budgets. The preset gate
requires at least 5% median paired process-start-to-first-token improvement,
a positive paired bootstrap interval, identical generated text/token counts,
and no median prefill/decode regression over 5%. Driver caches are not cleared.
This measures repeated process starts with warm OS/driver caches, not cold boot.

The [1.2 report](../../../docs/benchmarks/2026-10-23-tool-recovery.md) records the
observed decision and hardware coverage. A source patch's presence is not a
speedup claim or a release qualification receipt.
