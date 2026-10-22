---
id: code-review
name: Review a change
description: How to review a diff for correctness, safety and fit before it is accepted.
tags: [review, quality, correctness]
triggers: [review, look over, check my code, pull request, is this correct, feedback on]
tools: [git_diff, read_file, search_text, run_command]
priority: 70
---

# Review a change

## When this applies

The user asks you to review code, or you are checking your own change before
reporting it done.

## Procedure

1. **Read the diff in full first**, without commenting. Understand what the
   change is trying to do before judging how it does it.

2. **Check correctness** before anything else:
   - Are the boundary cases handled - empty, one element, maximum size?
   - What happens on the error path? Is a failure return checked?
   - Are there new lifetime problems - a reference outliving its owner?
   - Does the change hold for concurrent callers, if any exist?
   - Does it actually fix the reported problem, or only its symptom?

3. **Check the change against its surroundings.** Read the file around each
   hunk, not just the hunk. A change that is correct in isolation can violate an
   invariant established elsewhere in the file.

4. **Look for what is missing**, which is harder than looking at what is there:
   a new error path with no handling, a new field that is not initialised, a new
   branch with no test.

5. **Separate severity.** Distinguish:
   - **Defects** - it is wrong; it will break.
   - **Risks** - it works now but is fragile.
   - **Preferences** - style, naming. Say least about these.

6. **Verify claims by running things** where you can - build it, run the tests.
   A review backed by a build is worth more than one backed by reading.

## How to report

Lead with the defects, each with file and line, the concrete failing case, and
the smallest fix. Say plainly when you found nothing serious. Do not pad a
review with preferences to look thorough.

## Failure modes

- **Style commentary crowding out a real defect.** Rank by severity.
- **Reviewing only the added lines.** Deletions break things too.
- **Assuming the tests cover it.** Check whether a test actually exercises the
  changed branch.
- **Vague objections.** "This could be cleaner" is not actionable. Name the
  problem and the fix.
