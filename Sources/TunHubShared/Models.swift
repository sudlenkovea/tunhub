import Foundation

public enum TunnelKind: String, Codable, CaseIterable, Identifiable {
    case wireguard, amneziawg, openvpn
    public var id: String { rawValue }
    public var label: String {
        switch self {
        case .wireguard: return "WireGuard"
        case .amneziawg: return "AmneziaWG"
        case .openvpn:   return "OpenVPN"
        }
    }
    /// Core binary bundled for this tunnel kind. AmneziaWG uses v0.2.x which is
    /// backward-compatible with the 1.5 protocol, so a single core covers both.
    public var coreBinary: String {
        switch self {
        case .wireguard: return TunHub.Core.wireguard
        case .amneziawg: return TunHub.Core.amneziawg
        case .openvpn:   return TunHub.Core.openvpn
        }
    }
    /// WireGuard-family tunnels share the userspace-core + UAPI machinery; OpenVPN does not.
    public var isWireGuardFamily: Bool { self == .wireguard || self == .amneziawg }
}

public struct KeychainRef: Codable, Equatable, Hashable {
    public var account: String
    public init(account: String) { self.account = account }
}

// MARK: - AWG obfuscation

public struct AWGParams: Codable, Equatable {
    public var jc: Int?
    public var jmin: Int?
    public var jmax: Int?
    public var s1: Int?
    public var s2: Int?
    public var s3: Int?   // cookie-reply junk (AmneziaWG 2.x) — the official client sends it
    public var s4: Int?   // transport junk — critical: without it the server rejects our transport
    public var h1: UInt32?
    public var h2: UInt32?
    public var h3: UInt32?
    public var h4: UInt32?
    public var i1: String?
    public var i2: String?
    public var i3: String?
    public var i4: String?
    public var i5: String?
    public var itime: Int?

    public init() {}

    public var isEmpty: Bool {
        jc == nil && jmin == nil && jmax == nil && s1 == nil && s2 == nil &&
        s3 == nil && s4 == nil &&
        h1 == nil && h2 == nil && h3 == nil && h4 == nil &&
        i1 == nil && i2 == nil && i3 == nil && i4 == nil && i5 == nil && itime == nil
    }


    public func validate() -> [String] {
        var e: [String] = []
        if let v = jc, !(0...128).contains(v) { e.append("Jc must be 0…128") }
        if let a = jmin, let b = jmax, a > b { e.append("Jmin > Jmax") }
        if let v = jmin, !(0...1280).contains(v) { e.append("Jmin must be 0…1280") }
        if let v = jmax, !(0...1280).contains(v) { e.append("Jmax must be 0…1280") }
        if let v = s1, !(0...1132).contains(v) { e.append("S1 out of range (0…1132)") }
        if let v = s2, !(0...1188).contains(v) { e.append("S2 out of range (0…1188)") }
        let hs = [h1, h2, h3, h4].compactMap { $0 }
        if !hs.isEmpty {
            if hs.count != 4 { e.append("H1–H4 must all be set together") }
            if Set(hs).count != hs.count { e.append("H1–H4 must be pairwise distinct") }
        }
        return e
    }

    /// "Amnezia default" preset (safe: junk without changing headers).
    public static func amneziaDefault() -> AWGParams {
        var p = AWGParams()
        p.jc = Int.random(in: 3...10); p.jmin = 50; p.jmax = 1000
        p.s1 = 0; p.s2 = 0
        return p
    }

    public static func fullObfuscation() -> AWGParams {
        var p = AWGParams()
        p.jc = Int.random(in: 4...12); p.jmin = 40; p.jmax = 70
        p.s1 = Int.random(in: 15...150); p.s2 = Int.random(in: 15...150)
        var hs = Set<UInt32>()
        while hs.count < 4 { hs.insert(UInt32.random(in: 5...2_147_483_647)) }
        let a = Array(hs)
        p.h1 = a[0]; p.h2 = a[1]; p.h3 = a[2]; p.h4 = a[3]
        return p
    }
}

// MARK: - OpenVPN

public struct OpenVPNRemote: Codable, Equatable {
    public var host: String
    public var port: UInt16
    public var proto: String   // "udp" / "tcp"
    public init(host: String, port: UInt16, proto: String) {
        self.host = host; self.port = port; self.proto = proto
    }
}

public enum OpenVPNAuthMode: String, Codable {
    case cert            // certificate only
    case userPass        // username/password only
    case userPassCert    // both
}

public struct OpenVPNStaticChallenge: Codable, Equatable {
    public var text: String
    public var echo: Bool    // whether the OTP field should be shown (not masked)
    public init(text: String, echo: Bool) { self.text = text; self.echo = echo }
}

/// Parsed metadata for an OpenVPN profile. The full `.ovpn` (with sensitive inline blocks
/// replaced by placeholders) lives in `configText`; the actual secret material and
/// username/password live in the Keychain. Scripts are never executed (see design §7).
public struct OpenVPNProfile: Codable, Equatable {
    public var remotes: [OpenVPNRemote] = []
    public var authMode: OpenVPNAuthMode = .cert
    public var needsUsername: Bool = false           // `auth-user-pass` present (no inline creds)
    public var staticChallenge: OpenVPNStaticChallenge?
    public var cipher: String?                       // legacy single `cipher`
    public var dataCiphers: [String] = []            // `data-ciphers`
    public var redirectGateway: Bool = false
    public var dns: [String] = []                    // `dhcp-option DNS` in the profile (if any)
    public var searchDomains: [String] = []          // `dhcp-option DOMAIN`
    public var usesInlineCompression: Bool = false   // comp-lzo / compress (VORACLE warning)
    /// Raw `.ovpn` text with sensitive inline blocks (<key>, <tls-auth>, <tls-crypt>, …)
    /// and any `auth-user-pass` inline creds replaced by placeholders resolved at connect time.
    public var configText: String = ""
    public init() {}
}

// MARK: - Config

public struct InterfaceConfig: Codable, Equatable {
    public var privateKeyRef: KeychainRef?
    public var publicKey: String = ""            // derived, cached for the UI
    public var addresses: [IPAddressRange] = []
    public var listenPort: UInt16?
    public var dns: [String] = []                // resolver IP addresses
    public var dnsSearchDomains: [String] = []
    public var mtu: Int?
    /// Scripts are parsed and stored, but NEVER executed (see design §7).
    public var preUp: [String] = []
    public var postUp: [String] = []
    public var preDown: [String] = []
    public var postDown: [String] = []
    public init() {}
}

public struct PeerConfig: Codable, Equatable, Identifiable {
    public var id: UUID = UUID()
    public var publicKey: String = ""
    public var presharedKeyRef: KeychainRef?
    public var endpoint: String?                 // "host:port"
    public var allowedIPs: [IPAddressRange] = []
    public var persistentKeepalive: UInt16?
    public init() {}
}

public enum DNSMode: Codable, Equatable {
    case global
    case split([String])   // match domains
    case disabled
}

public enum RouteMode: Codable, Equatable {
    case fromAllowedIPs
    case custom([IPAddressRange])
}

public enum HealthAction: String, Codable, CaseIterable, Identifiable {
    case notify, restart, failover
    public var id: String { rawValue }
}

public struct HealthCheckConfig: Codable, Equatable {
    public var host: String = ""                 // probe target (inside the tunnel)
    public var port: UInt16 = 443
    public var intervalSec: Int = 30
    public var failureThreshold: Int = 3
    public var action: HealthAction = .notify
    public init() {}
}

public struct TunnelOptions: Codable, Equatable {
    public var dnsMode: DNSMode = .global
    public var routeMode: RouteMode = .fromAllowedIPs
    public var autoConnectOnLaunch: Bool = false
    public var killSwitch: Bool = false
    public var healthCheck: HealthCheckConfig?
    public var failoverGroup: String?
    public var failoverPriority: Int = 0
    public init() {}
}

public struct TunnelMeta: Codable, Equatable {
    public var createdAt: Date = Date()
    public var lastConnectedAt: Date?
    public var group: String?
    public var notes: String = ""
    public var sortOrder: Int = 0
    public init() {}
}

public struct TunnelConfig: Codable, Equatable, Identifiable {
    public var id: UUID = UUID()
    public var name: String = ""
    public var kind: TunnelKind = .wireguard
    public var interface: InterfaceConfig = .init()
    public var peers: [PeerConfig] = []
    public var awg: AWGParams?
    public var openvpn: OpenVPNProfile?
    public var options: TunnelOptions = .init()
    public var meta: TunnelMeta = .init()
    public var schemaVersion: Int = 1
    public init() {}

    /// The routes that will actually be applied.
    public func effectiveRoutes() -> [IPAddressRange] {
        switch options.routeMode {
        case .custom(let r): return r
        case .fromAllowedIPs:
            var seen = Set<String>()
            var out: [IPAddressRange] = []
            for p in peers {
                for r in p.allowedIPs where seen.insert(r.canonical).inserted {
                    out.append(r)
                }
            }
            return out
        }
    }

    public var hasDefaultRoute: Bool {
        effectiveRoutes().contains { $0.prefix <= 1 }
    }

    /// Effective DNS mode. A split tunnel (no default route) does NOT capture the system
    /// DNS globally — otherwise two such tunnels would fight over the system resolver.
    public var effectiveDNSMode: DNSMode {
        switch options.dnsMode {
        case .global:
            return hasDefaultRoute ? .global : .disabled
        default:
            return options.dnsMode
        }
    }
}

// MARK: - Resolved spec (app → daemon, carries secrets, lives only in memory)

public struct ResolvedPeer: Codable {
    public var publicKey: String
    public var presharedKey: String?
    public var endpoint: String?
    public var allowedIPs: [IPAddressRange]
    public var keepalive: UInt16?
    public init(publicKey: String, presharedKey: String?, endpoint: String?,
                allowedIPs: [IPAddressRange], keepalive: UInt16?) {
        self.publicKey = publicKey; self.presharedKey = presharedKey
        self.endpoint = endpoint; self.allowedIPs = allowedIPs; self.keepalive = keepalive
    }
}

/// OpenVPN payload of a resolved spec: the full `.ovpn` with inline secrets already
/// substituted (ready to write to a root-only temp file) plus credentials/OTP.
public struct ResolvedOpenVPN: Codable {
    public var configText: String          // complete .ovpn, secrets inlined
    public var username: String?
    public var password: String?
    public var otp: String?                // pre-entered OTP (static-challenge)
    public var staticChallenge: OpenVPNStaticChallenge?
    public var remotes: [OpenVPNRemote]    // for kill-switch endpoint pinning
    public var dns: [String]               // client-side dhcp-option DNS (if any)
    public var redirectGateway: Bool
    public init(configText: String, username: String?, password: String?, otp: String?,
                staticChallenge: OpenVPNStaticChallenge?, remotes: [OpenVPNRemote],
                dns: [String], redirectGateway: Bool) {
        self.configText = configText; self.username = username; self.password = password
        self.otp = otp; self.staticChallenge = staticChallenge; self.remotes = remotes
        self.dns = dns; self.redirectGateway = redirectGateway
    }
}

public struct ResolvedTunnelSpec: Codable {
    public var id: UUID
    public var name: String
    public var kind: TunnelKind
    public var privateKey: String                // base64
    public var addresses: [IPAddressRange]
    public var listenPort: UInt16?
    public var mtu: Int?
    public var dnsServers: [String]
    public var dnsSearchDomains: [String]
    public var dnsMode: DNSMode
    public var routes: [IPAddressRange]
    public var awg: AWGParams?
    public var openvpn: ResolvedOpenVPN?
    public var killSwitch: Bool
    public var peers: [ResolvedPeer]
    public init(id: UUID, name: String, kind: TunnelKind,
                privateKey: String,
                addresses: [IPAddressRange], listenPort: UInt16?, mtu: Int?,
                dnsServers: [String], dnsSearchDomains: [String], dnsMode: DNSMode,
                routes: [IPAddressRange], awg: AWGParams?, killSwitch: Bool, peers: [ResolvedPeer],
                openvpn: ResolvedOpenVPN? = nil) {
        self.id = id; self.name = name; self.kind = kind
        self.privateKey = privateKey
        self.addresses = addresses; self.listenPort = listenPort; self.mtu = mtu
        self.dnsServers = dnsServers; self.dnsSearchDomains = dnsSearchDomains
        self.dnsMode = dnsMode; self.routes = routes; self.awg = awg
        self.killSwitch = killSwitch; self.peers = peers
        self.openvpn = openvpn
    }
}

// MARK: - Runtime state (daemon → app)

public enum TunnelPhase: String, Codable {
    case stopped, starting, up, degraded, failed, stopping
}

public struct PeerRuntime: Codable, Identifiable {
    public var publicKey: String = ""
    public var endpoint: String?
    public var lastHandshake: Date?
    public var rxBytes: UInt64 = 0
    public var txBytes: UInt64 = 0
    public var id: String { publicKey }
    public init() {}
}

public struct TunnelRuntimeState: Codable, Identifiable {
    public var id: UUID
    public var name: String
    public var phase: TunnelPhase
    public var utunName: String?
    public var errorMessage: String?
    public var peers: [PeerRuntime]
    public var since: Date?
    public init(id: UUID, name: String, phase: TunnelPhase, utunName: String?,
                errorMessage: String?, peers: [PeerRuntime], since: Date?) {
        self.id = id; self.name = name; self.phase = phase; self.utunName = utunName
        self.errorMessage = errorMessage; self.peers = peers; self.since = since
    }

    public var rxTotal: UInt64 { peers.reduce(0) { $0 + $1.rxBytes } }
    public var txTotal: UInt64 { peers.reduce(0) { $0 + $1.txBytes } }
    public var lastHandshake: Date? { peers.compactMap(\.lastHandshake).max() }
    public var handshakeFresh: Bool {
        guard let h = lastHandshake else { return false }
        return Date().timeIntervalSince(h) < 185
    }
}

// MARK: - JSON helpers

public enum TunJSON {
    public static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        return e
    }()
    public static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }()
}

public enum ByteFormat {
    public static func human(_ v: UInt64) -> String {
        let f = ByteCountFormatter()
        f.countStyle = .binary
        return f.string(fromByteCount: Int64(v))
    }
    public static func rate(_ bytesPerSec: Double) -> String {
        human(UInt64(max(0, bytesPerSec))) + "/s"
    }
}
