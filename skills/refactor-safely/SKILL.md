---
id: refactor-safely
name: Refactor without changing behaviour
description: How to move, rename or restructure code so that the result provably does the same thing as before.
tags: [editing, refactoring, discipline, tests]
triggers: [refactor, rename, extract, restructure, clean up, reorganise, split, move]
priority: 72
---

# Refactor without changing behaviour

A refactor that also fixes a bug is not a refactor; it is two changes wearing one
coat, and when something breaks nobody can tell which half did it.

## When this applies

Renaming, extracting a function, splitting a file, moving a type, changing a
signature, or reorganising a module - where the intended observable behaviour is
identical afterwards.

## The rule

**Separate the refactor from the change.** Do the behaviour-preserving move
first and confirm the tests still pass. Then make the behavioural change as its
own edit. If asked for both at once, do them in that order and say which was
which.

## Procedure

1. **Establish the baseline.** Run the tests *before* touching anything and note
   the result. Without this you cannot tell whether you broke something or found
   it already broken. If there are no tests covering the area, say so - a
   refactor without a safety net is a bigger risk than it looks.

2. **Find every use.** `search_text` for the symbol, not just the definition.
   Include comments, documentation and build files. For a public name check
   whether tests reference it too.

3. **Do not trust a textual rename.** `Widget` matches `WidgetFactory`,
   `MyWidget` and the word in a comment about something else. Read each hit and
   change it deliberately. `replace_in_file` on a precise, unique surrounding
   string is safer than a blanket substitution.

4. **Move in one direction at a time.** Extract the function first, verify, then
   change its parameters. Combining the two makes the diff unreadable.

5. **Rebuild and re-run the same tests.** Same result as the baseline means the
   move was clean. Anything else means stop and understand it before continuing.

## Signature changes

Changing a parameter type or adding one silently changes overload resolution and
implicit conversions. After such a change, check each call site individually
rather than trusting that it compiled - compiling is not the same as calling the
function you intended.

## What counts as evidence

"It builds" is not evidence a refactor was safe. The evidence is: the same tests
that passed before pass now, and the diff contains nothing that changes control
flow or values.

## Failure modes

- **Sneaking in an improvement.** The one line you also fixed is the one that
  breaks, and it will be looked for last.
- **Renaming through comments and strings without reading them.** User-visible
  text and log messages are not identifiers.
- **Refactoring code with no test coverage and reporting it as verified.** Say
  plainly that it is unverified.
- **Leaving the old name behind in a doc comment.** Search the docs too.
