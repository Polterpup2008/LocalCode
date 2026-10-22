#include "Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>

namespace localcode {
namespace {

constexpr std::size_t kRingCapacity = 4000;

}  // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

std::string_view to_string(LogChannel channel) noexcept {
    switch (channel) {
        case LogChannel::App:      return "app";
        case LogChannel::Backend:  return "backend";
        case LogChannel::Tool:     return "tool";
        case LogChannel::Agent:    return "agent";
        case LogChannel::Security: return "security";
        case LogChannel::Count:    break;
    }
    return "app";
}

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string format_timestamp(std::int64_t unix_ms) {
    const std::time_t secs = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, static_cast<int>(unix_ms % 1000));
    return buf;
}

// ---------------------------------------------------------------------------
struct Logger::Impl {
    mutable std::mutex        mutex;
    std::atomic<LogLevel>     min_level{LogLevel::Info};
    std::filesystem::path     directory;
    std::array<std::ofstream, static_cast<std::size_t>(LogChannel::Count)> files;
    std::map<int, LogSink>    sinks;
    int                       next_sink_id = 1;
    std::deque<LogEntry>      ring;
};

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::configure(const std::filesystem::path& log_directory, LogLevel level) {
    std::lock_guard lock(impl_->mutex);
    std::error_code ec;
    if (!log_directory.empty()) std::filesystem::create_directories(log_directory, ec);
    impl_->directory = log_directory;
    impl_->min_level.store(level);

    for (std::size_t i = 0; i < static_cast<std::size_t>(LogChannel::Count); ++i) {
        // Reconfiguration can follow an application restart in the same
        // process. Opening an already-open stream silently disables logging.
        impl_->files[i].close();
        impl_->files[i].clear();
        if (log_directory.empty()) continue;
        const auto channel = static_cast<LogChannel>(i);
        auto path = log_directory / (std::string{to_string(channel)} + ".log");
        // Roll a previous session's log aside rather than growing without bound.
        std::error_code size_ec;
        if (std::filesystem::exists(path, size_ec) &&
            std::filesystem::file_size(path, size_ec) > 8ull * 1024 * 1024) {
            std::filesystem::rename(path, path.string() + ".1", size_ec);
        }
        impl_->files[i].open(path, std::ios::out | std::ios::app | std::ios::binary);
    }
}

void Logger::set_min_level(LogLevel level) { impl_->min_level.store(level); }
LogLevel Logger::min_level() const { return impl_->min_level.load(); }

int Logger::add_sink(LogSink sink) {
    std::lock_guard lock(impl_->mutex);
    const int id = impl_->next_sink_id++;
    impl_->sinks.emplace(id, std::move(sink));
    return id;
}

void Logger::remove_sink(int id) {
    std::lock_guard lock(impl_->mutex);
    impl_->sinks.erase(id);
}

void Logger::log(LogLevel level, LogChannel channel, std::string message, LogFields fields) {
    if (level < impl_->min_level.load()) return;

    LogEntry entry;
    entry.level = level;
    entry.channel = channel;
    entry.timestamp_ms = now_unix_ms();
    entry.message = std::move(message);
    entry.fields = std::move(fields);

    std::vector<LogSink> sinks_copy;
    {
        std::lock_guard lock(impl_->mutex);

        const auto index = static_cast<std::size_t>(channel);
        if (index < impl_->files.size() && impl_->files[index].is_open()) {
            auto& out = impl_->files[index];
            out << format_timestamp(entry.timestamp_ms) << ' ' << to_string(level) << ' '
                << entry.message;
            for (const auto& [key, value] : entry.fields) {
                out << ' ' << key << '=' << value;
            }
            out << '\n';
            out.flush();
        }

        impl_->ring.push_back(entry);
        if (impl_->ring.size() > kRingCapacity) impl_->ring.pop_front();

        sinks_copy.reserve(impl_->sinks.size());
        for (const auto& [id, sink] : impl_->sinks) sinks_copy.push_back(sink);
    }

    // Sinks are invoked outside the lock: a GUI sink that blocks must never
    // stall a worker thread holding the logger mutex.
    for (const auto& sink : sinks_copy) sink(entry);
}

std::filesystem::path Logger::log_directory() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->directory;
}

std::filesystem::path Logger::channel_file(LogChannel channel) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->directory.empty()) return {};
    return impl_->directory / (std::string{to_string(channel)} + ".log");
}

std::vector<LogEntry> Logger::recent(std::size_t max_entries) const {
    std::lock_guard lock(impl_->mutex);
    const std::size_t count = std::min(max_entries, impl_->ring.size());
    return {impl_->ring.end() - static_cast<std::ptrdiff_t>(count), impl_->ring.end()};
}

void log_trace(LogChannel ch, std::string msg, LogFields f) {
    Logger::instance().log(LogLevel::Trace, ch, std::move(msg), std::move(f));
}
void log_debug(LogChannel ch, std::string msg, LogFields f) {
    Logger::instance().log(LogLevel::Debug, ch, std::move(msg), std::move(f));
}
void log_info(LogChannel ch, std::string msg, LogFields f) {
    Logger::instance().log(LogLevel::Info, ch, std::move(msg), std::move(f));
}
void log_warn(LogChannel ch, std::string msg, LogFields f) {
    Logger::instance().log(LogLevel::Warn, ch, std::move(msg), std::move(f));
}
void log_error(LogChannel ch, std::string msg, LogFields f) {
    Logger::instance().log(LogLevel::Error, ch, std::move(msg), std::move(f));
}

}  // namespace localcode
