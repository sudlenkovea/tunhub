#include "tunhub/log.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "tunhub/paths.h"
#include "tunhub/str.h"
#include "tunhub/util.h"

namespace tunhub {
namespace {

int64_t nowWithMillis(int& millis) {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME is 100 ns ticks since 1601; convert to Unix seconds + milliseconds.
    const uint64_t ticks = u.QuadPart - 116444736000000000ULL;
    millis = static_cast<int>((ticks / 10000ULL) % 1000ULL);
    return static_cast<int64_t>(ticks / 10000000ULL);
}

}  // namespace

std::string levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        default:              return "info";
    }
}

LogLevel levelFromName(const std::string& s) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "warn")  return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return LogLevel::Info;
}

std::string LogLine::formatted() const {
    static const char* glyphs[] = {"·", "▹", "•", "!", "✕"};
    return util::formatLogTime(time, millis) + " " +
           glyphs[static_cast<int>(level)] + " [" + category + "] " + message;
}

Json LogLine::toJson() const {
    Json j = Json::object();
    j.set("time", Json(static_cast<long long>(time)));
    j.set("millis", Json(millis));
    j.set("level", Json(levelName(level)));
    j.set("category", Json(category));
    j.set("message", Json(message));
    return j;
}

LogLine LogLine::fromJson(const Json& j) {
    LogLine l;
    l.time = j["time"].asInt64(0);
    l.millis = j["millis"].asInt(0);
    l.level = levelFromName(j["level"].asString("info"));
    l.category = j["category"].asString("");
    l.message = j["message"].asString("");
    return l;
}

// ── capture mode ─────────────────────────────────────────────────────────────

LogLevel modeMinLevel(LogCaptureMode m) {
    return m == LogCaptureMode::Verbose ? LogLevel::Trace : LogLevel::Info;
}

std::string modeCoreLogLevel(LogCaptureMode m) {
    return m == LogCaptureMode::Verbose ? "verbose" : "error";
}

std::string modeLabel(LogCaptureMode m) {
    return m == LogCaptureMode::Verbose ? "Verbose (debug)" : "Normal";
}

std::string modeToString(LogCaptureMode m) {
    return m == LogCaptureMode::Verbose ? "verbose" : "normal";
}

LogCaptureMode modeFromString(const std::string& s) {
    return str::iequals(str::trim(s), "verbose") ? LogCaptureMode::Verbose : LogCaptureMode::Normal;
}

namespace log_settings {

LogCaptureMode read() {
    std::ifstream f(std::filesystem::path(str::widen(paths::logModeFile())), std::ios::binary);
    if (!f) return LogCaptureMode::Normal;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return modeFromString(s);
}

bool write(LogCaptureMode mode) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(str::widen(paths::stateDir())), ec);
    std::ofstream f(std::filesystem::path(str::widen(paths::logModeFile())),
                    std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const auto text = modeToString(mode);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

}  // namespace log_settings

// ── FileLog ──────────────────────────────────────────────────────────────────

FileLog::FileLog(std::string path) : path_(std::move(path)) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(str::widen(path_)).parent_path(), ec);
    openFile();
}

FileLog::~FileLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
}

void FileLog::openFile() {
    // FILE_SHARE_READ so the UI (and a support engineer with a text editor) can read the log
    // while the service holds it open.
    HANDLE h = CreateFileW(str::widen(path_).c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { handle_ = nullptr; return; }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    written_ = size.QuadPart;
    handle_ = h;
}

void FileLog::trimLocked() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    constexpr long long keep = kMaxBytes * 4 / 5;   // leave headroom so we don't trim per write
    std::vector<char> tail;
    {
        std::ifstream in(std::filesystem::path(str::widen(path_)), std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const long long size = in.tellg();
            const long long start = size > keep ? size - keep : 0;
            in.seekg(start, std::ios::beg);
            tail.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    // Start on a record boundary so the first line isn't a fragment.
    size_t offset = 0;
    for (size_t i = 0; i < tail.size(); ++i)
        if (tail[i] == '\n') { offset = i + 1; break; }

    {
        std::ofstream out(std::filesystem::path(str::widen(path_)),
                          std::ios::binary | std::ios::trunc);
        if (out && offset < tail.size())
            out.write(tail.data() + offset, static_cast<std::streamsize>(tail.size() - offset));
    }
    openFile();
}

void FileLog::log(LogLevel level, const std::string& category, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(minLevel_)) return;

    LogLine line;
    line.time = nowWithMillis(line.millis);
    line.level = level;
    line.category = category;
    line.message = message;

    const std::string text = line.formatted() + "\r\n";

    std::lock_guard<std::mutex> lock(mutex_);
    ring_.push_back(std::move(line));
    while (ring_.size() > kRingCapacity) ring_.pop_front();

    if (!handle_ || handle_ == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(handle_), text.data(),
              static_cast<DWORD>(text.size()), &written, nullptr);
    written_ += written;
    if (written_ > kMaxBytes) trimLocked();
}

std::vector<LogLine> FileLog::tail(size_t maxLines) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (maxLines == 0 || ring_.empty()) return {};
    const size_t take = maxLines < ring_.size() ? maxLines : ring_.size();
    return std::vector<LogLine>(ring_.end() - static_cast<ptrdiff_t>(take), ring_.end());
}

}  // namespace tunhub
