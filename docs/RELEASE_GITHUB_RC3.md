![LocalCode — Build with local intelligence](https://raw.githubusercontent.com/Polterpup2008/LocalCode/v1.0.0-rc.3/docs/images/localcode-hero.svg)

**Your GGUF model, a native coding workspace, and a clear record of the work.**

LocalCode brings your model and project into one Windows desktop app. Ask for
a focused change, follow its tools, review the diff, and inspect the checks
behind the result. The local runtime is included; bring your own compatible
model and your project's usual development tools.

## Download for Windows x64

- **[Windows installer](https://github.com/Polterpup2008/LocalCode/releases/download/v1.0.0-rc.3/LocalCode-1.0.0-rc.3-Windows-x64-Setup.exe)** — install for your Windows account; optionally include the LocalCode Discord plugin.
- **[Portable ZIP](https://github.com/Polterpup2008/LocalCode/releases/download/v1.0.0-rc.3/LocalCode-1.0.0-rc.3-Windows-x64.zip)** — extract the complete archive and run `bin\LocalCode.exe`; keep the accompanying files together.
- **[SHA256SUMS.txt](https://github.com/Polterpup2008/LocalCode/releases/download/v1.0.0-rc.3/SHA256SUMS.txt)** — checksums for the published packages.

Windows 10/11 x64. Qt and **llama.cpp.localcode** (upstream b10655) are included.
GGUF weights are supplied separately. Memory needs, GPU-driver requirements and
model compatibility depend on your selected model and runtime.

## What's new in RC3

- **Choose Discord during installation.** Select **LocalCode Discord plugin**
  on **Choose Components** to include the optional integration. It is unchecked
  by default. Setup adds the worker, manifest and clean configuration and does
  not connect a bot. Configure your own credentials, allowed servers/channels,
  model and runtime from **View > Plugins**, then start the bot when ready.
- **Proprietary license, visible source.** Official binaries remain available
  to download and run for personal or internal business use. You may inspect
  the source and build locally for your own use. Redistribution, resale and
  reuse in other products require permission. See
  [LICENSE](https://github.com/Polterpup2008/LocalCode/blob/v1.0.0-rc.3/LICENSE).
  Earlier MIT grants and third-party component rights remain applicable.
- **Everything on the project page.** The repository README and GitHub Releases
  provide the presentation and downloads directly. The GitHub Pages site was removed.

The bundled runtime with a custom-runtime option, persistent dated output
workspaces, right-click chat deletion and matching orange icon remain included
from RC2. No custom runtime optimization is claimed.

## Your first task

1. Install or extract, then launch LocalCode.
2. Choose **File > Add GGUF model…**, select your model, review its profile and
   load it. The bundled runtime is selected by default.
3. Open a project, or choose **File > Work without a project**. Send a small,
   clear request with **Ctrl+Enter**, then inspect **Changes** and tool output.

## Validation and release-candidate limits

RC3 passed default installation without Discord and explicit installation with
the plugin, including worker, manifest and compiler runtime checks. Its native
Qt Plugins panel initialized with a fresh private home without bot credentials
or starting a bot. Installed desktop/plugin license files matched the published
proprietary license.

Bundled-runtime digests and native Qwen READY inference passed. Clean-PATH startup,
installation to a path with spaces and non-ASCII characters, reinstall, restart
and uninstall passed, preserving user data and an added user file. No owned
process remained. Default and explicit Discord selection also passed a small
NSIS installer fixture.

The previous RC2's affected automated checks passed **24 cases / 383 assertions**.
Its installed startup, update, restart and uninstall checks passed, along with
native Qwen inference through the installed default runtime. These are prior
RC2 results; a full automated suite was not repeated for RC3. See the
[release status](https://github.com/Polterpup2008/LocalCode/blob/v1.0.0-rc.3/docs/RELEASE_STATUS_1.0.md)
for the validation record.

**RC3 is not Stable.** Ling switching, task Stop, final interactive persistence
and small-window acceptance remain incomplete. The package is unsigned.
Approved commands run with your Windows account's access; OS-level command
confinement is not implemented. Review commands and changes and keep backups.

Read the [setup guide](https://github.com/Polterpup2008/LocalCode/blob/v1.0.0-rc.3/docs/GETTING_STARTED.md)
and [known limitations](https://github.com/Polterpup2008/LocalCode/blob/v1.0.0-rc.3/docs/KNOWN_LIMITATIONS_1.0.md).
Share a small reproduction or compatibility report through the
[issue templates](https://github.com/Polterpup2008/LocalCode/issues/new/choose).
