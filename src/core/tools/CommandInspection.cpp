#include "CommandInspection.h"

#include <algorithm>
#include <regex>

#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {

// Names the control character that a JSON escape left behind, or empty when
// the text is clean.
//
// A filename cannot contain any of these, so their presence is not ambiguous:
// the argument was written into a JSON string with single backslashes, and the
// parser read the backslash and the letter after it as one escape.
std::string escape_damage(std::string_view text) {
    for (const unsigned char c : text) {
        switch (c) {
            case 0x0c: return "a form feed, which is what \\f becomes";
            case 0x08: return "a backspace, which is what \\b becomes";
            case 0x0b: return "a vertical tab, which is what \\v becomes";
            case 0x0d: return "a carriage return, which is what \\r becomes";
            case 0x0a: return "a line break, which is what \\n becomes";
            case 0x09: return "a tab, which is what \\t becomes";
            default: break;
        }
    }

    // The other shape the same mistake takes. `C:\Users\...` has no escape that
    // maps to a control character - \U is simply invalid - so the JSON comes
    // apart instead, and what arrives is wreckage like `C:", `. None of these
    // characters can appear in a Windows filename, so finding one is not a
    // judgement call.
    //
    // The drive colon is the exception: it is legal at position 1 and nowhere
    // else.
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ':' && i == 1) continue;
        switch (c) {
            case '"':  return "a double quote";
            case '<':  return "a '<'";
            case '>':  return "a '>'";
            case '|':  return "a '|'";
            case '?':  return "a '?'";
            case '*':  return "a '*'";
            case ':':  return "a colon somewhere other than after the drive letter";
            default: break;
        }
    }
    return {};
}

// The executables a build just produced, as the build tool itself named them.
//
// A model that has just built something then has to construct the path to it,
// and on Windows that means an absolute path full of backslashes going into a
// JSON string - which is where two separate runs died. It does not have to
// guess: MSBuild prints "fibonacci.vcxproj -> C:\...\fibonacci.exe" and the
// Makefile and Ninja generators print "Linking CXX executable app.exe". Handing
// that back, with forward slashes, removes the step that was failing.
bool command_selects_msvc_compiler(const std::vector<std::string>& argv) {
    for (const auto& argument : argv) {
        const auto lowered = strings::to_lower(argument);
        if (lowered.find("-dcmake_cxx_compiler") == std::string::npos &&
            lowered.find("-dcmake_c_compiler") == std::string::npos) {
            continue;
        }
        // The value may be joined with = or, when cmake is given -D and the
        // definition separately, be the whole argument.
        const auto equals = lowered.rfind('=');
        const auto value = equals == std::string::npos ? lowered : lowered.substr(equals + 1);
        if (strings::ends_with(value, "cl.exe") || strings::ends_with(value, R"(\cl)") ||
            strings::ends_with(value, "/cl")) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> executables_produced(std::string_view output,
                                              const std::filesystem::path& working_directory,
                                              const std::filesystem::path& build_directory) {
    static const std::regex msbuild{R"(->\s+(\S.*\.(?:exe|dll)))", std::regex::icase};
    static const std::regex linking{R"(Linking\s+\w+\s+executable\s+(.+)$)", std::regex::icase};

    std::vector<std::string> found;
    for (const auto& line : strings::split_lines(output)) {
        std::smatch match;
        std::string path;
        if (std::regex_search(line, match, msbuild)) {
            path = strings::trim(match[1].str());
        } else if (std::regex_search(line, match, linking)) {
            path = strings::trim(match[1].str());
        }
        if (path.empty()) continue;

        // Relative to the workspace where that is shorter and clearer, and
        // always with forward slashes, which need no escaping anywhere.
        std::error_code ec;
        std::filesystem::path absolute{path};
        if (!absolute.is_absolute() && !build_directory.empty()) {
            absolute = (build_directory / absolute).lexically_normal();
            path = absolute.generic_string();
        }
        if (absolute.is_absolute() && !working_directory.empty()) {
            const auto relative = std::filesystem::relative(absolute, working_directory, ec);
            if (!ec && !relative.empty() &&
                relative.native().rfind(std::filesystem::path{".."}.native(), 0) != 0) {
                path = relative.string();
            }
        }
        std::replace(path.begin(), path.end(), '\\', '/');
        if (std::find(found.begin(), found.end(), path) == found.end()) found.push_back(path);
        if (found.size() >= 8) break;
    }
    return found;
}

std::vector<ResolvedResource> resources_produced(std::string_view output,
                                                 const std::filesystem::path& working_directory,
                                                 const std::filesystem::path& build_directory) {
    // Which build tool said so, so the answer can cite its source rather than
    // asserting a path from nowhere.
    const bool msbuild = strings::icontains(output, ".vcxproj ->");
    const char* source = msbuild ? "MSBuild link line" : "build tool link line";

    std::vector<ResolvedResource> resources;
    for (const auto& path : executables_produced(output, working_directory, build_directory)) {
        if (strings::ends_with(strings::to_lower(path), ".dll")) continue;
        ResolvedResource resource;
        resource.kind = "executable";
        resource.name = std::filesystem::path{path}.stem().string();
        resource.id = "build:" + resource.name;
        resource.path = path;
        resource.source = source;
        resource.recorded_at_unix_ms = now_unix_ms();
        resources.push_back(std::move(resource));
    }
    return resources;
}

}  // namespace localcode
