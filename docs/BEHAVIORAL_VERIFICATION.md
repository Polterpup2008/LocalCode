# Behavioural verification

How LocalCode decides that a program does what was asked, as distinct from
deciding that it built and ran.

For the stage model this sits inside, see
[VERIFICATION_MODEL.md](VERIFICATION_MODEL.md).

## The problem in one line

Exit code 0 means the program did not crash. There is no exit code for
"the answer was right".

## Six ways a behaviour can be established

Only the first two are implemented as first-class evidence today. The rest are
listed because the design deliberately admits them and because saying which
exist and which do not is more useful than a roadmap that reads like a feature
list.

| | Source | Status |
|---|---|---|
| A | The project's own tests pass | implemented — `Stage::Test` |
| B | A stated expectation is compared against real output | implemented — `expect` on `run_command` |
| C | A test written during the run, then run | works today, arrives as A |
| D | A property holds over generated input | not implemented |
| E | Output matches a reference implementation | not implemented |
| F | A human confirms it | not implemented — deliberately, see below |

### A. The project's own tests

`ctest`, `pytest`, `cargo test`, `go test`, a produced binary whose name
contains "test". A passing run sets `Stage::Test`, which the completion gate
accepts in place of an explicit output check.

The trap here is a suite that passes because there is nothing in it. `ctest`
in a directory with no tests exits 0. That is recorded as `not applicable`
with the reason attached, never as a pass — the same lie in a different place.

### B. A stated expectation

The expectation is part of the call that runs the program, so it is fixed
before the output exists:

```json
{
  "command": ["build/stats"],
  "expect": {
    "description": "prints the mean, median and population standard deviation",
    "stdout_equals": "mean: 5 median: 4.5 stddev: 2",
    "criterion": 3
  }
}
```

Available comparisons:

| Field | Meaning |
|---|---|
| `exit_code` | the exit code the command must produce |
| `stdout_contains` | text that must appear (list, or a single string) |
| `stdout_absent` | text that must not appear |
| `stdout_equals` | the whole output, whitespace collapsed |
| `stdout_matches` | an ECMAScript regular expression |
| `check_stderr` | check stderr instead of stdout |
| `criterion` | which acceptance criterion this settles, by number |

Rules that make it worth having:

- **LocalCode performs the comparison.** The model states what the output has
  to be; it does not report whether it was.
- **Whitespace is collapsed** on both sides, so `0 1 1 2 3` matches output
  printed one number per line. Formatting does not decide correctness.
- **Both sides appear in the failure.** Expected and actual, side by side, so
  the discrepancy can be seen rather than summarised.
- **An expectation that checks nothing is refused.** No comparison fields
  means the call fails rather than recording a check that checked nothing.
- **A description is required.** A verdict nobody can audit is not a verdict.
- **A malformed regex is an error, not a silent non-match.**
- **Parsed before the command runs**, so a mistyped expectation is reported
  while there is still time to state it properly.

### F. Why not "ask the user"

A confirmation prompt is easy to add and produces a green tick, which is what
makes it dangerous. A user asked "did that work?" at the end of a long run
answers from the transcript — which is exactly the artefact the run controls.
It would launder the model's own account of events into evidence.

If it is added, it must record what the user was shown at the moment they
answered, and it must be a distinct kind of evidence from a machine check, not
a substitute for one.

## What this does not establish

**A wrong expectation, met.** A model that expects the wrong output and gets it
records a satisfied behaviour check. The check is real; the expectation was
not. What the design buys is that the expectation is written down, shown in the
verification panel and in the report, and can be read by a person who knows the
right answer.

That is a smaller claim than "LocalCode knows the program is correct", and it
is the true one.

**A partial expectation.** `stdout_contains: ["mean:"]` passes on
`mean: 5.71429`. The check is honest about what it checked, and the report
prints the comparison it made, so an expectation that proves less than it
appears to is visible rather than hidden.

**Anything about code that was not run.** Behaviour is established for the
build that ran. Edit a file afterwards and the stage goes stale, along with
everything else established against that revision.

## Green signals that are not evidence

Two things defeat every stage in the model, because both produce genuinely
green results.

**A stub.** `double median(std::vector<double>) { return 0.0; }  // TODO` is
working code by every mechanical measure. It compiles, links, runs, and turns
the whole table green. Only reading it shows nothing was implemented.

**A weakened test.** Disabling a test, deleting its assertions, or editing the
expected value to match the output turns a red signal green without touching
the defect.

Both are detected from the text of an edit as it is written, and reported at
that moment. See `src/core/tools/CodeIntegrity.h` for what is matched and why
each finding carries a confidence rather than a verdict.

## Worked example

The Run 9 failure, and what happens to it now.

```
step 1   set_acceptance_criteria
           1. the project builds with no errors
           2. running it prints the first ten Fibonacci numbers

step 2   write_file src/fib.cpp
step 3   run_command  cmake -B build            -> configure PASS  [E001]
step 4   run_command  cmake --build build       -> build PASS      [E002]

step 5   run_command  build/fib.exe
           expect: { description: "prints the first ten Fibonacci numbers",
                     stdout_equals: "0 1 1 2 3 5 8 13 21 34",
                     criterion: 2 }

           Behaviour check: prints the first ten Fibonacci numbers
             FAIL  stdout does not match
                   expected: 0 1 1 2 3 5 8 13 21 34
                   actual:   1 3 6 12 24 48 96 192 384 768
             -> the output is NOT what the task requires.

         execution PASS  [E003]     the program did run
         behaviour FAIL  [E004]     what it printed was wrong
         criterion 2 settled, not satisfied
```

The model cannot report this finished. The gate names the failure, it fixes the
recurrence, rebuilds — which makes `build` stale until it does — and runs the
check again.

Had it skipped step 5 entirely, the gate would have said so in as many words:
*the program was run but its output was never checked against what the task
requires — running is not the same as being right.*
