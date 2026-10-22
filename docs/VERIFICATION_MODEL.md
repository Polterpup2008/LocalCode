# The verification model

## The failure this exists to prevent

A run was asked to write a program that prints the first ten Fibonacci
numbers. It created the project, configured it, built it, ran it to exit
code 0, and reported that the numbers had been printed correctly.

The program had printed:

```
1 3 6 12 24 48 96 192 384 768
```

Nothing in that run was a lie. CMake did configure. The compiler did produce a
binary. The binary did exit 0. Every piece of evidence LocalCode held was true,
and the conclusion drawn from them was false — because none of the evidence was
about the answer.

That is not a model problem you can prompt your way out of. It is a category
error in the evidence system: it treated *an operation succeeded* and *the
user's request is satisfied* as the same kind of fact.

## Stages

Verification is modelled as stages, with a direction of dependency:

```
SOURCE → CONFIGURE → BUILD → EXECUTION → BEHAVIOUR → OBJECTIVE
                       └───→ TEST ───────────────────┘
```

| Stage | What a pass means |
|---|---|
| `source` | not a status — the current revision of the tree |
| `configure` | a build system exists and accepted the project |
| `build` | it compiled and linked |
| `tests` | the project's own tests ran and passed |
| `execution` | the produced program ran |
| `behaviour` | what it produced was checked against what the task requires |
| `objective` | every acceptance criterion is settled |

Each stage is one of `not established`, `pass`, `fail`, `stale` or
`not applicable`.

`not applicable` is there for honesty in the other direction. "Make this
compile" has no behaviour to check, and reporting behaviour as "unknown"
forever would be pedantry. But it has to be *stated* rather than assumed, so
somebody has decided it rather than nobody having looked.

## The two rules

### 1. A stage is only as current as the source it was established against

Every stage records the revision it passed at. Writing a file bumps the
revision. A pass established against an earlier revision resolves to `stale` —
not to `pass`, and not to `unknown`.

```
build       stale (passed at r0, source is now r1)
```

The history is not erased: the build did pass, at r0. What is refused is the
inference that it therefore says something about r1.

A **failure** does not go stale. A stale pass means "we no longer know"; a
failure stays a failure until something establishes otherwise.

### 2. A lower stage never implies a higher one

Building is not running. Running is not behaving. Behaving is not the
objective. Nothing in the system reads an exit code and concludes that the
answer was right, because an exit code does not carry that information.

The Fibonacci run, expressed in these terms:

```
configure   pass
build       pass
execution   pass
behaviour   not established
objective   not established
```

Every true thing is still recorded as true. The false conclusion is simply not
available.

## Evidence

Every stage transition names a piece of evidence, and evidence has an identity:

```
E004 tests_run behaviour check while running: build\fib.exe
     - expected 0 1 1 2 3 5 8 13 21 34, actual 1 3 6 12 24 48 96 192 384 768
     [failed]  @r2
```

That makes a claim checkable against something other than the model's
confidence. `behaviour: FAIL [E004]` can be followed to the comparison that
produced it, and the comparison carries both sides.

## Reaching the behaviour stage

There is exactly one route, and it does not involve an exit code.

The expectation is stated **before the command runs**, as part of the call
that runs it, and LocalCode performs the comparison itself:

```json
{
  "command": ["build/fib.exe"],
  "expect": {
    "description": "prints the first ten Fibonacci numbers",
    "stdout_equals": "0 1 1 2 3 5 8 13 21 34",
    "criterion": 2
  }
}
```

The model says what the output has to be. It does not get to say whether it
was. The comparison runs against the captured output, whitespace is collapsed
so line breaks do not decide correctness, and the failure text carries expected
and actual side by side.

`criterion` names the acceptance criterion this settles, **by number**.
Matching a check to a criterion by how similar the two sentences look would be
exactly the kind of plausible inference this subsystem exists to remove, so an
out-of-range number settles nothing rather than settling the nearest thing.

### What this does and does not buy

It does not make the model's expectation true. A model that expects the wrong
output and gets it will record a satisfied behaviour check.

What it buys is that the expectation becomes an explicit, recorded, auditable
claim rather than an unexamined assumption — visible in the transcript, in the
verification panel, and in the run's report, where a person can see that
LocalCode checked for `0 1 1 2 3 5 8 13 21 34` and say whether that was the
right thing to check for. That is a smaller claim than "LocalCode knows the
answer is correct", and it is the honest one.

## Acceptance criteria

`set_acceptance_criteria` makes the agent commit to a definition of done
*before* the work. A definition written afterwards is not a definition; it is a
description of whatever happened.

Once any criterion has been settled, the plan cannot be replaced. A run that
could rewrite its own definition of done once the results were in would have no
definition at all.

`objective` is derived from the plan and the stages, never asserted. It holds
only when there is a plan, every check in it is satisfied, and nothing it
depends on has gone stale.

## The completion gate

When the model reports the task finished, the ledger is consulted. Where they
disagree, the ledger wins.

The gate blocks when:

1. any stage is a `fail` that was never made to pass;
2. `build`, `tests` or `behaviour` is `stale` — files changed after the last
   thing proved about them;
3. the program was run and its output was never checked, and no test suite
   passed either;
4. files were changed and nothing was built, run or tested at all;
5. an acceptance criterion failed, or is still unsettled.

A blocked run is sent back with the specific gap named and instructions for
closing it, at most twice. A model that cannot verify its work will not learn
to on the fifth attempt, and an honest "unverified" beats an endless loop.

### What the gate deliberately does *not* do

**It does not gate a run that changed nothing.** Answering "what does this
function do?" is not a build task, and demanding a build for it would make the
tool useless for the thing it is asked to do most often.

**A missing acceptance plan is not on its own a blocking fault.** A run that
built the project and passed its test suite has proved something real. Failing
it because a bookkeeping tool went uncalled would be ceremony, not rigour — and
it would teach people to distrust the gate, which is worse than not having one.
The absence of criteria counts only when nothing else established behaviour.

### After the gate

If the gap is still open after the retries, the answer stands with an honest
note under it listing what was not established. Whether the run counts as
*completed* then turns on what the model said, not on how much it verified:

- "I changed the return value; I have not built it, so this is unverified" —
  a good answer to an unverified run. Completed, with the note.
- "It builds and all tests pass", on the same evidence — a false claim.
  Not completed.

The difference between those two outcomes is the claim, not the work.

## Code that only looks finished

Two things defeat every stage above, because both produce genuinely green
signals.

A **stub** compiles, links, runs, and turns the whole table green, because a
stub is technically working code. Only reading it reveals nothing was
implemented.

**Test tampering** aims at the check itself: disabling a test, deleting its
assertions, or editing the expected value to match whatever the code produced.
That converts a red signal into a green one without touching the defect.

Both are detected at the moment the file is written, from the text of the edit,
and reported to the model immediately — when it is still cheap to fix. Findings
carry a **confidence** and a **reason**, and are limited to what *this edit*
introduced: a pre-existing `TODO` is the project's business.

Nothing is refused on the strength of them. `return nullptr;` is a stub in one
function and correct in another; a removed assertion is sometimes a test that
was wrong. LocalCode says what it noticed. It does not claim to know intent.

## Where to look

| Concern | File |
|---|---|
| Stages, staleness, evidence, the plan, the gate | `src/core/agent/Verification.h/.cpp` |
| What a tool result proves | `src/core/agent/EvidenceInterpreter.h/.cpp` |
| Stating and checking an expectation | `src/core/tools/OutputExpectation.h/.cpp` |
| Committing to a definition of done | `src/core/tools/VerificationTools.cpp` |
| Stubs and weakened tests | `src/core/tools/CodeIntegrity.h/.cpp` |
| Reading a build log | `src/core/tools/Diagnostics.h/.cpp` |
| What the user sees | `src/gui/widgets/VerificationPanel.cpp` |

Tests: `tests/test_verification.cpp`, `tests/test_evidence_interpreter.cpp`,
`tests/test_output_expectation.cpp`, `tests/test_completion_gate.cpp`,
`tests/test_code_integrity.cpp`, `tests/test_diagnostics.cpp`.
