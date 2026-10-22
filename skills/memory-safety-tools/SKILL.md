---
id: memory-safety-tools
name: Catch memory errors with the tools that exist
description: How to make a leak, an overrun or a use-after-free reproduce and point at its own cause, instead of guessing from a crash address.
tags: [debugging, memory, sanitizers, cpp, msvc]
triggers: [leak, heap, corruption, use after free, buffer overrun, access violation, sanitizer, valgrind, asan]
priority: 76
---

# Catch memory errors with the tools that exist

A memory bug crashes somewhere unrelated to where it was caused. Reading the
crash address rarely helps; making the program fail *at the moment of the
mistake* almost always does.

## When this applies

Access violations, heap corruption, memory that grows without bound, or a crash
whose stack makes no sense.

## Reach for a tool before a theory

**AddressSanitizer** is available in MSVC, Clang and GCC and catches overruns,
use-after-free and double-free at the point they happen.

```
cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="/fsanitize=address"      # MSVC
cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -g"   # Clang/GCC
```

Build and run the failing case under it. ASan output names the operation, the
allocation it belongs to, and both stacks - that is the answer, not a hint.

On MSVC, ASan and `/RTC` are incompatible and edit-and-continue must be off; if
the configure fails, that is usually why.

**UBSan** (`-fsanitize=undefined`, Clang/GCC) catches signed overflow,
misaligned access and bad shifts, which produce corruption that looks like a
memory bug but is not.

**Valgrind** on Linux catches uninitialised reads that ASan misses. Slow, but it
needs no rebuild flags.

## Leaks

Growth is not a leak. Measure before concluding:

1. Record the process's memory over time under a repeating workload.
2. Flat after an initial rise is a cache, not a leak.
3. Monotonic growth proportional to iterations is a leak.

On Windows, the CRT debug heap reports leaks at exit without a special build:
`_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF)`. LeakSanitizer
(bundled with ASan on Clang/GCC) gives allocation stacks, which is more useful.

## If no sanitizer is available

Narrow it by construction rather than by reading the crash:

1. Find the smallest input that reproduces it.
2. Add assertions on invariants - sizes, null-ness, ownership - and run again.
   The first assertion to fire is much closer to the cause than the crash was.
3. Check the obvious shapes: an index compared with `<=` instead of `<`, a
   pointer kept past the lifetime of the container that owned it, a `memcpy`
   sized from the wrong operand, a returned reference to a local.

## Procedure

1. Reproduce the failure once and record the exact command.
2. Configure a sanitizer build in a **separate** build directory.
3. Run the same case under it.
4. Fix what it names, not what you suspected.
5. Re-run under the sanitizer to confirm it is silent, then run the normal suite.

## Failure modes

- **Debugging from the crash address.** Corruption surfaces far from its cause.
- **Adding a null check where it crashed.** That hides a lifetime bug and moves
  the crash somewhere worse.
- **Building the sanitizer into the main build directory.** It changes the ABI
  and layout; keep it separate so the normal build stays usable.
- **Declaring a leak fixed from one run.** Show the memory curve over the same
  workload before and after.
