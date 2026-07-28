#pragma once
// On-disk locations. Machine-wide state lives under %ProgramData%\TunHub so the service and
// the (unprivileged) UI can share it; per-user preferences live under %LOCALAPPDATA%.

#include <string>

namespace tunhub::paths {

/// %ProgramData%\TunHub
std::string base();

std::string stateDir();        // helper state (ownership registry, log mode)
std::string logsDir();
std::string tunnelsDir();      // one .json per tunnel
std::string tempDir();         // transient OpenVPN config / management password files

std::string helperLogFile();
std::string appLogFile();      // per-user
std::string ownershipFile();
std::string logModeFile();
std::string settingsFile();    // per-user UI preferences

/// Directory of the running executable — also where the bundled cores live.
std::string executableDir();
std::string coreBinary(const std::string& name);

void ensureDirectories();

}  // namespace tunhub::paths
