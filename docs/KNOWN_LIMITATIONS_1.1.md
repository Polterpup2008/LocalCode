# LocalCode 1.1 — hardware coverage and limitations

The Windows application is unsigned. The command execution and security
boundaries in [the existing limitations](KNOWN_LIMITATIONS_1.0.md) still apply.
The proprietary LocalCode license does not replace third-party license grants.

## Older drivers and low-memory computers

The reported GTX 1650 Ti / 4 GB VRAM / 16 GB RAM computer was not accessible in
the development environment. Its reported driver 528.02 is older than the driver
branch required for CUDA 13. This explains a compatibility risk, not proof of
which backend the original session actually used. The actual model filename,
serialized request and server logs from that computer were not available.

The Vulkan alternative still needs a working Vulkan-capable driver and support
for the model's operations. Device enumeration can succeed while a later model
allocation or operation fails. Such errors retain their server explanation;
LocalCode does not repeatedly restart and replay a failed request with different
backends. CPU fallback remains available when device enumeration finds no
compatible GPU. An explicitly selected custom runtime remains the user's choice.

First Vulkan use can be substantially slower while the driver initializes
pipelines. The local 4B check took 37.3 seconds to its first token on first use;
a fresh-process repeat took 0.214 seconds with the same request and settings.
Both measurements are retained. Driver cache behavior on the older laptop is
unverified; this is not credited as a LocalCode optimization.

Automatic placement can put only some layers on the GPU. A model that does not
fit in dedicated memory can still be slower than a smaller installed model.
Context size, model size, cache precision and draft-model choices are separate
tradeoffs; this update does not count reductions in them as engine speed gains.

The startup allocation figure is reported by the runtime, not a live driver
measurement of total VRAM use. Missing layer counts are shown as unconfirmed.
Runtime logs are retained locally for diagnosis; optional wire traces are off
unless `LOCALCODE_TRACE_DIR` is explicitly set.

## Checks on the older laptop

1. Load the same installed GGUF. Confirm the loaded notice shows the chosen
   backend and a positive GPU offload count, or an explicit CPU/unconfirmed
   result. Keep the model-server log if acceleration fails.
2. In a fresh conversation with Auto selected, send `hello how are you`. The
   request should omit coding tools, skills and repository guidance. Record the
   prompt token count, prompt time/rate and generation time/rate separately.
3. Try an already-installed 1.5B–4B model and the 4K or 8K context preset if
   needed. A manual context change changes the workload; compare like settings
   when attributing a performance difference to the engine.
4. Switch to Code and check a small existing project task. Confirm file tools,
   command approvals, verification, cancellation and unload still work.

AMD/Intel GPU and Apple inference performance were not established on this
machine. Compilation or simulated device tests do not substitute for inference
measurements on those devices. No universal performance percentage is claimed.
