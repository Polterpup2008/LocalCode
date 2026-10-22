---
id: git-safe-workflow
name: Use git without destroying work
description: Which git operations are safe to run unprompted, which need a human, and how to inspect changes without risking someone's uncommitted work.
tags: [git, safety, version-control]
triggers: [git, commit, branch, stash, reset, checkout, discard, revert, uncommitted, staged]
tools: [git_status, git_diff, git_log, run_command]
priority: 85
---

# Use git without destroying work

The user may have hours of uncommitted work in the tree. Nothing you do should
be able to lose it.

## Safe to run unprompted

`git_status`, `git_diff`, `git_log`, `git show`, `git branch --list`,
`git rev-parse`. All read-only.

## Requires the user's approval

LocalCode will ask before these, and it is right to:

- `git reset --hard` - discards every uncommitted change, unrecoverably
- `git clean -fd` - deletes untracked files, which git cannot restore
- `git checkout -- <path>` / `git restore` - discards that file's changes
- `git push`, and `--force` above all - publishes or rewrites shared history
- `git rebase`, `git commit --amend` - rewrites history
- `git stash` - it is recoverable, but it makes work vanish from the tree

Never argue with a refusal. Find another route.

## Procedure

1. **Check the state before changing anything.** `git_status` at the start of a
   task tells you which files were already modified. LocalCode records this and
   will warn you before you overwrite one of them.

2. **Treat a dirty tree as a constraint.** If a file you need to change already
   has uncommitted changes, prefer a targeted `replace_in_file` over rewriting
   it, and say in your final message that you edited a file that had local
   modifications.

3. **Review with `git_diff` before finishing.** Use `since_task_start: true` to
   see everything that changed during this task, including staged changes.

4. **Do not commit unless asked.** The user decides what enters their history
   and what the message says.

5. **If you need a clean tree, ask for one.** Explain why; do not create it.

## Failure modes

- **`git checkout .` to "start fresh".** This is the single most destructive
  common git command. It silently deletes uncommitted work.
- **Committing to be helpful.** Unasked-for commits are noise in someone's
  history.
- **Assuming untracked means unimportant.** A new file the user has been
  writing is untracked.
- **`git stash` as a workaround.** The work disappears from the working tree
  and the user does not know where it went.
