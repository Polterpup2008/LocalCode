---
id: verify-before-done
name: Verify before claiming done
description: How to establish that a change actually works before reporting it, and how to report honestly when it does not.
tags: [verification, quality, build, test]
triggers: [fix, done, finished, complete, works, verify, check, test it, make it work]
tools: [run_command, git_diff, read_file]
priority: 95
always: true
---

# Verify before claiming done

An unverified fix is a guess. This procedure turns a guess into a fact, or into
an honest statement that it is still a guess.

## When this applies

Always, before the final message of any task that changed code.

## Procedure

1. **Re-read your own diff.** Run `git_diff` (or `git_diff` with
   `since_task_start: true`). Read every hunk. You are looking for:
   - changes you did not intend to make
   - debugging code you forgot to remove
   - a fix applied to the wrong branch of a condition
   - files touched that have nothing to do with the task

2. **Build.** Run the project's real build command with `run_command`. Not a
   syntax check, not "it looks right" - the build.

3. **Read the result.** Exit code 0 is necessary but not sufficient: read the
   warnings too. A new warning at the site you just edited is usually the bug
   you just introduced.

4. **Run the tests.** If the project has tests, run them. If your change is not
   covered by any test and the project has a test suite, consider whether a
   test should exist - but do not add one unless the task calls for it.

5. **Exercise the actual behaviour** when the task was about behaviour rather
   than compilation. If the bug was "the renderer draws nothing", build and run
   the thing, or run the test that reproduces it. A compile is not a
   demonstration that a rendering bug is fixed.

6. **State the verification status explicitly** in your final message.

## What you may claim

| Claim | Requires |
|---|---|
| "It builds" | a build command that exited 0, after your last edit |
| "The tests pass" | a test run that exited 0, after your last edit |
| "The bug is fixed" | a reproduction that now behaves correctly |
| "X is not installed" | an `environment_info` or `find_executable` check |

If you have not done the corresponding thing, do not make the corresponding
claim. Say what you did do:

> I changed the initialisation order in `render.cpp:112`. The project builds
> cleanly. I could not run the tests because this project has none, so the
> behavioural fix is unverified - run the application and check whether the
> geometry appears.

## Failure modes

- **Building before the last edit.** A green build from three edits ago proves
  nothing. Build again after the final change.
- **Testing the wrong target.** `cmake --build build` may build a different
  configuration than the one the failing test uses. Check which.
- **Reading only the exit code.** Some test runners exit 0 while reporting
  failures in their output. Read the summary line.
- **Declaring victory on a partial fix.** If you fixed three of five compiler
  errors, say so; do not report the build as fixed.
