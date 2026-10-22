---
id: linker-errors
name: Resolve a linker error
description: How to read an unresolved-symbol or duplicate-symbol error and fix the actual cause, which is almost never the file the error names.
tags: [build, linker, cpp, msvc, toolchain]
triggers: [unresolved, LNK2019, LNK1120, LNK2005, undefined reference, duplicate symbol, linker, link error]
priority: 80
---

# Resolve a linker error

A compile error tells you where the mistake is. A linker error tells you only
that something promised does not exist, and the file it names is usually the
innocent party asking for it.

## When this applies

The compile succeeded and the failure mentions `LNK` (MSVC), `undefined
reference` (GCC/Clang), duplicate symbols, or an architecture mismatch.

## Read the error first

The symbol is the evidence. Do not skip past it.

- **MSVC** mangles names. `?frobnicate@Widget@@QEAAXH@Z` is
  `public: void __cdecl Widget::frobnicate(int)`. The error usually prints the
  demangled form after the mangled one - read that line.
- The `referenced in function ...` part is **who wanted it**, not what is wrong.
- `unresolved external symbol` means declared but never defined, or defined
  somewhere that was not linked in.

## The four causes, in the order they actually occur

1. **Declared but never defined.** A member function in a header with no body in
   any `.cpp`. Search for the symbol with `search_text`: if the only hit is the
   declaration, that is your answer.

2. **Defined, but that object file is not in the link.** The `.cpp` is missing
   from `add_executable`/`add_library`, or the library is missing from
   `target_link_libraries`. Check the build file before touching any code.

3. **Defined differently than declared.** A `const` mismatch, a different
   parameter type, a missing `inline`, or a definition inside an anonymous
   namespace. The mangled names differ, so the linker sees two unrelated
   symbols. Compare the declaration and definition character by character.

4. **Built with different settings.** Mixing debug and release runtimes, x86
   objects with x64, or different `_ITERATOR_DEBUG_LEVEL` values. MSVC reports
   this as a mismatch detected in the object file - believe it, and rebuild the
   dependency with matching settings rather than forcing the flag.

## Duplicate symbols

`LNK2005` / `multiple definition` means a definition landed in more than one
translation unit. Usual causes:

- A function defined in a header without `inline`.
- A non-`const` global in a header without `extern`.
- The same `.cpp` listed twice in the build, or compiled into two libraries that
  are both linked.

Mark it `inline`, move the definition to one `.cpp`, or declare it `extern` in
the header and define it once.

## Procedure

1. Read the symbol and demangle it mentally.
2. `search_text` for the symbol's plain name across the project.
3. Nothing but a declaration → write the definition.
4. A definition exists → check the build file lists that source or library.
5. Both look right → compare the signatures exactly, including `const`.
6. Still failing → check architecture and runtime settings match.

## Failure modes

- **Adding a stub definition to make it link.** That converts a link error into
  a runtime one. Find why the real definition is missing.
- **Guessing at a library to add.** Confirm the symbol is in it first.
- **Deleting the call site.** You removed the symptom and the feature.
- **Assuming a clean rebuild will fix it.** It fixes stale-object cases only;
  try it once, and if it fails again the cause is structural.
