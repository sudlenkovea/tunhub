#pragma once
// wg-quick .conf parser/serializer with the AmneziaWG extensions
// (Jc, Jmin, Jmax, S1–S4, H1–H4, I1–I5, ITime).

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tunhub/models.h"

namespace tunhub {

/// Parse result: the config with secrets stripped out, plus the secrets separately so the
/// caller can hand them straight to the credential store — they never touch the config file.
struct ParsedTunnel {
    TunnelConfig config;
    std::string privateKey;
    std::map<std::string, std::string> presharedKeys;   // peer id → PSK
    std::vector<std::string> warnings;
};

struct ParseError {
    int line = 0;
    std::string message;
    std::string text() const {
        return line > 0 ? "line " + std::to_string(line) + ": " + message : message;
    }
};

namespace wgquick {

/// Returns nullopt and fills `error` on malformed input; warnings are surfaced in the result
/// so the import preview can show them without blocking the import.
std::optional<ParsedTunnel> parse(const std::string& name, const std::string& text,
                                  ParseError* error);

std::string serialize(const TunnelConfig& config,
                      const std::string& privateKey,
                      const std::map<std::string, std::string>& presharedKeys,
                      bool redactSecrets);

}  // namespace wgquick
}  // namespace tunhub
