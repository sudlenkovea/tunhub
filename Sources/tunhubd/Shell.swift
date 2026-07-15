import Foundation
import os
import TunHubShared

let dlog = Logger(subsystem: "com.tunhub.daemon", category: "daemon")

/// Daemon file log (0644 — read by the app). Everything is written here verbosely.
let flog: FileLog = {
    let l = FileLog(path: TunHub.DaemonPath.logFile)
    l.echoStderr = true          // mirror to stderr → visible in `log stream`/launchd
    return l
}()

struct CommandResult {
    let status: Int32
    let stdout: String
    let stderr: String
    var ok: Bool { status == 0 }
}

@discardableResult
func run(_ path: String, _ args: [String], stdin: String? = nil) -> CommandResult {
    let started = Date()
    let cmdline = "\((path as NSString).lastPathComponent) \(args.joined(separator: " "))"
    flog.trace("exec", "$ \(cmdline)")
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    p.arguments = args
    let out = Pipe(), err = Pipe()
    p.standardOutput = out
    p.standardError = err
    if let stdin {
        let inPipe = Pipe()
        p.standardInput = inPipe
        inPipe.fileHandleForWriting.write(stdin.data(using: .utf8)!)
        inPipe.fileHandleForWriting.closeFile()
    }
    do { try p.run() } catch {
        flog.error("exec", "spawn failed: \(cmdline): \(error)")
        return CommandResult(status: -1, stdout: "", stderr: "\(error)")
    }
    let outData = out.fileHandleForReading.readDataToEndOfFile()
    let errData = err.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    let res = CommandResult(status: p.terminationStatus,
                            stdout: String(data: outData, encoding: .utf8) ?? "",
                            stderr: String(data: errData, encoding: .utf8) ?? "")
    let ms = Int(Date().timeIntervalSince(started) * 1000)
    let level: LogLevel = res.ok ? .trace : .warn
    var tail = ""
    let combined = (res.stdout + res.stderr).trimmingCharacters(in: .whitespacesAndNewlines)
    if !combined.isEmpty { tail = " → " + combined.replacingOccurrences(of: "\n", with: " ⏎ ").prefix(300) }
    flog.log(level, "exec", "exit=\(res.status) (\(ms)ms) \(cmdline)\(tail)")
    return res
}

struct DaemonError: Error, LocalizedError {
    let message: String
    var errorDescription: String? { message }
    init(_ m: String) { self.message = m }
}

enum Paths {
    static let varDir = TunHub.DaemonPath.varDir
    static let runDir = TunHub.DaemonPath.runDir
    static let ownership = TunHub.DaemonPath.ownership
    static let dnsBackupFile = varDir + "/dns-backup.json"
    static let pfRulesFile = varDir + "/pf.rules"
    static let pfMainFile = varDir + "/pf-main.conf"
    static let pfStateFile = varDir + "/pf-state.json"

    static func ensure() {
        for d in [varDir, runDir] {
            try? FileManager.default.createDirectory(atPath: d, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
        }
    }
}

enum Resolver {
    /// host → numeric IP (v4 preferred). Literals are returned as-is.
    static func resolve(_ host: String) -> String? {
        if IPAddressRange.pton(host) != nil { return host }
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_DGRAM
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, nil, &hints, &res) == 0, let first = res else { return nil }
        defer { freeaddrinfo(first) }
        var best: String?
        var p: UnsafeMutablePointer<addrinfo>? = first
        while let cur = p {
            var buf = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(cur.pointee.ai_addr, cur.pointee.ai_addrlen,
                           &buf, socklen_t(buf.count), nil, 0, NI_NUMERICHOST) == 0 {
                let ip = String(cString: buf)
                if cur.pointee.ai_family == AF_INET { return ip }
                if best == nil { best = ip }
            }
            p = cur.pointee.ai_next
        }
        return best
    }

    /// getaddrinfo with a hard timeout (on a separate thread), so it doesn't hang for 30s
    /// when the system DNS has gone into the tunnel.
    static func resolveWithTimeout(_ host: String, timeout: TimeInterval) -> String? {
        if IPAddressRange.pton(host) != nil { return host }
        let sem = DispatchSemaphore(value: 0)
        var result: String?
        let q = DispatchQueue(label: "com.tunhub.resolve", attributes: .concurrent)
        q.async {
            result = resolve(host)
            sem.signal()
        }
        return sem.wait(timeout: .now() + timeout) == .success ? result : nil
    }
}
