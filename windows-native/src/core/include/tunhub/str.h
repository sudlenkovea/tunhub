#pragma once
// Text helpers. Everything inside TunHub is UTF-8 std::string; conversion to UTF-16 happens
// only at the Win32 boundary.

#include <string>
#include <string_view>
#include <vector>

namespace tunhub::str {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

std::string trim(std::string_view s);
std::string lower(std::string_view s);

/// Split on a single delimiter. Empty pieces are dropped when `keepEmpty` is false.
std::vector<std::string> split(std::string_view s, char delim, bool keepEmpty = false);

/// Split on commas and trim each piece — the form used throughout wg-quick configs
/// (Address, DNS, AllowedIPs).
std::vector<std::string> splitList(std::string_view s);

std::string join(const std::vector<std::string>& parts, std::string_view sep);

bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);
bool icontains(std::string_view haystack, std::string_view needle);

/// Base64 <-> raw bytes. WireGuard keys travel as base64 in configs and as hex over UAPI.
std::string base64Encode(const std::vector<unsigned char>& data);
bool base64Decode(std::string_view b64, std::vector<unsigned char>& out);

std::string hexEncode(const std::vector<unsigned char>& data);
bool hexDecode(std::string_view hex, std::vector<unsigned char>& out);

/// Human-readable byte counts / rates for the UI.
std::string humanBytes(unsigned long long bytes);
std::string humanRate(double bytesPerSecond);

}  // namespace tunhub::str
