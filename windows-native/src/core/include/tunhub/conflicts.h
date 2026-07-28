#pragma once
// Route / DNS / address / port overlap detection between tunnels.

#include <string>
#include <vector>

#include "tunhub/models.h"

namespace tunhub {

enum class FindingSeverity { Info, Warning, Error };

struct ConflictFinding {
    FindingSeverity severity = FindingSeverity::Info;
    std::string code;
    std::string message;
    std::vector<std::string> tunnelNames;
    std::string fixHint;
};

namespace conflicts {

/// Findings for starting `candidate` while `active` are already running.
std::vector<ConflictFinding> check(const TunnelConfig& candidate,
                                   const std::vector<TunnelConfig>& active);

/// All pairs in the set ("Check all").
std::vector<ConflictFinding> checkAll(const std::vector<TunnelConfig>& tunnels);

bool hasErrors(const std::vector<ConflictFinding>& findings);

std::string severityLabel(FindingSeverity s);

}  // namespace conflicts
}  // namespace tunhub
