---
id: web-documentation-grounding
name: Ground an answer in real documentation
description: How to use web search and fetch to confirm an API or an error rather than recalling it approximately.
tags: [web, documentation, research, api]
triggers: [documentation, api reference, what does this error mean, latest version, changelog, release notes, how do I use]
tools: [web_search, web_fetch, search_text]
priority: 65
---

# Ground an answer in real documentation

## When this applies

You need a fact the project cannot supply: the exact signature of a library
function, the meaning of an unfamiliar error, whether a bug is known, what
changed in a release.

## When it does not

If the answer is in the repository, read the repository. Searching the web for
how a project's own function behaves is slower and less reliable than reading
it.

## Procedure

1. **Search the code first.** For a third-party library, the headers are often
   vendored or installed locally, and `search_text` finds the real declaration
   in seconds - which beats any documentation, because it is the version
   actually being compiled.

2. **Search with specifics.** Include the library name, the version, and the
   exact error text. `"CMake Error: Could NOT find Qt6" vcpkg` finds the answer;
   "cmake qt problem" does not.

3. **Prefer primary sources.** Official documentation, the project's own
   repository, its release notes and issue tracker. Blog posts and forum
   answers age badly, and a 2019 answer about a library that changed its API in
   2023 is worse than no answer.

4. **Fetch the page and read it.** A search snippet is not evidence. Use
   `web_fetch` on the result that looks authoritative.

5. **Check the version applies.** Look for what version the page documents and
   compare against what the project uses.

6. **Say where it came from** when it matters: "According to the CMake 3.28
   documentation for `FetchContent_Declare`, ...". If you could not confirm
   something, say that instead of stating it confidently.

## Failure modes

- **Trusting a snippet.** Snippets are truncated and often misleading.
- **Reading documentation for the wrong version.** Check.
- **Searching for something in the repository.** Read the code.
- **Reporting a Stack Overflow answer as authoritative.** Note the source and
  its age.
