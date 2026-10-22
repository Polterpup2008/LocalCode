---
id: regression-hunting
name: Find what broke it
description: How to locate the change that introduced a regression, using history rather than guesswork.
tags: [git, regression, bisect, history]
triggers: [used to work, regression, worked before, broke, since the update, stopped working, when did this break]
tools: [git_log, git_diff, run_command, read_file]
priority: 75
---

# Find what broke it

## When this applies

Something worked before and does not now. The history knows when.

## Procedure

1. **Establish the failure precisely.** Get a command that fails now and would
   have passed before - a test, a build, a reproduction. Bisecting needs a
   yes/no test, so define it before you start.

2. **Find the last known-good point.** Ask the user if they know. Otherwise
   check out an older commit and run the test.

3. **Look at the history of the relevant files first.** `git_log` with a `path`
   filter is often enough to spot the culprit without bisecting - especially if
   only a handful of commits touched the area.

4. **Bisect when the history is large.** `git bisect` is a confirmation
   command, and LocalCode will ask before it changes your checkout:

   ```
   git bisect start
   git bisect bad HEAD
   git bisect good <known-good-commit>
   ```

   Then for each step: build, run the test, `git bisect good` or
   `git bisect bad`. Finish with `git bisect reset` - do not leave the user's
   repository in a bisect state.

5. **Read the guilty commit** with `git_diff` against its parent. The change is
   usually small and the mechanism obvious once you see it.

6. **Understand why it broke before fixing.** The commit that surfaced the bug
   is not always the commit that is wrong; it may have exposed a latent problem
   elsewhere. Reverting blindly can reintroduce whatever it fixed.

## Failure modes

- **Bisecting with a flaky test.** You will get a random answer. Confirm the
  test is deterministic first.
- **Leaving the repository mid-bisect.** Always `git bisect reset`.
- **Reverting without understanding.** The commit may have been a correct fix
  for something else.
- **Ignoring dependency changes.** A regression can come from an updated
  library rather than the project's own commits - check lockfiles too.
