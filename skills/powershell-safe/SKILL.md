---
id: powershell-safe
name: Shell commands on Windows without surprises
description: How to run commands on Windows so that quoting, exit codes and encoding behave the way you expect.
tags: [windows, shell, powershell, cmd, quoting]
triggers: [powershell, cmd, batch, shell command, quoting, exit code, script]
tools: [run_command, find_executable]
priority: 65
---

# Shell commands on Windows without surprises

## When this applies

Running any command on Windows, particularly one with paths, quotes or a
pipeline.

## Prefer the argv form

LocalCode's `run_command` takes a `command` array. Use it:

```
["cmake", "--build", "build", "--config", "Release"]
```

Nothing is re-parsed, so spaces and quotes in paths cannot break. Reach for
`shell_command` only when you genuinely need a pipe or `&&`.

## The pitfalls

**Exit codes.** In PowerShell, `$?` reflects the last *PowerShell* operation,
not always a native program's exit code - use `$LASTEXITCODE` for that.
`run_command` reports the process exit code directly, which is what you want.

**stderr is not failure.** Many tools write progress to stderr. Judge by the
exit code, not by whether stderr had content.

**PowerShell 5.1 has no `&&` or `||`.** Those are PowerShell 7 syntax. In 5.1
use `;` and check `$LASTEXITCODE`, or run each command separately - which is
better anyway, because you see each result.

**Encoding.** Output may arrive as UTF-16 or in the system code page.
LocalCode normalises this, but if you write a file with `Set-Content`, pass
`-Encoding utf8` or other tools will misread it.

**Unix commands do not exist.** No `head`, `tail`, `which`, `touch`, `wc`, or
`rm -rf`. Use `Get-Content -TotalCount`, `Get-Command`, `New-Item`,
`Measure-Object`, `Remove-Item -Recurse`. Git Bash provides the Unix versions
if the project already depends on it.

**Interactive commands hang.** `Read-Host`, `Get-Credential`, `pause`, `git
rebase -i` and anything opening an editor will sit until the timeout. Never
run them.

## Procedure

1. Use the argv form unless a shell feature is required.
2. Quote nothing yourself - the argv form handles it.
3. Judge success by exit code.
4. If a command hangs to the timeout, assume it wanted input.

## Failure modes

- **Building a command string by concatenation.** The quoting will eventually
  be wrong. Use the array.
- **Treating stderr output as an error.**
- **Using `&&` on PowerShell 5.1.** It is a parse error.
- **`New-Item -Force` on an existing file.** It truncates it.
