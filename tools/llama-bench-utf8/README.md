# UTF-8 benchmark launcher

This optional launcher forwards Windows' original wide command-line arguments
as mutable UTF-8 strings to `llama_bench`. Legacy narrow-argv launchers can corrupt
non-ASCII filenames before llama.cpp receives them. POSIX forwards native argv.
The Windows launcher advertises `LocalCode Windows UTF-8 argv v1` during `--help`.
It changes no inference code, model, sampling, thread count or saved setting.

Build with CMake and `LOCALCODE_BENCH_IMPL_LIBRARY` pointing to the import library
from the **same runtime build**. Use its executable with that runtime's adjacent
`llama-bench-impl` and other libraries. Never mix inference libraries or import
libraries from different llama.cpp builds. Normal CPU dispatch stays in the
runtime, with no CPU model or machine-specific instruction in this launcher.

Pinned b10655 also needs `bench-json-b10655.patch` in its benchmark implementation:
its original JSON encoder mistakes signed UTF-8 bytes for control characters.
Apply this patch after the existing optional repack extension, then rebuild that
implementation with the same runtime's import libraries. The patch changes only
JSON string escaping; all inference libraries remain unchanged. The companion
archive contains both fixes. A launcher alone still loads Unicode files with
the older implementation, but its malformed filename output is rejected by the
desktop's measurement validation.

This does not change llama-bench's comma-separated model-list syntax. A filename
containing a comma remains unsupported by LocalCode's single-model comparison.
