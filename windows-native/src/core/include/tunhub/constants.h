#pragma once
#include <string>

namespace tunhub {

/// Bumped when the app↔helper IPC contract changes; the app offers to reinstall the helper
/// when the two disagree.
inline constexpr const char* kProtocolVersion = TUNHUB_VERSION;

namespace core_binary {
inline constexpr const char* kWireGuard = "wireguard-go.exe";
inline constexpr const char* kAmneziaWg = "amneziawg-go.exe";
inline constexpr const char* kOpenVpn = "openvpn.exe";
}  // namespace core_binary

namespace ipc {
/// Named pipe for UI ↔ helper. A pipe (rather than AF_UNIX) so the server can carry a real
/// security descriptor and identify callers.
inline constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\TunHubHelper";
inline constexpr const wchar_t* kServiceName = L"TunHubHelper";
}  // namespace ipc

/// Environment stamp on every core process we spawn, so a running process can be positively
/// identified as ours (adapters can be renamed, environment can't be forged by accident).
inline constexpr const char* kOwnerEnvKey = "TUNHUB_OWNER";

}  // namespace tunhub
