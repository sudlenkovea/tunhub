#include "tunhub/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>

#include "tunhub/str.h"

#pragma comment(lib, "shell32.lib")

namespace tunhub::paths {
namespace {

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SHGetKnownFolderPath(id, 0, nullptr, &raw) != S_OK) return {};
    std::string out = str::narrow(raw);
    CoTaskMemFree(raw);
    return out;
}

std::string userBase() {
    auto local = knownFolder(FOLDERID_LocalAppData);
    return local.empty() ? base() : local + "\\TunHub";
}

}  // namespace

std::string base() {
    auto data = knownFolder(FOLDERID_ProgramData);
    return data.empty() ? "C:\\ProgramData\\TunHub" : data + "\\TunHub";
}

std::string stateDir()   { return base() + "\\state"; }
std::string logsDir()    { return base() + "\\logs"; }
std::string tunnelsDir() { return base() + "\\tunnels"; }
std::string tempDir()    { return base() + "\\tmp"; }

std::string helperLogFile() { return logsDir() + "\\helper.log"; }
std::string appLogFile()    { return userBase() + "\\app.log"; }
std::string ownershipFile() { return stateDir() + "\\owned.json"; }
std::string logModeFile()   { return stateDir() + "\\log-mode"; }
std::string settingsFile()  { return userBase() + "\\settings.json"; }

std::string executableDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return {};
    std::filesystem::path p(std::wstring(buf, n));
    return str::narrow(p.parent_path().wstring());
}

std::string coreBinary(const std::string& name) {
    return executableDir() + "\\" + name;
}

void ensureDirectories() {
    std::error_code ec;
    for (const auto& d : {base(), stateDir(), logsDir(), tunnelsDir(), tempDir()})
        std::filesystem::create_directories(std::filesystem::path(str::widen(d)), ec);
    if (auto u = userBase(); !u.empty())
        std::filesystem::create_directories(std::filesystem::path(str::widen(u)), ec);
}

}  // namespace tunhub::paths
