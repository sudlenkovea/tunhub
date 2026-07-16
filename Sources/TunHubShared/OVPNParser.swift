import Foundation

/// Parse result for an OpenVPN profile: metadata + secrets pulled out for the Keychain.
public struct ParsedOpenVPN {
    public var profile: OpenVPNProfile
    /// Sensitive material to store in the Keychain (tag → PEM/base64, plus optional
    /// "username"/"password" if the profile carried inline credentials).
    public var secrets: [String: String]
    public var warnings: [String]
}

/// Parser for OpenVPN `.ovpn` profiles. Inline secret blocks (`<key>`, `<tls-auth>`,
/// `<tls-crypt>`, `<tls-crypt-v2>`, `<pkcs12>`) are stripped to placeholders and their
/// contents returned as secrets. Script directives are never executed — they are dropped
/// with a warning (same policy as wg-quick PreUp/PostUp).
public enum OVPNParser {

    public struct OVPNParseError: Error, LocalizedError {
        public let message: String
        public var errorDescription: String? { message }
        public init(_ m: String) { message = m }
    }

    /// Inline blocks that hold secrets (redacted out of the stored config text).
    static let secretTags: Set<String> = ["key", "tls-auth", "tls-crypt", "tls-crypt-v2", "pkcs12"]

    /// Directives that run external code — dropped for safety (never executed).
    static let scriptDirectives: Set<String> = [
        "up", "down", "route-up", "route-pre-down", "ipchange", "tls-verify",
        "auth-user-pass-verify", "client-connect", "client-disconnect",
        "learn-address", "up-restart", "script-security"
    ]

    public static func parse(name: String, text: String) throws -> ParsedOpenVPN {
        var profile = OpenVPNProfile()
        var secrets: [String: String] = [:]
        var warnings: [String] = []

        var hasCert = false, hasKey = false, hasAuthUserPass = false
        var globalProto = "udp"
        var globalPort: UInt16 = 1194
        // remotes captured with optional per-line overrides, resolved against globals at the end.
        var rawRemotes: [(host: String, port: UInt16?, proto: String?)] = []
        var outLines: [String] = []

        let lines = text.components(separatedBy: .newlines)
        var i = 0
        while i < lines.count {
            let raw = lines[i]
            let trimmed = raw.trimmingCharacters(in: .whitespaces)

            // Inline block: <tag> ... </tag>
            if trimmed.hasPrefix("<"), trimmed.hasSuffix(">"), !trimmed.hasPrefix("</") {
                let tag = String(trimmed.dropFirst().dropLast())
                var body: [String] = []
                i += 1
                while i < lines.count, lines[i].trimmingCharacters(in: .whitespaces) != "</\(tag)>" {
                    body.append(lines[i]); i += 1
                }
                let content = body.joined(separator: "\n")
                if tag == "cert" { hasCert = true }
                if secretTags.contains(tag) {
                    secrets[tag] = content
                    if tag == "key" { hasKey = true }
                    outLines.append("<\(tag)>")
                    outLines.append("##SECRET:\(tag)##")
                    outLines.append("</\(tag)>")
                } else {
                    // Public material (ca, cert, dh, extra-certs): keep inline.
                    outLines.append("<\(tag)>")
                    outLines.append(contentsOf: body)
                    outLines.append("</\(tag)>")
                }
                i += 1
                continue
            }

            let isComment = trimmed.hasPrefix("#") || trimmed.hasPrefix(";")
            if trimmed.isEmpty || isComment { outLines.append(raw); i += 1; continue }

            let tokens = tokenize(trimmed)
            let key = tokens[0].lowercased()
            let args = Array(tokens.dropFirst())

            switch key {
            case "remote":
                guard let host = args.first else { break }
                let port = args.count > 1 ? UInt16(args[1]) : nil
                let proto = args.count > 2 ? normalizeProto(args[2]) : nil
                rawRemotes.append((host, port, proto))
                outLines.append(raw)
            case "proto":
                if let p = args.first { globalProto = normalizeProto(p) }
                outLines.append(raw)
            case "port":
                if let p = args.first, let v = UInt16(p) { globalPort = v }
                outLines.append(raw)
            case "cipher":
                profile.cipher = args.first
                outLines.append(raw)
            case "data-ciphers", "ncp-ciphers":
                profile.dataCiphers = (args.first ?? "").split(separator: ":").map(String.init)
                outLines.append(raw)
            case "auth-user-pass":
                hasAuthUserPass = true
                // A file argument can't be read (and shouldn't be trusted); the management
                // interface will supply credentials. Strip any path.
                outLines.append("auth-user-pass")
            case "static-challenge":
                // static-challenge "prompt text" <echo 0|1>
                let echo = args.last == "1"
                let promptTokens = echo ? args.dropLast() : args[...]
                let promptText = promptTokens.joined(separator: " ")
                    .trimmingCharacters(in: CharacterSet(charactersIn: "\""))
                profile.staticChallenge = OpenVPNStaticChallenge(text: promptText, echo: echo)
                outLines.append(raw)
            case "redirect-gateway", "redirect-private":
                profile.redirectGateway = true
                outLines.append(raw)
            case "dhcp-option":
                if args.count >= 2 {
                    switch args[0].uppercased() {
                    case "DNS", "DNS6": profile.dns.append(args[1])
                    case "DOMAIN", "DOMAIN-SEARCH", "ADAPTER_DOMAIN_SUFFIX": profile.searchDomains.append(args[1])
                    default: break
                    }
                }
                outLines.append(raw)
            case "comp-lzo", "compress":
                profile.usesInlineCompression = true
                warnings.append("compression enabled (\(key)) — vulnerable to VORACLE; consider disabling on the server")
                outLines.append(raw)
            case "verb", "mute":
                // Verbosity is controlled by the daemon (so errors aren't hidden by --mute).
                break
            case "key", "tls-auth", "tls-crypt", "tls-crypt-v2", "pkcs12":
                // File-based secret reference (not inline). We can't bundle the external file;
                // warn and keep the directive so the user can inline it instead.
                warnings.append("directive '\(key)' points to an external file — inline it into the .ovpn (<\(key)>…</\(key)>) so TunHub can store it securely")
                if key == "key" { hasKey = true }
                outLines.append(raw)
            case "cert":
                hasCert = true
                outLines.append(raw)
            default:
                if scriptDirectives.contains(key) {
                    warnings.append("script directive '\(key)' is ignored for security (never executed)")
                    // dropped
                } else {
                    outLines.append(raw)
                }
            }
            i += 1
        }

        // Resolve remotes against global proto/port.
        for r in rawRemotes {
            profile.remotes.append(OpenVPNRemote(host: r.host,
                                                 port: r.port ?? globalPort,
                                                 proto: r.proto ?? globalProto))
        }
        guard !profile.remotes.isEmpty else {
            throw OVPNParseError("no `remote` found in the OpenVPN profile")
        }

        // Auth mode.
        if hasAuthUserPass && (hasCert || hasKey) { profile.authMode = .userPassCert }
        else if hasAuthUserPass { profile.authMode = .userPass; profile.needsUsername = true }
        else { profile.authMode = .cert }
        if hasAuthUserPass && profile.authMode == .userPassCert { profile.needsUsername = true }

        profile.configText = outLines.joined(separator: "\n")
        return ParsedOpenVPN(profile: profile, secrets: secrets, warnings: warnings)
    }

    // MARK: helpers

    private static func normalizeProto(_ p: String) -> String {
        let l = p.lowercased()
        if l.hasPrefix("tcp") { return "tcp" }
        return "udp"
    }

    /// Split a directive line into tokens, honoring double quotes (for `static-challenge`).
    private static func tokenize(_ line: String) -> [String] {
        var tokens: [String] = []
        var cur = ""
        var inQuotes = false
        for ch in line {
            if ch == "\"" { inQuotes.toggle(); continue }
            if ch == " " || ch == "\t", !inQuotes {
                if !cur.isEmpty { tokens.append(cur); cur = "" }
            } else {
                cur.append(ch)
            }
        }
        if !cur.isEmpty { tokens.append(cur) }
        return tokens
    }
}
