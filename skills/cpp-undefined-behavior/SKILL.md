---
id: cpp-undefined-behavior
name: Find undefined behaviour
description: How to recognise and confirm undefined behaviour in C++, especially bugs that appear only in release builds or on one machine.
tags: [cpp, undefined-behavior, sanitizer, correctness]
triggers: [undefined behavior, undefined behaviour, works in debug, only in release, heisenbug, random crash, uninitialized, data race]
tools: [run_command, read_file_range, search_text]
priority: 80
---

# Find undefined behaviour

## When this applies

Symptoms that make no sense: works in Debug but not Release, works on one
machine only, changes when you add a print statement, or "fixes itself" when
you change something unrelated.

## Why those symptoms point here

Undefined behaviour lets the optimiser assume the offending case never happens.
Debug builds do not optimise, so the bug hides. That asymmetry *is* the
diagnostic signal.

## Procedure

1. **Run under UBSan and ASan.** This is the first step, not the last:

   ```
   cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
     -DCMAKE_CXX_FLAGS="-fsanitize=undefined,address -fno-omit-frame-pointer -g"
   ```

   Then run the failing test under it. The report names the file and line.

2. **If sanitizers are unavailable** (MSVC has ASan but not UBSan), raise the
   warning level - `/W4 /permissive-` or `-Wall -Wextra` - and read every
   warning at the suspicious site. Compilers warn about much of this.

3. **Look for the common forms:**
   - reading an uninitialised variable
   - signed integer overflow (unsigned wraps legally, signed does not)
   - shifting by more than the width of the type, or by a negative amount
   - out-of-bounds array or `std::vector` access via `operator[]`
   - dereferencing a null or dangling pointer
   - a strict-aliasing violation from casting between unrelated pointer types
   - `std::memcpy` into a type that is not trivially copyable
   - unsequenced modification: `i = i++ + 1`
   - a data race - two threads, one write, no synchronisation
   - returning a reference or pointer to a local

4. **Confirm before fixing.** A sanitizer report or a warning is confirmation.
   Suspicion is not - fixing a theory leaves the real bug in place.

5. **Verify with the sanitizer build again**, then with the normal build.

## Failure modes

- **Concluding "compiler bug".** It is not. It is almost never the compiler.
- **Fixing by rearranging code until it works.** That hides the bug for exactly
  as long as nobody touches it again.
- **Assuming a debug build proves correctness.** Debug builds zero memory and
  skip optimisation; both hide real defects.
