import Foundation

/// Parse result: config without secrets + secrets separately (for the Keychain).
public struct ParsedTunnel {
    public var config: TunnelConfig
    public var privateKey: String
    public var presharedKeys: [UUID: String]   // peer.id → PSK
    public var warnings: [String]
}

public struct ParseError: Error, LocalizedError {
    public let line: Int
    public let message: String
    public var errorDescription: String? { "line \(line): \(message)" }
    public init(line: Int, message: String) { self.line = line; self.message = message }
}

/// wg-quick .conf parser/serializer with AmneziaWG extensions
/// (Jc, Jmin, Jmax, S1, S2, H1–H4, I1–I5, ITime).
public enum WGQuickParser {

    // MARK: Parse

    public static func parse(name: String, text: String) throws -> ParsedTunnel {
        enum Section { case none, interface, peer }
        var section = Section.none
        var cfg = TunnelConfig()
        cfg.name = name
        var awg = AWGParams()
        var privateKey: String?
        var currentPeer: PeerConfig?
        var peerPSKs: [UUID: String] = [:]
        var currentPSK: String?
        var warnings: [String] = []

        func flushPeer(line: Int) throws {
            guard let p = currentPeer else { return }
            guard WGKey.isValidKey(p.publicKey) else {
                throw ParseError(line: line, message: "[Peer] has no valid PublicKey")
            }
            if let psk = currentPSK { peerPSKs[p.id] = psk }
            if p.allowedIPs.isEmpty { warnings.append("peer \(p.publicKey.prefix(8))…: empty AllowedIPs") }
            cfg.peers.append(p)
            currentPeer = nil
            currentPSK = nil
        }

        let lines = text.components(separatedBy: .newlines)
        for (i, raw) in lines.enumerated() {
            let lineNo = i + 1
            var line = raw
            if let hash = line.firstIndex(of: "#") { line = String(line[..<hash]) }
            line = line.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }

            if line.lowercased() == "[interface]" {
                try flushPeer(line: lineNo); section = .interface; continue
            }
            if line.lowercased() == "[peer]" {
                try flushPeer(line: lineNo)
                section = .peer
                currentPeer = PeerConfig()
                continue
            }
            guard let eq = line.firstIndex(of: "=") else {
                throw ParseError(line: lineNo, message: "expected key = value")
            }
            let key = line[..<eq].trimmingCharacters(in: .whitespaces).lowercased()
            let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            let list = value.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }

            switch section {
            case .interface:
                switch key {
                case "privatekey":
                    guard WGKey.isValidKey(value) else {
                        throw ParseError(line: lineNo, message: "PrivateKey is not a 32-byte base64 key")
                    }
                    privateKey = value
                case "address":
                    for a in list {
                        guard let r = IPAddressRange(string: a) else {
                            throw ParseError(line: lineNo, message: "invalid Address: \(a)")
                        }
                        cfg.interface.addresses.append(r)
                    }
                case "listenport":
                    guard let p = UInt16(value) else { throw ParseError(line: lineNo, message: "invalid ListenPort") }
                    cfg.interface.listenPort = p
                case "dns":
                    for d in list {
                        if EndpointUtil.isIPLiteral(d) { cfg.interface.dns.append(d) }
                        else { cfg.interface.dnsSearchDomains.append(d) }
                    }
                case "mtu":
                    guard let m = Int(value), (576...9200).contains(m) else {
                        throw ParseError(line: lineNo, message: "invalid MTU")
                    }
                    cfg.interface.mtu = m
                case "table", "fwmark", "saveconfig":
                    warnings.append("line \(lineNo): \(key) is not supported on macOS, ignored")
                case "preup": cfg.interface.preUp.append(value)
                case "postup": cfg.interface.postUp.append(value)
                case "predown": cfg.interface.preDown.append(value)
                case "postdown": cfg.interface.postDown.append(value)
                // AmneziaWG
                case "jc": awg.jc = Int(value)
                case "jmin": awg.jmin = Int(value)
                case "jmax": awg.jmax = Int(value)
                case "s1": awg.s1 = Int(value)
                case "s2": awg.s2 = Int(value)
                case "s3": awg.s3 = Int(value)
                case "s4": awg.s4 = Int(value)
                case "h1": awg.h1 = UInt32(value)
                case "h2": awg.h2 = UInt32(value)
                case "h3": awg.h3 = UInt32(value)
                case "h4": awg.h4 = UInt32(value)
                case "i1": awg.i1 = value
                case "i2": awg.i2 = value
                case "i3": awg.i3 = value
                case "i4": awg.i4 = value
                case "i5": awg.i5 = value
                case "itime": awg.itime = Int(value)
                case "j1", "j2", "j3":
                    warnings.append("line \(lineNo): parameter \(key.uppercased()) is not yet supported by the core, ignored")
                default:
                    warnings.append("line \(lineNo): unknown [Interface] key \(key)")
                }
            case .peer:
                switch key {
                case "publickey":
                    guard WGKey.isValidKey(value) else {
                        throw ParseError(line: lineNo, message: "PublicKey is not a 32-byte base64 key")
                    }
                    currentPeer?.publicKey = value
                case "presharedkey":
                    guard WGKey.isValidKey(value) else {
                        throw ParseError(line: lineNo, message: "PresharedKey is not a 32-byte base64 key")
                    }
                    currentPSK = value
                case "allowedips":
                    for a in list {
                        guard let r = IPAddressRange(string: a) else {
                            throw ParseError(line: lineNo, message: "invalid AllowedIPs: \(a)")
                        }
                        currentPeer?.allowedIPs.append(r)
                    }
                case "endpoint":
                    guard EndpointUtil.split(value) != nil else {
                        throw ParseError(line: lineNo, message: "invalid Endpoint (expected host:port)")
                    }
                    currentPeer?.endpoint = value
                case "persistentkeepalive":
                    guard let k = UInt16(value) else { throw ParseError(line: lineNo, message: "invalid PersistentKeepalive") }
                    currentPeer?.persistentKeepalive = k
                default:
                    warnings.append("line \(lineNo): unknown [Peer] key \(key)")
                }
            case .none:
                throw ParseError(line: lineNo, message: "key outside an [Interface]/[Peer] section")
            }
        }
        try flushPeer(line: lines.count)

        guard let pk = privateKey else { throw ParseError(line: 0, message: "no PrivateKey in [Interface]") }
        guard !cfg.peers.isEmpty else { throw ParseError(line: 0, message: "no [Peer] section found") }

        let awgErrors = awg.validate()
        guard awgErrors.isEmpty else { throw ParseError(line: 0, message: awgErrors.joined(separator: "; ")) }

        if !awg.isEmpty {
            cfg.kind = .amneziawg
            cfg.awg = awg
        }
        cfg.interface.publicKey = WGKey.publicKey(fromPrivate: pk) ?? ""
        if !cfg.interface.postUp.isEmpty || !cfg.interface.preUp.isEmpty {
            warnings.append("config contains PreUp/PostUp scripts — TunHub stores but never executes them (security)")
        }
        if cfg.interface.addresses.isEmpty { warnings.append("no Address in [Interface]") }

        return ParsedTunnel(config: cfg, privateKey: pk, presharedKeys: peerPSKs, warnings: warnings)
    }

    // MARK: Serialize

    public static func serialize(config: TunnelConfig,
                                 privateKey: String?,
                                 presharedKeys: [UUID: String],
                                 redactSecrets: Bool) -> String {
        var out = "[Interface]\n"
        out += "PrivateKey = \(redactSecrets ? "<REDACTED>" : (privateKey ?? "<MISSING>"))\n"
        if !config.interface.addresses.isEmpty {
            out += "Address = \(config.interface.addresses.map { "\($0.addressString)/\($0.prefix)" }.joined(separator: ", "))\n"
        }
        if let p = config.interface.listenPort { out += "ListenPort = \(p)\n" }
        let dnsAll = config.interface.dns + config.interface.dnsSearchDomains
        if !dnsAll.isEmpty { out += "DNS = \(dnsAll.joined(separator: ", "))\n" }
        if let m = config.interface.mtu { out += "MTU = \(m)\n" }
        if let a = config.awg, config.kind == .amneziawg {
            func put(_ k: String, _ v: CustomStringConvertible?) { if let v { out += "\(k) = \(v)\n" } }
            put("Jc", a.jc); put("Jmin", a.jmin); put("Jmax", a.jmax)
            put("S1", a.s1); put("S2", a.s2); put("S3", a.s3); put("S4", a.s4)
            put("H1", a.h1); put("H2", a.h2); put("H3", a.h3); put("H4", a.h4)
            put("I1", a.i1); put("I2", a.i2); put("I3", a.i3); put("I4", a.i4); put("I5", a.i5)
            put("ITime", a.itime)
        }
        for p in config.peers {
            out += "\n[Peer]\n"
            out += "PublicKey = \(p.publicKey)\n"
            if p.presharedKeyRef != nil || presharedKeys[p.id] != nil {
                out += "PresharedKey = \(redactSecrets ? "<REDACTED>" : (presharedKeys[p.id] ?? "<MISSING>"))\n"
            }
            if !p.allowedIPs.isEmpty {
                out += "AllowedIPs = \(p.allowedIPs.map { "\($0.addressString)/\($0.prefix)" }.joined(separator: ", "))\n"
            }
            if let e = p.endpoint { out += "Endpoint = \(e)\n" }
            if let k = p.persistentKeepalive { out += "PersistentKeepalive = \(k)\n" }
        }
        return out
    }
}
