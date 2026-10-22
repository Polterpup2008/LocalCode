# Contributing to LocalCode

LocalCode's source is visible under a [proprietary license](LICENSE). You may
inspect it and build locally for your own permitted use. Redistribution, resale
and reuse in other products require permission. Earlier MIT grants remain in
effect for the versions to which they applied; third-party components retain
their own terms.

Useful contributions include reproducible bug reports, clearer setup guidance,
model/runtime compatibility reports and focused fixes. Check the
[known limitations](docs/KNOWN_LIMITATIONS_1.0.md) and existing
[issues](https://github.com/Polterpup2008/LocalCode/issues) before opening a new one.
For a larger change, describe the problem and proposed scope in an issue first.
Please discuss code contributions with the maintainer before submitting a pull
request so contribution rights can be agreed explicitly. Public source
visibility does not grant an open-source license.

## Build on Windows

Install Visual Studio 2022 with the C++ desktop workload, CMake 3.24 or newer,
and Git. The desktop build also needs Qt 6.5 or newer with its MSVC x64 kit
(RC2 used Qt 6.9.1). Configuration downloads the declared source dependencies,
so the first configure needs network access.

From PowerShell at the repository root, import the MSVC environment:

```powershell
. .\scripts\msvc-env.ps1
```

Build and test the core without Qt or a model server:

```powershell
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOCALCODE_BUILD_GUI=OFF -DLOCALCODE_BUILD_TESTS=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

For the desktop app and GUI tests, replace the example Qt path with your kit:

```powershell
cmake -S . -B build/desktop -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.9.1/msvc2022_64" -DLOCALCODE_BUILD_GUI=ON -DLOCALCODE_BUILD_TESTS=ON
cmake --build build/desktop
ctest --test-dir build/desktop --output-on-failure
```

The app is `build/desktop/bin/LocalCode.exe`. GUI tests use native Windows
widgets and need an interactive desktop. Model-backed and toolchain-dependent
checks may require additional setup or skip when their prerequisites are absent;
report skips along with passes. The downloaded release's runtime bundle is
separate from a normal source build: select a compatible custom `llama-server.exe`
in **Settings > Models**, or prepare a runtime package using the packaging scripts.
GGUF model weights and project-specific build tools are supplied separately.

## Keep changes focused

- Keep `localcode_core` independent of Qt. Desktop presentation belongs in
  `src/gui`; shared behaviour belongs in the core.
- Preserve explicit failures and capability checks. Do not invent model
  reasoning features or silently emulate unsupported capabilities.
- Keep tool output bounded and preserve evidence needed to assess the result.
- Treat path checks, command classification and approvals as boundaries that
  need explicit review. State any change to what an operation permits.
- Match nearby C++23 style: four-space indentation, 100 columns, `PascalCase`
  types, `snake_case` core functions and Qt-style `camelCase` in GUI code.
- Avoid unrelated formatting changes and justify new dependencies.

Add a regression test when it meaningfully demonstrates a bug or boundary.
Core tests live in `tests/` and use fixtures such as `MockBackend` for repeatable
model turns. GUI tests cover widgets, settings, persistence and workflows;
they are not limited to the core. Exercise the changed behaviour and error paths
without relying on a real model when a deterministic fixture will suffice.

## Submit a pull request

Explain the concrete problem, the resulting behaviour and the commands you ran
to verify it. Include relevant failures, skips and checks you could not run.
For visible UI changes, include an image that shows the result. One focused
concern per pull request makes review easier.

## Report a bug or compatibility issue

Use an [issue template](https://github.com/Polterpup2008/LocalCode/issues/new/choose).
Include the LocalCode version or commit, Windows version, reproduction steps,
model name and quantization, runtime build and the relevant command output.
`localcode-doctor backends` helps identify the selected runtime.

Logs are normally under `%LOCALAPPDATA%\LocalCode\logs`; **Settings > General**
shows the actual path. Attach only the relevant excerpts and **remove tokens,
private source code, conversation content and identifying paths** before posting.
Report security issues privately as described in [SECURITY.md](SECURITY.md).
