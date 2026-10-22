# LocalCode 1.0 — Release candidate 2

Windows x64, 22 October 2026. The package is unsigned. **RC2 is not Stable:**
the remaining installed-GUI checks below have not been completed.

## Current RC2 evidence

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

## Previous RC1 evidence — not repeated as RC2 full-suite acceptance

- Core: 1,001 passed, 7 opt-in skips, 11,692 assertions. HTTP wrapper: 6 passed,
  2 opt-in skips, 109 assertions. GUI: 91 passed, 6 opt-in skips and one fixture
  failure; its corrected case then passed 23 assertions. No full GUI rerun.
- The previous interactive installed-GUI smoke loaded Qwen, answered a question,
  opened a workspace, read and edited files, and ran two Python tests with exit 0.
  The edit was checked on disk, and closing stopped its managed model server.
  This is previous RC1 evidence, not a fresh RC2 interactive smoke result.

## Remaining before Stable

Ling import/load/chat, switching back to Qwen, Stop during a task, the final
interactive conversation/file-persistence restart, and small-window GUI
acceptance remain incomplete. The passed automated installer restart does not
replace those interactive checks. These gaps are not claimed passes.

Distribution: `dist/1.0.0-rc.2`. See [Getting started](GETTING_STARTED.md),
[release notes](RELEASE_NOTES_1.0.md) and [known limitations](KNOWN_LIMITATIONS_1.0.md).
OS-level command confinement is not implemented; application checks and user
approvals remain in place. This status document is outside the installed payload.
