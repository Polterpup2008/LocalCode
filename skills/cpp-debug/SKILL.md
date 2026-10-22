---
id: cpp-debug
name: Debug a C++ failure
description: How to find the cause of a crash, a wrong result or a hang in C++ from the evidence available.
tags: [cpp, debugging, crash, runtime]
triggers: [crash, segfault, segmentation fault, access violation, hangs, freezes, wrong output, assertion, stack trace]
tools: [run_command, read_file_range, search_text, git_log]
priority: 85
---

# Debug a C++ failure

## When this applies

The code builds but does the wrong thing at runtime: crashes, hangs, or produces
an incorrect result.

## Procedure

1. **Reproduce it first.** Run the failing program or test and capture the real
   failure. A bug you cannot reproduce is one you cannot verify you have fixed.

2. **Read the failure precisely.** An access violation at `0x0` is a null
   dereference. A small address such as `0x8` is a null object plus a member
   offset - so the object is null and the question is why. A large garbage
   address suggests a dangling pointer or a corrupted object.

3. **Get a stack trace.** A debug build with symbols usually names the failing
   line directly. `-DCMAKE_BUILD_TYPE=Debug` is worth the rebuild.

4. **Narrow before theorising.** Which input triggers it? Does a smaller case
   still fail? Did it work before - check `git_log` on the file.

5. **Check the usual causes,** roughly in order of likelihood:
   - use of an object after it was moved from or destroyed
   - a dangling pointer or reference into a container that reallocated
   - an off-by-one in an index or size calculation
   - uninitialised members, especially raw pointers and bools
   - iterator invalidation after insert or erase
   - a signed/unsigned comparison that wraps
   - a missing virtual destructor on a polymorphic base

6. **Use the sanitizers** when the cause is not obvious. They find in seconds
   what reading finds in hours:

   ```
   cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g"
   ```

   MSVC supports `/fsanitize=address`.

7. **Fix the cause, not the symptom**, then re-run the reproduction.

## Failure modes

- **Reading code for the bug without reproducing it.** You will find something
  that looks wrong and fix the wrong thing.
- **Adding null checks everywhere.** This turns a loud crash into a quiet wrong
  answer.
- **Trusting a release build's line numbers.** Inlining moves them.
- **Assuming the crash site is the bug site.** With memory corruption the crash
  happens long after the damage - which is exactly when a sanitizer pays off.
