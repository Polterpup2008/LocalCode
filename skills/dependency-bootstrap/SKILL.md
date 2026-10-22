---
id: dependency-bootstrap
name: Resolve a missing dependency
description: What to do when a build needs something that is not installed, before concluding a human has to intervene.
tags: [dependencies, environment, build, setup]
triggers: [not found, missing dependency, not installed, could not find, no such file, install, package]
tools: [environment_info, find_executable, run_command, read_file, web_search]
priority: 80
---

# Resolve a missing dependency

"Install X" is the last answer, not the first. Work through this order.

## When this applies

A build or command fails because something is not available.

## Procedure

1. **Confirm it is actually missing.** Run `environment_info` or
   `find_executable`. Very often the tool is installed but not on PATH, and the
   real fix is to use its full path. Never report something as missing without
   having checked.

2. **Check for it in a well-known location.** On Windows, installers routinely
   leave tools out of PATH: CMake in `Program Files\CMake\bin`, MSVC behind
   `vcvars64.bat`, the Windows SDK under `Program Files (x86)\Windows Kits`.
   `environment_info` reports these.

3. **Check whether the project already provides it.** Look for:
   - a `vcpkg.json`, `conanfile.txt` or `requirements.txt`
   - `FetchContent` or `ExternalProject` in the CMake files
   - a `third_party/` or `vendor/` directory
   - a bootstrap script (`bootstrap.sh`, `setup.ps1`, `Makefile` target)

   If the project has a way to fetch its own dependencies, use it.

4. **Use a project-local install** where one is possible - a Python virtual
   environment, `npm ci`, `cargo fetch`, `vcpkg install` into the project's
   own tree. This changes nothing outside the workspace.

5. **Only then consider a system-wide install** - and stop. A system package
   manager affects the whole machine, so LocalCode requires the user's explicit
   approval. Explain what is needed, why, and what the command would be, and
   let them decide.

6. **If it genuinely cannot proceed, say exactly what is blocked**, what you
   checked, and the smallest thing the user could do to unblock it.

## Failure modes

- **"Install Chocolatey" as a first response.** Almost always premature.
- **Silently changing the build to skip the dependency.** That is a different
  program, and the user did not ask for it.
- **Assuming a package name.** Names differ across package managers; check
  before recommending.
- **Installing globally without asking.** LocalCode will refuse; do not fight it.
