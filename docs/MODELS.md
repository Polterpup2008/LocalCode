# Models

## Model-agnostic by construction

There is no list of blessed model families anywhere in this codebase, and no
`if (model == "qwen")`. What a model can do is either read out of its own GGUF
header or set explicitly by the user.

## Reading the header

LocalCode parses the GGUF header directly — a few kilobytes of I/O even for a
40 GB file. From it:

| | |
|---|---|
| `general.architecture` | llama, qwen3, deepseek2, gemma3, … |
| `general.name`, `general.size_label` | display name, parameter count |
| `general.file_type` | quantization, decoded to Q4_K_M, IQ4_XS, BF16, … |
| `<arch>.context_length` | what the model was trained for |
| `<arch>.block_count`, `.embedding_length`, `.attention.*` | shape |
| `<arch>.expert_count` | mixture-of-experts width |
| `tokenizer.ggml.model`, `tokenizer.ggml.tokens` | tokenizer, vocabulary size |
| **`tokenizer.chat_template`** | the interesting one |

The parser is defensive: implausible length fields are refused rather than
chased into gigabytes of allocation, vocabularies of hundreds of thousands of
strings are skipped rather than materialised, and a header that is truncated
partway through still yields whatever was read before the corruption, loudly
logged.

## Capability inference

**The chat template is the evidence.** It is the model telling us, in its own
words, what it was trained to produce.

| Capability | Established by |
|---|---|
| `supports_tools` | the template renders a `tools` variable or `tool_calls` |
| `supports_native_reasoning` | the template contains a reasoning section — `<think>`, `reasoning_content`, `enable_thinking` |
| `supports_reasoning_content` | that reasoning is delimited, so it can be displayed separately from the answer |
| `supports_vision` | a vision architecture, or `clip.*` / `*.vision.*` keys |
| `supports_long_context` | trained context ≥ 32768 |
| `supports_mtp` | `nextn` / `mtp` keys — multi-token prediction heads |
| `supports_json_schema`, `supports_grammar` | always true: these are properties of the **runtime**, not the weights |
| `supports_speculative_decoding` | false unless a draft model is configured — it depends on how the server is started |

Every inference records **why**, and the reason is shown in the model panel:

> chat template renders tool calls; chat template contains a reasoning section;
> reasoning is delimited and can be displayed separately; trained context is
> 262144 tokens; grammar and JSON schema are provided by the runtime

Anything inferred can be overridden per profile. The inference is a good default,
not a verdict.

## Nothing is emulated

If a model has no native reasoning, there is no thinking section and the
reasoning controls are disabled with an explanation. There are no invented
reasoning tiers and no synthetic effort levels.

The single exception is tool calling, where a documented plain-text protocol is
offered for models without native support — because an agent that cannot call
tools is not an agent. That fallback is explicit: the profile says
`text-fallback`, the model panel shows it, and the system prompt documents the
format.

## Profiles

```cpp
struct ModelProfile {
    std::string           id;              // stable: file stem + hash of the path
    std::string           display_name;
    std::filesystem::path model_path;

    BackendPreference     backend;         // Auto / LlamaCpp / IkLlama
    std::filesystem::path server_binary;   // pin a specific build
    std::vector<std::string> extra_server_arguments;

    std::uint32_t context_size;
    int           gpu_layers;              // -1 = all
    std::uint32_t batch_size, threads;
    bool          flash_attention, mmap, mlock;
    std::string   cache_type_k, cache_type_v;

    SamplingParams    sampling;
    ModelCapabilities capabilities;
    ToolProtocol      tool_protocol;

    bool        reasoning_enabled, show_reasoning;
    std::string native_reasoning_effort;   // passed through verbatim
};
```

Ids are derived from the file stem plus a short hash of the full path, so two
quantizations of the same model in different folders stay distinct and an id
survives a restart.

Defaults from the header: the context is the model's trained length capped at
32768 and floored at 4096 — something a workstation can actually hold — with all
layers on the GPU.

Profiles are stored in SQLite as JSON. Re-importing a model preserves the user's
settings and refreshes only the facts read from the file.

## Adding a model

File → Add GGUF model. The dialog reads the header and shows the architecture,
context length, quantization and inferred capabilities **before** you commit to
loading it, including a warning when a model has no native tool calling.

**Model files are never copied or moved.** LocalCode reads them where they are.
Nothing is re-downloaded.

## Inspecting from the terminal

```bash
localcode-doctor gguf path/to/model.gguf
```

prints the header, the inferred capabilities with their evidence, the first part
of the chat template, and the profile that would be created.

## Choosing a model

Supply a GGUF supported by your selected runtime, with enough RAM and VRAM for
its size and context window. For coding tasks, prefer a model trained for tool
calling and review the capabilities shown during import. A smaller model can be
useful for chat and short tasks; it can also need more guidance.

Qwen3.5-9B Q4_K_M was used for the installed RC2 inference check. The user guide
also describes a Ling FAST workflow, whose remaining interactive acceptance
checks are listed in [release status](RELEASE_STATUS_1.0.md). These are workflow
examples, not bundled model weights or a guarantee for every GGUF.

LocalCode references your model files in place. Weights, their licenses and
hardware requirements are separate from the application.
