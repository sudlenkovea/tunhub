import Foundation

public enum LogLevel: String, Codable, Comparable, CaseIterable {
    case trace, debug, info, warn, error
    var rank: Int { LogLevel.allCases.firstIndex(of: self)! }
    public static func < (a: LogLevel, b: LogLevel) -> Bool { a.rank < b.rank }
    public var glyph: String {
        switch self {
        case .trace: return "·"; case .debug: return "▹"; case .info: return "•"
        case .warn: return "!"; case .error: return "✕"
        }
    }
}

/// How much detail we capture. `.verbose` is a debugging mode: it multiplies log volume
/// (every route/exec call, plus the core's own DEBUG stream) and costs real CPU, so it is
/// opt-in and only takes effect after a restart.
public enum LogCaptureMode: String, Codable, CaseIterable, Sendable {
    case normal, verbose
    public var minLevel: LogLevel { self == .verbose ? .trace : .info }
    /// LOG_LEVEL handed to wireguard-go / amneziawg-go.
    public var coreLogLevel: String { self == .verbose ? "verbose" : "error" }
    public var label: String { self == .verbose ? "Verbose (debug)" : "Normal" }
}

public struct LogLine: Codable, Identifiable {
    public var id = UUID()
    public var ts: Date
    public var level: LogLevel
    public var category: String
    public var message: String
    public init(ts: Date, level: LogLevel, category: String, message: String) {
        self.ts = ts; self.level = level; self.category = category; self.message = message
    }
    private static let tf: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f
    }()
    /// Cached on first use: the log viewer renders this for thousands of lines, and
    /// re-deriving it (DateFormatter + interpolation) on every SwiftUI layout pass was a
    /// measurable share of the UI's CPU time.
    private var _formatted: String?
    public var formatted: String {
        mutating get {
            if let f = _formatted { return f }
            let f = "\(Self.tf.string(from: ts)) \(level.glyph) [\(category)] \(message)"
            _formatted = f
            return f
        }
    }
    /// Non-mutating variant for read-only contexts (does not populate the cache).
    public var formattedValue: String {
        _formatted ?? "\(Self.tf.string(from: ts)) \(level.glyph) [\(category)] \(message)"
    }
    private enum CodingKeys: String, CodingKey { case id, ts, level, category, message }
    public static func parse(_ raw: String) -> LogLine? {
        // "2026-07-12T10:00:00.123Z\tLEVEL\tCAT\tMSG"
        let parts = raw.components(separatedBy: "\t")
        guard parts.count >= 4,
              let ts = ISO8601DateFormatter.withMillis.date(from: parts[0]),
              let lvl = LogLevel(rawValue: parts[1]) else { return nil }
        return LogLine(ts: ts, level: lvl, category: parts[2],
                       message: parts[3...].joined(separator: "\t"))
    }
}

extension ISO8601DateFormatter {
    static let withMillis: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()
}

/// Thread-safe file logger with rotation. Line format: TSV (machine-readable + tail-friendly).
public final class FileLog: @unchecked Sendable {
    private let queue = DispatchQueue(label: "com.tunhub.filelog")
    private let path: String
    private let maxBytes: Int
    private var handle: FileHandle?
    /// Default is `.info`: `.trace`/`.debug` capture is a debugging mode that floods the file
    /// (and burns CPU on both the writer and the log viewer). Opt in via LogCaptureMode.
    public var minLevel: LogLevel = .info
    public var echoStderr = false

    public init(path: String, maxBytes: Int = 5_000_000, filePerms: Int = 0o644) {
        self.path = path
        self.maxBytes = maxBytes
        let dir = (path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        if !FileManager.default.fileExists(atPath: path) {
            FileManager.default.createFile(atPath: path, contents: nil,
                attributes: [.posixPermissions: filePerms])
        }
        handle = FileHandle(forWritingAtPath: path)
        _ = try? handle?.seekToEnd()
    }

    public func log(_ level: LogLevel, _ category: String, _ message: @autoclosure () -> String) {
        guard level >= minLevel else { return }
        let msg = message()
        let echo = echoStderr
        queue.async { [weak self] in
            guard let self else { return }
            let ts = ISO8601DateFormatter.withMillis.string(from: Date())
            let line = "\(ts)\t\(level.rawValue)\t\(category)\t\(msg)\n"
            guard let data = line.data(using: .utf8) else { return }
            self.handle?.write(data)
            if echo { FileHandle.standardError.write(data) }
            self.rotateIfNeeded()
        }
    }

    public func trace(_ c: String, _ m: @autoclosure () -> String) { log(.trace, c, m()) }
    public func debug(_ c: String, _ m: @autoclosure () -> String) { log(.debug, c, m()) }
    public func info(_ c: String, _ m: @autoclosure () -> String)  { log(.info, c, m()) }
    public func warn(_ c: String, _ m: @autoclosure () -> String)  { log(.warn, c, m()) }
    public func error(_ c: String, _ m: @autoclosure () -> String) { log(.error, c, m()) }

    /// Keep a single file trimmed to the last `maxBytes` (5 MB) instead of rotating to `.1`.
    /// One bounded file is simpler to reason about and halves what `tail` has to consider.
    private func rotateIfNeeded() {
        guard let size = try? handle?.offset(), Int(size) > maxBytes else { return }
        guard let fh = FileHandle(forReadingAtPath: path) else { return }
        defer { try? fh.close() }
        // Keep the last 80% of the budget so we don't re-trim on every write.
        let keep = maxBytes * 4 / 5
        try? fh.seek(toOffset: UInt64(max(0, Int(size) - keep)))
        var data = (try? fh.readToEnd()) ?? Data()
        // Drop the partial first line so the file always starts on a record boundary.
        if let nl = data.firstIndex(of: 0x0A) { data = data.subdata(in: (nl + 1)..<data.endIndex) }
        handle?.closeFile()
        try? data.write(to: URL(fileURLWithPath: path), options: .atomic)
        // .atomic replaces the inode, so perms must be re-applied.
        try? FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: path)
        handle = FileHandle(forWritingAtPath: path)
        _ = try? handle?.seekToEnd()
    }

    /// Last N lines (for UI/XPC).
    ///
    /// Reads only the tail of the file rather than slurping and splitting the whole thing:
    /// the viewer calls this once per second on both the app and (over XPC) the root daemon,
    /// and parsing megabytes each time was burning CPU in both processes.
    public func tail(maxLines: Int) -> [LogLine] {
        queue.sync {
            guard let fh = FileHandle(forReadingAtPath: path) else { return [] }
            defer { try? fh.close() }
            let size = Int((try? fh.seekToEnd()) ?? 0)
            // ~220 bytes/line is a generous estimate; cap the read so a huge maxLines
            // can never pull in more than the file budget.
            let want = min(size, max(64 * 1024, maxLines * 220))
            try? fh.seek(toOffset: UInt64(size - want))
            guard var data = try? fh.readToEnd(), !data.isEmpty else { return [] }
            if want < size, let nl = data.firstIndex(of: 0x0A) {
                data = data.subdata(in: (nl + 1)..<data.endIndex)   // drop partial first line
            }
            guard let text = String(data: data, encoding: .utf8) else { return [] }
            return text.split(separator: "\n").suffix(maxLines)
                .compactMap { LogLine.parse(String($0)) }
        }
    }

    public var filePath: String { path }
}

/// Persisted log-capture mode, shared by the app and the root daemon.
///
/// The mode is read ONCE at process start and only applied on the next launch — switching
/// verbosity live would leave a half-verbose log and (for the cores) require respawning every
/// tunnel. The UI therefore asks the user to restart when the mode changes.
public enum LogSettings {
    /// World-readable so the unprivileged app can read what the root daemon wrote, while
    /// only root can change the daemon-side copy.
    public static let sharedFile = "/var/db/tunhub/log-mode"

    public static func read(from file: String = sharedFile) -> LogCaptureMode {
        guard let s = try? String(contentsOfFile: file, encoding: .utf8) else { return .normal }
        return LogCaptureMode(rawValue: s.trimmingCharacters(in: .whitespacesAndNewlines)) ?? .normal
    }

    @discardableResult
    public static func write(_ mode: LogCaptureMode, to file: String = sharedFile) -> Bool {
        let dir = (file as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        guard (try? mode.rawValue.write(toFile: file, atomically: true, encoding: .utf8)) != nil else { return false }
        try? FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: file)
        return true
    }
}
