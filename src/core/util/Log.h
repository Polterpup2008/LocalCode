#pragma once
// ---------------------------------------------------------------------------
// Structured logging.
//
// Logs are split into independent channels so a crash in the model backend can
// be read without wading through filesystem tool chatter. Each channel writes to
// its own file under <data>/logs and, when a sink is installed, is mirrored into
// the GUI's log views.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace localcode {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

std::string_view to_string(LogLevel level) noexcept;

// Channels are fixed: the GUI builds one viewer per channel.
enum class LogChannel {
    App = 0,      // application lifecycle, settings, storage
    Backend,      // inference server management + HTTP traffic metadata
    Tool,         // tool invocations and their results
    Agent,        // agent loop decisions, context management
    Security,     // policy decisions: allowed, confirmed, blocked
    Count
};

std::string_view to_string(LogChannel channel) noexcept;

using LogFields = std::vector<std::pair<std::string, std::string>>;

// A single log entry as delivered to sinks.
struct LogEntry {
    LogLevel     level = LogLevel::Info;
    LogChannel   channel = LogChannel::App;
    std::int64_t timestamp_ms = 0;
    std::string  message;
    LogFields    fields;   // rendered as key=value in the text log
};

using LogSink = std::function<void(const LogEntry&)>;

class Logger {
public:
    static Logger& instance();

    // Enables file logging. Safe to call once at start-up.
    void configure(const std::filesystem::path& log_directory, LogLevel min_level);

    void set_min_level(LogLevel level);
    [[nodiscard]] LogLevel min_level() const;

    // Sinks receive every entry that passes the level filter. The callback runs
    // on the logging thread - GUI sinks must marshal to the UI thread.
    int  add_sink(LogSink sink);
    void remove_sink(int id);

    void log(LogLevel level, LogChannel channel, std::string message, LogFields fields = {});

    [[nodiscard]] std::filesystem::path log_directory() const;
    [[nodiscard]] std::filesystem::path channel_file(LogChannel channel) const;

    // Most recent entries, newest last. Used to seed GUI views on start-up.
    [[nodiscard]] std::vector<LogEntry> recent(std::size_t max_entries) const;

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- Convenience free functions -------------------------------------------
void log_trace(LogChannel ch, std::string msg, LogFields fields = {});
void log_debug(LogChannel ch, std::string msg, LogFields fields = {});
void log_info (LogChannel ch, std::string msg, LogFields fields = {});
void log_warn (LogChannel ch, std::string msg, LogFields fields = {});
void log_error(LogChannel ch, std::string msg, LogFields fields = {});

// Current wall-clock time in milliseconds since the Unix epoch.
[[nodiscard]] std::int64_t now_unix_ms();

// ISO-8601 formatting of a Unix millisecond timestamp (local time).
[[nodiscard]] std::string format_timestamp(std::int64_t unix_ms);

}  // namespace localcode
