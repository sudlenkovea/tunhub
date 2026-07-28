#pragma once
// WireGuard key handling: base64 (config form) ↔ hex (UAPI form), plus keypair generation.

#include <optional>
#include <string>

namespace tunhub::wgkey {

constexpr size_t kKeyBytes = 32;

/// Is this a syntactically valid WireGuard key (32 bytes, base64)?
bool isValid(const std::string& base64Key);

/// base64 → lowercase hex, as required by the UAPI `set=1` protocol. Empty on bad input.
std::string base64ToHex(const std::string& base64Key);
/// hex → base64, for reading UAPI `get=1` output back into config form.
std::string hexToBase64(const std::string& hexKey);

struct KeyPair {
    std::string privateKey;   // base64
    std::string publicKey;    // base64
};

/// Generate a new X25519 keypair using the system CSPRNG.
std::optional<KeyPair> generateKeyPair();

/// Derive the public key from a base64 private key (X25519 base-point multiplication).
std::optional<std::string> publicKeyFrom(const std::string& base64PrivateKey);

}  // namespace tunhub::wgkey
