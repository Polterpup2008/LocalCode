# Rejected CUDA experiment

`rejected-ada-two-row.patch` targets llama.cpp b11829 / 184f6536024668e8908ab8c86033d56708e62b40.
It is retained for inspection. New LocalCode additions use the project proprietary LICENSE;
original upstream portions retain the MIT terms in ../LICENSE-llama.cpp.
It passed correctness but did not demonstrate the required 5% speed gain on the
RTX 4060 Laptop GPU. It is excluded from the default runtime and build script.
See ../../../docs/benchmarks/2026-10-23-runtime.md for the measured results.
