# LocalCode 1.0 for Windows — Getting started

The Windows x64 package is unsigned. See the distribution's release status
and validation notes for the checks performed on that package.

## Install and launch

Use the Windows x64 installer on Windows 10 or 11, then launch **LocalCode**
from the Start menu. If using a portable distribution, extract the complete ZIP
to a writable location and run `bin\LocalCode.exe`; keep its accompanying folders
and DLLs together. Visual Studio is not needed just to launch the desktop app.
Compiling or testing your own projects requires their normal development tools.
The application uses an orange icon and matching theme accents.

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
5. Choose a model from the toolbar, or select it in **Models** and click **Load**.
   Wait for loading to finish. The model profile provides context size, GPU
   layers, tool calling and native reasoning controls; click **Apply** after
   changing a profile. If memory is insufficient, lower context size or GPU
   layers, or choose a smaller model.

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
profiles. Read [Known limitations](KNOWN_LIMITATIONS_1.0.md) before using tools
on important projects.

The normal distribution includes user help, license notices and built-in skill
Markdown files. Developer tools, audit reports and saved test conversations are
not part of the user package.
