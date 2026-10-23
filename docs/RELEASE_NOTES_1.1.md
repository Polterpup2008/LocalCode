# LocalCode 1.1 for Windows

LocalCode 1.1 reduces unnecessary work for conversation and improves runtime
selection on computers with limited memory or older GPU drivers.

- **Auto, Chat and Code:** greetings use a small conversational request in Auto.
  Chat omits project tools and coding instructions. Code keeps the full coding
  agent, project conventions, approvals, tools and verification. Ambiguous Auto
  requests keep the coding path. Changing modes preserves the saved conversation.
- **GPU compatibility:** the Windows package includes a Vulkan alternative to
  its CUDA runtime. LocalCode asks each runtime for actual available devices;
  bundled-runtime failures can select Vulkan or clearly report CPU fallback.
  Compatible dedicated adapters are preferred over integrated adapters in the
  Vulkan fallback. Drivers are never installed or updated automatically.
- **Memory-aware starting profiles:** new imports start at 4K on low-memory
  systems, 8K when hardware is unknown, or a larger recommendation on systems
  with more memory. Saved context settings remain intact. Unbounded legacy GPU
  placement defaults migrate to automatic layer fitting; explicit layer counts
  and saved 1.1 manual overrides are retained. Fitting keeps the selected context
  and cache precision. The Models panel offers 4K, 8K, 16K and 32K presets and
  manual controls. Smaller context recommendations are configuration tradeoffs,
  not credited as inference optimizations.
- **Bounded saved prompt states:** new profiles cap the saved prompt cache at
  128 MiB on constrained hardware or 512 MiB otherwise. An untuned profile on
  a system with at most 16 GiB RAM uses 128 MiB when supported. Explicit saved
  cache budgets take precedence. The live context and K/V precision are retained.
- **One managed agent slot:** explicit slot configuration matches the serialized
  agent and avoids unused parallel state. The measured b11829 baseline already
  reported 32K per slot with four shared slots; no context-capacity increase is
  claimed. Prompt reuse is requested explicitly.
- **Readable diagnostics:** startup, prompt evaluation and generation durations
  are separate. GPU placement reports the runtime's actual offloaded layer count
  and device-buffer logs; unknown telemetry remains unconfirmed. A detected GPU
  or Task Manager's 3D graph alone is not treated as proof of GPU inference.

The existing orange/grey interface, output-folder workflow, chat deletion,
optional Discord installer component and proprietary LocalCode license remain.
Models and personal conversations are not included in the download. Original
third-party license terms remain with their components.

See the [performance measurements](https://github.com/Polterpup2008/LocalCode/blob/main/docs/benchmarks/2026-10-23-low-end.md) and
[hardware coverage and limitations](KNOWN_LIMITATIONS_1.1.md).
