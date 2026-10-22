---
id: concurrency-bugs
name: Diagnose a race, a deadlock or a hang
description: How to find a threading bug from the evidence, given that it will not reproduce on demand and adding a print can hide it.
tags: [debugging, threads, concurrency, deadlock, cpp]
triggers: [race, deadlock, hang, freeze, thread, mutex, intermittent, flaky, sometimes fails]
priority: 78
---

# Diagnose a race, a deadlock or a hang

Concurrency bugs break the normal debugging loop: they do not reproduce
reliably, and the act of observing them changes the timing that causes them.
Reason about the code, then confirm with a tool.

## When this applies

Something works most of the time; fails under load; fails only in release; hangs
with no CPU use; or a test is flaky.

## Tell the three apart first

They have different signatures and different fixes.

- **Deadlock** - the process is alive, using no CPU, and never finishes. Two
  threads each hold what the other wants.
- **Livelock or spin** - alive, using 100% CPU, making no progress.
- **Data race** - wrong results, corrupted state, or a crash in code that looks
  correct. Timing-dependent.

Check CPU usage before anything else; it splits the space in half.

## Deadlock

1. Get a stack for every thread. In Visual Studio, break into the process and
   read the Threads window; with a dump, `~*k` in WinDbg.
2. Find the threads blocked in a lock acquisition. The cycle is usually visible
   immediately.
3. The fix is almost always **lock ordering**: establish a global order in which
   locks may be taken and follow it everywhere. Document it in a comment.

The most common self-inflicted case in a codebase like this one is a method that
takes a lock and then calls another method that takes the same non-recursive
lock. Look for that before anything exotic.

## Data race

1. **Identify the shared state.** Every mutable object touched by more than one
   thread is a candidate; everything else is not.
2. **For each, name the thing that protects it** - a mutex, an atomic, a
   happens-before edge from thread creation or join. If you cannot name one,
   that is the bug.
3. Confirm with a sanitizer where the toolchain has one: `-fsanitize=thread`
   with Clang or GCC. MSVC has no TSan; use `/fsanitize=address` for the memory
   consequences and rely on reasoning for the ordering.

`volatile` is not a synchronisation primitive in C++ and never was. Neither is
"it is only an int".

## Hang with no deadlock

A thread waiting on a condition variable that is never signalled, or a queue
that nobody drains. Check that every `wait` has a matching `notify` on **every**
path out of the producer, including the error and cancellation paths.

## Procedure

1. Record what actually happened - exit code, CPU use, which test, how often.
2. Classify: deadlock, spin, or wrong data.
3. Enumerate the shared state and its protection.
4. Reproduce under a sanitizer if one exists for this toolchain.
5. Fix the ordering or the missing synchronisation, not the symptom.
6. Run the failing case many times; a single pass proves nothing here.

## Failure modes

- **Adding a sleep until it stops failing.** The bug is still there and now it
  is slower.
- **Widening a lock until the symptom goes.** You may have serialised the whole
  program and hidden the real race.
- **Concluding it is fixed after one green run.** Run it in a loop; state how
  many iterations passed.
- **Adding prints and declaring it fixed because it stopped.** Output is
  synchronised and changes the timing.
