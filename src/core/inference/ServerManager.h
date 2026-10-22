#pragma once
// ---------------------------------------------------------------------------
// Model server lifecycle.
//
// The user should never have to keep a PowerShell window open running
// llama-server by hand. This class starts it, waits for the model to finish
// loading, watches it, restarts it when it dies unexpectedly, and shuts it down
// cleanly - including unloading the previous model before a switch, because two
// large models will not fit in VRAM at once.
//
// Server arguments are not guessed. The binary is asked for its --help once and
// only flags it actually advertises are passed, so a two-year-old build and
// last night's build both work without the user editing anything.
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../model/ModelProfile.h"
#include "../platform/CpuPlacement.h"
#include "../util/Cancellation.h"
#include "../util/Result.h"

namespace localcode {

enum class ServerState {
    Stopped,
    Starting,     // process launched, model still loading
    Restarting,   // owned ready process died; a bounded recovery is in progress
    Ready,
    Stopping,
    Failed,   // exited unexpectedly or never became healthy
};

std::string_view to_string(ServerState state) noexcept;

// What a particular server binary supports, learned from its own --help output.
struct ServerBinaryFeatures {
    bool jinja = false;              // --jinja: use the model's chat template
    bool reasoning_format = false;   // --reasoning-format: split out reasoning
    bool reasoning_budget = false;   // --reasoning-budget: native per-turn budget
    bool flash_attention = false;
    bool flash_attention_takes_value = false;   // newer builds want on/off/auto
    bool cache_type_k = false;
    bool cache_type_v = false;
    bool no_webui = false;
    bool threads = false;
    bool threads_batch = false;
    bool cpu_placement = false;
    bool batch_size = false;
    bool ubatch_size = false;
    bool cache_ram = false;
    bool repack = false;
    bool no_repack = false;
    bool ngram_mod = false;   // --spec-type explicitly advertises ngram-mod
    bool parallel = false;
    bool device = false;   // --device none: disable GPU operation offload too
    bool chat_template_file = false;
    bool mlock = false;
    bool no_mmap = false;
    bool load_mode = false;
    std::string help_text_excerpt;
    std::string version;
};

struct ServerConfig {
    std::filesystem::path binary;
    ModelProfile profile;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;   // 0 = choose a free port
    std::vector<std::string> extra_arguments;
    std::chrono::seconds startup_timeout{600};
    bool auto_restart = true;
    std::size_t max_restarts = 1;   // budget per explicit start, never reset on recovery
    // Resolved from this machine at launch; never saved into a model profile.
    std::optional<CpuPlacementPlan> cpu_placement;
    std::optional<CpuPoolPlan> cpu_pool;
};

struct ServerStatus {
    ServerState state = ServerState::Stopped;
    std::string base_url;
    std::string message;
    std::string model_id;
    int exit_code = 0;
    std::uint32_t process_id = 0;   // only the process currently owned by this manager
    std::size_t restart_count = 0;
    std::int64_t started_unix_ms = 0;
    std::int64_t ready_unix_ms = 0;
};

class ModelServer {
public:
    ModelServer();
    ~ModelServer();

    ModelServer(const ModelServer&) = delete;
    ModelServer& operator=(const ModelServer&) = delete;

    // Starts the server and blocks until it reports healthy or fails. The
    // health probe is supplied by the caller so this class stays independent of
    // any particular HTTP client.
    using HealthProbe = std::function<Status(const std::string& base_url, CancellationToken)>;

    Status start(ServerConfig config, HealthProbe probe, CancellationToken cancel = {});

    // Stops the server and waits for the process to exit.
    void stop();

    Status restart(CancellationToken cancel = {});

    [[nodiscard]] ServerStatus status() const;
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] bool ready() const;

    // The server's own stdout/stderr, newest last.
    [[nodiscard]] std::vector<std::string> recent_log(std::size_t max_lines = 400) const;
    [[nodiscard]] std::filesystem::path log_file() const;

    // Notified on every state change, from a worker thread.
    void set_state_callback(std::function<void(const ServerStatus&)> callback);

    // Reads a binary's --help once and caches the result per path.
    static Result<ServerBinaryFeatures> probe_binary(const std::filesystem::path& binary,
                                                     CancellationToken cancel = {});

    // Builds the argument vector for a profile against a probed binary. Physical
    // placement requires config.cpu_placement; unresolved requests return empty.
    static std::vector<std::string> build_arguments(const ServerConfig& config,
                                                    const ServerBinaryFeatures& features,
                                                    std::uint16_t port);

    // Finds a free TCP port on the loopback interface.
    static Result<std::uint16_t> find_free_port();

    // Whether a flag takes a value, judged from a --help listing. Exposed
    // because the runtimes notate this differently and getting it wrong stops
    // the model loading at all, so it is worth testing directly.
    [[nodiscard]] static bool help_flag_takes_value(const std::string& help_text,
                                                    std::string_view flag);
    [[nodiscard]] static bool help_advertises_switch(const std::string& help_text,
                                                     std::string_view flag);

    // The explanation for a server that died during start-up, read out of its
    // own output, or an empty string when nothing recognisable was said. Exposed
    // for the same reason: an unhelpful message here sends the operator looking
    // in the wrong place entirely.
    [[nodiscard]] static std::string explain_startup_failure(
        const std::vector<std::string>& server_output);

private:
    void run_process(ServerConfig config);
    bool set_state(ServerState state, std::string message);
    void append_log(std::string_view chunk);

    mutable std::mutex mutex_;
    std::recursive_mutex state_notify_mutex_;
    std::recursive_mutex worker_mutex_;
    std::condition_variable cv_;
    ServerStatus status_;
    ServerConfig config_;
    HealthProbe probe_;

    std::thread worker_;
    CancellationSource process_cancel_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> process_running_{false};

    std::deque<std::string> log_lines_;
    std::string log_partial_;
    std::filesystem::path log_file_;

    std::function<void(const ServerStatus&)> state_callback_;
};

}   // namespace localcode
