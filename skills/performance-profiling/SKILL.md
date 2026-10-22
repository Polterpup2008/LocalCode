---
id: performance-profiling
name: Make it faster, on evidence
description: How to find where time actually goes before optimising anything.
tags: [performance, profiling, optimization]
triggers: [slow, performance, optimize, optimise, speed up, too slow, profiling, bottleneck, latency]
tools: [run_command, read_file, search_text]
priority: 70
---

# Make it faster, on evidence

Intuition about where a program spends its time is wrong far more often than it
is right. Measure first, every time.

## When this applies

Something is slow and the task is to make it faster.

## Procedure

1. **Get a number before touching anything.** A reproducible measurement of the
   slow operation: a benchmark, a timed run, a stopwatch around the phase. If
   you cannot measure it, you cannot know whether you improved it.

2. **Check you are measuring a release build.** Optimising a debug build is
   measuring the wrong program. `-DCMAKE_BUILD_TYPE=RelWithDebInfo` gives both
   optimisation and symbols.

3. **Profile.** Use whatever the platform offers - `perf record` on Linux,
   Visual Studio's profiler or WPA on Windows, or a sampling profiler the
   project already depends on. Failing that, time the phases with explicit
   instrumentation.

4. **Find the actual hot path.** Look for the function with the highest
   *inclusive* time first to find the area, then *exclusive* time to find the
   line. Frequently the cost is not in the algorithm but in allocation,
   copying, or an unexpected O(n^2) from a nested lookup.

5. **Fix the biggest item first**, and only that one. Then measure again. An
   optimisation that is not visible in the measurement is not an optimisation;
   revert it.

6. **Report the before and after numbers.** "Reduced load time from 4.2s to
   0.8s" is a result. "Made it faster" is not.

## Common wins, in rough order of frequency

- an unnecessary copy where a reference or a move would do
- allocation inside a hot loop
- a linear scan that should be a hash lookup
- work repeated per item that could be hoisted out
- an unreserved container growing element by element
- I/O or logging inside the loop

## Failure modes

- **Optimising without measuring.** The usual outcome is uglier code at the
  same speed.
- **Micro-optimising outside the hot path.** Saving a microsecond in code that
  runs once is not progress.
- **Trading correctness for speed** without saying so.
- **Benchmarking once.** Run it several times; note the variance.
