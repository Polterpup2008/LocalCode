---
id: windows-native-development
name: Native development on Windows
description: The toolchain facts that make MSVC, the Windows SDK and Windows paths behave, and the mistakes that waste the most time.
tags: [windows, msvc, toolchain, paths]
triggers: [msvc, visual studio, windows sdk, vcvars, cl.exe, windows path, long path, dll not found, link error]
tools: [environment_info, run_command, find_executable]
priority: 75
---

# Native development on Windows

## When this applies

Building native code on Windows, or debugging something that only goes wrong
there.

## The facts that matter

**The compiler needs its environment.** `cl.exe` does not work from a plain
shell: it needs INCLUDE, LIB and PATH set by `vcvars64.bat`. Either run the
build from a developer prompt, or use the Visual Studio CMake generator, which
handles it. `environment_info` reports where `vcvars64.bat` is.

**Toolset and SDK are separate.** The MSVC toolset (`14.4x`) supplies the
compiler and the C++ standard library; the Windows SDK supplies `windows.h` and
the system libraries. A missing `windows.h` is an SDK problem, not a compiler
problem.

**Paths.**
- `MAX_PATH` is 260 characters unless long paths are enabled. Deep build trees
  hit this, and the error is usually a confusing "cannot open file".
- Paths are case-insensitive but case-preserving. Do not rely on case.
- Backslash is the separator, but every API and CMake accept forward slashes.
  Prefer forward slashes in build files.
- Spaces in paths are normal (`C:\Program Files\...`). Always quote.

**Runtime library mismatch** causes link errors that look like duplicate or
missing symbols in the CRT. Every object and every library in one binary must
agree on `/MD` vs `/MT` and on debug vs release.

**A missing DLL at startup** produces a dialog or exit code `0xC0000135`, not a
link error. The DLL must sit next to the .exe or be on PATH.

**`NOMINMAX` and `WIN32_LEAN_AND_MEAN`** are worth defining. Without the first,
`windows.h` defines `min` and `max` as macros and breaks `std::min`.

## Procedure

1. Run `environment_info` and read what is actually installed.
2. Choose a generator that matches the environment you have.
3. Build, and read the first error properly - CRT and SDK errors are distinctive.
4. For a runtime failure, check the exit code before assuming a crash.

## Failure modes

- **Running `cl` directly without the developer environment.** It will not find
  its own headers.
- **Mixing configurations.** Debug and release CRTs in one binary will not link.
- **Assuming forward slashes break.** They do not, in CMake or the Win32 API.
- **Ignoring exit code `0xC0000135`.** It means a missing DLL, not a bug in
  your code.
