#pragma once
// Small cross-cutting helpers that don't belong to a specific domain module.

#include <cstdint>
#include <string>

namespace tunhub::util {

/// Fresh GUID in the canonical "XXXXXXXX-XXXX-..." form used for tunnel and peer ids.
std::string newGuid();

int64_t nowUnix();

/// "HH:MM:SS.mmm" in local time — the log-line prefix.
std::string formatLogTime(int64_t unixSeconds, int millis);

/// "2026-07-28 09:24:11" in local time, for UI fields.
std::string formatDateTime(int64_t unixSeconds);

/// "3m 12s" style uptime for the tunnel list.
std::string formatDuration(int64_t seconds);

/// Initialise Winsock once per process. Required before ANY ws2_32 call — including
/// inet_pton/inet_ntop, which the CIDR parsing uses, so both the service and the GUI need it.
/// Safe to call more than once.
void initSockets();

}  // namespace tunhub::util
