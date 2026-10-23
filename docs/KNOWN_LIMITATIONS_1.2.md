# LocalCode 1.2 — scope and limitations

The [1.1 hardware limitations](KNOWN_LIMITATIONS_1.1.md) and
[command-execution boundaries](KNOWN_LIMITATIONS_1.0.md) still apply.
The Windows application is unsigned.

Discarding incomplete tool calls prevents a broken request from poisoning
later turns. It cannot guarantee that every model will produce valid tool
arguments or complete a large program. After two recovery retries, LocalCode
stops with guidance to try a smaller first step or adjust the reply-token
limit. Files from previously successful batches remain; only the rejected
batch is guaranteed not to have run.

The reply limit includes reasoning and generated tool arguments. Raising it
permits longer generation and may take longer. It does not make decoding faster
or enlarge the context window. Saved profile changes take effect after Apply
and model reload. The app does not automatically raise your output budget.

Old malformed calls are filtered only from outgoing request history; the saved
transcript is preserved. The repair does not remove valid execution receipts or
assert that an unfinished task succeeded.

Runtime measurements on the development laptop do not establish performance on
AMD, Intel, Apple or the reported GTX 1650 Ti system. A runtime cache experiment
is described with its admission decision in the
[measurement report](benchmarks/2026-10-23-tool-recovery.md). No universal speed
percentage is claimed.
