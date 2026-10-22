#include "PathOrigin.h"

#include <system_error>

#include "../util/Strings.h"
#include "../platform/PathEncoding.h"
#include "../workspace/Workspace.h"

namespace localcode {

std::string_view to_string(PathOrigin origin) noexcept {
    switch (origin) {
        case PathOrigin::WorkspaceRelative: return "workspace-relative";
        case PathOrigin::InsideWorkspace:   return "inside the workspace";
        case PathOrigin::ExistsOutside:     return "outside the workspace, but real";
        case PathOrigin::ResourceResolved:  return "a resource LocalCode resolved";
        case PathOrigin::Unsupported:       return "unsupported";
    }
    return "unsupported";
}

PathOrigin classify_path_origin(const Workspace& workspace, const std::string& input) {
    const auto asked = path_from_utf8(input);

    // On Windows "/scratchpad/x" is not is_absolute - it has a root directory
    // but no drive - and it is exactly the shape being caught here. Treating it
    // as relative would resolve it against the workspace and hide the mistake.
    const bool rooted = asked.is_absolute() || asked.has_root_directory();
    if (!rooted) return PathOrigin::WorkspaceRelative;

    if (workspace.resolved_resource(input)) return PathOrigin::ResourceResolved;

    std::error_code ec;
    if (std::filesystem::exists(asked, ec) && !ec) {
        return workspace.paths().evaluate(input, PathAccess::Read).inside_workspace
                   ? PathOrigin::InsideWorkspace
                   : PathOrigin::ExistsOutside;
    }

    // Does not exist. Inside the workspace that is an ordinary missing file -
    // creating one is the point of write_file - so it is not suspicious.
    const auto decision = workspace.paths().evaluate(input, PathAccess::Read);
    if (decision.inside_workspace) return PathOrigin::InsideWorkspace;

    return PathOrigin::Unsupported;
}

std::string unsupported_path_note(const Workspace& workspace, const std::string& input) {
    if (classify_path_origin(workspace, input) != PathOrigin::Unsupported) return {};

    const auto asked = path_from_utf8(input);
    const auto leaf = path_to_utf8(asked.filename());

    std::string note = "UNRESOLVED ABSOLUTE PATH\n\n";
    note += "  asked for:       " + input + "\n";
    note += "  workspace root:  " + path_to_utf8(workspace.root()) + "\n";
    note +=
        "\nThat path is not in this workspace, is not on this machine, and is not "
        "anything LocalCode has resolved - so nothing supports it existing. It has "
        "not been rewritten to something else, because guessing which path was meant "
        "would hide the mistake.\n";

    if (!leaf.empty() && leaf != "." && leaf != "..") {
        note += "\nFor a file in this project, name it relative to the root: " + leaf + "\n";
    }

    // The resources it could have named instead, when there are any. This is
    // the half that makes the message actionable rather than merely correct.
    const auto resources = workspace.resolved_resources();
    if (!resources.empty()) {
        note += "\nAlready resolved here, and usable by name:\n";
        for (const auto& resource : resources) {
            note += "  " + resource.id + "   -> " + resource.path + "\n";
        }
    }
    note += "\nlist_directory with no arguments shows what is actually there.\n";
    return note;
}

}  // namespace localcode
