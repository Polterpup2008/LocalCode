# Security model

LocalCode runs a language model with real access to your filesystem and your
shell. This document says exactly what that means, what is prevented, and what
is not.

The policy applies to requests routed through LocalCode's tools. Allowed native
commands and descendants run with the account's privileges; they are not confined
to the workspace by an OS filesystem boundary. **Hard command confinement is NOT
IMPLEMENTED.** Experimental Windows confinement mechanisms are not enabled in
the release. See [known limitations](KNOWN_LIMITATIONS_1.0.md).

## The governing principle

**The model does not decide what is safe. The application does.**

The model proposes; `PathPolicy` and `CommandPolicy` dispose. No prompt, no
model output and no tool argument can change a policy decision. A model that is
confidently wrong, or that has been steered by content it read from a file or a
web page, still cannot get past them.

## Three distinctions that are kept separate

These get conflated constantly, and conflating them is what makes an agent
either useless or dangerous:

| | |
|---|---|
| **Where the executable lives** | Irrelevant. `C:\Program Files\CMake\bin\cmake.exe` building a project in `D:\Projects\Game` is ordinary. Tools are found on PATH and run from wherever they are installed. |
| **The working directory** | The workspace, by default. Changeable to any readable directory. |
| **Read permission** | Broad. System headers, the Windows SDK, a sibling checkout, LocalCode's own logs — all readable. An agent that cannot read the SDK cannot do its job. |
| **File-tool write policy** | The selected workspace plus explicitly granted roots, checked by the application. This does not constrain arbitrary allowed native commands or descendants. |

## Command classification

Every command is classified before it runs.

### Blocked — never, at any autonomy level

These cannot be enabled by a setting, by a prompt, or by the user asking twice.

| Category | Examples |
|---|---|
| Disk destruction | `format`, `diskpart`, `mkfs`, `fdisk`, `parted` |
| Boot configuration | `bcdedit`, `bootrec`, `bootsect`, `grub-install`, `efibootmgr` |
| Power state | `shutdown`, `reboot`, `Stop-Computer`, `Restart-Computer` |
| Backup destruction | `vssadmin delete`, `wbadmin delete`, `wmic shadowcopy delete` |
| Anti-malware | `Set-MpPreference`, `Add-MpPreference`, stopping WinDefend |
| Firewall | `netsh advfirewall … off` |
| Credential theft | `reg save hklm\sam`, LSASS dumping, mimikatz and friends |
| Registry destruction | deleting `HKLM\SYSTEM\CurrentControlSet` and similar |
| Obscured execution | `powershell -EncodedCommand`, `FromBase64String` |
| Remote code pipes | `curl … \| sh`, `iwr … \| iex` |
| Filesystem wipes | `rm -rf /`, `del /s /q C:\`, `cipher /w` |
| Accounts and persistence | `net user … /add`, `net localgroup administrators … /add`, `schtasks /create` |

A blocked command is refused with an explanation that says plainly it cannot be
overridden, so the model stops trying and finds another route.

### Confirmation required — a human answers

| Category | Why |
|---|---|
| `git reset --hard`, `git clean -fd`, `git checkout -- <path>` | destroys uncommitted work |
| `git push`, especially `--force` | publishes or rewrites shared history |
| `choco/winget/apt/brew install`, `npm install -g`, `pip install --user` | changes the machine outside this project |
| `ssh`, `scp`, `psexec`, `Invoke-Command` | reaches another machine |
| `takeown`, `icacls`, `chown`, `chmod -R` | changes ownership or permissions |
| `setx`, `reg add/import/delete` | persists machine-wide state |
| Any download | writes a file from a source the user has not seen |

The prompt shows the exact command, the working directory and why approval was
needed. The safe answer is the default. A refusal is fed back to the model as a
fact to work around, not an error to retry.

### Allowed — the everyday work of writing code

Build tools, compilers, linkers, static analysis, language runtimes,
project-local package resolution, test runners, read-only git, and inspection
utilities. Running the project's own binaries is included: an agent that cannot
run what it just built cannot verify anything.

### Anything else

Governed by the autonomy level:

- **Restricted** — unrecognised commands ask first.
- **Standard** (default) — unrecognised commands run inside the selected
  workspace.
- **Full** — unrecognised commands run.

No level unblocks a blocked command. Recursive deletion is always checked against
the path policy first, whatever the level: inside the workspace it is ordinary
cleanup, outside it is refused.

## Path security

Every path a tool receives is validated, then canonicalised, then checked for
containment.

**Rejected on syntax alone:**

- embedded NUL bytes
- device-namespace prefixes (`\\?\`, `\\.\`)
- UNC network paths
- NTFS alternate data streams (`notes.txt:hidden`)
- reserved Windows device names (`CON`, `NUL`, `COM1`, `LPT9`, and with any
  extension — `NUL.txt` is still `NUL`)
- components with a trailing dot or space, which Win32 silently strips, turning
  `workspace/.. ./escape` into an escape
- characters that are invalid on Windows (`<>|"*?`)

**Canonicalisation** resolves the deepest existing ancestor through
`GetFinalPathNameByHandleW`, which follows symlinks, NTFS junctions and 8.3 short
names, then re-appends the components that do not exist yet. A lexical check
alone is escapable by planting a junction inside the workspace; the test suite
creates one and asserts the escape is refused.

**Containment** compares whole path components, case-insensitively on Windows.
`C:\projects\game-backup` is not inside `C:\projects\game`.

**Protected locations** are never writable regardless of configuration: the
Windows directory, Program Files, ProgramData, and `~/.ssh`, `~/.aws`, `~/.gnupg`
— even if one of them were somehow made the workspace.

## Protecting uncommitted work

Before an agent run starts, LocalCode records which files git already reports as
modified. Then:

- editing one of those files is logged and shown in the diff panel with a
  warning;
- `write_file` and `apply_patch` **refuse** to replace such a file unless the
  caller passes `overwrite_uncommitted: true`;
- `git_status` marks which changes were made by the agent in this session and
  which were already there.

Losing a morning's uncommitted work to a confident model is the failure this is
engineered against.

## Web access

- Search and fetch are ordinary HTTP GETs, off by a single setting.
- Downloads always require confirmation, whatever the source.
- Executables and archives may only be downloaded from the trusted-source list:
  official vendor domains, official source forges and the canonical package
  registries. The list is extensible from Settings, deliberately by hand.
- A near-miss domain does not pass: `notgithub.com` and `github.com.evil.example`
  are both refused for a `github.com` rule.

## Resource limits

- Every command has a timeout; the whole process tree is killed when it expires.
- Captured output is capped per stream; the full log goes to a file.
- Tool results are excerpted with an explicit "omitted" marker, never silently.
- The agent has an iteration budget and a wall-clock budget.
- The progress monitor stops a run that is not achieving anything.
- Tool-call arguments are validated against a schema before a tool sees them.

## What this does not do

Stated plainly, because a security section that only lists strengths is not
useful:

- **It is not a sandbox.** An allowed command runs with your user's full
  privileges. LocalCode checks command requests and file-tool paths; it does not
  contain a command once it is running. A permitted build
  script can do anything your account can do.
- **The command policy is a denylist plus heuristics.** It covers the destructive
  operations that are actually plausible, and the classifier inspects shell
  chains, substitutions and interpreter wrappers. It is not a proof. A
  sufficiently creative encoding could slip past it.
- **Reads are broad by design.** With `allow_read_outside_workspace` on (the
  default), the agent can read anything your account can read, and could include
  it in a web request if web access is enabled. Turn one or both off for
  sensitive work.
- **Prompt injection is not solved.** Content read from a file or a web page is
  data, and the policy applies to whatever the model does next — but a model can
  still be steered into a legitimate-looking action. The policy is the backstop,
  not the model's judgement.
- **The confirmation dialog is only as good as the reader.** Approving without
  reading defeats it.

## Reporting a vulnerability

See [SECURITY.md](../SECURITY.md) in the repository root.
