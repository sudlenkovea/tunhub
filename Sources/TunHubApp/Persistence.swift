import Foundation
import Security
import TunHubShared

// MARK: - Keychain (secrets: private keys, PSK)

enum KeychainService {
    static let service = TunHub.Keychain.legacyService

    @discardableResult
    static func store(_ secret: String, ref: KeychainRef) -> Bool {
        let data = Data(secret.utf8)
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ref.account
        ]
        let attrs: [String: Any] = [kSecValueData as String: data]
        let status = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if status == errSecItemNotFound {
            var add = query
            add[kSecValueData as String] = data
            add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
        }
        return status == errSecSuccess
    }

    static func load(_ ref: KeychainRef) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ref.account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func delete(_ ref: KeychainRef) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ref.account
        ]
        SecItemDelete(query as CFDictionary)
    }

    static func interfaceRef(_ tunnelID: UUID) -> KeychainRef {
        KeychainRef(account: "\(tunnelID.uuidString)/if")
    }
    static func pskRef(_ tunnelID: UUID, peerID: UUID) -> KeychainRef {
        KeychainRef(account: "\(tunnelID.uuidString)/psk/\(peerID.uuidString)")
    }

    /// Delete all secrets of a tunnel.
    static func purge(tunnel: TunnelConfig) {
        if let r = tunnel.interface.privateKeyRef { delete(r) }
        for p in tunnel.peers { if let r = p.presharedKeyRef { delete(r) } }
        deleteSecrets(tunnelID: tunnel.id)
    }

    // MARK: - Combined secrets (ONE keychain item per tunnel → a single password prompt)

    struct TunnelSecrets: Codable {
        var privateKey: String
        var psks: [String: String] = [:]   // peerID.uuidString → PSK
    }

    private static let secretsService = TunHub.Keychain.secretsService

    @discardableResult
    static func saveSecrets(tunnelID: UUID, _ secrets: TunnelSecrets) -> Bool {
        guard let data = try? JSONEncoder().encode(secrets) else { return false }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: secretsService,
            kSecAttrAccount as String: tunnelID.uuidString
        ]
        let status = SecItemUpdate(query as CFDictionary,
                                   [kSecValueData as String: data] as CFDictionary)
        if status == errSecItemNotFound {
            var add = query
            add[kSecValueData as String] = data
            add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
        }
        return status == errSecSuccess
    }

    static func loadSecrets(tunnelID: UUID) -> TunnelSecrets? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: secretsService,
            kSecAttrAccount as String: tunnelID.uuidString,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data,
              let s = try? JSONDecoder().decode(TunnelSecrets.self, from: data) else { return nil }
        return s
    }

    static func deleteSecrets(tunnelID: UUID) {
        SecItemDelete([
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: secretsService,
            kSecAttrAccount as String: tunnelID.uuidString
        ] as CFDictionary)
    }

    /// Migrate the old scheme (separate private key / PSK items) → combined item.
    /// Returns the secrets if it could assemble them; nil if there's no old data either.
    static func migrateLegacySecrets(config: TunnelConfig) -> TunnelSecrets? {
        let ref = config.interface.privateKeyRef ?? interfaceRef(config.id)
        guard let pk = load(ref) else { return nil }
        var s = TunnelSecrets(privateKey: pk)
        for p in config.peers {
            let r = p.presharedKeyRef ?? pskRef(config.id, peerID: p.id)
            if let psk = load(r) { s.psks[p.id.uuidString] = psk }
        }
        _ = saveSecrets(tunnelID: config.id, s)
        // clean up the old items
        delete(ref)
        for p in config.peers { if let r = p.presharedKeyRef { delete(r) } }
        return s
    }
}

// MARK: - TunnelStore (JSON on disk, no secrets)

final class TunnelStore {
    let dir: URL

    init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        dir = base.appendingPathComponent(TunHub.AppPath.tunnels, isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    func loadAll() -> [TunnelConfig] {
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        var out: [TunnelConfig] = []
        for f in files where f.pathExtension == "json" {
            if let d = try? Data(contentsOf: f),
               let c = try? TunJSON.decoder.decode(TunnelConfig.self, from: d) {
                out.append(c)
            }
        }
        return out.sorted { ($0.meta.sortOrder, $0.name) < ($1.meta.sortOrder, $1.name) }
    }

    func save(_ config: TunnelConfig) throws {
        let url = dir.appendingPathComponent("\(config.id.uuidString).json")
        let data = try TunJSON.encoder.encode(config)
        try data.write(to: url, options: .atomic)
    }

    func delete(_ config: TunnelConfig) {
        try? FileManager.default.removeItem(at: dir.appendingPathComponent("\(config.id.uuidString).json"))
        KeychainService.purge(tunnel: config)
    }
}

// MARK: - TrafficLedger (daily traffic totals)

struct DayTraffic: Codable { var rx: UInt64 = 0; var tx: UInt64 = 0 }

final class TrafficLedger {
    private var data: [String: [String: DayTraffic]] = [:]   // tunnelID → day → totals
    private var lastCounters: [UUID: (rx: UInt64, tx: UInt64)] = [:]
    private let url: URL

    init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        url = base.appendingPathComponent(TunHub.AppPath.traffic)
        if let d = try? Data(contentsOf: url),
           let decoded = try? TunJSON.decoder.decode([String: [String: DayTraffic]].self, from: d) {
            data = decoded
        }
    }

    private static let dayFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd"
        return f
    }()

    func update(id: UUID, rx: UInt64, tx: UInt64) {
        let prev = lastCounters[id]
        lastCounters[id] = (rx, tx)
        guard let prev else { return }
        // counters may have reset when the tunnel restarted
        let dRx = rx >= prev.rx ? rx - prev.rx : rx
        let dTx = tx >= prev.tx ? tx - prev.tx : tx
        guard dRx > 0 || dTx > 0 else { return }
        let day = Self.dayFormatter.string(from: Date())
        var t = data[id.uuidString, default: [:]][day, default: DayTraffic()]
        t.rx += dRx; t.tx += dTx
        data[id.uuidString, default: [:]][day] = t
    }

    func tunnelReset(id: UUID) { lastCounters[id] = nil }

    func monthTotals(id: UUID) -> DayTraffic {
        let prefix = String(Self.dayFormatter.string(from: Date()).prefix(7))
        var total = DayTraffic()
        for (day, t) in data[id.uuidString] ?? [:] where day.hasPrefix(prefix) {
            total.rx += t.rx; total.tx += t.tx
        }
        return total
    }

    func persist() {
        if let d = try? TunJSON.encoder.encode(data) {
            try? d.write(to: url, options: .atomic)
        }
    }

    func csv(id: UUID) -> String {
        var out = "day,rx_bytes,tx_bytes\n"
        for (day, t) in (data[id.uuidString] ?? [:]).sorted(by: { $0.key < $1.key }) {
            out += "\(day),\(t.rx),\(t.tx)\n"
        }
        return out
    }
}
