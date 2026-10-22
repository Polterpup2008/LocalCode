---
id: repo-map
name: Orient yourself in an unfamiliar repository
description: How to build an accurate picture of a project you have not seen before, cheaply and without guessing.
tags: [orientation, exploration, structure]
triggers: [new project, unfamiliar, what does this do, explore, understand the project, repository, codebase, structure, where is]
tools: [list_directory, read_file, search_text, search_files, git_log]
priority: 80
---

# Orient yourself in an unfamiliar repository

The goal is a correct mental model in as few tool calls as possible. Reading
files at random is slow and gives a distorted picture.

## When this applies

The first time you touch a project, or when a task takes you into a part of it
you have not seen.

## Procedure

1. **Look at the top level first.** `list_directory` with no path. The layout
   tells you the conventions: `src/`, `include/`, `tests/`, `cmake/` is a very
   different project from a flat directory of `.cpp` files.

2. **Read the build file before any source.** `CMakeLists.txt`,
   `Cargo.toml`, `package.json`, `meson.build`. It names the targets, the
   dependencies, and which directories actually matter. This single file
   usually answers more questions than ten source files.

3. **Read the README if there is one** - but treat it as intent, not fact. It
   may describe a version that no longer exists. Where it conflicts with the
   build file, the build file is right.

4. **Find the entry points.** `search_text` for `int main`, `fn main`, or the
   framework's equivalent. Follow from there.

5. **Locate the thing the task is about**, rather than reading everything.
   `search_text` for the symbol, the error message, or the feature's vocabulary.
   Use `search_files` when you know roughly what a file is called.

6. **Check recent history** with `git_log` when the task concerns a regression
   or "it used to work". The commit that broke it is often obvious in the log.

## Reading order that pays off

For a C++ project: build file → public headers → the one or two source files
implementing the subsystem in question. Headers are where the design lives;
implementation files are where the details are.

## Failure modes

- **Reading files in alphabetical order.** Wasteful. Follow the structure.
- **Assuming from a filename.** `utils.cpp` may contain the core algorithm.
  Search for how a symbol is used, not just where it is declared.
- **Ignoring the tests.** The test directory is often the clearest available
  documentation of what the code is supposed to do.
- **Reading generated or vendored code.** `build/`, `node_modules/`, `third_party/`
  are usually not where the answer is. They are ignored by default for a reason.
