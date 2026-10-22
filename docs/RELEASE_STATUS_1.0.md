# LocalCode 1.0 — Release candidate 3

Windows x64, 22 October 2026. The package is unsigned. **RC3 is not Stable:**
the remaining installed-GUI checks below have not been completed.

## RC3 scope and validation

RC3 adds the optional **LocalCode Discord plugin** installer component and
changes LocalCode's first-party license to proprietary with publicly visible
source. Official downloads and local builds for your own personal or internal
business use are permitted; redistribution, resale and reuse in other products
require permission. Earlier MIT grants and third-party licenses remain applicable.
The GitHub repository and Releases page provide the presentation and downloads;
the GitHub Pages website has been removed.

RC3 package checks passed:

- A default install omitted Discord. Explicit selection installed its worker,
  manifest and adjacent compiler runtime DLLs. The native Qt Plugins panel
  initialized with a fresh private home, without credentials or starting a bot.
- Installed desktop and plugin proprietary LICENSE files matched the root
  LICENSE by SHA256. Bundled b10655 file digests and native Qwen READY inference
  passed from the installed location.
- Startup used a clean System32 PATH and a path with spaces and non-ASCII
  characters. Reinstall, restart and uninstall passed; settings, user data and
  an added user file were preserved. No owned process remained.
- A small NSIS fixture passed default, `/DISCORD=0` and `/DISCORD=1` selection checks.

Evidence: `build/installer-smoke-687fbdb4a3964fd0a2d467ccc1231d46/result.json`
and `runtime-result.json`. No full automated suite was repeated for RC3; the
24 cases / 383 assertions below remain prior RC2 evidence.

## Previous RC2 evidence

- The installed application passed native startup from a path containing spaces
  and non-ASCII characters with a clean, restricted PATH.
- Installation, update, restart and uninstall checks passed. Updates preserved
  user settings and data; uninstall preserved user data and the added user file.
- The installed default **llama.cpp.localcode** bundle, build **b10655**, reached
  Qwen READY and passed the native inference check from that installed location.
- Affected automated tests passed: core **11 cases / 115 assertions** and GUI
  **13 cases / 268 assertions**; **24 cases / 383 assertions** in total.
- RC2 includes the orange icon/theme accents, bundled runtime with custom-runtime
  selection, chat deletion with confirmation, and persistent dated output workspaces.
  Model GGUFs are still selected separately. No runtime optimization is claimed.

Installed-check records: `build/installer-smoke-3cb0d50e9bf24e929579c12773df45d1/`
contains `result.json` and `runtime-result.json`.

## Previous RC1 evidence — not repeated as current full-suite acceptance

- Core: 1,001 passed, 7 opt-in skips, 11,692 assertions. HTTP wrapper: 6 passed,
  2 opt-in skips, 109 assertions. GUI: 91 passed, 6 opt-in skips and one fixture
  failure; its corrected case then passed 23 assertions. No full GUI rerun.
- The previous interactive installed-GUI smoke loaded Qwen, answered a question,
  opened a workspace, read and edited files, and ran two Python tests with exit 0.
  The edit was checked on disk, and closing stopped its managed model server.
  This is previous RC1 evidence, not a fresh RC3 interactive smoke result.

## Remaining before Stable

Ling import/load/chat, switching back to Qwen, Stop during a task, the final
interactive conversation/file-persistence restart, and small-window GUI
acceptance remain incomplete. The passed automated installer restart does not
replace those interactive checks. These gaps are not claimed passes.

Distribution: `dist/1.0.0-rc.3`. See [Getting started](GETTING_STARTED.md),
[release notes](RELEASE_NOTES_1.0.md) and [known limitations](KNOWN_LIMITATIONS_1.0.md).
OS-level command confinement is not implemented; application checks and user
approvals remain in place. This status document is outside the installed payload.
