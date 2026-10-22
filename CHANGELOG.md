# Changelog

## [1.0.0-rc.3](https://github.com/Polterpup2008/LocalCode/releases/tag/v1.0.0-rc.3) — 22 October 2026

- **Optional Discord integration:** the Windows installer's **Choose Components**
  page now offers **LocalCode Discord plugin**, unchecked by default. Its worker,
  manifest and clean configuration are installed only when selected. Configure
  your own bot credentials, allowed channels, model and runtime in **View > Plugins**
  before starting it. The portable ZIP includes the plugin files.
- **Source-visible proprietary license:** official binaries remain downloadable
  for personal and internal business use. Local builds for your own use are
  permitted; redistribution, resale and reuse in other products require
  permission. Earlier MIT grants and third-party license rights remain applicable.
- **GitHub project and downloads:** the README and Releases page provide the
  presentation and downloads directly. The GitHub Pages website was removed.

RC2's prior validation remains recorded below. RC3 retains the documented
release-candidate limitations; see the [release status](docs/RELEASE_STATUS_1.0.md)
for package checks and the remaining work before Stable.

## [1.0.0-rc.2](https://github.com/Polterpup2008/LocalCode/releases/tag/v1.0.0-rc.2) — 22 October 2026

A Windows x64 release candidate for working with local GGUF models in a native
coding workspace.

### Added and improved

- **Bundled runtime:** `llama.cpp.localcode` is the default. LocalCode manages
  its server process; **Settings > Models** retains a custom-runtime option.
  RC2 includes upstream build b10655; runtime optimization is deferred.
- **Work without a project:** create a unique dated output workspace that
  remains on disk after closing the app.
- **Chat deletion:** right-click a saved chat and confirm deletion. Project
  files and generated output folders are kept.
- **Matching appearance:** the orange application icon now follows the UI's
  orange and grey palette.
- **Cleaner packaging:** installer and portable ZIP include application files,
  the runtime, selected help, licenses and built-in skills. Development outputs,
  audit reports, model weights and saved test conversations are excluded.
- **Clearer server settings:** the external-server option explains when
  LocalCode manages a runtime and when the user manages an existing endpoint.

### Validation and status

Affected core and GUI checks passed **24 cases / 383 assertions**. Installed
startup, update, restart and uninstall checks passed, along with native inference
through the installed default runtime. This is not a fresh full-suite result.

**RC2 is not Stable.** Some interactive acceptance checks remain incomplete.
The Windows package is unsigned; allowed commands run with the user's privileges
and have no OS-level workspace confinement.

See [release notes](docs/RELEASE_NOTES_1.0.md), the
[validation record and remaining checks](docs/RELEASE_STATUS_1.0.md), and
[known limitations](docs/KNOWN_LIMITATIONS_1.0.md). Earlier development details
remain available in the repository's Git history.
