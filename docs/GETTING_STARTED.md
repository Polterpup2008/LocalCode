# LocalCode 1.1 for Windows — Getting started

The Windows x64 package is unsigned. See the distribution's release status
and validation notes for the checks performed on that package.

## Install and launch

Use the Windows x64 installer on Windows 10 or 11, then launch **LocalCode**
from the Start menu. If using a portable distribution, extract the complete ZIP
to a writable location and run `bin\LocalCode.exe`; keep its accompanying folders
and DLLs together. Visual Studio is not needed just to launch the desktop app.
Compiling or testing your own projects requires their normal development tools.
The application uses an orange icon and matching theme accents.

On the installer's **Choose Components** page, **LocalCode** is required.
**Desktop shortcut** and **LocalCode Discord plugin** are optional. Leave the
plugin unchecked for the coding app alone, or select it to configure your own
Discord bot after installation. Setup does not start or connect a bot.

Keep projects and model files outside the application installation folder.
Large GGUF model weights are supplied separately. You need enough RAM, and GPU
memory if offloading, for your chosen model and context size.

## Configure a runtime and models

1. LocalCode includes **llama.cpp.localcode** as its default runtime. Open
   **File > Settings > Models** to inspect it or choose a custom runtime.
   For a custom runtime, browse to `llama-server.exe` from a compatible Windows
   x64 llama.cpp package and keep its supporting DLLs together. Use a CPU or GPU
   build appropriate to your hardware; a CUDA build also needs a compatible
   NVIDIA driver.
2. After changing a runtime path, click **Scan again**, check the discovered
   runtime information, then **Save**.
   The **General** settings page also shows the runtime directory. The Models
   panel's **Set up runtime and model…** dialog offers another setup route;
   manually selecting files is sufficient.
3. Choose **File > Add GGUF model…**, browse to your `.gguf`, review its metadata,
   and click **Add model**. LocalCode references the file in place; do not move
   or delete it while the profile uses it.
4. Import your Qwen GGUF for the **DEFAULT / AGENT** workflow, and your Ling GGUF
   for **FAST** chat or smaller tasks. These describe how to use the models;
   imported profile names come from the model, and weights are not downloaded
   or assigned to these roles automatically.
   These roles do not limit the model library: compatible installed 1.5B–4B
   GGUFs can also be imported and selected for a computer with less memory.
5. Choose a model from the toolbar, or select it in **Models** and click **Load**.
   Wait for loading to finish. The model profile provides context size, GPU
   layers, reply token limit, tool calling and native reasoning controls; click
   **Apply** and reload after changing a profile. If memory is insufficient,
   lower context size or GPU
   layers, or choose a smaller model.

The bundled runtime checks actual GPU devices and can use its Vulkan alternative
when CUDA is unavailable. The loaded notice reports actual full/partial offload
or CPU placement; an unknown result stays unconfirmed. No driver is installed
or updated by LocalCode. First Vulkan use may take longer while driver pipelines
are initialized; loading, prompt evaluation and generation are reported separately.

New low-memory profiles start with a 4K context and bounded saved prompt states.
Use **Models > Context size > Presets** for 4K, 8K, 16K or 32K, or enter a manual
value. **Fit GPU layers to available memory** adjusts placement while retaining
that context. Disable it for a manual layer count. Smaller contexts trade capacity
for memory and are not counted as engine optimization gains.

Beside Send, **Auto** routes greetings to lightweight conversation and keeps
coding tools for other requests. **Chat** uses no project tools or coding-agent
instructions. **Code** always enables the full coding workflow and its checks.
Switching modes does not delete the conversation or the preceding coding record.

Stop an active task before switching models. Selecting Ling unloads the previous
model before loading Ling; choose Qwen again when returning to agent work.
If loading fails, read the visible error and **Output** (`Ctrl+J`), correct the
runtime/model settings, and try **Load** again. **Unload** stops a managed model
server.

Leave **Settings > Advanced > Use a server I started myself** **unchecked** for
normal use. LocalCode starts and stops the bundled or custom runtime selected
in **Models**. Check it only to attach to an already running server using its
**Base URL** and, if required, **API key**. LocalCode does not start or stop that
external server; you manage it yourself. This checkbox does not select a custom
`llama-server.exe` file.

## Open a project and chat

Choose **File > Open project folder…** (`Ctrl+O`) to work in an existing project.
For chat or new output without choosing a project, select **File > Work without
a project**. It creates and opens a unique dated folder under
`%LOCALAPPDATA%\LocalCode\data\outputs`, such as
`2026-10-22_18-30-00-aBc123`. The folder is a normal workspace: file tools use its
existing safety rules, and generated files remain there after closing LocalCode.
**Settings > General** shows the actual data path if it has been changed.

Type a request and press **Ctrl+Enter**. Plain Enter adds a new line.

For a first coding task, give a small scope and an explicit check, for example:

> Read README.md, fix one typo, and show the change. Do not run commands.

Open files through the **Project** sidebar and edit them in **Files** (`Ctrl+2`).
Press **Ctrl+S** to save a manual edit. Agent file edits affect the project on
disk; review **Changes** (`Ctrl+3`) and the tool results before keeping them.
Use version control or a backup for work you need to recover.

For commands, specify the command and what it should establish, for example:
“Run this project's test command and report its exit status and any failures.”
Commands require the relevant tools to be installed and available. The
**Output** panel shows process output; tool details can open the full log.

## Approvals, results and Stop

**Settings > Agent** controls autonomy and **Read-only mode**. Restricted mode
asks about unrecognized commands; Standard permits recognized development
commands; Full permits more unmatched commands. Review the exact command in
**Approval required**, then choose **Run it** or **Don't run**. File tools check
workspace paths, but command processes run with your Windows account's access.
**OS-level command confinement is not implemented.**

Click **Stop**, or press **Esc** while the prompt has focus, to cancel a task.
Wait for it to finish stopping before switching projects or models. Cancellation
does not undo files already written or command effects already performed.

Open **Details** for activity, tool and verification information. **Answered**
means a response was produced; it does not mean the project was tested.
**Verified** reflects the recorded checks and criteria, whose scope you should
review. **Verified — no summary** means the record and final prose have separate
outcomes. Read any unchecked or unsupported-claim notices. Reasoning display is
available for models that provide it; it is not evidence that a task succeeded.

If the model repeatedly returns an incomplete tool call, start with a smaller
working module or increase **Models > Reply token limit**, then Apply and reload.
This allowance includes reasoning and tool arguments; increasing it can make
generation longer. Context size is a separate setting. LocalCode discards a
rejected batch before execution and tries two short-call recoveries.

## Repository context and saved conversations

The **Repository** sidebar provides **Refresh**, **Reindex all** and **Stop**.
Treat index matches as leads and check the source. If an index is unavailable
or unhelpful, ask for ordinary file reads and text searches, or navigate files
yourself. Repository Intelligence is not required to review or edit files.

Conversations save automatically. Use **Chats** to reopen one and **New
conversation** (`Ctrl+N`) to start another. To remove a saved conversation,
right-click its row in **Chats**, choose **Delete chat**, and confirm **Yes**.
**No** cancels. Deletion removes that chat and its messages, including when it is
not the currently open chat. It does not delete the dated output folder or any
project files. Stop an active task before deleting a chat.

On restart, LocalCode attempts to
restore the workspace, conversation, open-file list and unsent prompt. Reopen
missing or moved files manually, and load a model before sending another request.
Save manual editor changes before closing; an open-file list is not a backup
of unsaved file contents.

By default, the conversation database is
`%LOCALAPPDATA%\LocalCode\data\localcode.sqlite3`; logs are under
`%LOCALAPPDATA%\LocalCode\logs`. **Settings > General** shows the actual paths,
and **View > Open the log folder** opens logs. Close LocalCode before backing
up its data folder. Keep that data when updating to retain conversations and
profiles. Read [Known limitations](KNOWN_LIMITATIONS_1.1.md) before using tools
on important projects.

The normal distribution includes user help, license notices and built-in skill
Markdown files. Developer tools, audit reports and saved test conversations are
not part of the user package.

## Optional Discord plugin

Select **LocalCode Discord plugin** during installation to include its worker,
manifest, dependency notices and clean configuration. The portable ZIP includes
the plugin files as well; starting it still requires configuration. No token,
private persona, model weights or conversation history is included.

In LocalCode, choose **View > Plugins**. Use your own Discord application and
bot token, enable **Message Content Intent** in the Discord developer portal,
and enter the allowed server IDs, channel IDs and owner IDs. Select a compatible
model and runtime in the plugin's model settings. **Save settings**, then use
**Start bot** when ready. Stop the bot before changing settings. The integration
connects to Discord only when you start your configured bot.

Closing LocalCode leaves a started bot online. Use **Stop bot** in the plugin
panel when you want to disconnect it.

Keep the token private and share only redacted logs when reporting a problem.
Installing the plugin grants no Discord account, credentials or model license.

## License

LocalCode's source is publicly visible under the proprietary [license](../LICENSE).
Official binaries may be downloaded and run, and local builds are permitted for
your own personal or internal business use. Redistribution, resale and reuse in
other products require permission. Earlier MIT grants and third-party component
licenses retain their applicable rights.
