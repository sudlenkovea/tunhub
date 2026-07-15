import Foundation
import ZIPFoundation
import TunHubShared

struct ImportCandidate: Identifiable {
    let id = UUID()
    var parsed: ParsedTunnel
    var sourceName: String
    var include: Bool = true
    var findings: [ConflictFinding] = []
}

enum ImportService {
    static let maxZipEntries = 1000
    static let maxEntrySize = 1_000_000
    static let maxZipSize = 10_000_000

    /// Universal entry point: .conf / .zip / any file containing config text.
    static func candidates(fromFiles urls: [URL], existing: [TunnelConfig]) -> (ok: [ImportCandidate], errors: [String]) {
        var out: [ImportCandidate] = []
        var errors: [String] = []
        for url in urls {
            let ext = url.pathExtension.lowercased()
            if ext == "zip" {
                do {
                    let data = try Data(contentsOf: url)
                    let (c, e) = candidates(fromZip: data, existing: existing + out.map(\.parsed.config))
                    out += c; errors += e
                } catch { errors.append("\(url.lastPathComponent): \(error.localizedDescription)") }
            } else {
                do {
                    let text = try String(contentsOf: url, encoding: .utf8)
                    let name = url.deletingPathExtension().lastPathComponent
                    out.append(try candidate(name: name, text: text,
                                             existing: existing + out.map(\.parsed.config)))
                } catch let pe as ParseError {
                    errors.append("\(url.lastPathComponent): \(pe.localizedDescription)")
                } catch {
                    errors.append("\(url.lastPathComponent): \(error.localizedDescription)")
                }
            }
        }
        return (out, errors)
    }

    static func candidates(fromZip data: Data, existing: [TunnelConfig]) -> (ok: [ImportCandidate], errors: [String]) {
        var out: [ImportCandidate] = []
        var errors: [String] = []
        guard data.count <= maxZipSize else { return ([], ["ZIP larger than 10 MB"]) }
        let archive: Archive
        do { archive = try Archive(data: data, accessMode: .read) }
        catch { return ([], ["could not open ZIP"]) }
        var count = 0
        for entry in archive {
            count += 1
            if count > maxZipEntries { errors.append("too many files in ZIP"); break }
            let path = entry.path
            // zip-slip and macOS junk
            guard !path.contains(".."), !path.hasPrefix("/"),
                  !path.contains("__MACOSX"), !path.hasSuffix(".DS_Store") else { continue }
            guard path.lowercased().hasSuffix(".conf"), entry.type == .file else { continue }
            guard entry.uncompressedSize <= maxEntrySize else {
                errors.append("\(path): file too large"); continue
            }
            var content = Data()
            do {
                _ = try archive.extract(entry, consumer: { content.append($0) })
            } catch { errors.append("\(path): \(error.localizedDescription)"); continue }
            guard let text = String(data: content, encoding: .utf8)
                    ?? String(data: content, encoding: .windowsCP1251) else {
                errors.append("\(path): not UTF-8"); continue
            }
            let base = (path as NSString).lastPathComponent
            let name = (base as NSString).deletingPathExtension
            do {
                out.append(try candidate(name: name, text: text,
                                         existing: existing + out.map(\.parsed.config)))
            } catch let pe as ParseError {
                errors.append("\(base): \(pe.localizedDescription)")
            } catch {
                errors.append("\(base): \(error.localizedDescription)")
            }
        }
        return (out, errors)
    }

    static func candidate(name: String, text: String, existing: [TunnelConfig]) throws -> ImportCandidate {
        var parsed = try WGQuickParser.parse(name: uniqueName(name, existing: existing), text: text)
        parsed.config.name = uniqueName(name, existing: existing)
        var c = ImportCandidate(parsed: parsed, sourceName: name)
        c.findings = ConflictChecker.check(candidate: parsed.config, against: existing)
        return c
    }

    static func uniqueName(_ name: String, existing: [TunnelConfig]) -> String {
        let names = Set(existing.map(\.name))
        if !names.contains(name) { return name }
        var i = 2
        while names.contains("\(name) (\(i))") { i += 1 }
        return "\(name) (\(i))"
    }

    /// Persist the selected candidates: secrets → Keychain, config → Store.
    static func commit(_ candidates: [ImportCandidate], store: TunnelStore) throws -> [TunnelConfig] {
        var saved: [TunnelConfig] = []
        for c in candidates where c.include {
            var cfg = c.parsed.config
            var secrets = KeychainService.TunnelSecrets(privateKey: c.parsed.privateKey)
            cfg.interface.privateKeyRef = KeychainService.interfaceRef(cfg.id)
            for i in cfg.peers.indices {
                if let psk = c.parsed.presharedKeys[cfg.peers[i].id] {
                    secrets.psks[cfg.peers[i].id.uuidString] = psk
                    cfg.peers[i].presharedKeyRef = KeychainService.pskRef(cfg.id, peerID: cfg.peers[i].id)
                }
            }
            guard KeychainService.saveSecrets(tunnelID: cfg.id, secrets) else {
                throw AppError("Keychain unavailable (secrets for “\(cfg.name)”)")
            }
            try store.save(cfg)
            saved.append(cfg)
        }
        return saved
    }
}

enum ExportService {
    static func confText(_ config: TunnelConfig, includeSecrets: Bool) -> String {
        var psks: [UUID: String] = [:]
        var pk: String?
        if includeSecrets {
            let secrets = KeychainService.loadSecrets(tunnelID: config.id)
            pk = secrets?.privateKey
            for p in config.peers {
                if p.presharedKeyRef != nil {
                    psks[p.id] = secrets?.psks[p.id.uuidString] ?? "<MISSING>"
                }
            }
        }
        return WGQuickParser.serialize(config: config, privateKey: pk,
                                       presharedKeys: psks, redactSecrets: !includeSecrets)
    }

    static func zipData(_ configs: [TunnelConfig], includeSecrets: Bool) throws -> Data {
        let archive: Archive
        do { archive = try Archive(accessMode: .create) }
        catch { throw AppError("could not create ZIP") }
        for c in configs {
            let text = confText(c, includeSecrets: includeSecrets)
            let data = Data(text.utf8)
            let safe = c.name.replacingOccurrences(of: "/", with: "_")
            try archive.addEntry(with: "\(safe).conf", type: .file,
                                 uncompressedSize: Int64(data.count),
                                 provider: { position, size in
                data.subdata(in: Int(position)..<Int(position) + size)
            })
        }
        guard let out = archive.data else { throw AppError("ZIP is empty") }
        return out
    }
}
