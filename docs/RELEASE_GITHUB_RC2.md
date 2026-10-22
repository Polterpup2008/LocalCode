# LocalCode 1.0 RC2 — Your models. Your projects. The work in view.

**Turn your local GGUF model into a native coding workspace with file editing,
command output, saved chats and evidence you can inspect.**

LocalCode brings the model and the project into one Windows desktop app. Ask for
a focused change, follow the tools it uses, review the diff, and see which checks
support the result. RC2 makes getting started easier with an included runtime,
fresh output folders and simpler chat management.

## Download for Windows x64

- **[Setup.exe](https://github.com/Polterpup2008/LocalCode/releases/download/v1.0.0-rc.2/LocalCode-1.0.0-rc.2-Windows-x64-Setup.exe)** — install for your Windows account and launch from the Start menu.
- **[Portable ZIP](https://github.com/Polterpup2008/LocalCode/releases/download/v1.0.0-rc.2/LocalCode-1.0.0-rc.2-Windows-x64.zip)** — extract the complete archive and run `bin\LocalCode.exe`; keep the included files together.
- **SHA256SUMS.txt** — checksums for the published packages.

The packages include Qt and **llama.cpp.localcode** (upstream b10655). Bring your
own compatible **GGUF model**; weights are not included. Visual Studio is not
needed to open the app, while your projects still need their usual development
tools. Runtime compatibility and memory needs depend on the model and hardware.

## Three steps to your first task

1. **Install or extract**, then launch LocalCode.
2. Choose **File > Add GGUF model…**, select your model, review its profile and
   load it. The bundled runtime is selected by default.
3. **Open a project folder**, or choose **File > Work without a project**. Send
   a small, clear request with **Ctrl+Enter**, then inspect **Changes** and tool output.

## What's new in RC2

- **Runtime included, choice retained.** LocalCode starts and stops its default
  runtime. Choose **Custom llama.cpp** in Settings when needed. No custom
  performance optimization is claimed; that work is planned for later.
- **Start from a blank folder.** Work without a project creates a persistent,
  unique dated output workspace.
- **Tidy saved chats.** Right-click **Delete chat** and confirm. Project files
  and generated folders stay on disk.
- **A matching orange icon**, clearer runtime settings and a clean user package
  that excludes development outputs and test conversations.

## Validation and release-candidate limits

Affected automated checks passed **24 test cases / 383 assertions**. Installed
startup, update, restart and uninstall checks passed, plus native inference with
Qwen through the installed default runtime. These are the recorded RC2 checks,
not a fresh full-suite acceptance result.

**RC2 is not Stable.** Ling switching, task Stop, final interactive persistence
and small-window acceptance still need completion. The package is **unsigned**.
Approved commands run with your Windows account's access; OS-level command
confinement is not implemented. Review commands and changes and keep backups.

Read [Getting started](https://github.com/Polterpup2008/LocalCode/blob/main/docs/GETTING_STARTED.md),
[release status](https://github.com/Polterpup2008/LocalCode/blob/main/docs/RELEASE_STATUS_1.0.md)
and [known limitations](https://github.com/Polterpup2008/LocalCode/blob/main/docs/KNOWN_LIMITATIONS_1.0.md).
Try a small task and share reproducible feedback through the
[issue templates](https://github.com/Polterpup2008/LocalCode/issues/new/choose).
