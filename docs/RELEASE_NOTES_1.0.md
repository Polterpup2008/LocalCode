# LocalCode 1.0 for Windows — Release candidate 4

The Windows x64 package is unsigned. Release status and completed validation
are recorded with the distribution.

RC4 updates the default **llama.cpp.localcode** runtime to upstream b11829 with
LocalCode's Windows mapped-file working-set change. The matched 9B test reduced
resident process memory from 5.27 GiB to 0.52 GiB with the same 32K context,
f16 cache and generated tokens. The six-pair timing run stayed within the
predeclared 5% observed-median regression limit. This is a resident memory saving;
no decode speed gain, private commit or VRAM reduction is claimed. CPU backends
and the stock CUDA backend are retained. The tested CUDA kernel experiment showed
no useful speed gain and is excluded. The source patch and provenance accompany
the bundled runtime; see the [benchmark report](https://github.com/Polterpup2008/LocalCode/blob/main/docs/benchmarks/2026-10-23-runtime.md)
for measurements, hardware, thermal conditions and coverage limits.

RC3 added an optional **LocalCode Discord plugin** checkbox to the installer's
**Choose Components** page. It installs the plugin worker, manifest and clean
configuration; no bot connects during setup. Configure your own credentials,
allowed servers/channels, model and runtime from **View > Plugins** before
starting it. The portable ZIP also includes the plugin files.

LocalCode now uses a proprietary license with publicly visible source and
downloadable official binaries. Personal and internal business use, including
local builds for your own use, are permitted. Redistribution, resale and reuse
in other products require permission. Earlier MIT grants remain in effect for
their applicable versions; third-party components keep their own license terms.
See [LICENSE](../LICENSE).

LocalCode provides a native Qt desktop interface for local GGUF models through
the bundled **llama.cpp.localcode** runtime, with a custom runtime option in
**Settings > Models**. The interface uses an orange application icon and theme
accents. The 1.0 workflow includes:

- Import and switch model profiles: Qwen for DEFAULT / AGENT work, and Ling for
  FAST tasks, using your own compatible GGUF files.
- Open a project or use **File > Work without a project** to create a persistent,
  unique dated workspace under the user's `data\outputs` folder.
- Chat, display available model reasoning, reopen saved conversations, and
  right-click **Delete chat** with confirmation. Deleting a chat keeps its
  output folder and project files.
- Read, search and edit project files; review changes and tool output; run
  installed build and test commands subject to application policy and approvals.
- Inspect objective/check states and final responses separately, and cancel
  ongoing work.
- Refresh repository context, with ordinary file and search tools as a manual
  fallback.
- Save settings, model profiles, conversations and session state in the user's
  profile rather than the application installation directory.

Leave **Use a server I started myself** unchecked for the normal managed
bundled/custom runtime workflow. Checking it attaches an existing endpoint;
LocalCode does not start or stop that external server.

Manual model selection, custom runtime paths, explicit build/test commands,
model reloads and repository refreshes are supported workflows for 1.0. The
normal package includes user help, license notices and built-in skill Markdown,
with no developer tools, audit reports or saved test conversations. GGUF weights
and project-specific development tools are supplied separately.

OS-level command confinement is not implemented. Application checks and command
approvals do not isolate approved programs from the rest of your account.
Model output and caller-associated checks still need review; this release makes
no claim of universal coding reliability or Repository Intelligence speed gains.

See [Getting started](GETTING_STARTED.md) for setup and
[Known limitations](KNOWN_LIMITATIONS_1.0.md) for the practical boundaries.
