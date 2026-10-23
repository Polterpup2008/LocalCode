// ---------------------------------------------------------------------------
// localcode-doctor - headless diagnostics and a headless agent.
//
// Everything the GUI relies on lives in localcode_core, so it can all be
// exercised without a display. This is the tool to run when a model will not
// load, a command is unexpectedly refused, or a build machine needs checking -
// and it is how the end-to-end agent run is exercised outside the GUI.
//
//   localcode-doctor env                    development environment report
//   localcode-doctor gguf <file>            GGUF header + inferred capabilities
//   localcode-doctor backends               discovered inference runtimes
//   localcode-doctor path <ws> <p> <access> path policy decision
//   localcode-doctor cmd <ws> <command>     command policy decision
//   localcode-doctor run <ws> <exe> [args]  run a command through the policy
//   localcode-doctor skills [dir]           list discovered skills
//   localcode-doctor agent <ws> <model.gguf> <prompt>
//                                           run the whole agent loop headlessly
// ---------------------------------------------------------------------------
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

#if defined(_WIN32)
// shellapi.h depends on the Windows declarations; keep this order.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include "Application.h"
#include "model/GgufMetadata.h"
#include "model/ModelProfile.h"
#include "platform/Environment.h"
#include "platform/PathEncoding.h"
#include "platform/Paths.h"
#include "platform/Process.h"
#include "repo/RepoIndex.h"
#include "repo/RepoQuery.h"
#include "security/CommandPolicy.h"
#include "security/PathPolicy.h"
#include "skills/SkillRegistry.h"
#include "util/Log.h"
#include "util/Strings.h"
#include "workspace/Workspace.h"

using namespace localcode;

namespace {

std::vector<std::string> command_line_arguments(int argc, char** argv) {
    std::vector<std::string> result;
#if defined(_WIN32)
    // The narrow CRT argv uses the process code page, which loses characters
    // and can produce invalid UTF-8 in JSON. Read the original Unicode command
    // line and use the same UTF-8 boundary as the GUI and tool protocol.
    int count = 0;
    auto* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) throw std::runtime_error("cannot read the Unicode command line");
    const auto release = [](wchar_t** args) { LocalFree(args); };
    const std::unique_ptr<wchar_t*, decltype(release)> owned(wide, release);
    for (int i = 0; i < count; ++i) {
        const int length = static_cast<int>(std::wcslen(wide[i]));
        if (!length) {
            result.emplace_back();
            continue;
        }
        const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], length,
                                               nullptr, 0, nullptr, nullptr);
        if (!needed) throw std::runtime_error("command line contains invalid Unicode");
        std::string text(static_cast<std::size_t>(needed), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], length, text.data(),
                                 needed, nullptr, nullptr))
            throw std::runtime_error("cannot encode the command line as UTF-8");
        result.push_back(std::move(text));
    }
#else
    for (int i = 0; i < argc; ++i)
        result.emplace_back(argv[i]);
#endif
    return result;
}

// A Ctrl+C during an agent run should cancel the run and shut the model server
// down, not kill the process and leave it holding VRAM.
std::atomic<CancellationSource*> g_active_cancel{nullptr};

void handle_interrupt(int) {
    if (auto* source = g_active_cancel.load()) source->cancel();
}

int usage() {
    std::cout << "localcode-doctor - diagnostics for LocalCode\n\n"
                 "  env                          development environment report\n"
                 "  gguf <file>                  GGUF header and inferred capabilities\n"
                 "  backends                     discovered llama.cpp / ik_llama.cpp runtimes\n"
                 "  path <workspace> <path> <read|write|create|delete>\n"
                 "  cmd <workspace> <command...> command policy decision\n"
                 "  run <workspace> <exe> [args...]\n"
                 "  skills [directory]           discovered skills\n"
                 "  models                       models in the library\n"
                 "  models add <file|dir>...     import a GGUF, or every GGUF in a folder\n"
                 "  config                       paths used to find runtimes and data\n"
                 "  config set <key> <value>     change one of them\n"
                 "  repo <workspace> [status|refresh|full|bench]\n"
                 "  repo <workspace> [symbol|refs|search] <term>\n"
                 "                               index the project and report what it knows\n"
                 "  agent <workspace> <model.gguf> <prompt>\n"
                 "                               run the agent loop against a real model\n"
                 "\nOptions for `agent`:\n"
                 "  --backend <auto|llama.cpp|ik_llama.cpp>\n"
                 "  --server <path>              use this server binary\n"
                 "  --ctx <tokens>               context size (default: from the model)\n"
                 "  --mode <auto|chat|code>      greeting detection, conversation or full agent\n"
                 "  --gpu-layers <n>             -1 for all (default)\n"
                 "  --max-steps <n>              agent iteration budget (default 30)\n"
                 "  --time-budget <minutes>      task wall-clock budget (1 to 480)\n"
                 "  --yes                        approve every confirmation prompt\n"
                 "  --read-only                  inspect without modifying anything\n"
                 "  --no-repo                    run without the repository index\n"
                 "  --no-repo-refresh            use the stored index without updating it\n"
                 "\nEnvironment:\n"
                 "  LOCALCODE_LLAMA_SERVER       path to a llama.cpp server binary\n"
                 "  LOCALCODE_IK_LLAMA_SERVER    path to an ik_llama.cpp server binary\n"
                 "  LOCALCODE_HOME               relocate all application data\n";
    return 2;
}

void print_row(std::string_view key, std::string_view value, std::size_t width = 25) {
    std::cout << "  " << key;
    for (std::size_t i = key.size(); i < width; ++i)
        std::cout << ' ';
    std::cout << value << "\n";
}

void print_heading(std::string_view text) {
    std::cout << "\n" << text << "\n" << std::string(text.size(), '-') << "\n";
}

int command_env() {
    EnvironmentService service;
    const auto& report = service.report();

    std::cout << "Operating system\n";
    print_row("name", report.os.name);
    print_row("version", report.os.version);
    print_row("architecture", report.os.architecture);
    print_row("logical CPUs", std::to_string(report.os.logical_cpus));
    print_row("memory", strings::human_bytes(report.os.total_memory_bytes));

    print_heading("Tools");
    for (const auto& tool : report.tools) {
        std::string value;
        if (tool.found) {
            value = path_to_utf8(tool.path);
            if (!tool.version.empty()) value += "   [" + tool.version + "]";
            if (tool.source != "PATH") value += "   (" + tool.source + ")";
        } else {
            value = "not found";
        }
        print_row(tool.id, value);
    }

    if (!report.visual_studio.empty()) {
        print_heading("Visual Studio");
        for (const auto& vs : report.visual_studio) {
            print_row(vs.version, vs.display_name + "  " + path_to_utf8(vs.install_path));
            if (!vs.msvc_toolsets.empty()) {
                print_row("  toolsets", strings::join(vs.msvc_toolsets, ", "));
            }
            if (!vs.msbuild.empty()) print_row("  msbuild", path_to_utf8(vs.msbuild));
        }
    }

    if (!report.windows_sdks.empty()) {
        print_heading("Windows SDKs");
        for (const auto& sdk : report.windows_sdks) {
            print_row(sdk.version, path_to_utf8(sdk.include_dir));
        }
    }

    if (!report.notable_variables.empty()) {
        print_heading("Environment variables");
        for (const auto& [name, value] : report.notable_variables)
            print_row(name, value);
    }
    return 0;
}

int command_gguf(const std::string& file) {
    auto metadata = read_gguf_metadata(path_from_utf8(file));
    if (!metadata) {
        std::cerr << "error: " << metadata.error().to_string() << "\n";
        return 1;
    }

    std::cout << "GGUF header\n";
    for (const auto& [key, value] : metadata->summary())
        print_row(key, value);

    const auto capabilities = ModelCapabilities::infer(*metadata);
    print_heading("Inferred capabilities");
    for (const auto& [key, value] : capabilities.describe())
        print_row(key, value);

    if (!metadata->chat_template.empty()) {
        print_heading("Chat template (first 400 characters)");
        std::cout << strings::utf8_truncate(metadata->chat_template, 400) << "\n";
    }

    const auto profile = ModelProfile::from_gguf(*metadata);
    print_heading("Suggested profile");
    print_row("id", profile.id);
    print_row("display name", profile.display_name);
    print_row("context size", std::to_string(profile.context_size));
    print_row("gpu layers", profile.gpu_layers < 0 ? "all" : std::to_string(profile.gpu_layers));
    print_row("backend", std::string{to_string(profile.backend)});
    print_row("tool protocol", std::string{to_string(profile.tool_protocol)});
    return 0;
}

int command_backends(Application& application) {
    auto runtimes = application.backends().discover_runtimes(true);
    if (runtimes.empty()) {
        std::cout << "No llama.cpp or ik_llama.cpp server was found.\n\n"
                     "Set a path in Settings, set LOCALCODE_LLAMA_SERVER, put llama-server on\n"
                     "PATH, or place a build under\n"
                  << path_to_utf8(app_paths().runtime_dir() / "llama.cpp") << "\n";
        return 1;
    }

    std::cout << runtimes.size() << " runtime(s) found\n";
    for (const auto& runtime : runtimes) {
        print_heading(std::string{to_string(runtime.kind)});
        print_row("binary", path_to_utf8(runtime.server_binary));
        print_row("source", runtime.source);
        if (!runtime.version.empty()) print_row("version", runtime.version);

        std::vector<std::string> features;
        if (runtime.features.jinja) features.emplace_back("--jinja");
        if (runtime.features.reasoning_format) features.emplace_back("--reasoning-format");
        if (runtime.features.flash_attention) {
            features.emplace_back(runtime.features.flash_attention_takes_value
                                      ? "--flash-attn <value>"
                                      : "--flash-attn");
        }
        if (runtime.features.cache_type_k) features.emplace_back("--cache-type-k");
        if (runtime.features.no_webui) features.emplace_back("--no-webui");
        print_row("supports",
                  features.empty() ? "(nothing recognised)" : strings::join(features, ", "));
          const auto devices = ModelServer::probe_devices(runtime.server_binary, runtime.features);
          if (devices && devices->supported) {
              print_row("GPU devices", devices->devices.empty() ? "none enumerated" : strings::join(devices->devices, "; "));
              if (!devices->succeeded) print_row("GPU probe", devices->detail);
          } else print_row("GPU devices", "enumeration unsupported or unavailable; acceleration unconfirmed");
    }
    return 0;
}

int command_path(const std::string& workspace, const std::string& path, const std::string& mode) {
    PathAccess access = PathAccess::Read;
    if (mode == "write")
        access = PathAccess::Write;
    else if (mode == "create")
        access = PathAccess::Create;
    else if (mode == "delete")
        access = PathAccess::Delete;
    else if (mode != "read") {
        std::cerr << "unknown access mode: " << mode << "\n";
        return 2;
    }

    PathPolicy policy{path_from_utf8(workspace)};
    const auto decision = policy.evaluate(path, access);
    print_row("workspace", path_to_utf8(policy.workspace_root()));
    print_row("input", path);
    print_row("access", std::string{to_string(access)});
    print_row("decision", decision.allowed ? "ALLOWED" : "REFUSED");
    print_row("canonical", path_to_utf8(decision.canonical));
    print_row("inside workspace", decision.inside_workspace ? "yes" : "no");
    if (!decision.relative.empty()) print_row("relative", path_to_utf8(decision.relative));
    if (!decision.reason.empty()) print_row("reason", decision.reason);
    return decision.allowed ? 0 : 1;
}

int command_cmd(const std::string& workspace, const std::vector<std::string>& parts) {
    PathPolicy paths{path_from_utf8(workspace)};
    CommandPolicy policy;
    policy.set_path_policy(&paths);

    const auto line = strings::join(parts, " ");
    const auto evaluation = policy.evaluate_shell(line);
    print_row("workspace", path_to_utf8(paths.workspace_root()));
    print_row("command", line);
    print_row("decision", strings::to_upper(std::string{to_string(evaluation.decision)}));
    print_row("rule", evaluation.rule_id);
    print_row("reason", evaluation.reason);
    if (!evaluation.matched_text.empty()) print_row("matched", evaluation.matched_text);
    return evaluation.decision == CommandDecision::Allow ? 0 : 1;
}

int command_run(const std::string& workspace, const std::vector<std::string>& parts) {
    if (parts.empty()) return usage();

    PathPolicy paths{path_from_utf8(workspace)};
    CommandPolicy policy;
    policy.set_path_policy(&paths);

    ProcessRequest request;
    // resolve_executable, not find_executable: this diagnostic must agree with
    // what the run_command tool would actually do.
    auto resolved = resolve_executable(parts[0], path_from_utf8(workspace));
    if (!resolved) {
        std::cerr << "error: executable not found: " << parts[0] << "\n";
        return 1;
    }
    request.executable = *resolved;
    request.arguments.assign(parts.begin() + 1, parts.end());
    request.working_directory = paths.workspace_root();
    request.timeout = std::chrono::seconds(120);

    // The tool supplies the MSVC developer environment for the compiler and its
    // friends; this diagnostic has to do the same or it diagnoses something
    // other than what happens in a run.
    if (needs_msvc_environment(request.executable)) {
        const auto& developer = msvc_environment();
        for (const auto& [name, value] : developer) {
            request.environment.push_back({name, value});
        }
        print_row("msvc environment",
                  developer.empty() ? "NOT captured - the compiler will run without INCLUDE and LIB"
                                    : std::to_string(developer.size()) + " variables applied");
    }

    const auto evaluation = policy.evaluate(request);
    print_row("decision", strings::to_upper(std::string{to_string(evaluation.decision)}));
    print_row("rule", evaluation.rule_id);
    if (evaluation.decision == CommandDecision::Block) {
        std::cerr << "blocked: " << evaluation.reason << "\n";
        return 1;
    }

    auto result = ProcessRunner::run(request);
    if (!result) {
        std::cerr << "error: " << result.error().to_string() << "\n";
        return 1;
    }
    print_row("command", result->display_command);
    print_row("exit code", std::to_string(result->exit_code));
    print_row("duration", strings::human_duration_ms(result->duration_ms));
    print_row("stdout bytes", std::to_string(result->stdout_total_bytes));
    print_row("stderr bytes", std::to_string(result->stderr_total_bytes));
    if (result->timed_out) print_row("timed out", "yes");
    std::cout << "\n--- stdout ---\n" << result->stdout_text << "\n";
    if (!result->stderr_text.empty()) {
        std::cout << "--- stderr ---\n" << result->stderr_text << "\n";
    }
    return result->exit_code;
}

// `config` shows the paths that decide where runtimes and data are found, and
// sets them. Without this the only way to point LocalCode at a runtime folder is
// the settings dialog, which is no use on a build machine - or when the very
// thing being diagnosed is that no runtime was found.
int command_config(const std::vector<std::string>& rest) {
    Application application;
    if (auto status = application.initialise(); !status) {
        std::cerr << "error: " << status.error().to_string() << "\n";
        return 1;
    }

    auto settings = application.settings();

    if (!rest.empty() && rest[0] == "set") {
        if (rest.size() < 3) {
            std::cerr << "error: usage: config set <key> <value>\n";
            return 2;
        }
        const auto& key = rest[1];
        const auto& value = rest[2];

        if (key == "runtime-dir")
            settings.runtime_directory = value;
        else if (key == "llama-server") {
            settings.llama_server_path = value;
            settings.use_bundled_llama = value.empty();
        } else if (key == "ik-llama-server")
            settings.ik_llama_server_path = value;
        else if (key == "data-dir")
            settings.data_directory = value;
        else if (key == "cache-dir")
            settings.cache_directory = value;
        else {
            std::cerr << "error: unknown key '" << key
                      << "'. Known keys: runtime-dir, llama-server, ik-llama-server, "
                         "data-dir, cache-dir\n";
            return 2;
        }

        if (auto status = application.apply_settings(settings); !status) {
            std::cerr << "error: " << status.error().to_string() << "\n";
            return 1;
        }
        std::cout << "  " << key << " = " << value << "\n";
        return 0;
    }

    print_heading("Configuration");
    print_row("runtime-dir", settings.runtime_directory);
    print_row("llama-server", settings.llama_server_path);
    print_row("ik-llama-server", settings.ik_llama_server_path);
    print_row("data-dir", settings.data_directory);
    print_row("cache-dir", settings.cache_directory);
    std::cout << "\n  Set one with:  localcode-doctor config set <key> <value>\n";
    return 0;
}

// `models` lists the library; `models add` imports files or whole folders.
//
// Importing used to be possible only as a side effect of starting an agent run,
// which meant loading a model just to register it.
int command_models(const std::vector<std::string>& rest) {
    Application application;
    if (auto status = application.initialise(); !status) {
        std::cerr << "error: " << status.error().to_string() << "\n";
        return 1;
    }

    if (!rest.empty() && rest[0] == "add") {
        if (rest.size() < 2) {
            std::cerr << "error: give at least one .gguf file or a folder to scan\n";
            return 2;
        }

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            const auto target = path_from_utf8(rest[i]);
            if (std::filesystem::is_directory(target, ec)) {
                for (const auto& entry :
                     std::filesystem::recursive_directory_iterator(target, ec)) {
                    if (entry.is_regular_file(ec) && entry.path().extension() == ".gguf") {
                        files.push_back(entry.path());
                    }
                }
            } else {
                files.push_back(target);
            }
        }
        if (files.empty()) {
            std::cerr << "error: no .gguf files found\n";
            return 1;
        }
        std::sort(files.begin(), files.end());

        int imported = 0;
        int failed = 0;
        for (const auto& file : files) {
            auto profile = application.import_model(file);
            if (!profile) {
                std::cout << "  FAILED  " << path_to_utf8(file.filename()) << "\n"
                          << "          " << profile.error().to_string() << "\n";
                ++failed;
                continue;
            }
            application.save_model_profile(*profile);
            std::cout << "  added   " << profile->display_name << "\n"
                      << "          " << profile->architecture << ", context "
                      << profile->context_size << ", tools "
                      << (profile->capabilities.supports_tools ? "yes" : "no") << ", reasoning "
                      << (profile->capabilities.supports_native_reasoning ? "yes" : "no") << "\n";
            ++imported;
        }
        std::cout << "\n" << imported << " imported";
        if (failed > 0) std::cout << ", " << failed << " failed";
        std::cout << "\n";
        return failed > 0 ? 1 : 0;
    }

    auto profiles = application.model_profiles();
    if (!profiles) {
        std::cerr << "error: " << profiles.error().to_string() << "\n";
        return 1;
    }
    if (profiles->empty()) {
        std::cout << "No models in the library. Add one with:\n"
                     "  localcode-doctor models add <file.gguf|folder>\n";
        return 0;
    }

    print_heading("Models");
    for (const auto& profile : *profiles) {
        std::cout << "  " << profile.display_name << "\n";
        print_row("id", profile.id);
        print_row("file", path_to_utf8(profile.model_path));
        print_row("architecture", profile.architecture);
        print_row("context", std::to_string(profile.context_size));
        print_row("tools", profile.capabilities.supports_tools ? "yes" : "no");
        print_row("reasoning", profile.capabilities.supports_native_reasoning ? "yes" : "no");
        std::cout << "\n";
    }
    return 0;
}

// --- repo ------------------------------------------------------------------

namespace {

std::string thousands(std::uint64_t value) {
    auto text = std::to_string(value);
    for (int position = static_cast<int>(text.size()) - 3; position > 0; position -= 3) {
        text.insert(static_cast<std::size_t>(position), ",");
    }
    return text;
}

std::int64_t millis_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

std::uint64_t directory_size(const std::filesystem::path& directory) {
    std::uint64_t total = 0;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (entry.is_regular_file(code)) total += entry.file_size(code);
    }
    return total;
}

void print_stats(const repo::IndexStats& stats) {
    std::cout << "  scanned " << stats.files_scanned << ", reindexed " << stats.files_reindexed
              << ", added " << stats.files_added << ", removed " << stats.files_removed
              << ", unchanged " << stats.files_unchanged << "\n"
              << "  symbols " << thousands(stats.symbols) << ", facts dropped "
              << stats.facts_invalidated << ", " << stats.duration_ms << " ms"
              << (stats.cancelled ? " (cancelled)" : "") << "\n";
}

void print_overview(const repo::RepoOverview& map) {
    print_heading("Overview");
    std::string languages;
    for (std::size_t i = 0; i < map.languages.size() && i < 8; ++i) {
        if (i != 0) languages += ", ";
        languages +=
            map.languages[i].language + " (" + std::to_string(map.languages[i].files) + ")";
    }
    print_row("languages", languages);
    print_row("build systems", strings::join(map.build_systems, ", "));
    print_row("package managers", strings::join(map.package_managers, ", "));
    print_row("source roots", strings::join(map.source_roots, ", "));
    print_row("test roots", strings::join(map.test_roots, ", "));
    print_row("entry points", strings::join(map.entry_points, ", "));
    print_row("files indexed", thousands(map.indexed_files));
    print_row("files skipped", thousands(map.skipped_files));
    print_row("lines", thousands(map.total_lines));
    print_row("symbols", thousands(map.symbol_count));
    if (map.truncated) {
        std::cout << "  NOTE: the scan hit its limit, so these are a floor, not a total.\n";
    }
}

}   // namespace

int command_repo(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return usage();
    const auto root = std::filesystem::absolute(path_from_utf8(arguments[0]));
    if (!std::filesystem::is_directory(root)) {
        std::cerr << "error: not a directory: " << path_to_utf8(root) << "\n";
        return 1;
    }
    const std::string action = arguments.size() > 1 ? arguments[1] : "status";

    Workspace workspace;
    if (auto status = workspace.open(root); !status) {
        std::cerr << "error: " << status.error().to_string() << "\n";
        return 1;
    }
    const auto ignore = workspace.ignore_rules();

    // `bench` measures from nothing, so it gets an index directory of its own
    // that is removed afterwards. Anything else uses the application's, so what
    // it prints is what the agent would actually be answering from.
    const bool benchmarking = action == "bench";
    const auto index_directory = benchmarking
                                     ? std::filesystem::temp_directory_path() /
                                           ("localcode-repo-bench-" + std::to_string(now_unix_ms()))
                                     : AppPaths::instance().data_dir() / "repo-index";
    std::error_code directory_ec;
    const bool created = std::filesystem::create_directories(index_directory, directory_ec);
    if (directory_ec || (benchmarking && !created)) {
        std::cerr << "error: cannot create an exclusive index directory: "
                  << path_to_utf8(index_directory) << "\n";
        return 1;
    }

    struct BenchmarkDirectory {
        std::filesystem::path path;

        ~BenchmarkDirectory() {
            if (path.empty()) return;
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{benchmarking ? index_directory : std::filesystem::path{}};

    repo::RepoIndex index;
    if (auto status = index.open(index_directory, root); !status) {
        std::cerr << "error: " << status.error().to_string() << "\n";
        return 1;
    }

    CancellationSource cancel;
    std::cout << "repository: " << root.generic_string() << "\n";

    if (action == "status") {
        std::cout << "state: " << repo::to_string(index.state()) << "\n";
        auto overview = index.overview();
        if (!overview || !overview->has_value()) {
            std::cout << "nothing indexed yet - run `repo <workspace> refresh`\n";
            return 0;
        }
        print_overview(**overview);
        if (auto commands = index.store().commands(); commands && !commands->empty()) {
            print_heading("Commands");
            for (const auto& entry : *commands) {
                std::cout << "  " << entry.purpose << ": " << entry.command << "\n"
                          << "    " << (entry.verified ? "VERIFIED by running it" : "untried")
                          << " - from " << entry.source << "\n";
            }
        }
        return 0;
    }

    if (action == "symbol" || action == "search" || action == "refs") {
        if (arguments.size() < 3) {
            std::cerr << "error: usage: repo <workspace> " << action << " <name>\n";
            return 1;
        }
        const auto& term = arguments[2];
        repo::RepoQuery query{index.store(), root};

        if (action == "symbol") {
            auto found = query.find_symbol(term, 40);
            if (!found) return 1;
            std::cout << found->size() << " definition(s) of " << term << "\n";
            for (const auto& symbol : *found) {
                std::cout << "  " << to_string(symbol.kind) << "  " << symbol.name << "  "
                          << symbol.file << ":" << symbol.line << "\n";
            }
            return found->empty() ? 4 : 0;
        }
        if (action == "refs") {
            auto found = index.store().references_to_file(term, 40);
            if (!found) return 1;
            std::cout << found->size() << " file(s) include " << term << "\n";
            for (const auto& reference : *found) {
                std::cout << "  " << reference.from_file << ":" << reference.line << "\n";
            }
            return found->empty() ? 4 : 0;
        }

        repo::SearchOptions options;
        repo::SearchStats covered;
        auto found = query.search_text(term, options, &covered);
        if (!found) return 1;
        std::cout << found->size() << " match(es) for " << term
                  << (covered.complete() ? "" : "  [INCOMPLETE]") << "\n";
        for (const auto& hit : *found) {
            std::cout << "  " << hit.file << ":" << hit.line << "  " << hit.reason << "\n";
        }
        return found->empty() ? 4 : 0;
    }

    if (action == "refresh" || action == "full") {
        const bool full = action == "full";
        auto stats = index.refresh(ignore, cancel.token(), full);
        if (!stats) {
            std::cerr << "error: " << stats.error().to_string() << "\n";
            return 1;
        }
        std::cout << (full ? "full reindex\n" : "incremental refresh\n");
        print_stats(*stats);
        return 0;
    }

    if (!benchmarking) return usage();

    // --- bench -------------------------------------------------------------
    // Cold, warm, one changed file, and the queries. The scratch file is
    // written inside the scanned tree, because one written outside it is not
    // seen by the walk and the incremental number then measures nothing.
    print_heading("Cold (nothing on record)");
    auto cold = index.refresh(ignore, cancel.token(), false);
    if (!cold) {
        std::cerr << "error: " << cold.error().to_string() << "\n";
        return 1;
    }
    print_stats(*cold);

    print_heading("Warm (nothing changed)");
    auto warm = index.refresh(ignore, cancel.token(), false);
    if (!warm) return 1;
    print_stats(*warm);
    if (warm->files_reindexed != 0 || warm->files_added != 0) {
        std::cout << "  WARNING: a warm pass reindexed " << warm->files_reindexed << " and added "
                  << warm->files_added
                  << " files. Nothing changed, so it should have touched none.\n";
    }

    print_heading("Incremental (one file added)");
    const auto scratch =
        root / ("localcode-bench-scratch-" + std::to_string(now_unix_ms()) + ".cpp");
    {
        // Never truncate a user's file, including a pre-existing symlink.
        std::ofstream out(scratch, std::ios::binary | std::ios::out | std::ios::noreplace);
        if (!out) {
            std::cerr << "error: cannot create an exclusive benchmark source file\n";
            return 1;
        }
        out << "int localcode_bench_scratch_symbol() { return 1; }\n";
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(scratch, ec);
            return 1;
        }
    }
    auto incremental = index.refresh(ignore, cancel.token(), false);
    std::error_code scratch_ec;
    std::filesystem::remove(scratch, scratch_ec);
    if (scratch_ec) {
        std::cerr << "error: cannot remove benchmark source: " << scratch_ec.message() << "\n";
        return 1;
    }
    if (!incremental) return 1;
    print_stats(*incremental);

    print_heading("Full reindex");
    auto full = index.refresh(ignore, cancel.token(), true);
    if (!full) return 1;
    print_stats(*full);

    if (auto overview = index.overview(); overview && overview->has_value()) {
        print_overview(**overview);
    }

    print_heading("Queries");
    repo::RepoQuery query{index.store(), root};
    auto timed = [&](const char* label, auto&& action_fn) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = action_fn();
        const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
        std::cout << "  " << label << ": " << result << " result(s) in " << microseconds << " us\n";
    };
    timed("find_symbol(main)", [&] {
        auto found = query.find_symbol("main", 20);
        return found ? found->size() : 0;
    });
    timed("search_text(TODO)", [&] {
        repo::SearchOptions options;
        options.max_hits = 20;
        auto found = query.search_text("TODO", options);
        return found ? found->size() : 0;
    });
    timed("find_files(main)", [&] {
        auto found = query.find_files("main", 20);
        return found ? found->size() : 0;
    });

    // Measured after closing, not before. While the store is open the directory
    // also holds a write-ahead log that has not been checkpointed, and counting
    // it reported gin's 475KB index as 4.7MB - a transient buffer presented as
    // the cost of the thing.
    index.close();
    print_heading("Cost");
    print_row("index on disk", thousands(directory_size(index_directory)) + " bytes");

    std::error_code code;
    std::filesystem::remove_all(index_directory, code);
    return 0;
}

int command_skills(const std::string& directory) {
    SkillRegistry registry;
    const auto path =
        directory.empty() ? app_paths().builtin_skills_dir() : path_from_utf8(directory);
    auto status = registry.load_directory(path);
    if (!status) {
        std::cerr << "error: " << status.error().to_string() << "\n";
        return 1;
    }
    std::cout << "Skills in " << path_to_utf8(path) << "\n";
    for (const auto& skill : registry.skills()) {
        print_row(skill.id, skill.description, 28);
        if (!skill.triggers.empty()) {
            print_row("  triggers", strings::join(skill.triggers, ", "), 28);
        }
        if (!skill.tags.empty()) print_row("  tags", strings::join(skill.tags, ", "), 28);
        print_row("  body", std::to_string(skill.body.size()) + " bytes", 28);
    }
    std::cout << "\n" << registry.skills().size() << " skill(s) loaded";
    if (!registry.load_errors().empty()) {
        std::cout << ", " << registry.load_errors().size() << " error(s):\n";
        for (const auto& e : registry.load_errors())
            std::cout << "  " << e << "\n";
        return 1;
    }
    std::cout << "\n";
    return 0;
}

// --- agent -----------------------------------------------------------------
struct AgentOptions {
    std::string workspace;
    std::string model;
    std::string prompt;
    std::string backend = "auto";
    std::string server;
    int context = 0;
    int gpu_layers = -1;
    bool gpu_layers_explicit = false;
    RequestMode mode = RequestMode::Auto;
    int max_steps = 30;
    int time_budget_minutes = 0;
    bool approve_everything = false;
    bool read_only = false;
    // On by default, because that is the product. The flag exists so the agent
    // can be measured with it and without it, and so a run can be told to
    // ignore an index that is suspect.
    bool use_repo = true;
    bool refresh_repo = true;
};

int command_agent(Application& application, AgentOptions options) {
    // --- Workspace --------------------------------------------------------
    if (auto status = application.open_workspace(path_from_utf8(options.workspace)); !status) {
        std::cerr << "error: " << status.error().to_string() << "\n";
        return 1;
    }
    std::cout << "workspace: " << path_to_utf8(application.workspace().root()) << "\n";

    if (!options.use_repo) {
        // Both halves have to go. Clearing the index alone leaves the tools
        // registered and refusing, and the model then spends calls discovering
        // that - which measures a broken index rather than a tool set without
        // one. Unregistering them gives the comparison an honest baseline: the
        // agent as it was before any of this existed.
        application.agent().set_repo(nullptr);
        for (const char* name :
             {"repo_overview", "repo_search", "repo_symbol", "repo_tests_for", "repo_fact"}) {
            application.tools().remove(name);
        }
        std::cout << "repo:      off (--no-repo, tools unregistered)\n";
    } else if (options.refresh_repo) {
        CancellationSource repo_cancel;
        const auto started = std::chrono::steady_clock::now();
        auto stats = application.repository().refresh(application.workspace().ignore_rules(),
                                                      repo_cancel.token(), false);
        if (stats) {
            std::cout << "repo:      " << stats->files_scanned << " files, " << stats->symbols
                      << " symbols, " << millis_since(started) << " ms\n";
        } else {
            std::cout << "repo:      unavailable (" << stats.error().message << ")\n";
        }
    } else {
        std::cout << "repo:      using the stored index as it is\n";
    }

    // --- Model ------------------------------------------------------------
    auto profile = application.import_model(path_from_utf8(options.model));
    if (!profile) {
        std::cerr << "error: " << profile.error().to_string() << "\n";
        return 1;
    }
    if (options.context > 0) profile->context_size = static_cast<std::uint32_t>(options.context);
    profile->gpu_layers = options.gpu_layers;
    if (options.gpu_layers_explicit) profile->auto_gpu_layers = false;
    profile->backend = backend_preference_from_string(options.backend);
    if (!options.server.empty()) profile->server_binary = path_from_utf8(options.server);
    application.save_model_profile(*profile);

    std::cout << "model:     " << profile->display_name << "\n"
              << "           " << profile->architecture << ", " << profile->quantization
              << ", context " << profile->context_size << ", tools "
              << to_string(profile->tool_protocol) << "\n";

    auto selection = application.backends().select(*profile);
    if (!selection) {
        std::cerr << "error: " << selection.error().to_string() << "\n";
        if (!selection.error().detail.empty()) std::cerr << selection.error().detail << "\n";
        return 1;
    }
    std::cout << "backend:   " << to_string(selection->chosen) << "  (" << selection->reason
              << ")\n"
              << "server:    " << path_to_utf8(selection->install.server_binary) << "\n";

    // --- Load -------------------------------------------------------------
    CancellationSource cancel;
    g_active_cancel.store(&cancel);
    std::signal(SIGINT, handle_interrupt);

    std::cout << "\nloading the model...\n" << std::flush;
    const auto load_started = std::chrono::steady_clock::now();

    if (auto status = application.activate_model(profile->id, cancel.token()); !status) {
        std::cerr << "error: " << status.error().to_string() << "\n";
        if (!status.error().detail.empty()) std::cerr << status.error().detail << "\n";
        for (const auto& line : application.backends().server_log(40)) {
            std::cerr << "  " << line << "\n";
        }
        return 1;
    }
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - load_started)
                             .count();
    std::cout << "ready in " << strings::human_duration_ms(load_ms) << "\n";
    print_row("acceleration", application.backends().server_status().acceleration.describe());

    const auto info = application.backends().current_info();
    if (info.context_size) print_row("server context", std::to_string(info.context_size));
    if (!info.server_version.empty()) print_row("server build", info.server_version);
    print_row(
        "exact tokenizer",
        application.backends().current_backend()->capabilities().tokenize_endpoint ? "yes" : "no");

    // --- Agent ------------------------------------------------------------
    AgentConfig config = application.agent().config();
    config.max_iterations = options.max_steps;
    config.request_mode = options.mode;
    if (options.time_budget_minutes > 0)
        config.wall_clock_budget = std::chrono::minutes(options.time_budget_minutes);
    application.agent().set_config(config);

    if (options.read_only) application.tools().set_read_only(true);

    application.agent().set_confirmation_handler([&options](const ConfirmationRequest& request) {
        std::cout << "\n  [approval] " << request.title << "\n"
                  << "             " << request.command << "\n"
                  << "             " << request.reason << "\n";
        if (options.approve_everything) {
            std::cout << "             approved (--yes)\n";
            return true;
        }
        std::cout << "             refused (pass --yes to approve automatically)\n";
        return false;
    });

    int step = 0;
    // A reasoning model can spend minutes in its think block before emitting a
    // single visible token. Without a marker for it the tool looks hung, which
    // is exactly how it looked the first time this was run against one.
    bool in_reasoning = false;
    int reasoning_marks = 0;

    application.agent().set_event_sink([&](const AgentEvent& event) {
        auto end_reasoning = [&] {
            if (!in_reasoning) return;
            in_reasoning = false;
            reasoning_marks = 0;
            std::cout << "]\n" << std::flush;
        };

        switch (event.type) {
            case AgentEventType::ReasoningDelta:
                // Progress, not transcript: the think block is often longer than
                // the answer and is rarely what the operator wants to read.
                if (!in_reasoning) {
                    in_reasoning = true;
                    std::cout << "  [thinking " << std::flush;
                }
                if (++reasoning_marks % 40 == 0) std::cout << "." << std::flush;
                break;
            case AgentEventType::ContentDelta:
                end_reasoning();
                std::cout << event.text << std::flush;
                break;
            case AgentEventType::ToolCallStarted: {
                std::cout << "\n  -> " << event.tool_call.name << "\n";
                // The arguments exactly as the model produced them. Diagnosing
                // a failed run without these is guesswork: run 9 reported
                // "executable not found: build" and nothing in the trace said
                // what had been sent, so the cause could only be inferred.
                // Control characters are escaped rather than printed, because
                // the interesting failures are precisely the ones where a JSON
                // escape put a form feed in the middle of a path.
                const auto& raw = event.tool_call.arguments_json;
                if (!raw.empty()) {
                    std::cout << "     args "
                              << strings::utf8_truncate(
                                     strings::visible(strings::redact_secrets(raw)), 400)
                              << "\n";
                }
                std::cout << std::flush;
                break;
            }
            case AgentEventType::ToolCompleted: {
                std::cout << "     " << (event.tool_result.ok ? "ok   " : "FAIL ")
                          << event.tool_result.summary << "\n";
                // What actually ran, as opposed to what was asked for. The
                // resolved executable and the working directory are the two
                // facts a failed command is usually about, and neither is
                // recoverable from the summary.
                const auto& record = event.tool_result.structured;
                if (record.is_object() && record.contains("executable")) {
                    std::cout << "     ran  " << json_string(record, "executable") << "   cwd "
                              << json_string(record, "cwd") << "   form "
                              << json_string(record, "form");
                    if (json_bool(record, "msvc_environment_applied", false)) {
                        std::cout << "   +msvc-env";
                    }
                    std::cout << "\n";
                }
                std::cout << std::flush;
                break;
            }
            case AgentEventType::ProgressWarning:
                std::cout << "\n  [no progress] " << strings::utf8_truncate(event.text, 200) << "\n"
                          << std::flush;
                break;
            case AgentEventType::GenerationStats: {
                end_reasoning();
                const auto& s = event.stats;
                std::cout << "  [gen]";
                if (s.reasoning_characters > 0) {
                    std::cout << " reasoned ~" << s.estimated_reasoning_tokens << " tok";
                    // Only when the observation can support it: several deltas
                    // spread over time. A single buffered read spans nothing.
                    if (s.reasoning_delta_count >= 3 && s.observed_reasoning_stream_ms >= 1000) {
                        std::cout << " (streamed " << (s.observed_reasoning_stream_ms / 1000.0)
                                  << "s)";
                    }
                }
                if (s.completion_tokens > 0) {
                    std::cout << "  " << (s.usage_from_server ? "" : "~") << s.completion_tokens
                              << " tok";
                }
                if (s.decode_tokens_per_second > 0.0) {
                    std::cout << "  " << (s.timings_from_server ? "" : "~")
                              << s.decode_tokens_per_second << " tok/s decode";
                }
                if (s.timings_from_server && s.prompt_tokens_per_second > 0.0) {
                    std::cout << "  " << s.prompt_tokens_per_second << " tok/s prompt ("
                              << s.prompt_tokens_evaluated << " eval";
                    if (s.cached_prompt_tokens > 0) {
                        std::cout << ", " << s.cached_prompt_tokens << " cached";
                    }
                    std::cout << ")";
                }
                std::cout << "  ttft " << (s.time_to_first_token_ms / 1000.0) << "s\n"
                          << std::flush;
                break;
            }
            case AgentEventType::IterationFinished:
                std::cout << "  [step " << ++step << "]\n" << std::flush;
                break;
            case AgentEventType::ContextUpdated: {
                const auto& c = event.context;
                std::cout << "  [context] " << c.used << "/" << c.total
                          << (c.exact ? " exact" : " estimated") << "  (system " << c.system_prompt
                          << ", skills " << c.skills << ", task " << c.task_state << ", tools "
                          << c.tool_definitions << ", conversation " << c.conversation << ")\n"
                          << std::flush;
                if (!event.text.empty()) {
                    std::cout << "  [context] " << event.text << "\n" << std::flush;
                }
                break;
            }
            default: break;
        }
    });

    std::cout << "\n=== running ===\n" << std::flush;
    const auto summary = application.agent().run(options.prompt, cancel.token());

    print_heading("Result");
    print_row("completed", summary.completed ? "yes" : "no");
    print_row("stop reason", summary.stop_reason);
    print_row("steps", std::to_string(summary.iterations));
    print_row("tool calls", std::to_string(summary.tool_calls));
    print_row("duration", strings::human_duration_ms(summary.duration_ms));
    print_row("verification", std::string{to_string(summary.verification)});
    print_row("context used",
              std::to_string(summary.context.used) + " / " + std::to_string(summary.context.total));

    if (!summary.unsupported_claims.empty()) {
        print_heading("Claims the evidence did not support");
        for (const auto& claim : summary.unsupported_claims)
            std::cout << "  - " << claim << "\n";
    }

    const auto changes = application.workspace().agent_changes();
    if (!changes.empty()) {
        print_heading("Files changed");
        for (const auto& change : changes) {
            std::cout << "  " << to_string(change.kind) << ": " << change.relative_path
                      << (change.was_dirty_before ? "   [had uncommitted changes]" : "") << "\n";
        }
    }

    print_heading("Final answer");
    std::cout << summary.final_message << "\n";

    g_active_cancel.store(nullptr);
    application.deactivate_model();
    return summary.completed ? 0 : 1;
}

}   // namespace

int main(int argc, char** argv) {
    Logger::instance().set_min_level(LogLevel::Warn);

    std::vector<std::string> arguments;
    try {
        arguments = command_line_arguments(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    if (arguments.size() < 2) return usage();
    const std::string command = arguments[1];
    std::vector<std::string> rest;
    rest.assign(arguments.begin() + 2, arguments.end());

    try {
        if (command == "env") return command_env();
        if (command == "gguf" && rest.size() == 1) return command_gguf(rest[0]);
        if (command == "path" && rest.size() == 3) return command_path(rest[0], rest[1], rest[2]);
        if (command == "cmd" && rest.size() >= 2) {
            return command_cmd(rest[0], {rest.begin() + 1, rest.end()});
        }
        if (command == "run" && rest.size() >= 2) {
            return command_run(rest[0], {rest.begin() + 1, rest.end()});
        }
        if (command == "skills") return command_skills(rest.empty() ? "" : rest[0]);
        if (command == "models") return command_models(rest);
        if (command == "config") return command_config(rest);
        if (command == "repo") return command_repo(rest);

        // These need the full application: storage, backends, tools, skills.
        if (command == "backends" || command == "agent") {
            Application application;
            if (auto status = application.initialise(); !status) {
                std::cerr << "error: " << status.error().to_string() << "\n";
                return 1;
            }
            if (command == "backends") return command_backends(application);

            AgentOptions options;
            std::vector<std::string> positional;
            for (std::size_t i = 0; i < rest.size(); ++i) {
                const auto& argument = rest[i];
                auto next = [&]() -> std::string {
                    return (i + 1 < rest.size()) ? rest[++i] : std::string{};
                };
                if (argument == "--backend")
                    options.backend = next();
                else if (argument == "--server")
                    options.server = next();
                else if (argument == "--ctx")
                    options.context = std::atoi(next().c_str());
                else if (argument == "--gpu-layers") {
                    options.gpu_layers = std::atoi(next().c_str());
                    options.gpu_layers_explicit = true;
                }
                else if (argument == "--mode") {
                    const auto mode = next();
                    if (mode == "auto") options.mode = RequestMode::Auto;
                    else if (mode == "chat") options.mode = RequestMode::Chat;
                    else if (mode == "code") options.mode = RequestMode::Code;
                    else { std::cerr << "error: --mode must be auto, chat or code\n"; return 1; }
                }
                else if (argument == "--max-steps")
                    options.max_steps = std::atoi(next().c_str());
                else if (argument == "--time-budget") {
                    const auto value = next();
                    if (value.empty() ||
                        value.find_first_not_of("0123456789") != std::string::npos ||
                        value.size() > 3 ||
                        (options.time_budget_minutes = std::atoi(value.c_str())) < 1 ||
                        options.time_budget_minutes > 480) {
                        std::cerr
                            << "error: --time-budget must be an integer from 1 to 480 minutes\n";
                        return 1;
                    }
                } else if (argument == "--yes")
                    options.approve_everything = true;
                else if (argument == "--read-only")
                    options.read_only = true;
                else if (argument == "--no-repo")
                    options.use_repo = false;
                else if (argument == "--no-repo-refresh")
                    options.refresh_repo = false;
                else if (argument.rfind("--", 0) == 0) {
                    // Treating an unknown flag as a positional argument turns a
                    // typo into "workspace directory does not exist: --project",
                    // which sends you looking in the wrong place entirely.
                    std::cerr << "error: unknown option '" << argument << "'\n";
                    return usage();
                } else
                    positional.push_back(argument);
            }
            if (positional.size() < 3) return usage();

            options.workspace = positional[0];
            options.model = positional[1];
            options.prompt = strings::join({positional.begin() + 2, positional.end()}, " ");
            return command_agent(application, std::move(options));
        }
    } catch (const std::exception& e) {
        std::cerr << "unhandled exception: " << e.what() << "\n";
        return 3;
    }
    return usage();
}
