# LocalCode 1.2 for Windows

LocalCode 1.2 fixes a tool-call failure that could leave a conversation stuck
after the model ran out of reply tokens while writing tool arguments.

- **Recoverable tool calls.** LocalCode checks the whole batch before saving or
  running it. An incomplete or malformed batch runs no tools. Recovery asks for
  one short, complete call and stops after two retries with a readable explanation.
  Real connection and server failures still surface normally.
- **Existing conversations can recover.** Invalid tool arguments saved by older
  versions are omitted when constructing a request, before context budgeting.
  The original saved conversation stays intact. Valid tool calls, their results
  and verification evidence remain available.
- **Reply token limit in Models.** The profile form now exposes the generated
  token allowance, including reasoning and tool arguments. Apply and reload the
  model after changing it. This setting is separate from context capacity;
  existing defaults, context sizes and cache precision are retained.
- **Reachable profile controls.** The model profile form scrolls within the
  inspector, keeping its controls accessible in shorter windows.

The reported failure was reproduced with synthetic history: the same server
returned HTTP 500 before the repair and HTTP 200 afterwards, without a restart.
Regression tests cover incomplete batches, repeated malformed output, old
conversation replay, reused call identifiers and context budgeting.

The bundled Windows runtime retains the qualified working-set optimization and
the CUDA, Vulkan and CPU choices introduced in 1.1. See the
[1.2 runtime experiment and reliability results](benchmarks/2026-10-23-tool-recovery.md)
and [known limitations](KNOWN_LIMITATIONS_1.2.md).

The optional Discord installer component and proprietary LocalCode license
remain. Third-party components retain their own licenses. Models, chats and
development outputs are excluded from the distribution.
