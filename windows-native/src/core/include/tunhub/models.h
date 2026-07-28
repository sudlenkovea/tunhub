#pragma once
// Configuration and runtime models. Mirrors the macOS build's model layer so that config
// files stay interchangeable between platforms.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tunhub/ipaddr.h"
#include "tunhub/json.h"

namespace tunhub {

enum class TunnelKind { WireGuard, AmneziaWg, OpenVpn };

std::string kindLabel(TunnelKind k);
std::string kindCoreBinary(TunnelKind k);
/// WireGuard-family tunnels share the userspace core + UAPI machinery; OpenVPN does not.
bool kindIsWireGuardFamily(TunnelKind k);
TunnelKind kindFromString(const std::string& s, TunnelKind fallback = TunnelKind::WireGuard);
std::string kindToString(TunnelKind k);

/// Reference to a secret held in the OS credential store — never written to disk.
struct SecretRef {
    std::string account;
};

// ── AmneziaWG obfuscation ────────────────────────────────────────────────────

struct AwgParams {
    std::optional<int> jc, jmin, jmax;
    std::optional<int> s1, s2;
    std::optional<int> s3;   // cookie-reply junk (AmneziaWG 2.x) — the official client sends it
    std::optional<int> s4;   // transport junk — without it the server rejects our transport
    std::optional<uint32_t> h1, h2, h3, h4;             // magic headers
    std::optional<std::string> i1, i2, i3, i4, i5;      // signature packets
    std::optional<int> itime;

    bool empty() const;
    std::vector<std::string> validate() const;

    /// "Amnezia default" preset (safe: junk without changing headers).
    static AwgParams amneziaDefault();
    static AwgParams fullObfuscation();

    Json toJson() const;
    static AwgParams fromJson(const Json& j);
};

// ── Config ───────────────────────────────────────────────────────────────────

struct InterfaceConfig {
    std::optional<SecretRef> privateKeyRef;
    std::string publicKey;                 // derived, cached for the UI
    std::vector<IpAddressRange> addresses;
    std::optional<uint16_t> listenPort;
    std::vector<std::string> dns;          // resolver IP addresses
    std::vector<std::string> dnsSearchDomains;
    std::optional<int> mtu;
    // Parsed and preserved, but NEVER executed (security).
    std::vector<std::string> preUp, postUp, preDown, postDown;
};

struct PeerConfig {
    std::string id;                        // GUID string
    std::string publicKey;
    std::optional<SecretRef> presharedKeyRef;
    std::optional<std::string> endpoint;   // "host:port"
    std::vector<IpAddressRange> allowedIPs;
    std::optional<uint16_t> persistentKeepalive;
};

enum class DnsModeKind { Global, Split, Disabled };

struct DnsMode {
    DnsModeKind kind = DnsModeKind::Global;
    std::vector<std::string> matchDomains;
};

enum class RouteModeKind { FromAllowedIPs, Custom };

struct RouteMode {
    RouteModeKind kind = RouteModeKind::FromAllowedIPs;
    std::vector<IpAddressRange> custom;
};

struct TunnelOptions {
    DnsMode dnsMode;
    RouteMode routeMode;
    bool autoConnectOnLaunch = false;
    bool killSwitch = false;
};

struct TunnelMeta {
    int64_t createdAt = 0;                 // Unix seconds
    std::optional<int64_t> lastConnectedAt;
    std::string group;
    std::string notes;
    int sortOrder = 0;
};

/// OpenVPN profile: the .ovpn text is kept verbatim so the core sees exactly what the
/// provider shipped; only credentials live in the credential store.
struct OpenVpnProfile {
    std::string configText;
    bool authUserPass = false;
    bool staticChallenge = false;
    std::string staticChallengeText;
    std::optional<SecretRef> credentialsRef;
    std::string remoteSummary;             // "host:port/proto" for the UI
};

struct TunnelConfig {
    std::string id;                        // GUID string
    std::string name;
    TunnelKind kind = TunnelKind::WireGuard;
    InterfaceConfig iface;
    std::vector<PeerConfig> peers;
    std::optional<AwgParams> awg;
    std::optional<OpenVpnProfile> openVpn;
    TunnelOptions options;
    TunnelMeta meta;
    int schemaVersion = 1;

    /// Routes that will actually be applied.
    std::vector<IpAddressRange> effectiveRoutes() const;
    bool hasDefaultRoute() const;

    /// A split tunnel (no default route) does NOT capture the system resolver globally,
    /// otherwise two such tunnels would fight over it.
    DnsMode effectiveDnsMode() const;

    Json toJson() const;
    static TunnelConfig fromJson(const Json& j);
};

// ── Resolved spec (app → helper; carries secrets, memory only) ───────────────

struct ResolvedPeer {
    std::string publicKey;
    std::optional<std::string> presharedKey;
    std::optional<std::string> endpoint;
    std::vector<IpAddressRange> allowedIPs;
    std::optional<uint16_t> keepalive;
};

struct ResolvedOpenVpn {
    std::string configText;
    std::string username;
    std::string password;
    std::string otp;
};

struct ResolvedTunnelSpec {
    std::string id;
    std::string name;
    TunnelKind kind = TunnelKind::WireGuard;
    std::string privateKey;                // base64
    std::vector<IpAddressRange> addresses;
    std::optional<uint16_t> listenPort;
    std::optional<int> mtu;
    std::vector<std::string> dnsServers;
    std::vector<std::string> dnsSearchDomains;
    DnsMode dnsMode;
    std::vector<IpAddressRange> routes;
    std::optional<AwgParams> awg;
    bool killSwitch = false;
    std::vector<ResolvedPeer> peers;
    std::optional<ResolvedOpenVpn> openVpn;

    Json toJson() const;
    static ResolvedTunnelSpec fromJson(const Json& j);
};

// ── Runtime state (helper → app) ─────────────────────────────────────────────

enum class TunnelPhase { Stopped, Starting, Up, Degraded, Failed, Stopping };

std::string phaseToString(TunnelPhase p);
TunnelPhase phaseFromString(const std::string& s);

struct PeerRuntime {
    std::string publicKey;
    std::string endpoint;
    int64_t lastHandshake = 0;             // Unix seconds, 0 = never
    uint64_t rxBytes = 0;
    uint64_t txBytes = 0;
};

struct TunnelRuntimeState {
    std::string id;
    std::string name;
    TunnelPhase phase = TunnelPhase::Stopped;
    std::string interfaceName;
    std::string errorMessage;
    std::vector<PeerRuntime> peers;
    int64_t since = 0;
    std::vector<std::string> routes;       // effective routes (e.g. OpenVPN pushed ones)

    uint64_t rxTotal() const;
    uint64_t txTotal() const;
    int64_t lastHandshake() const;
    bool handshakeFresh() const;

    Json toJson() const;
    static TunnelRuntimeState fromJson(const Json& j);
};

}  // namespace tunhub
