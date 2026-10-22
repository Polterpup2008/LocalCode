# LocalCode 1.0 for Windows — Release notes

The Windows x64 package is unsigned. Release status and completed validation
are recorded with the distribution.

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
