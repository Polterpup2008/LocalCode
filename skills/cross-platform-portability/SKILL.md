---
id: cross-platform-portability
name: Write code that works on the other platform too
description: The differences that actually break builds when code written on Windows is compiled on Linux or macOS, and how to avoid them without ifdef sprawl.
tags: [portability, cpp, cmake, windows, linux]
triggers: [portable, cross-platform, linux, macos, posix, ifdef, path separator]
priority: 66
---

# Write code that works on the other platform too

Most portability failures are a handful of recurring mistakes, and nearly all of
them are found by compiling - which is why untested platform code should be
described as untested.

## When this applies

The project targets more than one platform, has a CI job for another one, or
contains `#ifdef _WIN32`.

## The differences that actually break things

**Paths.** Use `std::filesystem::path` and `/` as the separator - Windows
accepts forward slashes everywhere. Never concatenate strings with `\\`. Case
sensitivity is real: `Widget.h` and `widget.h` are the same file on Windows and
two files on Linux, so an `#include` with the wrong case builds on one and fails
on the other.

**Line endings.** Read files in binary and handle `\r\n` yourself, or accept
that text mode differs. A parser that splits on `\n` and leaves `\r` attached
produces values with an invisible trailing character - a bug that appears only
when the file came from the other platform.

**Types.** `long` is 32-bit on Windows and 64-bit on Linux. Use fixed-width
types from `<cstdint>`. `size_t` is not `unsigned long`. Format specifiers
differ; prefer streams or `std::format`.

**Headers.** `<windows.h>` has no counterpart; the POSIX equivalents live in
`<unistd.h>`, `<sys/stat.h>`, `<fcntl.h>`. Keep platform code behind a narrow
interface in one file rather than sprinkling `#ifdef` through the logic.

**Macros.** `<windows.h>` defines `min`, `max`, `near`, `far` and more. Define
`NOMINMAX` and `WIN32_LEAN_AND_MEAN` before including it.

**Case-insensitive comparison.** `stricmp` is Windows, `strcasecmp` is POSIX.
Write your own or use one already in the project.

## In CMake

Prefer target properties over global flags, and guard platform bits explicitly:

```cmake
if(WIN32)
    target_sources(app PRIVATE src/platform/Win32Thing.cpp)
else()
    target_sources(app PRIVATE src/platform/PosixThing.cpp)
endif()
```

Do not assume MSVC flags work elsewhere - `/W4` is `-Wall -Wextra` on GCC and
Clang. Use generator expressions or `if(MSVC)`.

## Honesty about what was tested

If you write or change a POSIX path on a Windows machine, **it has not been
compiled**. Say so. "The Linux path is written but unverified" is useful; "added
Linux support" is a claim you cannot back.

## Failure modes

- **Assuming an `#include` case is fine because it compiled.** It compiled on
  the case-insensitive filesystem.
- **`#ifdef` inside a function body**, repeated in twenty places, instead of two
  implementations behind one interface.
- **Hardcoding `C:\` or `/tmp`.** Ask the platform for its temp directory.
- **Claiming cross-platform support from a single-platform build.**
