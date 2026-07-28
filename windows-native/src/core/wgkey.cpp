#include "tunhub/wgkey.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <vector>

#include "tunhub/str.h"

#pragma comment(lib, "bcrypt.lib")

namespace tunhub::wgkey {
namespace {

// ── X25519 ───────────────────────────────────────────────────────────────────
// Compact curve25519 scalar multiplication (RFC 7748), the same construction every
// WireGuard implementation uses. Vendored rather than pulled from OpenSSL: this is the
// only asymmetric primitive the app needs, and linking a TLS stack for it would dwarf
// the whole binary.

using fe = int64_t[16];

void feCopy(fe out, const fe in) {
    for (int i = 0; i < 16; ++i) out[i] = in[i];
}

void feAdd(fe out, const fe a, const fe b) {
    for (int i = 0; i < 16; ++i) out[i] = a[i] + b[i];
}

void feSub(fe out, const fe a, const fe b) {
    for (int i = 0; i < 16; ++i) out[i] = a[i] - b[i];
}

void feCarry(fe o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15 ? 1 : 0)] += c - 1 + 37 * (c - 1) * (i == 15 ? 1 : 0);
        o[i] -= c << 16;
    }
}

void feMul(fe out, const fe a, const fe b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) out[i] = t[i];
    feCarry(out);
    feCarry(out);
}

void feSquare(fe out, const fe a) { feMul(out, a, a); }

void feInvert(fe out, const fe in) {
    fe c;
    feCopy(c, in);
    for (int i = 253; i >= 0; --i) {
        feSquare(c, c);
        if (i != 2 && i != 4) feMul(c, c, in);
    }
    feCopy(out, c);
}

void feSwap(fe p, fe q, int64_t bit) {
    const int64_t mask = ~(bit - 1);
    for (int i = 0; i < 16; ++i) {
        const int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void feUnpack(fe out, const unsigned char* in) {
    for (int i = 0; i < 16; ++i)
        out[i] = in[2 * i] + ((int64_t)in[2 * i + 1] << 8);
    out[15] &= 0x7FFF;   // clear the top bit, per RFC 7748
}

void fePack(unsigned char* out, const fe in) {
    fe t, m;
    feCopy(t, in);
    feCarry(t); feCarry(t); feCarry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xFFED;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
        const int64_t carry = (m[15] >> 16) & 1;
        m[14] &= 0xFFFF;
        feSwap(t, m, 1 - carry);
    }
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<unsigned char>(t[i] & 0xFF);
        out[2 * i + 1] = static_cast<unsigned char>(t[i] >> 8);
    }
}

/// out = scalar * point (point = nullptr → curve base point 9)
void scalarMult(unsigned char out[32], const unsigned char scalar[32],
                const unsigned char* point) {
    static const unsigned char basePoint[32] = {9};
    const unsigned char* p = point ? point : basePoint;

    unsigned char clamped[32];
    std::memcpy(clamped, scalar, 32);
    clamped[0] &= 248;
    clamped[31] &= 127;
    clamped[31] |= 64;

    fe x, a, b, c, d, e, f;
    feUnpack(x, p);
    for (int i = 0; i < 16; ++i) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;

    static const fe _121665 = {0xDB41, 1};
    for (int i = 254; i >= 0; --i) {
        const int64_t bit = (clamped[i >> 3] >> (i & 7)) & 1;
        feSwap(a, b, bit);
        feSwap(c, d, bit);
        feAdd(e, a, c);
        feSub(a, a, c);
        feAdd(c, b, d);
        feSub(b, b, d);
        feSquare(d, e);
        feSquare(f, a);
        feMul(a, c, a);
        feMul(c, b, e);
        feAdd(e, a, c);
        feSub(a, a, c);
        feSquare(b, a);
        feSub(c, d, f);
        feMul(a, c, _121665);
        feAdd(a, a, d);
        feMul(c, c, a);
        feMul(a, d, f);
        feMul(d, b, x);
        feSquare(b, e);
        feSwap(a, b, bit);
        feSwap(c, d, bit);
    }
    feInvert(c, c);
    feMul(a, a, c);
    fePack(out, a);
}

bool randomBytes(unsigned char* out, size_t len) {
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

}  // namespace

bool isValid(const std::string& base64Key) {
    std::vector<unsigned char> raw;
    return str::base64Decode(base64Key, raw) && raw.size() == kKeyBytes;
}

std::string base64ToHex(const std::string& base64Key) {
    std::vector<unsigned char> raw;
    if (!str::base64Decode(base64Key, raw) || raw.size() != kKeyBytes) return {};
    return str::hexEncode(raw);
}

std::string hexToBase64(const std::string& hexKey) {
    std::vector<unsigned char> raw;
    if (!str::hexDecode(hexKey, raw) || raw.size() != kKeyBytes) return {};
    return str::base64Encode(raw);
}

std::optional<KeyPair> generateKeyPair() {
    unsigned char priv[kKeyBytes];
    if (!randomBytes(priv, sizeof(priv))) return std::nullopt;
    priv[0] &= 248;
    priv[31] &= 127;
    priv[31] |= 64;

    unsigned char pub[kKeyBytes];
    scalarMult(pub, priv, nullptr);

    KeyPair kp;
    kp.privateKey = str::base64Encode({priv, priv + kKeyBytes});
    kp.publicKey = str::base64Encode({pub, pub + kKeyBytes});
    SecureZeroMemory(priv, sizeof(priv));
    return kp;
}

std::optional<std::string> publicKeyFrom(const std::string& base64PrivateKey) {
    std::vector<unsigned char> priv;
    if (!str::base64Decode(base64PrivateKey, priv) || priv.size() != kKeyBytes)
        return std::nullopt;
    unsigned char pub[kKeyBytes];
    scalarMult(pub, priv.data(), nullptr);
    SecureZeroMemory(priv.data(), priv.size());
    return str::base64Encode({pub, pub + kKeyBytes});
}

}  // namespace tunhub::wgkey
