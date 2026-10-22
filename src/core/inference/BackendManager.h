#pragma once
// ---------------------------------------------------------------------------
// Runtime discovery, backend selection and activation.
//
// AUTO does not mean "guess". It means: look at which runtimes are actually
// installed, at what the profile asks for, and at what the model is - then pick
// one and say why in plain language the user can read in the UI.
//
// LocalCode deliberately does not claim that either runtime is faster. They
// differ by model, quantization and hardware, and the only honest answer is to
// let the user try both. The selection reason always states the actual grounds.
// ---------------------------------------------------------------------------
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../model/ModelProfile.h"
#include "HttpClient.h"
#include "OpenAiCompatBackend.h"
#include "ServerManager.h"

namespace localcode {

struct RuntimeInstall {
    BackendPreference kind = BackendPreference::LlamaCpp;
    std::filesystem::path server_binary;
    std::string version;
    std::string source;   // "configured", "PATH", "managed runtime directory"
    ServerBinaryFeatures features;
    bool probed = false;

    [[nodiscard]] std::string display() const;
};

struct BackendSelection {
    BackendPreference chosen = BackendPreference::LlamaCpp;
    RuntimeInstall install;
    std::string reason;
};

// A value snapshot can be scanned without borrowing the manager's lifetime or
// reading mutable application paths after a dialog has closed.
struct RuntimeDiscovery {
    std::filesystem::path llama_binary;
    std::filesystem::path ik_llama_binary;
    std::filesystem::path runtime_root;
    bool use_bundled_llama = false;
};

// Where an already-running server can be attached to instead of spawning one.
struct ExternalEndpoint {
    bool enabled = false;
    std::string base_url;
    std::string api_key;
    BackendPreference kind = BackendPreference::LlamaCpp;
};

class BackendManager {
public:
    BackendManager();
    ~BackendManager();

    void set_http_client(std::shared_ptr<HttpClient> http);

    // Explicit binary paths from settings. Empty clears the override.
    void set_configured_binary(BackendPreference kind, std::filesystem::path binary);
    [[nodiscard]] std::filesystem::path configured_binary(BackendPreference kind) const;
    void set_use_bundled_llama(bool enabled);
    [[nodiscard]] bool use_bundled_llama() const;

    // Used when AUTO finds both runtimes and has no other reason to prefer one.
    void set_default_preference(BackendPreference preference);
    [[nodiscard]] BackendPreference default_preference() const;

    void set_external_endpoint(ExternalEndpoint endpoint);
    [[nodiscard]] ExternalEndpoint external_endpoint() const;

    // Scans configured paths, the managed runtime directory and PATH.
    std::vector<RuntimeInstall> discover_runtimes(bool refresh = false);
    [[nodiscard]] RuntimeDiscovery runtime_discovery() const;
    static std::vector<RuntimeInstall> discover_runtimes(const RuntimeDiscovery& request,
                                                         CancellationToken cancel = {});

    // Every server binary under `root`, paired with whether it looks like an
    // ik_llama build. Exposed because discover_runtimes() runs each binary to
    // probe it, which a test cannot do - and because getting this wrong reports
    // "no runtime was found" on a machine that has two.
    [[nodiscard]] static std::vector<std::pair<std::filesystem::path, bool>> scan_runtime_root(
        const std::filesystem::path& root, int max_depth = 3);

    // Decides which runtime to use for a profile, without starting anything.
    [[nodiscard]] Result<BackendSelection> select(const ModelProfile& profile);

    // Stops any running model, starts the selected runtime for `profile`, waits
    // for it to load, probes it and returns a ready backend.
    Result<BackendPtr> activate(const ModelProfile& profile, CancellationToken cancel = {});

    // Stops the model server and drops the backend.
    void deactivate();

    [[nodiscard]] BackendPtr current_backend() const;
    [[nodiscard]] ModelProfile current_profile() const;
    [[nodiscard]] ServerStatus server_status() const;

    // One sentence on whether the managed server survived a failed request, for
    // attaching to a transport-level error. Empty when there is nothing useful
    // to add.
    [[nodiscard]] std::string describe_server_liveness() const;
    [[nodiscard]] BackendInfo current_info() const;
    [[nodiscard]] std::vector<std::string> server_log(std::size_t max_lines = 400) const;

    ModelServer& server() { return server_; }

    void set_state_callback(std::function<void(const ServerStatus&)> callback);

    // Advice that is worth showing but is nobody's state - currently the video
    // memory preflight. Called on the thread that starts the load.
    void set_advisory_callback(std::function<void(const std::string&)> callback);

private:
    // Full-offload file-size advice; skips GPU queries when this heuristic
    // cannot apply. Advisory: it never stops a load.
    void warn_if_vram_is_short(const ModelProfile& profile);
    std::function<void(const std::string&)> advisory_callback_;
    [[nodiscard]] static std::vector<std::filesystem::path> candidate_binaries(
        BackendPreference kind, const std::filesystem::path& configured,
        const std::filesystem::path& runtime_root, bool bundled = false);

    mutable std::mutex mutex_;
    std::recursive_mutex transition_mutex_;
    std::shared_ptr<HttpClient> http_;
    ModelServer server_;

    std::filesystem::path llama_binary_;
    std::filesystem::path ik_llama_binary_;
    bool use_bundled_llama_ = false;
    BackendPreference default_preference_ = BackendPreference::LlamaCpp;
    ExternalEndpoint external_;

    std::vector<RuntimeInstall> runtimes_;
    bool runtimes_valid_ = false;

    BackendPtr backend_;
    ModelProfile profile_;
    BackendInfo info_;
};

}   // namespace localcode
