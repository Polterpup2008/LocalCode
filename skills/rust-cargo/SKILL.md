---
id: rust-cargo
name: Work in a Rust project
description: How to build, test and read errors in a Cargo project, where the compiler usually tells you the fix if you read to the end.
tags: [rust, cargo, build, tests]
triggers: [rust, cargo, clippy, Cargo.toml, borrow checker, rustc]
priority: 70
---

# Work in a Rust project

`rustc` produces the most actionable errors of any mainstream compiler. Most
tasks here are a matter of reading the whole message rather than the first line.

## When this applies

The workspace contains `Cargo.toml`.

## The commands that matter

- `cargo check` - types and borrows, no code generation. Fastest loop while
  fixing errors.
- `cargo build` - produces a binary. Use only when you need to run it.
- `cargo test` - builds and runs tests; `cargo test name_fragment` filters.
- `cargo clippy` - lints that catch real bugs, not only style.
- `cargo fmt` - formatting. Run it only if the project already uses it (a
  `rustfmt.toml`, or an already-formatted tree); do not reformat someone's code
  as a side effect.

Prefer `cargo check` while iterating. It is often several times faster than a
build and reports the same errors.

## Reading an error

Every `error[E....]` has three parts and the last is the useful one:

1. the message and the span,
2. `note:` explaining why the rule applies,
3. `help:` proposing a concrete change.

Read to the `help:` before deciding anything. `cargo explain E0502` gives the
full description of a code.

## Borrow checker errors

These are almost never fixed by adding `.clone()`, though that will compile.
Identify which of the three rules was broken:

- **Two mutable borrows at once** → narrow a scope, split the borrow, or take
  indices instead of references.
- **Mutable while immutable exists** → the immutable borrow lives too long;
  usually a `let` binding that could be an expression.
- **Value moved** → it was consumed by an earlier call; borrow instead, or clone
  deliberately and say why.

Cloning to silence the compiler is a real decision with a cost. If you do it,
say so in your final message.

## Adding a dependency

`cargo add <crate>` edits `Cargo.toml` correctly, including the current version.
Do not hand-write a version you remember - it will be stale. If the project
pins versions deliberately (a comment, an exact `=` requirement), match that
style.

## Failure modes

- **`.clone()` or `.unwrap()` everywhere to make it compile.** Both convert a
  compile-time problem into a runtime or performance one.
- **Reading only the first line of the error.** The `help:` usually contains the
  answer verbatim.
- **Running `cargo fmt` on a project that does not use it**, producing a diff of
  hundreds of unrelated lines.
- **Adding `unsafe` to get past the borrow checker.** That is a design decision,
  not a fix; raise it instead.
