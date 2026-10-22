---
id: cmake-build-fix
name: Diagnose a CMake build
description: How to configure, build and repair a CMake project, including the generator and toolchain problems specific to Windows.
tags: [cmake, build, cpp, windows, ninja, msvc]
triggers: [cmake, cmakelists, configure failed, build system, ninja, msbuild, generator, target not found]
tools: [run_command, read_file, environment_info, replace_in_file]
priority: 85
---

# Diagnose a CMake build

## When this applies

The project has a `CMakeLists.txt` and either does not configure, does not
build, or needs building for the first time.

## Procedure

1. **Check the toolchain exists** with `environment_info`. You need cmake, a
   generator (ninja or Visual Studio) and a compiler. Do not assume; look.

2. **Configure into a build directory**, never in the source tree:

   ```
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```

   On Windows with MSVC, Ninja needs the developer environment loaded first or
   `cl.exe` will not be found. If it is not loaded, use the Visual Studio
   generator, which sets that up itself:

   ```
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   ```

3. **Read configure errors carefully.** They are usually one of:
   - `Could NOT find <Package>` - a dependency is missing. Check whether the
     project vendors it, fetches it with FetchContent, or expects vcpkg or a
     system install. Look in `cmake/` and at the `find_package` call's options.
   - `No CMAKE_CXX_COMPILER could be found` - the compiler is not on PATH. Load
     the developer environment or choose a different generator.
   - `Target "x" links to target "y" but the target was not found` - a missing
     `add_subdirectory`, or a typo in a target name.

4. **Build through cmake, not the generator**, so one command works for every
   generator:

   ```
   cmake --build build --config RelWithDebInfo -j
   ```

5. **Re-configure after changing CMakeLists.txt.** `cmake --build` normally
   re-runs configure by itself; if behaviour looks stale, delete
   `build/CMakeCache.txt` and configure again. Delete the cache, not the source.

6. **Run the tests** with `ctest --test-dir build --output-on-failure`.

## Failure modes

- **Configuring in the source directory.** Pollutes the repository. Always `-B build`.
- **Deleting the whole build directory reflexively.** Slow and rarely needed.
  Remove `CMakeCache.txt` first.
- **Editing CMakeLists.txt to route around a missing dependency.** Find out why
  it is missing first; the project may already have a way to fetch it.
- **Assuming the generator.** Check `CMAKE_GENERATOR` in `build/CMakeCache.txt`
  before adding flags only one generator understands.
