#pragma once
// ---------------------------------------------------------------------------
// Reading a command's arguments and its output for things worth saying.
//
// Both functions here exist because of the same failure, met twice. A model
// builds a program, then has to construct the path to it, and on Windows that
// means an absolute path full of backslashes going into a JSON string. The
// backslashes are escapes: \f becomes a form feed, \U is not valid at all. The
// path arrives as wreckage, LocalCode said "executable not found", and the
// model - whose program was exactly where it thought it was - rebuilt instead
// of running it until the no-progress detector stopped the run.
//
// So: recognise the wreckage rather than calling it a missing file, and hand
// back the path the build tool already printed so it never has to be
// reconstructed.
// ---------------------------------------------------------------------------
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "../workspace/Workspace.h"

namespace localcode {

// Names what a JSON escape left behind, or empty when the text is clean.
//
// Two shapes: a control character, which no filename can contain, and a
// character that is illegal in a Windows filename, which is what turns up when
// the escape was invalid and the JSON came apart instead.
[[nodiscard]] std::string escape_damage(std::string_view text);

// Does this command line itself name an MSVC compiler?
//
// `cmake ... -DCMAKE_CXX_COMPILER=...\cl.exe` is the model stating the
// toolchain outright. Supplying the environment that compiler needs cannot
// change a choice that is written on the command line - it only lets the
// chosen compiler work, the same reasoning that already applies to a build
// tree whose cache names cl.exe.
//
// It matters because naming the compiler is not enough on its own: the link
// step runs rc.exe and mt.exe from the Windows SDK, neither is on PATH, and
// the configure fails at "RC Pass 1 ... no such file or directory" after
// looking as though it had got further.
[[nodiscard]] bool command_selects_msvc_compiler(const std::vector<std::string>& argv);

// The executables a build just produced, as the build tool itself named them:
// MSBuild's "app.vcxproj -> C:\...\app.exe" and the Makefile and Ninja
// generators' "Linking CXX executable app.exe".
//
// Returned relative to `working_directory` when that is shorter, and always
// with forward slashes, which need no escaping anywhere.
[[nodiscard]] std::vector<std::string> executables_produced(
    std::string_view output, const std::filesystem::path& working_directory,
    const std::filesystem::path& build_directory = {});

// The same thing as addressable resources: id, name and the tool that said so,
// ready to be recorded on the workspace and run by name.
[[nodiscard]] std::vector<ResolvedResource> resources_produced(
    std::string_view output, const std::filesystem::path& working_directory,
    const std::filesystem::path& build_directory = {});

}  // namespace localcode
