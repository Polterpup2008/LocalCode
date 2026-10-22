---
id: python-project
name: Work in a Python project
description: How to run, test and install a Python project without corrupting the interpreter the user depends on.
tags: [python, build, tests, environment]
triggers: [python, pip, venv, pytest, requirements.txt, pyproject, poetry, conda]
priority: 70
---

# Work in a Python project

The dangerous mistake here is installing into the wrong interpreter. Establish
which one the project uses before running anything.

## When this applies

The workspace contains `pyproject.toml`, `requirements.txt`, `setup.py`,
`Pipfile`, or a `.venv` directory.

## Find the interpreter first

1. A `.venv` or `venv` directory in the project is the answer. Use
   `.venv\Scripts\python.exe` on Windows, `.venv/bin/python` elsewhere - the
   full path, not a bare `python`.
2. No virtual environment → check `python --version` and `where python`, and say
   which one you are using before installing anything.
3. **Never `pip install` into a system interpreter without saying so first.**
   That modifies the machine outside the workspace. Create a virtual environment
   instead, or ask.

Run tools as `<python> -m pip`, `<python> -m pytest` rather than bare `pip` or
`pytest`: the bare command may belong to a different interpreter than the one
you think you are using.

## Which file defines the dependencies

- `pyproject.toml` with `[project]` → `python -m pip install -e .`
- `pyproject.toml` with `[tool.poetry]` → `poetry install`
- `requirements.txt` → `python -m pip install -r requirements.txt`
- Both present → `pyproject.toml` is authoritative; `requirements.txt` is often
  a pinned export.

## Running the tests

Look for the runner the project already uses before choosing one: a `pytest`
section in `pyproject.toml` or `pytest.ini`, a `tox.ini`, or a `tests/`
directory. Run the whole suite once to get a baseline, then the specific test
while iterating.

`python -m pytest -x -q tests/test_thing.py::test_case` stops at the first
failure and names one test.

## Reading a traceback

The **last** frame is where it raised; the frames above are how it got there.
The line that matters is usually the deepest one still inside the project rather
than inside a library. Read the exception type and message before the stack -
`KeyError: 'name'` and `ImportError` need completely different responses.

## Import errors

`ModuleNotFoundError` for the project's own package almost always means it was
not installed in editable mode, or the working directory is wrong - not that the
file is missing. Check `python -m pip show <package>` before editing imports.

## Failure modes

- **`pip install` into the system Python** because the virtual environment was
  not activated. Use the full interpreter path and the problem disappears.
- **Assuming `python` means Python 3.** On some machines it is 3, on others it
  does not exist and `py` is the launcher. Check the version.
- **Editing `requirements.txt` when `pyproject.toml` is authoritative.**
- **Reporting a test pass from a different environment than the failure.**
