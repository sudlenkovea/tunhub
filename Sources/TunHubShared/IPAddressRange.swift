import Foundation

/// CIDR range (v4/v6) with canonicalization and overlap math.
public struct IPAddressRange: Equatable, Hashable, CustomStringConvertible {
    public let addressString: String   // as the user typed it (address part)
    public let prefix: Int
    public let bytes: [UInt8]          // 4 or 16 bytes, network byte order

    public var isIPv6: Bool { bytes.count == 16 }
    public var maxPrefix: Int { isIPv6 ? 128 : 32 }
    public var isDefault: Bool { prefix == 0 }

    public init?(string: String) {
        let parts = string.split(separator: "/", maxSplits: 1).map(String.init)
        guard let first = parts.first, !first.isEmpty else { return nil }
        guard let b = Self.pton(first) else { return nil }
        let maxP = b.count == 16 ? 128 : 32
        var p = maxP
        if parts.count == 2 {
            guard let pp = Int(parts[1]), (0...maxP).contains(pp) else { return nil }
            p = pp
        }
        self.addressString = first
        self.prefix = p
        self.bytes = b
    }

    public static func pton(_ s: String) -> [UInt8]? {
        var v4 = in_addr()
        if inet_pton(AF_INET, s, &v4) == 1 {
            return withUnsafeBytes(of: &v4) { Array($0) }
        }
        var v6 = in6_addr()
        if inet_pton(AF_INET6, s, &v6) == 1 {
            return withUnsafeBytes(of: &v6) { Array($0) }
        }
        return nil
    }

    public static func ntop(_ bytes: [UInt8]) -> String {
        if bytes.count == 4 {
            return bytes.map(String.init).joined(separator: ".")
        }
        var buf = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
        let b = bytes
        _ = b.withUnsafeBytes { raw in
            inet_ntop(AF_INET6, raw.baseAddress, &buf, socklen_t(INET6_ADDRSTRLEN))
        }
        return String(cString: buf)
    }

    /// Network address (host bits zeroed).
    public var networkBytes: [UInt8] {
        var out = bytes
        let fullBytes = prefix / 8
        let remBits = prefix % 8
        for i in 0..<out.count {
            if i < fullBytes { continue }
            if i == fullBytes && remBits > 0 {
                out[i] &= UInt8(0xFF << (8 - remBits) & 0xFF)
            } else if i >= fullBytes && !(i == fullBytes && remBits > 0) {
                out[i] = 0
            }
        }
        return out
    }

    /// Canonical form "network/prefix".
    public var canonical: String { "\(Self.ntop(networkBytes))/\(prefix)" }
    public var description: String { canonical }

    /// self (as a network) fully contains the other network.
    public func contains(_ other: IPAddressRange) -> Bool {
        guard isIPv6 == other.isIPv6, prefix <= other.prefix else { return false }
        let a = networkBytes, b = other.networkBytes
        let fullBytes = prefix / 8
        let remBits = prefix % 8
        for i in 0..<fullBytes where a[i] != b[i] { return false }
        if remBits > 0 {
            let mask = UInt8(0xFF << (8 - remBits) & 0xFF)
            if (a[fullBytes] & mask) != (b[fullBytes] & mask) { return false }
        }
        return true
    }

    /// Whether the range contains a specific address.
    public func containsAddress(_ addr: String) -> Bool {
        guard let r = IPAddressRange(string: addr) else { return false }
        return contains(r)
    }

    public func overlaps(_ other: IPAddressRange) -> Bool {
        contains(other) || other.contains(self)
    }
}

extension IPAddressRange: Codable {
    public init(from decoder: Decoder) throws {
        let s = try decoder.singleValueContainer().decode(String.self)
        guard let r = IPAddressRange(string: s) else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath,
                debugDescription: "invalid CIDR: \(s)"))
        }
        self = r
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        try c.encode("\(addressString)/\(prefix)")
    }
}

public enum EndpointUtil {
    /// "host:port", "[v6]:port" → (host, port)
    public static func split(_ s: String) -> (host: String, port: UInt16)? {
        let t = s.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("[") {
            guard let close = t.firstIndex(of: "]") else { return nil }
            let host = String(t[t.index(after: t.startIndex)..<close])
            let rest = t[t.index(after: close)...]
            guard rest.hasPrefix(":"), let port = UInt16(rest.dropFirst()) else { return nil }
            return (host, port)
        }
        guard let colon = t.lastIndex(of: ":") else { return nil }
        let host = String(t[..<colon])
        guard let port = UInt16(t[t.index(after: colon)...]), !host.isEmpty else { return nil }
        return (host, port)
    }

    public static func isIPLiteral(_ host: String) -> Bool {
        IPAddressRange.pton(host) != nil
    }
}
