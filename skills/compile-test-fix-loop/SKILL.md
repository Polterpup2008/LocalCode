---
id: compile-test-fix-loop
name: The compile-fix loop
description: How to work through a list of compiler errors efficiently without thrashing or introducing new ones.
tags: [build, compiler, errors, cpp, iteration]
triggers: [build error, compile error, does not compile, build fails, compilation failed, fix the build]
tools: [run_command, read_file_range, replace_in_file, search_text]
priority: 85
---

# The compile-fix loop

Compiler errors cascade. Fixing them in the wrong order wastes most of the work.

## When this applies

A build fails and you have a list of diagnostics to work through.

## Procedure

1. **Build and read the whole output**, not just the first line. LocalCode
   extracts the diagnostics for you; read all of them before editing anything.

2. **Fix the FIRST error first.** In C and C++ especially, one missing
   semicolon or one unclosed brace generates dozens of downstream errors that
   vanish on their own. Never start from the bottom of the list.

3. **Read the actual source before editing.** Use `read_file_range` around the
   line the compiler named. The mistake is often on the line *before* the one
   reported - a missing semicolon shows up on the next statement.

4. **Understand the error before changing anything.**
   - `undeclared identifier` - a missing `#include`, a typo, or a namespace
     problem. Use `search_text` to find where the symbol is actually declared.
   - `no matching function` - read the candidate list the compiler prints; the
     mismatch is usually one argument's type or constness.
   - `unresolved external symbol` / `undefined reference` - this is a *linker*
     error, not a compile error. The declaration was found, the definition was
     not. Either the .cpp is not in the build, or a library is not linked.
   - `incomplete type` - a forward declaration where a full definition is needed.

5. **Rebuild after each logical fix**, not after each edit. The error count is
   your progress signal.

6. **Watch that count.** If it is not going down, stop editing and re-read.
   Something in your model of the code is wrong.

## Failure modes

- **Fixing symptoms.** Adding a cast to silence a type error usually hides the
  real problem and creates a runtime bug instead.
- **Fixing errors in the wrong file.** The error is reported where the compiler
  noticed it, which is not always where the mistake is - especially with
  templates and macros.
- **Guessing at includes.** Search for where the symbol is declared and include
  that specific header rather than adding headers speculatively.
- **Declaring success at "fewer errors".** Fewer is not zero. Say which
  remain.
