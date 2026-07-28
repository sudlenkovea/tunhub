#include "tunhub/ipaddr.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>

#include "tunhub/str.h"

#pragma comment(lib, "Ws2_32.lib")

namespace tunhub {
namespace {

/// Normalise a literal by round-tripping it through the system parser: this collapses
/// IPv6 forms ("FD00:0::1" → "fd00::1") so canonical() is genuinely canonical.
bool normalise(const std::string& text, bool& isV6, std::string& out,
               std::vector<unsigned char>& raw) {
    in_addr v4{};
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        char buf[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &v4, buf, sizeof(buf));
        isV6 = false;
        out = buf;
        raw.assign(reinterpret_cast<unsigned char*>(&v4),
                   reinterpret_cast<unsigned char*>(&v4) + 4);
        return true;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        char buf[INET6_ADDRSTRLEN]{};
        inet_ntop(AF_INET6, &v6, buf, sizeof(buf));
        isV6 = true;
        out = buf;
        raw.assign(reinterpret_cast<unsigned char*>(&v6),
                   reinterpret_cast<unsigned char*>(&v6) + 16);
        return true;
    }
    return false;
}

void applyMask(std::vector<unsigned char>& bytes, int prefix) {
    for (size_t i = 0; i < bytes.size(); ++i) {
        int bitsHere = prefix - static_cast<int>(i) * 8;
        if (bitsHere >= 8) continue;
        if (bitsHere <= 0) { bytes[i] = 0; continue; }
        bytes[i] = static_cast<unsigned char>(bytes[i] & (0xFF << (8 - bitsHere)));
    }
}

std::string bytesToText(const std::vector<unsigned char>& bytes, bool isV6) {
    char buf[INET6_ADDRSTRLEN]{};
    if (isV6) {
        in6_addr a{};
        std::copy(bytes.begin(), bytes.end(), reinterpret_cast<unsigned char*>(&a));
        inet_ntop(AF_INET6, &a, buf, sizeof(buf));
    } else {
        in_addr a{};
        std::copy(bytes.begin(), bytes.end(), reinterpret_cast<unsigned char*>(&a));
        inet_ntop(AF_INET, &a, buf, sizeof(buf));
    }
    return buf;
}

}  // namespace

std::optional<IpAddressRange> IpAddressRange::parse(const std::string& text) {
    auto t = str::trim(text);
    if (t.empty()) return std::nullopt;

    std::string host = t;
    std::optional<int> explicitPrefix;
    if (auto slash = t.rfind('/'); slash != std::string::npos) {
        host = str::trim(t.substr(0, slash));
        auto prefixText = str::trim(t.substr(slash + 1));
        if (prefixText.empty()) return std::nullopt;
        try {
            size_t consumed = 0;
            int p = std::stoi(prefixText, &consumed);
            if (consumed != prefixText.size()) return std::nullopt;
            explicitPrefix = p;
        } catch (...) {
            return std::nullopt;
        }
    }

    IpAddressRange r;
    std::vector<unsigned char> raw;
    if (!normalise(host, r.isV6, r.address, raw)) return std::nullopt;

    const int maxPrefix = r.isV6 ? 128 : 32;
    r.prefix = explicitPrefix.value_or(maxPrefix);
    if (r.prefix < 0 || r.prefix > maxPrefix) return std::nullopt;
    return r;
}

std::string IpAddressRange::canonical() const {
    return address + "/" + std::to_string(prefix);
}

std::vector<unsigned char> IpAddressRange::bytes() const {
    std::vector<unsigned char> raw;
    bool v6 = false;
    std::string norm;
    if (!normalise(address, v6, norm, raw)) return {};
    return raw;
}

IpAddressRange IpAddressRange::masked() const {
    IpAddressRange out = *this;
    auto raw = bytes();
    if (raw.empty()) return out;
    applyMask(raw, prefix);
    out.address = bytesToText(raw, isV6);
    return out;
}

bool IpAddressRange::contains(const IpAddressRange& other) const {
    if (isV6 != other.isV6) return false;
    if (other.prefix < prefix) return false;      // a longer prefix is the more specific one
    auto a = bytes(), b = other.bytes();
    if (a.empty() || b.empty()) return false;
    applyMask(a, prefix);
    applyMask(b, prefix);
    return a == b;
}

bool IpAddressRange::overlaps(const IpAddressRange& other) const {
    if (isV6 != other.isV6) return false;
    const int shared = std::min(prefix, other.prefix);
    auto a = bytes(), b = other.bytes();
    if (a.empty() || b.empty()) return false;
    applyMask(a, shared);
    applyMask(b, shared);
    return a == b;
}

bool isIpLiteral(const std::string& text) {
    std::vector<unsigned char> raw;
    bool v6 = false;
    std::string norm;
    return normalise(str::trim(text), v6, norm, raw);
}

std::optional<Endpoint> parseEndpoint(const std::string& text) {
    auto t = str::trim(text);
    if (t.empty()) return std::nullopt;

    std::string host;
    std::string portText;
    if (t.front() == '[') {                       // [fd00::1]:51820
        auto close = t.find(']');
        if (close == std::string::npos || close + 2 > t.size() || t[close + 1] != ':')
            return std::nullopt;
        host = t.substr(1, close - 1);
        portText = t.substr(close + 2);
    } else {
        auto colon = t.rfind(':');
        // A bare IPv6 literal has several colons and no port — reject it: an endpoint needs one.
        if (colon == std::string::npos || t.find(':') != colon) return std::nullopt;
        host = t.substr(0, colon);
        portText = t.substr(colon + 1);
    }
    if (host.empty() || portText.empty()) return std::nullopt;

    unsigned long port = 0;
    try {
        size_t consumed = 0;
        port = std::stoul(portText, &consumed);
        if (consumed != portText.size()) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    if (port == 0 || port > 65535) return std::nullopt;

    return Endpoint{host, static_cast<uint16_t>(port)};
}

std::vector<IpAddressRange> parseRangeList(const std::string& commaSeparated) {
    std::vector<IpAddressRange> out;
    for (const auto& piece : str::splitList(commaSeparated))
        if (auto r = IpAddressRange::parse(piece)) out.push_back(*r);
    return out;
}

std::string joinRanges(const std::vector<IpAddressRange>& ranges) {
    std::vector<std::string> parts;
    parts.reserve(ranges.size());
    for (const auto& r : ranges) parts.push_back(r.canonical());
    return str::join(parts, ", ");
}

}  // namespace tunhub
