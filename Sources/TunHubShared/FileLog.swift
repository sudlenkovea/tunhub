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
    public var formatted: String {
        "\(Self.tf.string(from: ts)) \(level.glyph) [\(category)] \(message)"
    }
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
    public var minLevel: LogLevel = .trace
    public var echoStderr = false

    public init(path: String, maxBytes: Int = 4_000_000, filePerms: Int = 0o644) {
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

    private func rotateIfNeeded() {
        guard let size = try? handle?.offset(), Int(size) > maxBytes else { return }
        handle?.closeFile()
        let old = path + ".1"
        try? FileManager.default.removeItem(atPath: old)
        try? FileManager.default.moveItem(atPath: path, toPath: old)
        FileManager.default.createFile(atPath: path, contents: nil, attributes: [.posixPermissions: 0o644])
        handle = FileHandle(forWritingAtPath: path)
    }

    /// Last N lines (for UI/XPC). Reads the file + the rotated tail.
    public func tail(maxLines: Int) -> [LogLine] {
        queue.sync {
            var text = ""
            if let prev = try? String(contentsOfFile: path + ".1", encoding: .utf8) { text += prev }
            if let cur = try? String(contentsOfFile: path, encoding: .utf8) { text += cur }
            let lines = text.split(separator: "\n").suffix(maxLines)
            return lines.compactMap { LogLine.parse(String($0)) }
        }
    }

    public var filePath: String { path }
}
