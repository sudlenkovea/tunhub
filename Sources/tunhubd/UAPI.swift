import Foundation
import TunHubShared

/// UAPI socket client for wireguard-go / amneziawg-go (set=1/get=1 protocol).
enum UAPIClient {

    static func request(socketPath: String, _ body: String) throws -> String {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw DaemonError("socket(): \(String(cString: strerror(errno)))") }
        defer { close(fd) }

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let maxLen = MemoryLayout.size(ofValue: addr.sun_path)
        guard socketPath.utf8.count < maxLen else { throw DaemonError("uapi path too long") }
        _ = socketPath.withCString { cs in
            withUnsafeMutablePointer(to: &addr.sun_path) { ptr in
                ptr.withMemoryRebound(to: CChar.self, capacity: maxLen) { dst in
                    strlcpy(dst, cs, maxLen)
                }
            }
        }
        let connected = withUnsafePointer(to: &addr) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                connect(fd, sa, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard connected == 0 else { throw DaemonError("connect \(socketPath): \(String(cString: strerror(errno)))") }

        var tv = timeval(tv_sec: 5, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        let data = Array(body.utf8)
        var sent = 0
        while sent < data.count {
            let n = data[sent...].withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
            guard n > 0 else { throw DaemonError("uapi write failed") }
            sent += n
        }

        var response = Data()
        var buf = [UInt8](repeating: 0, count: 4096)
        while true {
            let n = read(fd, &buf, buf.count)
            if n <= 0 { break }
            response.append(contentsOf: buf[0..<n])
            if response.count >= 2, response.suffix(2) == Data("\n\n".utf8) { break }
        }
        return String(data: response, encoding: .utf8) ?? ""
    }

    static func set(socketPath: String, config: String) throws {
        let resp = try request(socketPath: socketPath, config)
        guard let errLine = resp.split(separator: "\n").first(where: { $0.hasPrefix("errno=") }),
              errLine == "errno=0" else {
            throw DaemonError("uapi set failed: \(resp.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
    }

    static func get(socketPath: String) throws -> [PeerRuntime] {
        let resp = try request(socketPath: socketPath, "get=1\n\n")
        var peers: [PeerRuntime] = []
        var current: PeerRuntime?
        for line in resp.split(separator: "\n") {
            guard let eq = line.firstIndex(of: "=") else { continue }
            let key = String(line[..<eq])
            let value = String(line[line.index(after: eq)...])
            switch key {
            case "public_key":
                if let c = current { peers.append(c) }
                var p = PeerRuntime()
                p.publicKey = WGKey.hexToBase64(value) ?? value
                current = p
            case "endpoint": current?.endpoint = value
            case "rx_bytes": current?.rxBytes = UInt64(value) ?? 0
            case "tx_bytes": current?.txBytes = UInt64(value) ?? 0
            case "last_handshake_time_sec":
                if let sec = TimeInterval(value), sec > 0 {
                    current?.lastHandshake = Date(timeIntervalSince1970: sec)
                }
            default: break
            }
        }
        if let c = current { peers.append(c) }
        return peers
    }
}

/// Render ResolvedTunnelSpec → UAPI `set=1`.
enum ConfigRenderer {

    /// endpoints are already resolved: peerIndex → "ip:port"
    static func uapiSet(spec: ResolvedTunnelSpec, resolvedEndpoints: [Int: String]) throws -> String {
        guard let pk = WGKey.base64ToHex(spec.privateKey) else { throw DaemonError("bad private key") }
        var s = "set=1\n"
        s += "private_key=\(pk)\n"
        if let lp = spec.listenPort { s += "listen_port=\(lp)\n" }
        if spec.kind == .amneziawg, let a = spec.awg, !a.isEmpty {
            func put(_ k: String, _ v: CustomStringConvertible?) { if let v { s += "\(k)=\(v)\n" } }
            put("jc", a.jc); put("jmin", a.jmin); put("jmax", a.jmax)
            put("s1", a.s1); put("s2", a.s2); put("s3", a.s3); put("s4", a.s4)
            put("h1", a.h1); put("h2", a.h2); put("h3", a.h3); put("h4", a.h4)
            put("i1", a.i1); put("i2", a.i2); put("i3", a.i3); put("i4", a.i4); put("i5", a.i5)
            put("itime", a.itime)
        }
        s += "replace_peers=true\n"
        for (i, p) in spec.peers.enumerated() {
            guard let pub = WGKey.base64ToHex(p.publicKey) else { throw DaemonError("bad peer public key") }
            s += "public_key=\(pub)\n"
            if let psk = p.presharedKey, let pskHex = WGKey.base64ToHex(psk) {
                s += "preshared_key=\(pskHex)\n"
            }
            if let ep = resolvedEndpoints[i] { s += "endpoint=\(ep)\n" }
            s += "replace_allowed_ips=true\n"
            for r in p.allowedIPs { s += "allowed_ip=\(r.canonical)\n" }
            if let k = p.keepalive, k > 0 { s += "persistent_keepalive_interval=\(k)\n" }
        }
        s += "\n"
        return s
    }

    /// Resolve the endpoints of all peers. Throws on total DNS failure.
    /// Uses direct DNS over the PHYSICAL interface — otherwise resolution goes into
    /// an already-up tunnel with a default route and hangs.
    static func resolveEndpoints(spec: ResolvedTunnelSpec) throws -> [Int: String] {
        let physIf = RouteManager.shared.physicalDefaultGateway().iface
        var out: [Int: String] = [:]
        for (i, p) in spec.peers.enumerated() {
            guard let ep = p.endpoint, let (host, port) = EndpointUtil.split(ep) else { continue }
            // 1) direct DNS over the physical interface; 2) system resolver with a timeout
            let ip = DirectDNS.resolve(host, boundToInterface: physIf)
                ?? Resolver.resolveWithTimeout(host, timeout: 4)
            guard let ip else {
                throw DaemonError("could not resolve endpoint \(host)")
            }
            flog.debug("dns", "endpoint \(host) → \(ip) (iface=\(physIf ?? "-"))")
            out[i] = ip.contains(":") ? "[\(ip)]:\(port)" : "\(ip):\(port)"
        }
        return out
    }
}
