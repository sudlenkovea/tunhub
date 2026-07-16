import Foundation
import Darwin

/// Client for the OpenVPN management interface (unix socket). OpenVPN listens on the
/// socket; we connect, send commands, and react to the real-time `>…` notifications.
/// See https://openvpn.net/community-docs/management-interface.html
final class OpenVPNManagement {
    private let path: String
    private var fd: Int32 = -1
    private var reader: Thread?
    private var running = false
    private var buffer = Data()

    // Callbacks (invoked on the reader thread).
    var onState: ((String) -> Void)?          // raw payload after ">STATE:"
    var onByteCount: ((UInt64, UInt64) -> Void)?  // (rx, tx)
    var onPasswordNeed: ((String) -> Void)?   // raw payload after ">PASSWORD:"
    var onInfo: ((String) -> Void)?
    var onFatal: ((String) -> Void)?

    init(path: String) { self.path = path }

    /// Connect to the management socket (retrying until `timeout`).
    func connect(timeout: TimeInterval) throws {
        let deadline = Date().addingTimeInterval(timeout)
        repeat {
            if tryConnectOnce() { startReader(); return }
            usleep(100_000)
        } while Date() < deadline
        throw DaemonError("openvpn management socket did not accept a connection (timeout)")
    }

    private func tryConnectOnce() -> Bool {
        let s = socket(AF_UNIX, SOCK_STREAM, 0)
        guard s >= 0 else { return false }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let cap = MemoryLayout.size(ofValue: addr.sun_path)   // read before taking &addr.sun_path
        guard path.utf8.count < cap else { Darwin.close(s); return false }
        _ = path.withCString { cs in
            withUnsafeMutablePointer(to: &addr.sun_path) { raw in
                raw.withMemoryRebound(to: CChar.self, capacity: cap) { dst in
                    strcpy(dst, cs)
                }
            }
        }
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let r = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(s, $0, len) }
        }
        if r == 0 { fd = s; return true }
        Darwin.close(s)
        return false
    }

    private func startReader() {
        running = true
        let t = Thread { [weak self] in self?.readLoop() }
        t.stackSize = 1 << 20
        reader = t
        t.start()
    }

    private func readLoop() {
        var chunk = [UInt8](repeating: 0, count: 4096)
        while running {
            let n = read(fd, &chunk, chunk.count)
            if n <= 0 { break }
            buffer.append(contentsOf: chunk[0..<n])
            while let nl = buffer.firstIndex(of: 0x0A) {
                let lineData = buffer.subdata(in: buffer.startIndex..<nl)
                buffer.removeSubrange(buffer.startIndex...nl)
                if let line = String(data: lineData, encoding: .utf8) {
                    handle(line.trimmingCharacters(in: CharacterSet(charactersIn: "\r")))
                }
            }
        }
    }

    private func handle(_ line: String) {
        guard line.hasPrefix(">") else { return }   // ignore command replies (SUCCESS/ERROR/END)
        guard let colon = line.firstIndex(of: ":") else { return }
        let kind = String(line[line.index(after: line.startIndex)..<colon])
        let payload = String(line[line.index(after: colon)...])
        switch kind {
        case "STATE": onState?(payload)
        case "BYTECOUNT":
            let parts = payload.split(separator: ",")
            if parts.count == 2, let rx = UInt64(parts[0]), let tx = UInt64(parts[1]) {
                onByteCount?(rx, tx)
            }
        case "PASSWORD": onPasswordNeed?(payload)
        case "INFO": onInfo?(payload)
        case "FATAL": onFatal?(payload)
        default: break
        }
    }

    func send(_ command: String) {
        guard fd >= 0 else { return }
        let line = command + "\n"
        _ = line.withCString { write(fd, $0, strlen($0)) }
    }

    func close() {
        running = false
        if fd >= 0 { Darwin.close(fd); fd = -1 }
    }

    // MARK: - Credential helpers

    /// Reply to a username/password request, encoding a static-challenge OTP as SCRV1 if given.
    func sendCredentials(username: String, password: String, otp: String?) {
        send("username \"Auth\" \(quote(username))")
        if let otp, !otp.isEmpty {
            let pw = base64(password)
            let resp = base64(otp)
            send("password \"Auth\" \(quote("SCRV1:\(pw):\(resp)"))")
        } else {
            send("password \"Auth\" \(quote(password))")
        }
    }

    private func quote(_ s: String) -> String {
        "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }
    private func base64(_ s: String) -> String { Data(s.utf8).base64EncodedString() }
}
