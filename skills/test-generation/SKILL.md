---
id: test-generation
name: Write a useful test
description: How to add a test that actually catches the bug and fits the project's existing suite.
tags: [testing, quality, coverage]
triggers: [write a test, add tests, unit test, test coverage, test for this, reproduce with a test]
tools: [read_file, search_files, write_file, run_command]
priority: 70
---

# Write a useful test

## When this applies

You are asked for tests, or you fixed a bug that no existing test would have
caught.

## Procedure

1. **Read the existing tests first.** `search_files` for the test directory and
   read two or three. Match the framework, the file naming, the assertion
   style and the registration mechanism exactly. A test in a different style is
   friction for everybody afterwards.

2. **Check how tests are run and registered.** In CMake, a new test file
   usually needs adding to a target or picked up by a glob; find out which
   before writing code that never runs.

3. **Write the failing case first** when fixing a bug. Run it and watch it
   fail for the right reason. A test that passes before the fix tests nothing.

4. **Test behaviour, not implementation.** Assert on what the function
   promises. A test that asserts on private internals breaks on every refactor
   and catches nothing.

5. **Cover the edges that actually break:** empty input, a single element, the
   maximum, zero, negative, null, duplicate keys, non-ASCII text, and the error
   path.

6. **One reason to fail per test.** When a test with six assertions fails, the
   name should still tell you what is wrong.

7. **Run the whole suite**, not just your new test - to confirm you have not
   broken a neighbour.

## Failure modes

- **Tests that cannot fail.** Assert something specific; `assert(result)` on a
  non-empty container proves very little.
- **Depending on order.** Tests must pass when run individually and in any
  order.
- **Depending on the environment.** Absolute paths, the clock, the network, or
  a file left behind by another test make a suite that fails on somebody
  else's machine.
- **Adding a test framework the project does not use.** Use what is there.
