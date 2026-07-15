import Foundation
import CryptoKit

/// WireGuard keys (X25519), base64 ↔ hex, generation.
public enum WGKey {
    public static func generatePrivateKey() -> String {
        Curve25519.KeyAgreement.PrivateKey().rawRepresentation.base64EncodedString()
    }

    public static func publicKey(fromPrivate base64: String) -> String? {
        guard let data = Data(base64Encoded: base64), data.count == 32,
              let key = try? Curve25519.KeyAgreement.PrivateKey(rawRepresentation: data)
        else { return nil }
        return key.publicKey.rawRepresentation.base64EncodedString()
    }

    public static func generatePSK() -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, 32, &bytes)
        return Data(bytes).base64EncodedString()
    }

    public static func isValidKey(_ s: String) -> Bool {
        guard let d = Data(base64Encoded: s.trimmingCharacters(in: .whitespaces)) else { return false }
        return d.count == 32
    }

    public static func base64ToHex(_ s: String) -> String? {
        guard let d = Data(base64Encoded: s.trimmingCharacters(in: .whitespaces)), d.count == 32 else { return nil }
        return d.map { String(format: "%02x", $0) }.joined()
    }

    public static func hexToBase64(_ hex: String) -> String? {
        guard hex.count == 64 else { return nil }
        var data = Data()
        var idx = hex.startIndex
        while idx < hex.endIndex {
            let next = hex.index(idx, offsetBy: 2)
            guard let b = UInt8(hex[idx..<next], radix: 16) else { return nil }
            data.append(b)
            idx = next
        }
        return data.base64EncodedString()
    }
}
