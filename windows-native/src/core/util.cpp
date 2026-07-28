#include "tunhub/util.h"

#include <windows.h>

#include <cstdio>
#include <ctime>

namespace tunhub::util {

std::string newGuid() {
    GUID g{};
    if (CoCreateGuid(&g) != S_OK) return {};
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

int64_t nowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

std::string formatLogTime(int64_t unixSeconds, int millis) {
    std::tm tm{};
    const auto t = static_cast<time_t>(unixSeconds);
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return buf;
}

std::string formatDateTime(int64_t unixSeconds) {
    if (unixSeconds <= 0) return "—";
    std::tm tm{};
    const auto t = static_cast<time_t>(unixSeconds);
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string formatDuration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    char buf[64];
    if (seconds < 60) {
        std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(seconds));
    } else if (seconds < 3600) {
        std::snprintf(buf, sizeof(buf), "%lldm %llds",
                      static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60));
    } else if (seconds < 86400) {
        std::snprintf(buf, sizeof(buf), "%lldh %lldm",
                      static_cast<long long>(seconds / 3600),
                      static_cast<long long>((seconds % 3600) / 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%lldd %lldh",
                      static_cast<long long>(seconds / 86400),
                      static_cast<long long>((seconds % 86400) / 3600));
    }
    return buf;
}

}  // namespace tunhub::util
