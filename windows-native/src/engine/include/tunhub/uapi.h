#pragma once
// UAPI client for wireguard-go / amneziawg-go (the set=1 / get=1 protocol over a named pipe).

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tunhub/models.h"

namespace tunhub::uapi {

/// Pipe the core listens on. amneziawg-go uses its own prefix — pointing at the WireGuard one
/// makes every AmneziaWG tunnel time out waiting for a socket that will never appear.
std::wstring pipeName(const std::string& interfaceName, TunnelKind kind);

/// Wait for the pipe to appear (the core creates it a moment after start).
bool waitForPipe(const std::wstring& pipe, int timeoutMs);

/// Send a request, return the raw response. Empty optional on transport failure.
std::optional<std::string> request(const std::wstring& pipe, const std::string& body,
                                   std::string* error);

/// `set=1` — applies the config. Fails if the core answers with a non-zero errno.
bool set(const std::wstring& pipe, const std::string& config, std::string* error);

/// `get=1` — current peer statistics.
std::vector<PeerRuntime> get(const std::wstring& pipe);

/// Render a resolved spec into a `set=1` payload.
///
/// `resolvedEndpoints` maps peer index → "ip:port": endpoints are resolved before the call so
/// DNS never happens while the tunnel's own routes are in force.
std::string renderSetRequest(const ResolvedTunnelSpec& spec,
                             const std::map<int, std::string>& resolvedEndpoints,
                             std::string* error);

}  // namespace tunhub::uapi
