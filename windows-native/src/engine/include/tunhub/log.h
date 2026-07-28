#pragma once
// Bounded file log + in-memory ring buffer (the ring is what the UI reads over IPC).

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "tunhub/json.h"

namespace tunhub {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

std::string levelName(LogLevel l);
LogLevel levelFromName(const std::string& s);

struct LogLine {
    int64_t time = 0;       // Unix seconds
    int millis = 0;
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;

    std::string formatted() const;
    Json toJson() const;
    static LogLine fromJson(const Json& j);
};

/// How much detail we capture. Verbose multiplies log volume (every command we run, plus the
/// tunnel core's own debug stream) and costs real CPU, so it is opt-in and only takes effect
/// after a restart — that way a log never mixes two verbosity levels.
enum class LogCaptureMode { Normal, Verbose };

LogLevel modeMinLevel(LogCaptureMode m);
/// LOG_LEVEL handed to wireguard-go / amneziawg-go.
std::string modeCoreLogLevel(LogCaptureMode m);
std::string modeLabel(LogCaptureMode m);
std::string modeToString(LogCaptureMode m);
LogCaptureMode modeFromString(const std::string& s);

/// Persisted capture mode, shared by the UI and the privileged service. Read once at start.
namespace log_settings {
LogCaptureMode read();
bool write(LogCaptureMode mode);
}  // namespace log_settings

class FileLog {
public:
    explicit FileLog(std::string path);
    ~FileLog();

    FileLog(const FileLog&) = delete;
    FileLog& operator=(const FileLog&) = delete;

    void setMinLevel(LogLevel l) { minLevel_ = l; }
    LogLevel minLevel() const { return minLevel_; }

    void log(LogLevel level, const std::string& category, const std::string& message);
    void trace(const std::string& c, const std::string& m) { log(LogLevel::Trace, c, m); }
    void debug(const std::string& c, const std::string& m) { log(LogLevel::Debug, c, m); }
    void info(const std::string& c, const std::string& m)  { log(LogLevel::Info, c, m); }
    void warn(const std::string& c, const std::string& m)  { log(LogLevel::Warn, c, m); }
    void error(const std::string& c, const std::string& m) { log(LogLevel::Error, c, m); }

    /// Newest `maxLines` entries from the ring.
    std::vector<LogLine> tail(size_t maxLines) const;

    const std::string& path() const { return path_; }

private:
    void openFile();
    void trimLocked();          // keep the tail of the budget, drop the rest

    /// One file, trimmed in place — no unbounded growth and no rotated twin to merge.
    static constexpr long long kMaxBytes = 5'000'000;
    static constexpr size_t kRingCapacity = 1500;

    std::string path_;
    mutable std::mutex mutex_;
    void* handle_ = nullptr;    // HANDLE, kept open: re-opening per line is a syscall storm
    long long written_ = 0;
    std::deque<LogLine> ring_;
    LogLevel minLevel_ = LogLevel::Info;
};

}  // namespace tunhub
