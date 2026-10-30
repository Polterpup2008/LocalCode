# Bonsai 2 and runtime choices

LocalCode supports four runtime choices per model. AUTO keeps the existing
configured default; it does not compare speed or download a runtime silently.

| Choice | Installation |
| --- | --- |
| LocalCode optimized | Included in the Windows package, including its Vulkan alternative. |
| llama.cpp (upstream) | Download an official Windows x64 CPU, CUDA, or Vulkan build in model setup, or choose existing files. |
| ik_llama.cpp | Choose an existing server build and its accompanying libraries. The official project does not currently publish Windows binary archives. |
| PrismML llama.cpp (Bonsai 2) | Download the official PrismML Windows x64 CPU, CUDA, or Vulkan build in model setup, or choose existing files. |

## Download Bonsai 2 27B

1. Open **Models → Set up a local model**.
2. On **Runtime**, select **PrismML llama.cpp (Bonsai 2)** and leave the release
   tag at `latest`, or enter an exact PrismML release tag.
3. Find Windows builds and select one. For NVIDIA on Windows, CUDA 12.4 avoids
   the CUDA 13.3 startup issue documented by PrismML. CUDA downloads include the
   matching runtime libraries. Review the source, size, and checksum, then download.
4. On **Model**, the official repository is filled in:
   `prism-ml/Ternary-Bonsai-2-27B-gguf`. List the GGUFs. LocalCode selects
   `Ternary-Bonsai-2-27B-PTQ1_0.gguf` for this repository.
5. Download the model and wait until verification completes. Choose **Use these
   files**. The model profile retains its selected runtime. Load the model from
   the Models panel.

Downloads are staged separately and published only after their size and SHA256
match the provider's metadata. A `latest` runtime request resolves to an exact
release tag; model downloads use an immutable Hugging Face revision. Receipts
retain the source, revision, size, license metadata, and checksum.

The PTQ1_0 language-model file is 5,946,648,928 bytes. Its size is not a promise
that inference uses only 6 GB of memory. Allow additional memory for runtime
buffers, the KV cache, and context. A 4K context is a useful starting point on
an 8 GB GPU. The vision projector is a separate optional download and is not
needed for text coding tasks.

PTQ1_0 and PQ2_0 require PrismML's kernels. LocalCode recognizes their GGUF file
types when importing existing files, even when the file has been renamed, and
selects the PrismML backend. Ordinary upstream or LocalCode runtime builds are
not substitutes for those kernels. Changing a profile's backend explicitly
releases its previously pinned server so the new choice takes effect.

Settings includes a separate PrismML server path and AUTO preference. The CLI
accepts `--backend prism`, `LOCALCODE_PRISM_LLAMA_SERVER`, and
`config set prism-llama-server <server-path>`. Use `--backend localcode` for the
included optimized runtime and `--backend llama.cpp` for an upstream build.

## Reproduce a comparison

The comparison tool uses synthetic source text, a warm-up per server, equal
context and f16 cache precision, 128 output tokens per timed request, and a
forward/reverse process order. It records model/runtime hashes, request and
response artifacts, startup, TTFT, prompt/decode rates, process memory, GPU
observations, and cleanup of its own servers. Tokenizer differences remain a
limitation of comparisons between different models.

```powershell
python tools/llama.cpp.localcode/compare_models.py --case bonsai C:/runtimes/prism/llama-server.exe D:/models/Ternary-Bonsai-2-27B-PTQ1_0.gguf --case mimo C:/runtimes/localcode/llama-server.exe D:/models/MiMo-V2.6-Distill-Qwen-9B-Q4_K_M.gguf --context 4096 --output build/bonsai-vs-mimo-new
python tools/llama.cpp.localcode/qualify_runtime.py --server C:/runtimes/prism/llama-server.exe --model D:/models/Ternary-Bonsai-2-27B-PTQ1_0.gguf --context 4096 --output build/bonsai-api-new
```

The answer/API checks are a small compatibility screen, not a broad coding
benchmark or evidence that one model is generally more capable.

Sources: [official model card](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf),
[PrismML runtime releases](https://github.com/PrismML-Eng/llama.cpp/releases),
[known issues](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf/blob/main/KNOWN_ISSUES.md),
[ik_llama.cpp](https://github.com/ikawrakow/ik_llama.cpp).
