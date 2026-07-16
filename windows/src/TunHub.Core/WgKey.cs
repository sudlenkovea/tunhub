using System.Security.Cryptography;
using Org.BouncyCastle.Crypto.Parameters;

namespace TunHub.Core;

/// <summary>WireGuard keys (Curve25519), base64 ↔ hex, key generation.</summary>
public static class WgKey
{
    /// <summary>A valid WG key is 32 bytes encoded as standard base64 (44 chars incl. padding).</summary>
    public static bool IsValidKey(string b64)
    {
        if (string.IsNullOrWhiteSpace(b64)) return false;
        try
        {
            var data = Convert.FromBase64String(b64.Trim());
            return data.Length == 32;
        }
        catch (FormatException)
        {
            return false;
        }
    }

    /// <summary>base64 32-byte key → lowercase hex (UAPI wants hex). Null if invalid.</summary>
    public static string? Base64ToHex(string b64)
    {
        try
        {
            var data = Convert.FromBase64String(b64.Trim());
            return data.Length == 32 ? Convert.ToHexString(data).ToLowerInvariant() : null;
        }
        catch (FormatException)
        {
            return null;
        }
    }

    /// <summary>Generate a new private key (clamped Curve25519 scalar), base64-encoded.</summary>
    public static string GeneratePrivateKey()
    {
        var key = new byte[32];
        RandomNumberGenerator.Fill(key);
        // Curve25519 clamping.
        key[0] &= 248;
        key[31] &= 127;
        key[31] |= 64;
        return Convert.ToBase64String(key);
    }

    /// <summary>Derive the base64 public key from a base64 private key. Null on failure.</summary>
    public static string? PublicKey(string privateKeyBase64)
    {
        try
        {
            var priv = Convert.FromBase64String(privateKeyBase64.Trim());
            if (priv.Length != 32) return null;
            var sk = new X25519PrivateKeyParameters(priv, 0);
            var pub = sk.GeneratePublicKey().GetEncoded();
            return Convert.ToBase64String(pub);
        }
        catch (FormatException)
        {
            return null;
        }
    }
}
