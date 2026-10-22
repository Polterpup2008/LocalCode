# Experimental runtime builder (developer opt-in)

This source helper prepares a separate llama.cpp checkout at revision
`cb300598d5f90189cb69d2702f4930aaf99d32a2` with the supplied experimental
checkpoint patches. It is separate from the unchanged **llama.cpp.localcode**
runtime included in RC2. No optimization benefit is promised for your models
or hardware. These developer tools are excluded from the default installer.

## Prepare a candidate

Use Python 3.11+, Git, CMake and a suitable C/C++ compiler. Supply an existing
upstream checkout and a new output directory. The helper refuses an existing
output and does not replace the source checkout or installed runtime.

```powershell
python scripts/build-llama-checkpoint-candidate.py `
  --upstream C:/src/llama.cpp `
  --output C:/build/checkpoint-candidate `
  --prepare-only
```

Read `--help` before compiling. Choose backend, CPU instruction set and upstream
CMake arguments for the intended machine. Do not mix candidate libraries with
older common/server consumers; build a coherent runtime and validate it
separately before selecting it in LocalCode.

`--control` prepares the same revision without the sharing patch.
`--variant cpu-dispatch` selects the alternate CPU-dispatch patch.
The preparation manifest records revision, patches and build arguments; it is
provenance, not a successful inference or performance result.

For a developer distribution, `LOCALCODE_INSTALL_DEVELOPER_TOOLS=ON` includes
this helper and its patches. Leave it OFF for normal application packages.
Model weights, compiler tools and compatible hardware are supplied separately.
