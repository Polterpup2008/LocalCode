"""Create isolated coding cases from the frozen source corpus; never patch it."""
from pathlib import Path
import os
import shutil
import subprocess


CASES = {
    "one_file_fix": {
        "function": "starts_with",
        "original": "return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;",
        "regressed": "return s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0;",
        "checks": '''check(starts_with("abc", "abc"));
    check(starts_with("abc", "ab")); check(starts_with("", ""));
    check(!starts_with("ab", "abc")); check(!starts_with("abc", "bc"));''',
    },
    "multi_file_fix": {
        "checks": '''check(iends_with("Report.CPP", ".cpp"));
    check(iends_with("ABC", "abc")); check(iends_with("", ""));
    check(!iends_with("a", "long")); check(!iends_with("abc", "ab"));''',
    },
    "diagnose_regression": {
        "function": "icontains",
        "original": "if (needle.empty()) return true;",
        "regressed": "if (needle.empty()) return false;",
        "checks": '''check(icontains("anything", "")); check(icontains("", ""));
    check(icontains("aBcD", "Bc")); check(!icontains("", "a"));
    check(!icontains("abc", "abd"));''',
    },
    "edit_select_tests": {
        "function": "split",
        "original": "if (keep_empty || !piece.empty()) out.emplace_back(piece);",
        "regressed": "if (!piece.empty()) out.emplace_back(piece);",
        "checks": '''check(split("a,,b,", ',', true) == std::vector<std::string>{"a", "", "b", ""});
    check(split("a,,b,", ',', false) == std::vector<std::string>{"a", "b"});
    check(split("", ',', true) == std::vector<std::string>{""});
    check(split("", ',', false).empty());''',
    },
}


def create(source: Path, destination: Path, case_id: str):
    if destination.exists():
        raise FileExistsError(destination)
    if (source / ".git").exists():
        raise ValueError("Coding fixtures require a frozen source tree without Git metadata")
    shutil.copytree(source, destination)
    specification = CASES[case_id]
    implementation = destination / "src/core/util/Strings.cpp"
    text = implementation.read_text(encoding="utf-8")
    if "original" in specification:
        original = specification["original"]
        if text.count(original) != 1:
            raise ValueError(f"Regression injection is ambiguous for {case_id}")
        implementation.write_text(text.replace(original, specification["regressed"]),
                                  encoding="utf-8")
    fixture = destination / "performance_fixture"
    fixture.mkdir()
    (fixture / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.24)
project(corpus_fixture LANGUAGES CXX)
add_executable(corpus_check check.cpp ../src/core/util/Strings.cpp)
target_include_directories(corpus_check PRIVATE ../src/core)
target_compile_features(corpus_check PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(corpus_check PRIVATE /utf-8)
endif()
enable_testing()
add_test(NAME corpus_check COMMAND corpus_check)
''', encoding="utf-8")
    (fixture / "check.cpp").write_text('''#include "util/Strings.h"
#include <iostream>
#include <stdexcept>
using namespace localcode::strings;
void check(bool value) { if (!value) throw std::runtime_error("fixture assertion failed"); }
int main() {
    try {
    ''' + specification["checks"] + '''
    std::cout << "fixture: PASS\\n";
    return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\\n";
        return 1;
    }
}
''', encoding="utf-8")
    (fixture / "README.md").write_text('''# Coding corpus validation

The source tree is a frozen LocalCode snapshot with one controlled task.
The checker exercises production src/core/util/Strings.cpp, not a duplicate.
Do not edit the checker or build configuration.
From an MSVC developer environment at the workspace root:

    cmake -S performance_fixture -B build/corpus-check -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/corpus-check --parallel 2
    ctest --test-dir build/corpus-check --output-on-failure
    build/corpus-check/corpus_check.exe

A successful behaviour run must print exactly `fixture: PASS`.
''', encoding="utf-8")
    initialize_git(destination)
    return destination


def initialize_git(destination: Path):
    # A fixture nested below the development checkout must not inherit that
    # checkout's Git root. Otherwise git_diff reads unrelated host changes and
    # the agent's final review is both misleading and needlessly large.
    environment = os.environ.copy()
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"):
        environment.pop(key, None)
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    git = ["git", "-c", "core.autocrlf=false", "-c", "core.hooksPath=" + os.devnull,
           "-c", "user.name=LocalCode corpus", "-c", "user.email=corpus@local.invalid"]
    for arguments in (["init", "--template=", "--initial-branch=corpus"],
                      ["add", "--all"], ["commit", "--quiet", "-m", "Frozen coding fixture"]):
        subprocess.run(git + arguments, cwd=destination, env=environment,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)


def create_navigation(source: Path, destination: Path):
    if destination.exists():
        raise FileExistsError(destination)
    if (source / ".git").exists():
        raise ValueError("Navigation fixtures require a frozen source tree without Git metadata")
    shutil.copytree(source, destination)
    initialize_git(destination)
    return destination
