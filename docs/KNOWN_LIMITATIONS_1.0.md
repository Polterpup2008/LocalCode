# LocalCode 1.0 for Windows — Known limitations

- **Unsigned distribution.** The package has no signing certificate. Windows
  may show an unknown-publisher warning. Use only a package from a source you
  trust; do not disable Windows protection to make it run.
- **No OS command confinement.** Workspace checks apply to file tools, and
  command policies and approvals operate inside LocalCode. Approved commands
  run with your Windows account's filesystem and network access. They can have
  effects outside the workspace. Use Restricted or Read-only mode when
  appropriate, inspect commands, and keep backups. These controls are not a
  proven operating-system security sandbox.
- **Models can be wrong.** A model may choose the wrong tool, produce an invalid
  edit, omit a needed test, or fail to provide a useful final answer. A passing
  helper command or caller-associated criterion does not establish an entire
  build or feature. Review changes, actual commands and their results; run
  missing checks manually. Reasoning text is not verification.
- **Runtime and memory compatibility vary.** A GGUF must be supported by the
  bundled **llama.cpp.localcode** runtime or the custom runtime selected in
  **Settings > Models**, and by its chat template. Model files must still be
  selected separately. Native tool calling and reasoning
  controls depend on both. The text-tool fallback can be less reliable.
  Larger models and context windows need more memory. Keep the runtime's DLLs
  together and use a matching hardware build/driver. Failed loads can require
  a manual reload or runtime-path correction.
- **Repository context is optional assistance.** Index results can be stale or
  incomplete and do not prove correctness. Refresh or reindex when needed;
  fall back to file reads, text searches and manual navigation. No aggregate
  performance benefit is promised.
- **Stop is not rollback.** Cancellation cannot undo completed edits or external
  command effects. An application started outside LocalCode may need to be
  stopped manually. Leave **Use a server I started myself** unchecked to let
  LocalCode manage its bundled or custom runtime. Checking it attaches an
  existing endpoint, whose server LocalCode will not start or stop.
- **Persistence needs a writable user profile.** Conversations and profiles use
  the local database; failures are reported in the interface. Session recovery
  restores saved state, not every unsaved editor buffer or an interrupted
  command. Close the app before backing up its data folder. Keep projects and
  GGUF files outside the install directory, and preserve user data when updating.
- **Output folders and chats have separate lifetimes.** **File > Work without a
  project** creates a persistent dated workspace under the user's
  `data\outputs` directory. Right-click **Delete chat** asks for confirmation
  and permanently removes that conversation and its messages. It does not
  delete generated output folders or project files; manage those files yourself.
- **Build tools are separate.** The desktop app does not require Visual Studio
  just to open. Commands for your projects still require their own compiler,
  SDK, interpreter, Git or test tools. A local model does not supply those tools.
  User help, license notices and built-in skill Markdown are included;
  development tools, audit material and test conversations are not installed.

For setup and log locations, see [Getting started](GETTING_STARTED.md).
