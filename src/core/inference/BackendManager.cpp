#include "VramPreflight.h"
#include "model/GgufMetadata.h"
#include "platform/GpuProbe.h"

#include <filesystem>
#include "BackendManager.h"

#include <algorithm>
#include <cstdlib>

#include "../platform/PathEncoding.h"
#include "../platform/Paths.h"
#include "../platform/Process.h"
#include "../util/Log.h"
#include "../util/Strings.h"

namespace localcode {
namespace {

#if defined(_WIN32)
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

// Executable names each runtime is distributed under, most specific first.
std::vector<std::string> binary_names(BackendPreference kind) {
    if (kind == BackendPreference::IkLlama) {
        return {std::string{"ik_llama-server"} + kExeSuffix,
                std::string{"ik-llama-server"} + kExeSuffix,
                std::string{"llama-server"} + kExeSuffix};
    }
    return {std::string{"llama-server"} + kExeSuffix, std::string{"server"} + kExeSuffix};
}

// Subdirectories of the managed runtime directory, by convention one per
// runtime so a user can keep several llama.cpp builds side by side.
std::vector<std::string> runtime_subdirectories(BackendPreference kind) {
    if (kind == BackendPreference::IkLlama) {
        return {"ik_llama.cpp", "ik_llama", "ik-llama.cpp"};
    }
    return {"llama.cpp", "llamacpp", "llama"};
}

// Does this path, relative to the runtime root, name an ik_llama build?
//
// Both projects install a binary called llama-server.exe, so the file name
// cannot separate them; the folder usually can, because people keep one folder
// per build named after the flavour and version - ik-b5197-cuda13.3-alderlake
// beside llama-b10655-cuda13.3.
//
// "ik" is matched only as a whole leading token, so a runtime living under a
// folder called Nikola is not mistaken for one.
bool path_suggests_ik(const std::filesystem::path& relative) {
    for (const auto& part : relative) {
        const auto component = strings::to_lower(part.string());
        if (component.rfind("ik", 0) != 0) continue;
        if (component.size() == 2) return true;
        const char next = component[2];
        if (!std::isalpha(static_cast<unsigned char>(next))) return true;
    }
    return false;
}

// Every server binary under `root`, to a bounded depth.
//
// The previous code only looked in folders named exactly llama.cpp, llama,
// ik_llama.cpp and a few variants. Nobody names them that: a runtimes folder
// holds one directory per build, stamped with the version and the CUDA flavour,
// and none of those names ever matched - so a machine with two working runtimes
// installed reported that no runtime was found.
std::vector<std::pair<std::filesystem::path, bool>> scan_for_servers(
    const std::filesystem::path& root, int max_depth) {
    std::vector<std::pair<std::filesystem::path, bool>> found;   // path, is_ik
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return found;

    // Names either project might install, lower-cased for comparison.
    static const std::vector<std::string> kNames = {
        std::string{"llama-server"} + kExeSuffix,
        std::string{"ik_llama-server"} + kExeSuffix,
        std::string{"ik-llama-server"} + kExeSuffix,
        std::string{"server"} + kExeSuffix,
    };

    auto iterator = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return found;

    for (auto it = iterator; it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (it.depth() >= max_depth) it.disable_recursion_pending();
        if (!it->is_regular_file(ec)) continue;

        const auto name = strings::to_lower(it->path().filename().string());
        if (std::find(kNames.begin(), kNames.end(), name) == kNames.end()) continue;

        const auto relative = std::filesystem::relative(it->path(), root, ec);
        const bool is_ik = name.rfind("ik", 0) == 0 || (!ec && path_suggests_ik(relative));
        found.emplace_back(it->path(), is_ik);
    }
    return found;
}

// Environment overrides, so a CI job or a container can point LocalCode at a
// runtime without writing to the settings database first.
std::string env_value(const char* name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
#endif
}

std::string extract_version(const ServerBinaryFeatures& features) {
    // probe_binary() runs `--version`; nothing is invented when it says nothing.
    return features.version;
}

}   // namespace

std::string RuntimeInstall::display() const {
    std::string out{to_string(kind)};
    out += "  " + path_to_utf8(server_binary);
    if (!version.empty()) out += "  [" + version + "]";
    if (!source.empty()) out += "  (" + source + ")";
    return out;
}

BackendManager::BackendManager() = default;

BackendManager::~BackendManager() {
    deactivate();
}

void BackendManager::set_http_client(std::shared_ptr<HttpClient> http) {
    std::lock_guard lock(mutex_);
    http_ = std::move(http);
}

void BackendManager::set_configured_binary(BackendPreference kind, std::filesystem::path binary) {
    std::lock_guard lock(mutex_);
    if (kind == BackendPreference::IkLlama)
        ik_llama_binary_ = std::move(binary);
    else
        llama_binary_ = std::move(binary);
    runtimes_valid_ = false;
}

std::filesystem::path BackendManager::configured_binary(BackendPreference kind) const {
    std::lock_guard lock(mutex_);
    return kind == BackendPreference::IkLlama ? ik_llama_binary_ : llama_binary_;
}

void BackendManager::set_use_bundled_llama(bool enabled) {
    std::lock_guard lock(mutex_);
    use_bundled_llama_ = enabled;
    runtimes_valid_ = false;
}

bool BackendManager::use_bundled_llama() const {
    std::lock_guard lock(mutex_);
    return use_bundled_llama_;
}

void BackendManager::set_default_preference(BackendPreference preference) {
    std::lock_guard lock(mutex_);
    default_preference_ =
        preference == BackendPreference::Auto ? BackendPreference::LlamaCpp : preference;
}

BackendPreference BackendManager::default_preference() const {
    std::lock_guard lock(mutex_);
    return default_preference_;
}

void BackendManager::set_external_endpoint(ExternalEndpoint endpoint) {
    std::lock_guard lock(mutex_);
    external_ = std::move(endpoint);
}

ExternalEndpoint BackendManager::external_endpoint() const {
    std::lock_guard lock(mutex_);
    return external_;
}

std::vector<std::filesystem::path> BackendManager::candidate_binaries(
    BackendPreference kind, const std::filesystem::path& configured,
    const std::filesystem::path& runtime_root, bool bundled) {
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;

    if (bundled && kind == BackendPreference::LlamaCpp) {
        const auto binary = AppPaths::bundled_llama_server();
        if (std::filesystem::is_regular_file(binary, ec)) candidates.push_back(binary);
        const auto vulkan = binary.parent_path() / "vulkan" / binary.filename();
        if (std::filesystem::is_regular_file(vulkan, ec)) candidates.push_back(vulkan);
        return candidates;
    }

    if (!configured.empty() && std::filesystem::is_regular_file(configured, ec)) {
        candidates.push_back(configured);
    }

    // An explicit environment variable outranks everything but a configured path.
    const auto from_environment =
        env_value(kind == BackendPreference::IkLlama ? "LOCALCODE_IK_LLAMA_SERVER"
                                                     : "LOCALCODE_LLAMA_SERVER");
    if (!from_environment.empty() && std::filesystem::is_regular_file(from_environment, ec)) {
        candidates.emplace_back(from_environment);
    }

    // Managed runtime directory: <data>/runtimes/<runtime>/llama-server.exe
    for (const auto& subdirectory : runtime_subdirectories(kind)) {
        for (const auto& name : binary_names(kind)) {
            for (const char* nested : {"", "bin", "build/bin"}) {
                std::filesystem::path candidate = runtime_root / subdirectory;
                if (*nested) candidate /= nested;
                candidate /= name;
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    candidates.push_back(candidate);
                }
            }
        }
    }

    // Anything else under the runtime root, whatever its folders are called.
    for (const auto& [binary, is_ik] : scan_for_servers(runtime_root, 3)) {
        if (is_ik == (kind == BackendPreference::IkLlama)) candidates.push_back(binary);
    }

    // PATH. Only meaningful for the runtime whose binary name is distinctive:
    // a bare llama-server on PATH is assumed to be mainline llama.cpp.
    for (const auto& name : binary_names(kind)) {
        const bool ambiguous = (kind == BackendPreference::IkLlama) && name.rfind("ik", 0) != 0;
        if (ambiguous) continue;
        for (const auto& found : find_all_executables(name))
            candidates.push_back(found);
    }

    // De-duplicate while preserving priority order.
    std::vector<std::filesystem::path> unique;
    for (auto& candidate : candidates) {
        auto canonical = std::filesystem::weakly_canonical(candidate, ec);
        if (ec) canonical = candidate;
        if (std::find(unique.begin(), unique.end(), canonical) == unique.end()) {
            unique.push_back(std::move(canonical));
        }
    }
    return unique;
}

std::vector<std::pair<std::filesystem::path, bool>> BackendManager::scan_runtime_root(
    const std::filesystem::path& root, int max_depth) {
    return scan_for_servers(root, max_depth);
}

std::vector<RuntimeInstall> BackendManager::discover_runtimes(bool refresh, CancellationToken cancel) {
    {
        std::lock_guard lock(mutex_);
        if (runtimes_valid_ && !refresh) return runtimes_;
    }

    const auto request = runtime_discovery();
    auto found = discover_runtimes(request, cancel);
    if (cancel.cancelled()) return found;
    {
        std::lock_guard lock(mutex_);
        if (llama_binary_ == request.llama_binary && ik_llama_binary_ == request.ik_llama_binary &&
            app_paths().runtime_dir() == request.runtime_root &&
            use_bundled_llama_ == request.use_bundled_llama) {
            runtimes_ = found;
            runtimes_valid_ = true;
        }
    }
    return found;
}

RuntimeDiscovery BackendManager::runtime_discovery() const {
    std::lock_guard lock(mutex_);
    return {llama_binary_, ik_llama_binary_, app_paths().runtime_dir(), use_bundled_llama_};
}

std::vector<RuntimeInstall> BackendManager::discover_runtimes(const RuntimeDiscovery& request,
                                                              CancellationToken cancel) {
    std::vector<RuntimeInstall> found;
    for (auto kind : {BackendPreference::LlamaCpp, BackendPreference::IkLlama}) {
        const auto& configured =
            kind == BackendPreference::IkLlama ? request.ik_llama_binary : request.llama_binary;
        const auto candidates =
            candidate_binaries(kind, configured, request.runtime_root, request.use_bundled_llama);
        for (const auto& binary : candidates) {
            if (cancel.cancelled()) return found;
            RuntimeInstall install;
            install.kind = kind;
            install.server_binary = binary;

            const auto from_environment =
                env_value(kind == BackendPreference::IkLlama ? "LOCALCODE_IK_LLAMA_SERVER"
                                                             : "LOCALCODE_LLAMA_SERVER");
            if (request.use_bundled_llama && kind == BackendPreference::LlamaCpp) {
                install.source = "llama.cpp.localcode bundled with LocalCode";
            } else if (!configured.empty() && binary == configured) {
                install.source = "configured in settings";
            } else if (!from_environment.empty() &&
                       binary == std::filesystem::path{from_environment}) {
                install.source = "environment variable";
            } else if (binary.string().find(request.runtime_root.string()) != std::string::npos) {
                install.source = "managed runtime directory";
            } else {
                install.source = "PATH";
            }

            // Probing runs the binary once with --help. It is the only way to
            // know what this particular build supports, and the result is cached.
            auto features = ModelServer::probe_binary(binary, cancel);
            if (!features) {
                log_debug(LogChannel::Backend, "candidate rejected",
                          {{"binary", path_to_utf8(binary)}, {"reason", features.error().message}});
                continue;
            }
            install.features = *features;
            install.probed = true;
            install.version = extract_version(*features);

            // A binary that answers to ik_llama's name is ik_llama; a shared
            // llama-server found under an ik_llama directory is too.
            found.push_back(std::move(install));
        }
    }

    log_info(LogChannel::Backend, "runtime discovery finished",
             {{"count", std::to_string(found.size())}});
    return found;
}

Result<BackendSelection> BackendManager::select(const ModelProfile& profile, CancellationToken cancel) {
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Runtime selection cancelled");
    auto selection = select_runtime(profile, cancel);
    if (!selection || profile.gpu_layers == 0) return selection;
    auto probe = ModelServer::probe_devices(selection->install.server_binary, selection->install.features, cancel);
    if (!probe) return probe.error();
    if (!probe->supported) return selection;
    if (probe->succeeded && !probe->devices.empty()) {
        if (selection->install.source.starts_with("llama.cpp.localcode") &&
            selection->install.server_binary.parent_path().filename() == "vulkan") {
            selection->device = prefer_dedicated_device(probe->devices, probe_graphics_adapters());
            selection->compatibility_fallback = true;
        }
        selection->reason += "; enumerated GPU devices: " + strings::join(probe->devices, "; ");
        if (!selection->device.empty()) selection->reason += "; selected " + selection->device;
        return selection;
    }
    const auto primary_detail = probe->detail;
    if (use_bundled_llama() && profile.server_binary.empty() &&
        selection->chosen == BackendPreference::LlamaCpp) {
        const auto alternative = AppPaths::bundled_llama_server().parent_path() / "vulkan" /
            AppPaths::bundled_llama_server().filename();
        auto features = ModelServer::probe_binary(alternative, cancel);
        if (features) {
            auto devices = ModelServer::probe_devices(alternative, *features, cancel);
            if (!devices && devices.error().code == ErrorCode::Cancelled) return devices.error();
            if (devices && devices->succeeded && !devices->devices.empty()) {
                selection->install.server_binary = alternative;
                selection->install.features = *features;
                selection->compatibility_fallback = true;
                selection->device = prefer_dedicated_device(devices->devices, probe_graphics_adapters());
                selection->reason = "The primary bundled backend reported no usable GPU; using the "
                    "bundled Vulkan runtime" + (selection->device.empty() ? std::string{} : " on " + selection->device) + ".";
                return selection;
            }
        }
    }
    selection->force_cpu = true;
    selection->reason += "; GPU device enumeration found no usable device. CPU fallback selected. " +
        strings::utf8_truncate(primary_detail, 600);
    return selection;
}

Result<BackendSelection> BackendManager::select_runtime(const ModelProfile& profile, CancellationToken cancel) {
    // A profile that pins a binary wins outright: the user was explicit.
    if (!profile.server_binary.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(profile.server_binary, ec)) {
            return make_error(ErrorCode::NotFound,
                              "the server binary set on this model profile does not exist",
                              path_to_utf8(profile.server_binary));
        }
        auto features = ModelServer::probe_binary(profile.server_binary, cancel);
        if (!features) return features.error();

        BackendSelection selection;
        selection.chosen = profile.backend == BackendPreference::Auto ? BackendPreference::LlamaCpp
                                                                      : profile.backend;
        selection.install.kind = selection.chosen;
        selection.install.server_binary = profile.server_binary;
        selection.install.features = *features;
        selection.install.source = "set on the model profile";
        selection.install.probed = true;
        selection.reason = "this model profile pins a specific server binary";
        return selection;
    }

    if (use_bundled_llama() && profile.backend != BackendPreference::IkLlama) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(AppPaths::bundled_llama_server(), ec)) {
            return make_error(
                ErrorCode::NotFound, "The bundled LocalCode runtime is missing.",
                "Reinstall LocalCode, or select Custom llama.cpp in Settings > Models.");
        }
    }
    auto runtimes = discover_runtimes(false, cancel);
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "Runtime selection cancelled");
    if (runtimes.empty()) {
        return make_error(
            ErrorCode::NotFound, "no llama.cpp or ik_llama.cpp server was found",
            "Set the server binary path in Settings, put llama-server on PATH, or place a "
            "build under " +
                path_to_utf8(app_paths().runtime_dir()) + "/llama.cpp/");
    }

    auto first_of = [&runtimes](BackendPreference kind) -> const RuntimeInstall* {
        for (const auto& install : runtimes) {
            if (install.kind == kind) return &install;
        }
        return nullptr;
    };

    if (use_bundled_llama() && profile.backend == BackendPreference::Auto) {
        const auto* install = first_of(BackendPreference::LlamaCpp);
        if (!install) {
            return make_error(
                ErrorCode::BackendError, "The included LocalCode runtime could not start.",
                "Reinstall LocalCode, or choose Custom llama.cpp in Settings > Models. "
                "Check the runtime log for missing dependencies.");
        }
        BackendSelection selection;
        selection.chosen = BackendPreference::LlamaCpp;
        selection.install = *install;
        selection.reason = "using llama.cpp.localcode included with LocalCode";
        return selection;
    }

    // An explicit choice on the profile is honoured, or reported as missing.
    if (profile.backend != BackendPreference::Auto) {
        const auto* install = first_of(profile.backend);
        if (!install) {
            return make_error(ErrorCode::NotFound,
                              std::string{"this model is configured to use "} +
                                  std::string{to_string(profile.backend)} +
                                  ", but no such runtime was found",
                              "Change the backend to AUTO, or set its path in Settings.");
        }
        BackendSelection selection;
        selection.chosen = profile.backend;
        selection.install = *install;
        selection.reason =
            "the model profile explicitly selects " + std::string{to_string(profile.backend)};
        return selection;
    }

    // --- AUTO -------------------------------------------------------------
    const auto* llama = first_of(BackendPreference::LlamaCpp);
    const auto* ik = first_of(BackendPreference::IkLlama);

    BackendSelection selection;
    if (llama && !ik) {
        selection.chosen = BackendPreference::LlamaCpp;
        selection.install = *llama;
        selection.reason = "llama.cpp is the only runtime installed";
    } else if (ik && !llama) {
        selection.chosen = BackendPreference::IkLlama;
        selection.install = *ik;
        selection.reason = "ik_llama.cpp is the only runtime installed";
    } else if (llama && ik) {
        const auto preference = default_preference();
        const auto* picked = (preference == BackendPreference::IkLlama) ? ik : llama;
        selection.chosen = picked->kind;
        selection.install = *picked;
        selection.reason = "both runtimes are installed; using the configured default (" +
                           std::string{to_string(preference)} +
                           "). Neither is faster in general - switch on this model's profile "
                           "and compare if you want to know which is faster here.";
    } else {
        return make_error(ErrorCode::NotFound, "no usable runtime was found");
    }

    // Tool calling needs the model's own chat template, which needs --jinja.
    // If the chosen build predates that flag, prefer one that has it.
    const bool wants_tools =
        profile.capabilities.supports_tools && profile.tool_protocol == ToolProtocol::Native;
    if (wants_tools && !selection.install.features.jinja) {
        for (const auto& candidate : runtimes) {
            if (candidate.features.jinja) {
                selection.chosen = candidate.kind;
                selection.install = candidate;
                selection.reason =
                    "this model uses native tool calls, which needs a server "
                    "build that supports --jinja";
                break;
            }
        }
    }
    return selection;
}

Result<BackendPtr> BackendManager::activate(const ModelProfile& profile, CancellationToken cancel) {
    std::lock_guard transition(transition_mutex_);
    std::shared_ptr<HttpClient> http;
    {
        std::lock_guard lock(mutex_);
        http = http_;
    }
    if (!http) {
        return make_error(ErrorCode::NotSupported,
                          "no HTTP transport is available; LocalCode cannot reach a model server");
    }

    // Attaching to a server the user already runs skips process management.
    const auto external = external_endpoint();
    if (external.enabled && !external.base_url.empty()) {
        deactivate();
        BackendEndpoint endpoint;
        endpoint.base_url = external.base_url;
        endpoint.api_key = external.api_key;

        BackendPtr backend = external.kind == BackendPreference::IkLlama
                                 ? std::static_pointer_cast<IInferenceBackend>(
                                       std::make_shared<IkLlamaBackend>(endpoint, http))
                                 : std::static_pointer_cast<IInferenceBackend>(
                                       std::make_shared<LlamaCppBackend>(endpoint, http));

        auto health = backend->health(cancel);
        if (!health) {
            return make_error(ErrorCode::BackendError,
                              "cannot reach the configured server at " + external.base_url,
                              health.error().message);
        }
        auto info = backend->info(cancel);

        std::lock_guard lock(mutex_);
        backend_ = backend;
        profile_ = profile;
        if (info) info_ = *info;
        log_info(LogChannel::Backend, "attached to external server", {{"url", external.base_url}});
        return backend;
    }

    auto selection = select(profile, cancel);
    if (!selection) return selection.error();

    // Unload the current model before loading another: two large models will
    // not fit in VRAM together, and a half-freed one makes the next load fail
    // in ways that look like a corrupt file.
    deactivate();

    // Preflight, after the unload so the outgoing model's memory is already
    // back. This warns; it does not refuse. The runtime knows what it actually
    // needs and its refusal remains the authoritative one.
    warn_if_vram_is_short(profile);

    ServerConfig config;
    config.binary = selection->install.server_binary;
    config.profile = profile;
    config.device = selection->device;
    const auto ram = probe_system_memory_bytes();
    if (ram && *ram <= 16ull * 1024 * 1024 * 1024) {
        if (!config.profile.prompt_cache_mib && selection->install.features.cache_ram)
            config.profile.prompt_cache_mib = 128;
        if (profile.context_size >= 16384) {
            std::function<void(const std::string&)> callback;
            { std::lock_guard lock(mutex_); callback = advisory_callback_; }
            if (callback) callback("This saved profile requests " + std::to_string(profile.context_size) +
                " context tokens on a system with 16 GiB RAM or less. The setting is retained. "
                "For a smaller memory footprint, choose a 4K or 8K context preset in Models; "
                "this is a configuration tradeoff, not an inference speed optimization.");
        }
    }
    if (selection->force_cpu) {
        config.profile.gpu_layers = 0;
        config.profile.auto_gpu_layers = false;
    }
    if (selection->force_cpu || selection->compatibility_fallback) {
        std::function<void(const std::string&)> callback;
        { std::lock_guard lock(mutex_); callback = advisory_callback_; }
        if (callback) callback(selection->reason);
    }
    config.startup_timeout = std::chrono::seconds(900);

    auto probe = [http](const std::string& base_url, CancellationToken token) -> Status {
        BackendEndpoint endpoint;
        endpoint.base_url = base_url;
        OpenAiCompatBackend temporary{endpoint, http};
        return temporary.health(token);
    };

    auto started = server_.start(config, probe, cancel);
    if (!started) return started.error();

    BackendEndpoint endpoint;
    endpoint.base_url = server_.base_url();
    endpoint.native_reasoning_budget = selection->chosen == BackendPreference::LlamaCpp &&
                                       selection->install.features.reasoning_budget;

    BackendPtr backend = selection->chosen == BackendPreference::IkLlama
                             ? std::static_pointer_cast<IInferenceBackend>(
                                   std::make_shared<IkLlamaBackend>(endpoint, http))
                             : std::static_pointer_cast<IInferenceBackend>(
                                   std::make_shared<LlamaCppBackend>(endpoint, http));

    // Let a dropped connection say whether the process on the other end survived
    // it. This manager owns the server, so it is the only thing that can tell.
    if (auto* compat = dynamic_cast<OpenAiCompatBackend*>(backend.get())) {
        compat->set_liveness_reporter([this] { return describe_server_liveness(); });
    }

    auto info = backend->info(cancel);

    {
        std::lock_guard lock(mutex_);
        backend_ = backend;
        profile_ = profile;
        info_ = info ? *info : BackendInfo{};
    }

    log_info(LogChannel::Backend, "model activated",
             {{"model", profile.display_name},
              {"backend", std::string{to_string(selection->chosen)}},
              {"reason", selection->reason},
              {"url", endpoint.base_url}});
    return backend;
}

void BackendManager::deactivate() {
    std::lock_guard transition(transition_mutex_);
    {
        std::lock_guard lock(mutex_);
        backend_.reset();
        profile_ = ModelProfile{};
        info_ = BackendInfo{};
    }
    server_.stop();
}

BackendPtr BackendManager::current_backend() const {
    std::lock_guard lock(mutex_);
    if (!external_.enabled && !server_.ready()) return {};
    return backend_;
}

ModelProfile BackendManager::current_profile() const {
    std::lock_guard lock(mutex_);
    if (!backend_ || (!external_.enabled && !server_.ready())) return {};
    return profile_;
}

std::string BackendManager::describe_server_liveness() const {
    auto status = const_cast<ModelServer&>(server_).status();

    if (status.state == ServerState::Restarting)
        return "The managed model server stopped unexpectedly and is restarting. "
               "This interrupted request and its tools are not replayed; retry the task after "
               "Ready.";

    if (status.state == ServerState::Ready) {
        // A transport error can beat the process wait/log-drain notification.
        // Cached Ready does not prove the process survived this request.
        return "The manager has not observed a process exit yet (last state: Ready). "
               "This does not establish why the connection failed. Check the server log for " +
               status.base_url + ".";
    }

    std::string verdict =
        "The model server is no longer running (state: " + std::string{to_string(status.state)};
    if (status.exit_code != 0) {
        verdict += ", exit code " + std::to_string(status.exit_code);
    }
    verdict += "), so it went down while handling the request.";
    if (!status.message.empty()) {
        verdict += " It reported: " + strings::utf8_truncate(status.message, 600);
    }
    return verdict;
}

ServerStatus BackendManager::server_status() const {
    return const_cast<ModelServer&>(server_).status();
}

BackendInfo BackendManager::current_info() const {
    std::lock_guard lock(mutex_);
    return info_;
}

std::vector<std::string> BackendManager::server_log(std::size_t max_lines) const {
    return const_cast<ModelServer&>(server_).recent_log(max_lines);
}

void BackendManager::warn_if_vram_is_short(const ModelProfile& profile) {
    std::uint64_t model_bytes = 0;
    std::error_code ec;
    const auto size = std::filesystem::file_size(profile.model_path, ec);
    if (!ec) model_bytes = static_cast<std::uint64_t>(size);

    std::optional<std::uint64_t> repeating_layers;
    if (profile.gpu_layers > 0 && model_bytes > 0) {
        const auto metadata = read_gguf_metadata(profile.model_path);
        if (metadata && metadata->block_count > 0) repeating_layers = metadata->block_count;
    }

    if (!is_full_gpu_profile(profile.gpu_layers, repeating_layers)) {
        log_info(LogChannel::Backend, "vram preflight",
                 {{"verdict", "unknown"},
                  {"gpu_layers", std::to_string(profile.gpu_layers)},
                  {"reason", profile.gpu_layers == 0
                                 ? "CPU profile; GPU query skipped"
                                 : "partial or unknown placement; GPU query skipped"}});
        return;
    }

    const auto gpu = probe_gpu_memory();
    const auto check = check_vram(
        model_bytes, profile.gpu_layers,
        gpu ? std::optional<std::uint64_t>{gpu->free_bytes} : std::nullopt,
        gpu ? std::optional<std::uint64_t>{gpu->total_bytes} : std::nullopt, repeating_layers);

    // Logged whatever the verdict. A check that only leaves a trace when it
    // fires cannot be told apart from one that never ran - which is exactly the
    // position this was in until a load went through in silence.
    const LogFields fields{{"verdict", std::string{to_string(check.verdict)}},
                           {"model_mib", std::to_string(check.model_bytes / (1024 * 1024))},
                           {"free_mib", std::to_string(check.free_bytes / (1024 * 1024))},
                           {"gpu_layers", std::to_string(profile.gpu_layers)},
                           {"device", gpu ? gpu->name : std::string{"none detected"}}};

    if (!check.worth_reporting()) {
        log_info(LogChannel::Backend, "vram preflight", fields);
        return;
    }

    const std::string message = "Not enough video memory to be confident: " + check.message;
    log_warn(LogChannel::Backend, "vram preflight", fields);

    // Its own channel. Pushing this through the server's state callback would
    // mean inventing a ServerState for it, and the server's state is a fact
    // about the server rather than somewhere to put advice.
    std::function<void(const std::string&)> advisory;
    {
        std::lock_guard lock(mutex_);
        advisory = advisory_callback_;
    }
    if (advisory) advisory(message);
}

void BackendManager::set_advisory_callback(std::function<void(const std::string&)> callback) {
    std::lock_guard lock(mutex_);
    advisory_callback_ = std::move(callback);
}

void BackendManager::set_state_callback(std::function<void(const ServerStatus&)> callback) {
    server_.set_state_callback(std::move(callback));
}

}   // namespace localcode
