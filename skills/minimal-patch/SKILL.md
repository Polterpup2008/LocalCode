---
id: minimal-patch
name: Make the smallest correct change
description: How to scope an edit so it fixes the problem without collateral damage to code nobody asked you to touch.
tags: [editing, discipline, review, diff]
triggers: [fix, change, edit, modify, refactor, update, patch]
priority: 75
---

# Make the smallest correct change

Every line you change is a line someone has to review and a line that can break
something. Change as few as the fix requires, and no fewer.

## When this applies

Any time you edit code.

## Procedure

1. **Identify the actual cause** before editing. If you cannot say in one
   sentence why the current code is wrong, you are not ready to change it.

2. **Change only what the cause requires.** Do not reformat surrounding code,
   rename things you dislike, "improve" adjacent logic, add error handling
   nobody asked for, or update unrelated comments. If you notice a genuine
   second problem, mention it in your final message instead of silently fixing
   it.

3. **Prefer `replace_in_file` over `write_file`.** A targeted replacement
   produces a small diff and cannot destroy the rest of the file. Use
   `write_file` for new files.

4. **Match the surrounding style** - indentation, brace placement, naming,
   comment density. Your change should be indistinguishable from the code
   around it. Do not import conventions from another project.

5. **Read your own diff** with `git_diff` before finishing. Every hunk should be
   explainable in terms of the task.

## Judging the size

A good fix for "the renderer crashes on empty geometry" is a guard in the one
place the empty case is unhandled. A bad one restructures the render loop.

If the correct fix genuinely is large - a broken abstraction, a missing concept
- say so before doing it, and explain why the small fix would be wrong.

## Failure modes

- **Rewriting a file to change three lines.** The diff becomes unreviewable and
  you will lose the user's local edits.
- **Fixing unrelated warnings on the way past.** Mention them instead.
- **Reformatting because your usual style differs.** LocalCode preserves
  encoding and line endings deliberately; do not defeat that.
- **Leaving debug output behind.** Check your diff for prints you added.
