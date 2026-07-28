#pragma once
// CIDR ranges for addresses, AllowedIPs and routes.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tunhub {

struct IpAddressRange {
    std::string address;     // textual host part, normalised ("10.0.0.1", "fd00::1")
    int prefix = 0;
    bool isV6 = false;

    /// "address/prefix" — the canonical form used for de-duplication and for `route` calls.
    std::string canonical() const;

    /// Network address with the host bits cleared ("10.0.0.5/24" → "10.0.0.0/24").
    IpAddressRange masked() const;

    bool operator==(const IpAddressRange& o) const {
        return prefix == o.prefix && isV6 == o.isV6 && address == o.address;
    }

    /// Parse "10.0.0.1/24", "10.0.0.1" (implicit /32), "fd00::1/64", "::/0".
    static std::optional<IpAddressRange> parse(const std::string& text);

    /// Raw bytes of the address (4 for IPv4, 16 for IPv6). Empty when invalid.
    std::vector<unsigned char> bytes() const;

    /// Does this range fully contain `other`?
    bool contains(const IpAddressRange& other) const;
    /// Do the two ranges share any address?
    bool overlaps(const IpAddressRange& other) const;
};

/// Is `text` a valid IPv4/IPv6 literal (no prefix)?
bool isIpLiteral(const std::string& text);

struct Endpoint {
    std::string host;      // hostname or IP literal (IPv6 without the surrounding brackets)
    uint16_t port = 0;
};

/// Parse "host:port", "1.2.3.4:51820" or "[fd00::1]:51820".
std::optional<Endpoint> parseEndpoint(const std::string& text);

std::vector<IpAddressRange> parseRangeList(const std::string& commaSeparated);
std::string joinRanges(const std::vector<IpAddressRange>& ranges);

}  // namespace tunhub
