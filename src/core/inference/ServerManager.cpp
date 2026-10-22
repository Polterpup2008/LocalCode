#include "ServerManager.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>

#include "../platform/PathEncoding.h"
#include "../platform/Paths.h"
#include "../platform/Process.h"
#include "../util/Log.h"
#include "../util/Strings.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace localcode {
namespace {

constexpr std::size_t kMaxLogLines = 3000;

// Why a server died is usually stated plainly somewhere in its output, and then
// buried under a few hundred lines of allocation chatter. Reporting only the
// tail loses it: a run that failed because another process held the GPU was
// reported as "server exited with code -1" followed by twelve lines of dots.
//
// This looks through everything the server said for a signature it can explain,
// and returns a sentence the operator can act on. It deliberately reports only
// what it recognises - an unrecognised failure still gets the raw tail, which is
// better than a confident wrong guess.
struct StartupDiagnosis {
    std::string quote;    // the line from the server that says it
    std::string advice;   // what to do about it
};

std::optional<StartupDiagnosis> diagnose_startup_failure(const std::vector<std::string>& lines) {
    auto has = [](const std::string& line, std::string_view needle) {
        return strings::icontains(line, needle);
    };

    for (const auto& line : lines) {
        // Both runtimes print this pair when the model plus its KV cache will not
        // fit in what is left on the device.
        if (has(line, "memory available on all devices")) {
            return StartupDiagnosis{
                strings::trim(line),
                "The model and its KV cache do not fit in the free device memory. "
                "Free some by closing whatever else is using the GPU, or lower this "
                "model's context size or GPU layer count in its profile."};
        }
        if (has(line, "cudamalloc") || has(line, "out of memory") ||
            has(line, "failed to allocate") || has(line, "unable to allocate")) {
            return StartupDiagnosis{
                strings::trim(line),
                "The runtime could not allocate the memory it asked for. Lower the "
                "context size or the number of offloaded layers, or free memory on "
                "the device."};
        }
        if (has(line, "error while handling argument") || has(line, "invalid argument") ||
            has(line, "unknown argument") || has(line, "unrecognized argument")) {
            return StartupDiagnosis{
                strings::trim(line),
                "This build of the server rejected an argument LocalCode passed. "
                "Run 'localcode-doctor backends' to see what it advertises, and "
                "report the mismatch."};
        }
        // Distinct from a general load failure, and worth its own advice: the
        // file is fine, this build simply does not know the architecture. The
        // two runtimes support different sets, so the other one is the first
        // thing to try - though it may not know it either, as with 'zaya',
        // which neither llama.cpp b10655 nor ik_llama b5197 recognises.
        if (has(line, "unknown model architecture")) {
            return StartupDiagnosis{
                strings::trim(line),
                "This build does not support that model architecture. The file is not "
                "corrupt - the runtime has no code for it. Try the other runtime on this "
                "model's profile; if neither knows it, the architecture needs a newer build "
                "of llama.cpp or ik_llama.cpp than the ones installed."};
        }
        if (has(line, "failed to load model") || has(line, "unable to load model") ||
            has(line, "error loading model")) {
            return StartupDiagnosis{
                strings::trim(line),
                "The runtime could not load this file. Check that it is a complete "
                "GGUF and that this runtime supports its architecture and "
                "quantisation."};
        }
        if (has(line, "bind") && has(line, "address")) {
            return StartupDiagnosis{
                strings::trim(line),
                "The port was already taken. LocalCode picks a free one per server, "
                "so this usually means something else grabbed it in between."};
        }
    }
    return std::nullopt;
}

std::mutex& probe_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct ProbeFileStamp {
    std::string path;
    std::uintmax_t size;
    std::filesystem::file_time_type modified;
    bool operator==(const ProbeFileStamp&) const = default;
};

using ProbeStamp = std::vector<ProbeFileStamp>;

struct CachedProbe {
    ProbeStamp stamp;
    ServerBinaryFeatures features;
};

std::optional<ProbeStamp> probe_stamp(const std::filesystem::path& binary) {
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(binary, ec);
    if (ec) return std::nullopt;
    ProbeStamp stamp;
    auto add = [&](const std::filesystem::path& path) {
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) return false;
        const auto modified = std::filesystem::last_write_time(path, ec);
        if (ec) return false;
        stamp.push_back({path_to_utf8(path), size, modified});
        return true;
    };
    if (!add(resolved)) return std::nullopt;
    // Official server launchers can delegate the help/version definitions to
    // adjacent shared libraries. Stat those too, without reading model weights
    // or hashing DLL contents on every model switch.
    std::filesystem::directory_iterator it(resolved.parent_path(), ec), end;
    if (ec) return std::nullopt;
    for (; it != end; it.increment(ec)) {
        if (ec) return std::nullopt;
        const auto name = strings::to_lower(path_to_utf8(it->path().filename()));
        if (!(name.ends_with(".dll") || name.ends_with(".dylib") || name.ends_with(".so") ||
              name.find(".so.") != std::string::npos))
            continue;
        if (!it->is_regular_file(ec)) {
            if (ec) return std::nullopt;
            continue;
        }
        if (!add(it->path())) return std::nullopt;
    }
    if (ec) return std::nullopt;
    std::sort(stamp.begin(), stamp.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });
    return stamp;
}

std::map<std::string, CachedProbe>& probe_cache() {
    static std::map<std::string, CachedProbe> cache;
    return cache;
}

// Inspect declaration tokens only. Prefixes and mentions in descriptions do
// not establish that an option can actually be passed to this runtime.
std::size_t help_option_position(std::string_view line, std::string_view flag) {
    std::size_t cursor = 0;
    while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor])))
        ++cursor;
    while (cursor < line.size() && line[cursor] == '-') {
        const auto start = cursor;
        const auto end = line.find_first_of(", \t\r\n=([{<", start);
        cursor = end == std::string_view::npos ? line.size() : end;
        if (strings::iequals(line.substr(start, cursor - start), flag)) return start;
        while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor])))
            ++cursor;
        // llama.cpp/ik help separates aliases with commas; whitespace alone
        // introduces a value or description, even when it starts with a flag.
        if (cursor == line.size() || line[cursor] != ',') break;
        ++cursor;
        while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor])))
            ++cursor;
    }
    return std::string_view::npos;
}

// Returns an exact declaration line so another option cannot supply its arity.
std::string help_line_for(const std::string& help, std::string_view flag) {
    for (const auto& line : strings::split_lines(help)) {
        if (help_option_position(line, flag) != std::string_view::npos) return line;
    }
    return {};
}

bool advertises(const std::string& help, std::string_view flag) {
    return !help_line_for(help, flag).empty();
}

// Does `flag` take a value? Decided from the help line rather than from a list
// of known spellings, because the runtimes disagree about the notation:
//
//   llama.cpp     -fa,   --flash-attn [on|off|auto]   set Flash Attention use
//   ik_llama.cpp  -fa,   --flash-attn (auto|on|off|0|1)
//
// and a build that wants a value but is given none exits with a usage error, so
// getting this wrong means the model simply never loads.
bool flag_takes_value(const std::string& help, std::string_view flag) {
    const auto line = help_line_for(help, flag);
    if (line.empty()) return false;

    const auto position = help_option_position(line, flag);
    if (position == std::string::npos) return false;

    auto rest = std::string_view{line}.substr(position + flag.size());
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '	')) {
        rest.remove_prefix(1);
    }
    if (rest.empty()) return false;

    // A bracketed group of any kind is a value placeholder.
    switch (rest.front()) {
        case '(':
        case '[':
        case '{':
        case '<':
        case '=': return true;
        default: break;
    }

    // So is a bare metavariable: N, FNAME, FORMAT, SIMILARITY. Description text
    // starts with an ordinary word, which contains lower-case letters.
    std::size_t length = 0;
    while (length < rest.size() && rest[length] != ' ' && rest[length] != '	')
        ++length;
    const auto token = rest.substr(0, length);
    if (token.empty() || token.size() > 24) return false;
    for (char c : token) {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isupper(uc) || std::isdigit(uc) || c == '_')) return false;
    }
    return true;
}

}   // namespace

std::string_view to_string(ServerState state) noexcept {
    switch (state) {
        case ServerState::Stopped: return "stopped";
        case ServerState::Starting: return "starting";
        case ServerState::Restarting: return "restarting";
        case ServerState::Ready: return "ready";
        case ServerState::Stopping: return "stopping";
        case ServerState::Failed: return "failed";
    }
    return "stopped";
}

bool ModelServer::help_flag_takes_value(const std::string& help_text, std::string_view flag) {
    return flag_takes_value(help_text, flag);
}

bool ModelServer::help_advertises_switch(const std::string& help, std::string_view flag) {
    for (const auto& text : strings::split_lines(help)) {
        if (help_option_position(text, flag) == std::string_view::npos) continue;
        auto line = std::string_view(text);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.remove_prefix(1);
        if (line.empty() || line.front() != '-') continue;
        bool found = false;
        while (!line.empty()) {
            while (!line.empty() && (line.front() == ',' ||
                   std::isspace(static_cast<unsigned char>(line.front())))) line.remove_prefix(1);
            if (line.empty() || line.front() != '-') break;
            const auto end = line.find_first_of(", \t\r\n");
            const auto token = line.substr(0, end);
            found = found || token == flag;
            line.remove_prefix(end == std::string_view::npos ? line.size() : end);
        }
        if (!found) continue;
        if (line.empty()) return true;
        if (std::string_view("([{<=").find(line.front()) != std::string_view::npos) return false;
        const auto token = line.substr(0, line.find_first_of(" \t\r\n"));
        const bool variable = token.size() <= 24 && std::all_of(token.begin(), token.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return std::isupper(u) || std::isdigit(u) || c == '_';
        });
        return !variable;
    }
    return false;
}

std::string ModelServer::explain_startup_failure(const std::vector<std::string>& server_output) {
    const auto diagnosis = diagnose_startup_failure(server_output);
    return diagnosis ? diagnosis->advice : std::string{};
}

ModelServer::ModelServer() = default;

ModelServer::~ModelServer() {
    stop();
}

Result<std::uint16_t> ModelServer::find_free_port() {
#if defined(_WIN32)
    // WSAStartup is reference counted; the matching cleanup happens below.
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return make_error(ErrorCode::Internal, "WSAStartup failed");
    }

    struct WsaGuard {
        ~WsaGuard() { WSACleanup(); }
    } guard;

    using socket_t = SOCKET;
    const socket_t invalid = INVALID_SOCKET;
    auto close_socket = [](socket_t s) { closesocket(s); };
#else
    using socket_t = int;
    const socket_t invalid = -1;
    auto close_socket = [](socket_t s) { ::close(s); };
#endif

    socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == invalid) {
        return make_error(ErrorCode::Internal, "cannot create a probe socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;   // let the OS pick
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        close_socket(sock);
        return make_error(ErrorCode::Internal, "cannot bind a probe socket");
    }

    sockaddr_in bound{};
#if defined(_WIN32)
    int length = sizeof bound;
#else
    socklen_t length = sizeof bound;
#endif
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        close_socket(sock);
        return make_error(ErrorCode::Internal, "cannot read the probe socket's port");
    }
    const std::uint16_t port = ntohs(bound.sin_port);
    close_socket(sock);
    return port;
}

Result<ServerBinaryFeatures> ModelServer::probe_binary(const std::filesystem::path& binary,
                                                       CancellationToken cancel) {
    if (cancel.cancelled()) return make_error(ErrorCode::Cancelled, "runtime scan cancelled");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(binary, ec)) {
        return make_error(ErrorCode::NotFound, "server binary not found: " + path_to_utf8(binary));
    }
    const auto stamp = probe_stamp(binary);
    const auto key = path_to_utf8(binary);
    {
        std::lock_guard lock(probe_cache_mutex());
        const auto it = probe_cache().find(key);
        if (stamp && it != probe_cache().end() && it->second.stamp == *stamp)
            return it->second.features;
    }

    ProcessRequest request;
    request.executable = binary;
    request.arguments = {"--help"};
    request.timeout = std::chrono::seconds(30);
    request.max_captured_bytes = 512 * 1024;

    auto result = ProcessRunner::run(request, cancel);
    if (!result) {
        return make_error(ErrorCode::BackendError, "cannot run " + path_to_utf8(binary) + " --help",
                          result.error().message);
    }

    if (result->cancelled) return make_error(ErrorCode::Cancelled, "runtime scan cancelled");
    // Partial output is not evidence of supported flags. A runtime may print
    // usage before failing or stalling, and caching that prefix hides the
    // actual probe failure on every subsequent model activation.
    if (result->timed_out) {
        return make_error(ErrorCode::Timeout, "server --help timed out", path_to_utf8(binary));
    }
    if (result->exit_code != 0) {
        return make_error(ErrorCode::BackendError, "server --help failed",
                          "exit code " + std::to_string(result->exit_code));
    }
    if (result->stdout_truncated || result->stderr_truncated) {
        return make_error(ErrorCode::BackendError, "server --help exceeds the capture limit",
                          path_to_utf8(binary));
    }
    // llama-server prints its help on stdout in most builds and stderr in some.
    const std::string help = result->stdout_text + "\n" + result->stderr_text;
    if (help.size() < 40) {
        return make_error(ErrorCode::BackendError,
                          "server binary produced no help output; is it really a llama.cpp server?",
                          path_to_utf8(binary));
    }

    const auto help_lower = strings::to_lower(help);

    ServerBinaryFeatures features;
    features.jinja = advertises(help_lower, "--jinja");
    features.reasoning_format = advertises(help_lower, "--reasoning-format");
    features.reasoning_budget = advertises(help_lower, "--reasoning-budget") &&
                                flag_takes_value(help, "--reasoning-budget");
    features.flash_attention = advertises(help_lower, "--flash-attn");
    features.cache_type_k = advertises(help_lower, "--cache-type-k");
    features.cache_type_v = advertises(help_lower, "--cache-type-v");
    features.no_webui = advertises(help_lower, "--no-webui");
    features.threads = advertises(help_lower, "--threads");
    features.threads_batch = advertises(help_lower, "--threads-batch");
    features.cpu_placement = features.threads && features.threads_batch &&
        advertises(help_lower, "--cpu-mask") && advertises(help_lower, "--cpu-mask-batch") &&
        advertises(help_lower, "--cpu-strict") && advertises(help_lower, "--cpu-strict-batch");
    features.batch_size = advertises(help_lower, "--batch-size");
    features.ubatch_size = advertises(help_lower, "--ubatch-size");
    features.cache_ram =
        advertises(help_lower, "--cache-ram") && flag_takes_value(help, "--cache-ram");
    features.repack = help_advertises_switch(help, "--repack");
    features.no_repack = help_advertises_switch(help, "--no-repack");
    // A generic speculation flag alone does not establish this model-free mode.
    const auto speculation_line = help_line_for(help_lower, "--spec-type");
    const auto speculation_flag = help_option_position(speculation_line, "--spec-type");
    if (speculation_flag != std::string::npos) {
        auto values = strings::trim(speculation_line.substr(speculation_flag + 11));
        values = values.substr(0, values.find_first_of(" \t\r\n"));
        const auto modes = strings::split(values, ',');
        features.ngram_mod = std::find(modes.begin(), modes.end(), "ngram-mod") != modes.end();
    }
    features.parallel = advertises(help_lower, "--parallel");
    features.device = advertises(help_lower, "--device");
    features.chat_template_file = advertises(help_lower, "--chat-template-file");
    features.mlock = advertises(help_lower, "--mlock");
    features.no_mmap = advertises(help_lower, "--no-mmap");
    features.load_mode = advertises(help_lower, "--load-mode");

    // Whether --flash-attn wants a value differs between builds and between the
    // two runtimes; ask the help text rather than assuming.
    if (features.flash_attention) {
        features.flash_attention_takes_value = flag_takes_value(help, "--flash-attn");
    }

    features.help_text_excerpt = strings::utf8_truncate(help, 4000);

    // Ask for the version properly rather than scraping the help text, which
    // only contains the description of the --version flag itself.
    if (advertises(help_lower, "--version")) {
        ProcessRequest version_request;
        version_request.executable = binary;
        version_request.arguments = {"--version"};
        version_request.timeout = std::chrono::seconds(20);
        version_request.max_captured_bytes = 8 * 1024;
        if (auto version = ProcessRunner::run(version_request, cancel); version) {
            if (version->cancelled)
                return make_error(ErrorCode::Cancelled, "runtime scan cancelled");
            if (version->success() && !version->stdout_truncated && !version->stderr_truncated) {
                const auto text =
                    version->stdout_text.empty() ? version->stderr_text : version->stdout_text;
                for (const auto& line : strings::split_lines(text)) {
                    const auto trimmed = strings::trim(line);
                    if (trimmed.empty()) continue;
                    features.version = strings::utf8_truncate(trimmed, 120);
                    break;
                }
            }
        }
    }

    const auto after = probe_stamp(binary);
    if (stamp && after && *stamp == *after) {
        std::lock_guard lock(probe_cache_mutex());
        probe_cache()[key] = CachedProbe{*stamp, features};
    }
    log_info(LogChannel::Backend, "probed server binary",
             {{"binary", path_to_utf8(binary.filename())},
              {"jinja", features.jinja ? "yes" : "no"},
              {"reasoning_format", features.reasoning_format ? "yes" : "no"},
              {"flash_attn", features.flash_attention ? "yes" : "no"}});
    return features;
}

std::vector<std::string> ModelServer::build_arguments(const ServerConfig& config,
                                                      const ServerBinaryFeatures& features,
                                                      std::uint16_t port) {
    const auto& profile = config.profile;
    if (profile.cpu_placement == CpuPlacement::PhysicalCores &&
        (!config.cpu_placement || !features.cpu_placement)) return {};
    if (profile.cpu_placement == CpuPlacement::PhysicalPool &&
        (!config.cpu_pool || !features.threads || !features.threads_batch)) return {};
    std::vector<std::string> args;

    auto add = [&args](std::string flag, std::string value = {}) {
        args.push_back(std::move(flag));
        if (!value.empty()) args.push_back(std::move(value));
    };

    add("--model", path_to_utf8(profile.model_path));
    add("--host", config.host);
    add("--port", std::to_string(port));
    add("--ctx-size", std::to_string(profile.context_size));
    add("--n-gpu-layers", std::to_string(profile.gpu_layers < 0 ? 999 : profile.gpu_layers));
    // Zero offloaded layers can still leave operations on CUDA. Select no GPU
    // device for CPU profiles when the runtime offers explicit selection.
    if (profile.gpu_layers == 0 && features.device) add("--device", "none");
    add("--alias", profile.id);

    if (features.batch_size && profile.batch_size > 0)
        add("--batch-size", std::to_string(profile.batch_size));
    if (features.ubatch_size && profile.ubatch_size > 0)
        add("--ubatch-size", std::to_string(profile.ubatch_size));
    if (features.cache_ram && profile.prompt_cache_mib)
        add("--cache-ram", std::to_string(*profile.prompt_cache_mib));
    if (profile.weight_repacking == WeightRepacking::Enabled && features.repack)
        args.push_back("--repack");
    if (profile.weight_repacking == WeightRepacking::Disabled && features.no_repack)
        args.push_back("--no-repack");
    if (profile.cpu_placement == CpuPlacement::PhysicalCores && config.cpu_placement) {
        const auto& placement = *config.cpu_placement;
        add("--threads", std::to_string(placement.threads));
        add("--threads-batch", std::to_string(placement.threads_batch));
        add("--cpu-mask", placement.mask);
        add("--cpu-mask-batch", placement.mask_batch);
        add("--cpu-strict", "1");
        add("--cpu-strict-batch", "1");
    } else if (profile.cpu_placement == CpuPlacement::PhysicalPool && config.cpu_pool) {
        add("--threads", std::to_string(config.cpu_pool->threads));
        add("--threads-batch", std::to_string(config.cpu_pool->threads_batch));
    } else {
        if (features.threads && profile.threads > 0)
            add("--threads", std::to_string(profile.threads));
        if (features.threads_batch && profile.threads_batch > 0)
            add("--threads-batch", std::to_string(profile.threads_batch));
    }

    // The chat template is what turns tool definitions into something the model
    // understands, so enable it whenever the binary supports it.
    if (features.jinja) args.push_back("--jinja");

    // Ask the runtime to hand back reasoning in its own field instead of inline,
    // but only when the model actually has native reasoning and the binary knows
    // the flag. LocalCode does not fabricate a reasoning channel otherwise.
    if (features.reasoning_format && profile.capabilities.supports_native_reasoning &&
        profile.reasoning_enabled) {
        add("--reasoning-format", "auto");
    }

    if (features.flash_attention &&
        (profile.flash_attention || features.flash_attention_takes_value)) {
        if (features.flash_attention_takes_value)
            add("--flash-attn", profile.flash_attention ? "on" : "off");
        else
            args.push_back("--flash-attn");
    }

    if (features.cache_type_k && !profile.cache_type_k.empty()) {
        add("--cache-type-k", profile.cache_type_k);
    }
    if (features.cache_type_v && !profile.cache_type_v.empty()) {
        add("--cache-type-v", profile.cache_type_v);
    }
    if (features.load_mode && (profile.mlock || !profile.mmap)) {
        add("--load-mode", profile.mmap ? "mmap+mlock" : profile.mlock ? "mlock" : "none");
    } else {
        if (features.mlock && profile.mlock) args.push_back("--mlock");
        if (features.no_mmap && !profile.mmap) args.push_back("--no-mmap");
    }
    if (features.no_webui) args.push_back("--no-webui");

    for (const auto& extra : profile.extra_server_arguments)
        args.push_back(extra);
    for (const auto& extra : config.extra_arguments)
        args.push_back(extra);
    return args;
}

bool ModelServer::set_state(ServerState state, std::string message) {
    // Preserve notification order when readiness, process exit and Stop race.
    std::lock_guard notification(state_notify_mutex_);
    ServerStatus snapshot;
    std::function<void(const ServerStatus&)> callback;
    {
        std::lock_guard lock(mutex_);
        if (stopping_.load() && state != ServerState::Stopping && state != ServerState::Stopped)
            return false;
        if (state == ServerState::Ready && !process_running_.load()) return false;
        status_.state = state;
        status_.message = std::move(message);
        if (state == ServerState::Ready) status_.ready_unix_ms = now_unix_ms();
        snapshot = status_;
        callback = state_callback_;
    }
    cv_.notify_all();
    if (callback) {
        try {
            callback(snapshot);
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                state_callback_ = {};
            }
            log_error(LogChannel::Backend, "model state observer threw; disabled");
        }
    }
    return true;
}

void ModelServer::append_log(std::string_view chunk) {
    std::lock_guard lock(mutex_);
    log_partial_.append(chunk);

    std::size_t start = 0;
    while (true) {
        const auto newline = log_partial_.find('\n', start);
        if (newline == std::string::npos) break;
        std::string line = log_partial_.substr(start, newline - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        log_lines_.push_back(std::move(line));
        if (log_lines_.size() > kMaxLogLines) log_lines_.pop_front();
        start = newline + 1;
    }
    if (start > 0) log_partial_.erase(0, start);
    // Guard against a server that emits a very long line with no newline.
    if (log_partial_.size() > 64 * 1024) {
        log_lines_.push_back(log_partial_);
        if (log_lines_.size() > kMaxLogLines) log_lines_.pop_front();
        log_partial_.clear();
    }
}

void ModelServer::run_process(ServerConfig config) {
    if (config.profile.cpu_placement == CpuPlacement::Invalid) {
        set_state(ServerState::Failed, "CPU placement must be os, physical_cores or physical_pool.");
        process_running_.store(false);
        return;
    }
    if (config.profile.cpu_placement == CpuPlacement::PhysicalCores ||
        config.profile.cpu_placement == CpuPlacement::PhysicalPool) {
        const auto conflicts = [](const std::string& arg) {
            const auto flag = arg.substr(0, arg.find('='));
            return flag == "--cpu-mask" || flag == "-C" || flag == "--cpu-mask-batch" || flag == "-Cb" ||
                   flag == "--cpu-range" || flag == "-Cr" || flag == "--cpu-range-batch" || flag == "-Crb" ||
                   flag == "--cpu-strict" || flag == "--cpu-strict-batch" || flag == "--threads" ||
                   flag == "-t" || flag == "--threads-batch" || flag == "-tb";
        };
        if (std::any_of(config.profile.extra_server_arguments.begin(), config.profile.extra_server_arguments.end(), conflicts) ||
            std::any_of(config.extra_arguments.begin(), config.extra_arguments.end(), conflicts)) {
            set_state(ServerState::Failed, "Physical CPU placement conflicts with extra CPU mask, range or thread arguments.");
            process_running_.store(false);
            return;
        }
    }
    if (config.profile.weight_repacking == WeightRepacking::Invalid) {
        set_state(ServerState::Failed, "Weight repacking must be runtime default, enabled or disabled.");
        process_running_.store(false);
        return;
    }
    if (config.profile.weight_repacking != WeightRepacking::RuntimeDefault) {
        const auto overrides = [](const std::string& arg) {
            return arg == "--repack" || arg.starts_with("--repack=") ||
                   arg == "--no-repack" || arg.starts_with("--no-repack=") ||
                   arg == "-nr" || arg.starts_with("-nr=");
        };
        if (std::any_of(config.profile.extra_server_arguments.begin(),
                        config.profile.extra_server_arguments.end(), overrides) ||
            std::any_of(config.extra_arguments.begin(), config.extra_arguments.end(), overrides)) {
            set_state(ServerState::Failed,
                      "Weight repacking conflicts with an extra --repack / --no-repack / -nr argument. "
                      "Choose one source for this setting.");
            process_running_.store(false);
            return;
        }
    }
    if (config.profile.prompt_cache_mib) {
        const auto value = *config.profile.prompt_cache_mib;
        if (value < 0 || value > std::numeric_limits<std::int32_t>::max()) {
            set_state(ServerState::Failed,
                      "Prompt cache RAM must be between 0 and 2147483647 MiB, or runtime default.");
            process_running_.store(false);
            return;
        }
        const auto overrides_cache = [](const std::string& arg) {
            return arg == "--cache-ram" || arg.starts_with("--cache-ram=") || arg == "-cram" ||
                   arg.starts_with("-cram=");
        };
        if (std::any_of(config.profile.extra_server_arguments.begin(),
                        config.profile.extra_server_arguments.end(), overrides_cache) ||
            std::any_of(config.extra_arguments.begin(), config.extra_arguments.end(),
                        overrides_cache)) {
            set_state(ServerState::Failed,
                      "Prompt cache RAM conflicts with an extra --cache-ram / -cram argument. "
                      "Choose one source for this setting.");
            process_running_.store(false);
            return;
        }
    }
    // Advertising a mode is not evidence that it preserves target-model output.
    // Original9B GPU gold-copy checks reproducibly corrupt a token with ngram-mod.
    // Keep the stored setting readable, but require the owner to disable it.
    if (config.profile.ngram_speculation) {
        set_state(ServerState::Failed,
                  "Adaptive n-grams are unavailable after answer-integrity checks failed. "
                  "Set Repetitive text acceleration to Disabled in Inference performance.");
        process_running_.store(false);
        return;
    }
    auto features = probe_binary(config.binary, process_cancel_.token());
    if (!features) {
        set_state(features.error().code == ErrorCode::Cancelled ? ServerState::Stopped
                                                                : ServerState::Failed,
                  features.error().to_string());
        process_running_.store(false);
        return;
    }

    const auto& profile = config.profile;
    if (profile.batch_size > 0 && profile.ubatch_size > profile.batch_size) {
        set_state(ServerState::Failed, "Prompt microbatch must not exceed the prompt batch.");
        process_running_.store(false);
        return;
    }
    for (const auto& [flag, unsupported] : std::vector<std::pair<std::string, bool>>{
             {"--threads", profile.threads > 0 && !features->threads},
             {"--threads-batch", profile.threads_batch > 0 && !features->threads_batch},
             {"--batch-size",
              profile.batch_size > 0 && profile.batch_size != 512 && !features->batch_size},
             {"--ubatch-size", profile.ubatch_size > 0 && !features->ubatch_size},
             {"--cache-ram", profile.prompt_cache_mib.has_value() && !features->cache_ram},
             {"--repack", profile.weight_repacking == WeightRepacking::Enabled && !features->repack},
             {"--no-repack", profile.weight_repacking == WeightRepacking::Disabled && !features->no_repack},
             {"--cache-type-k", !profile.cache_type_k.empty() && !features->cache_type_k},
             {"--cache-type-v", !profile.cache_type_v.empty() && !features->cache_type_v},
             {"--no-mmap", !profile.mmap && !features->no_mmap && !features->load_mode},
             {"--mlock", profile.mlock && !features->mlock && !features->load_mode}}) {
        if (!unsupported) continue;
        set_state(ServerState::Failed,
                  "Runtime does not support the configured performance option " + flag);
        process_running_.store(false);
        return;
    }

    config.cpu_placement.reset();
    config.cpu_pool.reset();
    if (profile.cpu_placement == CpuPlacement::PhysicalCores) {
        if (!features->cpu_placement) {
            set_state(ServerState::Failed, "Runtime does not support physical CPU core placement.");
            process_running_.store(false);
            return;
        }
        const auto placement = physical_cpu_placement(profile.threads, profile.threads_batch);
        if (!placement) {
            set_state(ServerState::Failed, placement.error().to_string());
            process_running_.store(false);
            return;
        }
        config.cpu_placement = *placement;
    } else if (profile.cpu_placement == CpuPlacement::PhysicalPool) {
        if (!features->threads || !features->threads_batch) {
            set_state(ServerState::Failed, "Runtime does not support generation and prompt CPU thread settings.");
            process_running_.store(false);
            return;
        }
        const auto pool = physical_cpu_pool(profile.threads, profile.threads_batch);
        if (!pool) {
            set_state(ServerState::Failed, pool.error().to_string());
            process_running_.store(false);
            return;
        }
        config.cpu_pool = *pool;
    }
    std::uint16_t port = config.port;
    if (port == 0) {
        auto free_port = find_free_port();
        if (!free_port) {
            set_state(ServerState::Failed, free_port.error().to_string());
            process_running_.store(false);
            return;
        }
        port = *free_port;
    }

    const auto arguments = build_arguments(config, *features, port);

    {
        std::lock_guard lock(mutex_);
        status_.base_url = "http://" + config.host + ":" + std::to_string(port);
        status_.model_id = config.profile.id;
        status_.started_unix_ms = now_unix_ms();
        log_file_ = app_paths().log_dir() / ("server-" + config.profile.id + ".log");
        log_lines_.clear();
        log_partial_.clear();
    }

    ProcessRequest request;
    request.executable = config.binary;
    request.arguments = arguments;
    if (config.cpu_pool) request.cpu_allowance = config.cpu_pool->logical_processors;
    request.working_directory = config.binary.parent_path();
    request.timeout = std::chrono::milliseconds::zero();   // runs until stopped
    request.max_captured_bytes = 1;                        // logging goes to the ring buffer
    request.full_log_path = log_file_;
    request.on_output = [this](std::string_view chunk, bool) { append_log(chunk); };

    log_info(LogChannel::Backend, "starting model server",
             {{"binary", path_to_utf8(config.binary)},
              {"model", path_to_utf8(config.profile.model_path.filename())},
              {"port", std::to_string(port)},
              {"args", strings::join(arguments, " ")}});

    const auto master = process_cancel_.token();
    const auto url = base_url();
    std::size_t restarts = 0;
    while (!master.cancelled()) {
        if (restarts > 0 && profile.cpu_placement == CpuPlacement::PhysicalPool) {
            const auto pool = physical_cpu_pool(profile.threads, profile.threads_batch);
            if (!pool) {
                set_state(ServerState::Failed, pool.error().to_string());
                process_running_.store(false);
                return;
            }
            config.cpu_pool = *pool;
            request.cpu_allowance = pool->logical_processors;
            request.arguments = build_arguments(config, *features, port);
        }
        CancellationSource attempt;
        CancellationRegistration linked{master, [&] {
                                            attempt.cancel();
                                            cv_.notify_all();
                                        }};
        std::atomic<bool> reached_ready{false};
        bool startup_timed_out = false;
        std::string health_error;
        {
            std::lock_guard lock(mutex_);
            status_.process_id = 0;
            status_.started_unix_ms = now_unix_ms();
            status_.ready_unix_ms = 0;
        }
        if (restarts == 0)
            set_state(ServerState::Starting, "launching " + config.binary.filename().string());
        request.on_started = [this](std::uint32_t pid) {
            {
                std::lock_guard lock(mutex_);
                status_.process_id = pid;
                process_running_.store(true);
            }
            cv_.notify_all();
        };
        // Readiness is watched only during loading, not polled while idle.
        // The execution worker alone owns/reaps each process before replacing it.
        std::jthread readiness([&](std::stop_token stop) {
            const auto deadline = std::chrono::steady_clock::now() + config.startup_timeout;
            while (!stop.stop_requested() && !attempt.cancelled()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    startup_timed_out = true;
                    attempt.cancel();
                    cv_.notify_all();
                    return;
                }
                const bool launched = process_running_.load();
                if (launched && probe_) {
                    Status health;
                    try {
                        health = probe_(url, attempt.token());
                    } catch (...) {
                        health_error = "model health probe threw";
                        attempt.cancel();
                        return;
                    }
                    if (health.ok() && !attempt.cancelled() && !stop.stop_requested()) {
                        reached_ready.store(set_state(
                            ServerState::Ready,
                            restarts == 0 ? "model loaded"
                                          : "model recovered; interrupted task was not replayed"));
                        return;
                    }
                    if (!health) health_error = health.error().message;
                }
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(400), [&] {
                    return stop.stop_requested() || attempt.cancelled() ||
                           (!launched && process_running_.load());
                });
            }
        });
        auto result = ProcessRunner::run(request, attempt.token());
        process_running_.store(false);
        readiness.request_stop();
        attempt.cancel();   // interrupt any HTTP health request before joining it
        cv_.notify_all();
        readiness.join();
        {
            std::lock_guard lock(mutex_);
            status_.process_id = 0;
        }
        if (stopping_.load() || master.cancelled()) {
            set_state(ServerState::Stopped, "stopped by request");
            return;
        }
        if (!result) {
            set_state(ServerState::Failed, result.error().to_string());
            return;
        }
        if (startup_timed_out) {
            set_state(ServerState::Failed, "model server did not become ready within " +
                                               std::to_string(config.startup_timeout.count()) +
                                               "s: " + health_error);
            return;
        }

        // The owned process ended without us asking: that is a crash.
        std::string tail;
        std::optional<StartupDiagnosis> diagnosis;
        {
            std::lock_guard lock(mutex_);
            status_.exit_code = result->exit_code;
            const std::vector<std::string> all{log_lines_.begin(), log_lines_.end()};
            diagnosis = diagnose_startup_failure(all);
            const std::size_t count = std::min<std::size_t>(log_lines_.size(), 12);
            std::vector<std::string> lines{log_lines_.end() - static_cast<std::ptrdiff_t>(count),
                                           log_lines_.end()};
            tail = strings::join(lines, "\n");
        }

        std::string message = "server exited with code " + std::to_string(result->exit_code);
        if (diagnosis) {
            // Lead with the explanation. The raw tail still follows, because a
            // diagnosis is a reading of the evidence and the evidence should be
            // visible next to it.
            message += "\n\n" + diagnosis->advice + "\n\nThe server said:\n  " + diagnosis->quote;
        }
        if (!tail.empty()) message += "\n\nLast output:\n" + tail;

        log_error(LogChannel::Backend, "model server exited unexpectedly",
                  {{"exit_code", std::to_string(result->exit_code)},
                   {"diagnosis", diagnosis ? diagnosis->quote : std::string{"none recognised"}},
                   {"tail", tail}});
        if (reached_ready.load() && config.auto_restart && restarts < config.max_restarts) {
            ++restarts;
            {
                std::lock_guard lock(mutex_);
                status_.restart_count = restarts;
            }
            append_log("\n# bounded automatic restart " + std::to_string(restarts) + "\n");
            set_state(ServerState::Restarting,
                      config.profile.display_name + " server stopped unexpectedly. Restarting (" +
                          std::to_string(restarts) + "/" + std::to_string(config.max_restarts) +
                          ").\n" + message);
            // Reuse the endpoint so the existing backend and the next task remain
            // valid. No in-flight request or mutating tool is retried here.
            continue;
        }
        if (restarts > 0)
            message = "Automatic recovery failed or its restart budget is exhausted.\n" + message;
        set_state(ServerState::Failed, std::move(message));
        return;
    }
    set_state(ServerState::Stopped, "stopped by request");
}

Status ModelServer::start(ServerConfig config, HealthProbe probe, CancellationToken cancel) {
    {
        std::lock_guard worker_lock(worker_mutex_);
        stop();
        std::lock_guard lock(mutex_);
        config_ = config;
        probe_ = std::move(probe);
        status_ = ServerStatus{};
        status_.state = ServerState::Starting;
        stopping_.store(false);
        process_cancel_.reset();
        worker_ = std::thread([this, config] { run_process(config); });
    }
    CancellationRegistration wake{cancel, [this] { cv_.notify_all(); }};
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] {
        return cancel.cancelled() || status_.state == ServerState::Ready ||
               status_.state == ServerState::Failed || status_.state == ServerState::Stopped;
    });
    const auto current = status_;
    lock.unlock();
    if (cancel.cancelled()) {
        stop();
        return make_error(ErrorCode::Cancelled, "model loading cancelled");
    }
    if (current.state == ServerState::Ready) return ok_status();
    stop();
    return make_error(ErrorCode::BackendError, "model server failed to start", current.message);
}

void ModelServer::stop() {
    std::lock_guard worker_lock(worker_mutex_);
    if (!worker_.joinable()) {
        std::lock_guard lock(mutex_);
        if (status_.state != ServerState::Stopped) status_.state = ServerState::Stopped;
        return;
    }

    stopping_.store(true);
    set_state(ServerState::Stopping, "shutting down");

    // Cancelling the ProcessRunner terminates the whole job object, so a server
    // that spawned helpers goes away with it and no GPU memory is left pinned.
    process_cancel_.cancel();
    worker_.join();
    process_cancel_.reset();
    stopping_.store(false);

    set_state(ServerState::Stopped, "stopped");
    log_info(LogChannel::Backend, "model server stopped");
}

Status ModelServer::restart(CancellationToken cancel) {
    ServerConfig config;
    HealthProbe probe;
    {
        std::lock_guard lock(mutex_);
        config = config_;
        probe = probe_;
    }
    if (config.binary.empty()) {
        return make_error(ErrorCode::InvalidArgument, "no server has been configured yet");
    }
    return start(std::move(config), std::move(probe), cancel);
}

ServerStatus ModelServer::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

std::string ModelServer::base_url() const {
    std::lock_guard lock(mutex_);
    return status_.base_url;
}

bool ModelServer::ready() const {
    std::lock_guard lock(mutex_);
    return status_.state == ServerState::Ready;
}

std::vector<std::string> ModelServer::recent_log(std::size_t max_lines) const {
    std::lock_guard lock(mutex_);
    const std::size_t count = std::min(max_lines, log_lines_.size());
    return {log_lines_.end() - static_cast<std::ptrdiff_t>(count), log_lines_.end()};
}

std::filesystem::path ModelServer::log_file() const {
    std::lock_guard lock(mutex_);
    return log_file_;
}

void ModelServer::set_state_callback(std::function<void(const ServerStatus&)> callback) {
    std::lock_guard lock(mutex_);
    state_callback_ = std::move(callback);
}

}   // namespace localcode
