#pragma once
// .ovpn profile parser.

#include <optional>
#include <string>
#include <vector>

#include "tunhub/models.h"
#include "tunhub/wgquick.h"   // ParseError

namespace tunhub::ovpn {

struct ParsedOvpn {
    OpenVpnProfile profile;
    std::vector<std::string> warnings;
};

/// Parse and sanitise a .ovpn profile.
///
/// Script directives (`up`, `down`, `route-up`, `client-connect`, `script-security`, …) are
/// STRIPPED, not executed and not passed through: a downloaded profile must never be able to
/// run commands. Their removal is reported as a warning so the user knows the profile was
/// altered.
std::optional<ParsedOvpn> parse(const std::string& text, ParseError* error);

}  // namespace tunhub::ovpn
