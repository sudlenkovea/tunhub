import Foundation
import TunHubShared

/// Direct DNS resolver: a UDP query to a public resolver bound to the PHYSICAL
/// interface (IP_BOUND_IF), so it doesn't go into an already-up tunnel with a default route.
/// This fixes the endpoint-resolution hang when starting a second tunnel.
enum DirectDNS {
    static let publicResolvers = ["1.1.1.1", "8.8.8.8", "9.9.9.9"]

    /// Resolve A/AAAA. Returns the first IP (A preferred). nil on failure/timeout.
    static func resolve(_ host: String, boundToInterface ifName: String?, timeout: TimeInterval = 4) -> String? {
        if IPAddressRange.pton(host) != nil { return host }
        let ifIndex = ifName.flatMap { UInt32(if_nametoindex($0)) } ?? 0

        for server in publicResolvers {
            if let ip = query(host: host, type: 1, server: server, ifIndex: ifIndex, timeout: timeout) {
                return ip
            }
        }
        // AAAA as a fallback
        for server in publicResolvers {
            if let ip = query(host: host, type: 28, server: server, ifIndex: ifIndex, timeout: timeout) {
                return ip
            }
        }
        return nil
    }

    /// A single UDP DNS query. type: 1=A, 28=AAAA.
    private static func query(host: String, type: UInt16, server: String,
                              ifIndex: UInt32, timeout: TimeInterval) -> String? {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { return nil }
        defer { close(fd) }

        // Bind to the physical interface — traffic won't go into the tunnel.
        if ifIndex > 0 {
            var idx = ifIndex
            setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, socklen_t(MemoryLayout<UInt32>.size))
        }
        var tv = timeval(tv_sec: Int(timeout), tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(53).bigEndian
        inet_pton(AF_INET, server, &addr.sin_addr)

        let packet = buildQuery(host: host, type: type)
        let sent = packet.withUnsafeBytes { p in
            withUnsafePointer(to: &addr) { a in
                a.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    sendto(fd, p.baseAddress, p.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
        guard sent == packet.count else { return nil }

        var buf = [UInt8](repeating: 0, count: 1500)
        let n = recv(fd, &buf, buf.count, 0)
        guard n > 0 else { return nil }
        return parseAnswer(Array(buf[0..<n]), type: type)
    }

    private static func buildQuery(host: String, type: UInt16) -> [UInt8] {
        var p: [UInt8] = []
        let id = UInt16.random(in: 0...UInt16.max)
        p += [UInt8(id >> 8), UInt8(id & 0xFF)]
        p += [0x01, 0x00]              // flags: recursion desired
        p += [0x00, 0x01]              // QDCOUNT=1
        p += [0x00, 0x00, 0x00, 0x00, 0x00, 0x00]  // AN/NS/AR=0
        for label in host.split(separator: ".") {
            let bytes = Array(label.utf8)
            p.append(UInt8(bytes.count))
            p += bytes
        }
        p.append(0)                    // end of name
        p += [UInt8(type >> 8), UInt8(type & 0xFF)]  // QTYPE
        p += [0x00, 0x01]              // QCLASS=IN
        return p
    }

    private static func parseAnswer(_ data: [UInt8], type: UInt16) -> String? {
        guard data.count > 12 else { return nil }
        let anCount = (Int(data[6]) << 8) | Int(data[7])
        guard anCount > 0 else { return nil }

        // skip the question
        var i = 12
        while i < data.count && data[i] != 0 { i += Int(data[i]) + 1 }
        i += 1                          // null byte
        i += 4                          // QTYPE + QCLASS

        for _ in 0..<anCount {
            guard i + 12 <= data.count else { return nil }
            // name (may be a 0xC0.. pointer)
            if data[i] & 0xC0 == 0xC0 { i += 2 }
            else { while i < data.count && data[i] != 0 { i += Int(data[i]) + 1 }; i += 1 }
            guard i + 10 <= data.count else { return nil }
            let rrType = (Int(data[i]) << 8) | Int(data[i+1])
            let rdLen = (Int(data[i+8]) << 8) | Int(data[i+9])
            i += 10
            guard i + rdLen <= data.count else { return nil }
            if rrType == 1 && rdLen == 4 {           // A
                return "\(data[i]).\(data[i+1]).\(data[i+2]).\(data[i+3])"
            }
            if rrType == 28 && rdLen == 16 {         // AAAA
                return IPAddressRange.ntop(Array(data[i..<i+16]))
            }
            i += rdLen
        }
        return nil
    }
}
