#include "Languages.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace localcode::repo {
namespace {

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains_segment(std::string_view path, std::string_view segment) {
    // Matches a whole path component, so "src" does not match "srcgen" and
    // "test" does not match "latest".
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto piece = path.substr(start, slash == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : slash - start);
        if (piece == segment) return true;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return false;
}

const std::unordered_map<std::string, std::string>& extension_languages() {
    static const std::unordered_map<std::string, std::string> map = {
        {".c", "c"},         {".h", "c/c++ header"},
        {".cpp", "c++"},     {".cc", "c++"},      {".cxx", "c++"},
        {".hpp", "c++"},     {".hh", "c++"},      {".hxx", "c++"},   {".ipp", "c++"},
        {".rs", "rust"},
        {".py", "python"},   {".pyi", "python"},
        {".js", "javascript"}, {".mjs", "javascript"}, {".cjs", "javascript"},
        {".jsx", "javascript"},
        {".ts", "typescript"}, {".tsx", "typescript"}, {".mts", "typescript"},
        {".cs", "c#"},
        {".java", "java"},   {".kt", "kotlin"},   {".kts", "kotlin"},
        {".go", "go"},
        {".asm", "assembly"}, {".s", "assembly"}, {".inc", "assembly"},
        {".rb", "ruby"},     {".php", "php"},     {".swift", "swift"},
        {".lua", "lua"},     {".sh", "shell"},    {".bash", "shell"},
        {".ps1", "powershell"}, {".psm1", "powershell"},
        {".bat", "batch"},   {".cmd", "batch"},
        {".sql", "sql"},
        {".cmake", "cmake"},
        {".md", "markdown"}, {".markdown", "markdown"}, {".rst", "restructuredtext"},
        {".txt", "text"},
        {".json", "json"},   {".yaml", "yaml"},   {".yml", "yaml"},
        {".toml", "toml"},   {".ini", "ini"},     {".cfg", "ini"},
        {".xml", "xml"},     {".html", "html"},   {".htm", "html"},
        {".css", "css"},     {".scss", "css"},
    };
    return map;
}

const std::unordered_set<std::string>& binary_extensions() {
    static const std::unordered_set<std::string> set = {
        ".exe", ".dll", ".so", ".dylib", ".lib", ".a", ".o", ".obj", ".pdb", ".ilk",
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".svg", ".webp", ".tiff",
        ".mp3", ".wav", ".ogg", ".mp4", ".mkv", ".avi", ".mov",
        ".zip", ".gz", ".tar", ".7z", ".rar", ".xz", ".bz2",
        ".pdf", ".docx", ".xlsx", ".pptx",
        ".gguf", ".safetensors", ".bin", ".pt", ".pth", ".onnx", ".ckpt",
        ".ttf", ".otf", ".woff", ".woff2",
        ".db", ".sqlite", ".sqlite3", ".pyc", ".class", ".jar", ".wasm",
    };
    return set;
}

}  // namespace

std::string file_name_of(std::string_view relative_path) {
    const auto slash = relative_path.find_last_of("/\\");
    return lower(slash == std::string_view::npos ? relative_path
                                                 : relative_path.substr(slash + 1));
}

std::string extension_of(std::string_view relative_path) {
    const auto name = file_name_of(relative_path);
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    return name.substr(dot);
}

std::string language_of(std::string_view relative_path) {
    const auto name = file_name_of(relative_path);

    // Named files whose extension says nothing.
    if (name == "cmakelists.txt") return "cmake";
    if (name == "makefile" || name == "gnumakefile") return "make";
    if (name == "dockerfile") return "dockerfile";

    const auto& map = extension_languages();
    const auto it = map.find(extension_of(relative_path));
    return it == map.end() ? std::string{} : it->second;
}

bool is_indexable_text(std::string_view relative_path) {
    const auto extension = extension_of(relative_path);
    if (binary_extensions().count(extension) != 0) return false;

    // No extension: a few are worth reading, the rest are not worth guessing at.
    if (extension.empty()) {
        const auto name = file_name_of(relative_path);
        static const std::unordered_set<std::string> known = {
            "makefile", "gnumakefile", "dockerfile", "license", "licence", "readme",
            "notice", "authors", "changelog", "copying", ".gitignore", ".gitattributes",
        };
        return known.count(name) != 0;
    }
    return true;
}

bool is_generated_directory(std::string_view name) {
    static const std::unordered_set<std::string> set = {
        "build", "builds", "out", "bin", "obj", "dist", "target", "_build",
        "cmake-build-debug", "cmake-build-release", "__pycache__", ".pytest_cache",
        ".mypy_cache", ".gradle", ".next", ".nuxt", ".parcel-cache", ".turbo",
        "coverage", "htmlcov", ".tox", ".venv", "venv", "env", ".idea", ".vs",
        ".vscode", "debug", "release", "x64", "x86", ".cache", ".ccache",
    };
    return set.count(lower(name)) != 0;
}

bool is_vendored_directory(std::string_view name) {
    static const std::unordered_set<std::string> set = {
        "node_modules", "third_party", "thirdparty", "vendor", "vendored",
        "external", "externals", "_deps", "packages", "site-packages", "bower_components",
    };
    return set.count(lower(name)) != 0;
}

bool is_manifest(std::string_view relative_path) {
    const auto name = file_name_of(relative_path);
    static const std::unordered_set<std::string> set = {
        "cmakelists.txt", "cargo.toml", "package.json", "pyproject.toml", "setup.py",
        "setup.cfg", "requirements.txt", "pipfile", "go.mod", "pom.xml",
        "build.gradle", "build.gradle.kts", "settings.gradle", "makefile", "gnumakefile",
        "meson.build", "build.zig", "composer.json", "gemfile", "pubspec.yaml",
        "conanfile.txt", "conanfile.py", "vcpkg.json", "cmakepresets.json",
    };
    if (set.count(name) != 0) return true;

    const auto extension = extension_of(relative_path);
    return extension == ".vcxproj" || extension == ".csproj" || extension == ".sln" ||
           extension == ".fsproj" || extension == ".pro" || extension == ".cabal";
}

std::string build_system_of(std::string_view relative_path) {
    const auto name = file_name_of(relative_path);
    const auto extension = extension_of(relative_path);

    if (name == "cmakelists.txt" || name == "cmakepresets.json" || extension == ".cmake") {
        return "CMake";
    }
    if (name == "cargo.toml") return "Cargo";
    if (name == "makefile" || name == "gnumakefile") return "Make";
    if (name == "meson.build") return "Meson";
    if (name == "build.zig") return "Zig";
    if (name == "pom.xml") return "Maven";
    if (name == "build.gradle" || name == "build.gradle.kts") return "Gradle";
    if (name == "go.mod") return "Go modules";
    if (extension == ".sln" || extension == ".vcxproj" || extension == ".csproj") {
        return "MSBuild";
    }
    if (name == "pyproject.toml" || name == "setup.py") return "Python packaging";
    if (name == "package.json") return "npm scripts";
    return {};
}

std::string package_manager_of(std::string_view relative_path) {
    const auto name = file_name_of(relative_path);
    if (name == "package-lock.json") return "npm";
    if (name == "pnpm-lock.yaml") return "pnpm";
    if (name == "yarn.lock") return "yarn";
    if (name == "cargo.lock" || name == "cargo.toml") return "cargo";
    if (name == "requirements.txt" || name == "pipfile" || name == "poetry.lock") return "pip";
    if (name == "go.sum" || name == "go.mod") return "go";
    if (name == "vcpkg.json") return "vcpkg";
    if (name == "conanfile.txt" || name == "conanfile.py") return "conan";
    if (name == "packages.config") return "nuget";
    return {};
}

FileRole role_of(std::string_view relative_path, std::string_view language) {
    const auto path = lower(relative_path);
    const auto name = file_name_of(relative_path);

    // Location first: a source file inside node_modules is not this project's
    // source, whatever it looks like.
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto piece = path.substr(start, slash == std::string::npos ? std::string::npos
                                                                         : slash - start);
        if (!piece.empty() && slash != std::string::npos) {
            if (is_vendored_directory(piece)) return FileRole::Vendored;
            if (is_generated_directory(piece)) return FileRole::Generated;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    if (is_manifest(relative_path)) return FileRole::Build;

    if (language == "markdown" || language == "restructuredtext" ||
        name == "readme" || name == "license" || name == "licence" ||
        name == "changelog" || name == "notice") {
        return FileRole::Documentation;
    }

    if (contains_segment(path, "test") || contains_segment(path, "tests") ||
        contains_segment(path, "spec") || contains_segment(path, "specs") ||
        contains_segment(path, "__tests__") ||
        name.rfind("test_", 0) == 0 || name.rfind("test-", 0) == 0 ||
        name.find("_test.") != std::string::npos ||
        name.find(".test.") != std::string::npos ||
        name.find(".spec.") != std::string::npos ||
        name.find("_spec.") != std::string::npos) {
        return FileRole::Test;
    }

    if (language == "json" || language == "yaml" || language == "toml" ||
        language == "ini" || language == "xml") {
        return FileRole::Config;
    }
    if (language == "text") return FileRole::Documentation;
    if (!language.empty()) return FileRole::Source;
    return FileRole::Unknown;
}

}  // namespace localcode::repo
