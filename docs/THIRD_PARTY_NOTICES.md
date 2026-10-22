# Distribution notices

LocalCode is MIT licensed; see LICENSE. Its native UI dynamically links Qt.
Qt is not embedded in a single executable, and its DLLs can be replaced with
compatible modified builds. The application does not prevent debugging or
reverse engineering for modification of the LGPL library.

The Windows package uses Qt 6.9.1 (Core, Gui, Widgets, Concurrent and the runtime
plugins deployed by Qt). Qt is available under LGPL-3.0/GPL-3.0/commercial terms;
this distribution uses the LGPL option where offered. Copies of the LGPL and
GPL licenses are in `licenses/`. Corresponding unmodified Qt source:
https://download.qt.io/archive/qt/6.9/6.9.1/single/qt-everywhere-src-6.9.1.tar.xz
and https://code.qt.io/cgit/qt/qtbase.git/tree/?h=v6.9.1 . Qt's source archives
contain individual third-party copyright notices and license texts. The build
is dynamically linked with MSVC; use Qt's documented build procedure with the
same toolchain to replace the Qt DLLs.

Core dependencies are nlohmann/json 3.11.3 (MIT, copyright Niels Lohmann) and
SQLite amalgamation 3.46.1 (public domain). JSON's license is included below in
the packaged licenses directory. SQLite dedication:
https://www.sqlite.org/copyright.html . Catch2 is a build/test dependency and
is not shipped in production. Compiler runtime DLLs are Microsoft's
redistributable runtime files, installed beside the executable.

The desktop setup host statically links miniz 3.1.2 for bounded ZIP extraction
(MIT license). Its complete license is installed as `licenses/miniz-LICENSE.txt`.
Pinned source: https://github.com/richgel999/miniz/tree/3.1.2 . The headless core
does not link this dependency. Separately downloaded resources retain their own
license texts and a local receipt of source, revision, size and SHA256.

The optional Discord plugin is MIT licensed and carries its own LICENSE.
Its Rust dependency notice inventory is tracked in the separate plugin
repository. No Discord credentials, private persona or history are bundled.
Model files are separately supplied by the operator. The Windows distribution
includes an unchanged llama.cpp b10655 runtime in `bin/llama.cpp.localcode/`,
with its MIT LICENSE and upstream commit recorded in manifest.json. CPU backends,
LLVM OpenMP, Microsoft compiler redistributables, and the available CUDA runtime
libraries are included beside the server. LLVM/OpenMP and the NVIDIA CUDA 13.3
EULA, supplement and attachments have separate notice files in that directory;
their component terms apply. The CUDA notice source is
https://docs.nvidia.com/cuda/archive/13.3.0/eula/index.html . CUDA GPU use needs a
compatible NVIDIA GPU and driver. This package makes no custom optimization claim.
Custom runtimes retain their own licenses. The optional RAM runtime component
contains a modified llama.cpp core DLL (MIT, copyright the ggml authors), its
source patch and compiler redistributables. It is used only to prepare an
explicitly selected compatible runtime copy. Its LICENSE, source revision,
digests and patch are in `bin/runtime-memory/`. Corresponding original source:
https://github.com/ggml-org/llama.cpp/tree/cb300598d5f90189cb69d2702f4930aaf99d32a2 .
No model weights or GPU libraries are bundled with that component.

Deployment uses the official Qt 6.9 deployment script:
https://doc.qt.io/archives/qt-6.9/qt-generate-deploy-app-script.html .
