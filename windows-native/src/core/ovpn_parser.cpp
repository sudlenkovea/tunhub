#include "tunhub/ovpn.h"

#include <charconv>
#include <set>

#include "tunhub/str.h"

namespace tunhub::ovpn {
namespace {

/// Directives that make OpenVPN run external commands. A .ovpn is usually downloaded from a
/// provider, so these are stripped outright rather than merely warned about.
const std::set<std::string> kScriptDirectives = {
    "up", "down", "route-up", "route-pre-down", "ipchange", "tls-verify",
    "auth-user-pass-verify", "client-connect", "client-disconnect",
    "learn-address", "up-restart", "script-security"
};

/// Inline blocks holding private material — kept, but never logged.
const std::set<std::string> kSecretBlocks = {"key", "tls-auth", "tls-crypt", "tls-crypt-v2", "pkcs12"};

/// Directives whose argument is an external file we cannot bundle.
const std::set<std::string> kFileRefDirectives = {
    "key", "cert", "ca", "tls-auth", "tls-crypt", "tls-crypt-v2", "pkcs12", "dh", "extra-certs"
};

std::string normaliseProto(const std::string& p) {
    return str::startsWith(str::lower(p), "tcp") ? "tcp" : "udp";
}

/// Split a directive line into tokens, honouring double quotes (static-challenge uses them).
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuotes = false;
    for (char c : line) {
        if (c == '"') { inQuotes = !inQuotes; continue; }
        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct Remote {
    std::string host;
    int port = 0;
    std::string proto;
};

}  // namespace

std::optional<ParsedOvpn> parse(const std::string& text, ParseError* error) {
    ParsedOvpn result;
    OpenVpnProfile& profile = result.profile;
    auto& warnings = result.warnings;

    std::vector<std::string> outLines;      // sanitised config we will hand to openvpn.exe
    std::vector<Remote> remotes;
    std::string globalProto;
    int globalPort = 0;

    std::string normalised = text;
    for (auto& c : normalised) if (c == '\r') c = '\n';
    const auto lines = str::split(normalised, '\n', /*keepEmpty=*/true);

    std::string inlineTag;                  // non-empty while inside <tag>…</tag>
    std::vector<std::string> inlineBuffer;

    for (const auto& rawLine : lines) {
        const auto trimmed = str::trim(rawLine);

        // ── inline blocks (<ca>…</ca>, <key>…</key>) pass through verbatim ──
        if (!inlineTag.empty()) {
            inlineBuffer.push_back(rawLine);
            if (trimmed == "</" + inlineTag + ">") {
                for (const auto& l : inlineBuffer) outLines.push_back(l);
                inlineBuffer.clear();
                inlineTag.clear();
            }
            continue;
        }
        if (trimmed.size() > 2 && trimmed.front() == '<' && trimmed.back() == '>' &&
            !str::startsWith(trimmed, "</")) {
            inlineTag = trimmed.substr(1, trimmed.size() - 2);
            inlineBuffer.push_back(rawLine);
            continue;
        }

        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            outLines.push_back(rawLine);
            continue;
        }

        const auto tokens = tokenize(trimmed);
        if (tokens.empty()) continue;
        const auto key = str::lower(tokens[0]);
        const std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        if (kScriptDirectives.count(key)) {
            warnings.push_back("directive '" + key +
                               "' can run external commands — removed (TunHub never executes profile scripts)");
            continue;   // dropped entirely
        }

        if (key == "remote") {
            if (args.empty()) continue;
            Remote r;
            r.host = args[0];
            if (args.size() > 1) {
                int p = 0;
                auto* b = args[1].data();
                auto [ptr, ec] = std::from_chars(b, b + args[1].size(), p);
                if (ec == std::errc{}) r.port = p;
            }
            if (args.size() > 2) r.proto = normaliseProto(args[2]);
            remotes.push_back(std::move(r));
            outLines.push_back(rawLine);
            continue;
        }
        if (key == "proto") {
            if (!args.empty()) globalProto = normaliseProto(args[0]);
            outLines.push_back(rawLine);
            continue;
        }
        if (key == "port") {
            if (!args.empty()) {
                int p = 0;
                auto* b = args[0].data();
                auto [ptr, ec] = std::from_chars(b, b + args[0].size(), p);
                if (ec == std::errc{}) globalPort = p;
            }
            outLines.push_back(rawLine);
            continue;
        }
        if (key == "auth-user-pass") {
            // A file argument would point outside the profile; we always prompt instead.
            profile.authUserPass = true;
            outLines.push_back("auth-user-pass");
            continue;
        }
        if (key == "static-challenge") {
            profile.staticChallenge = true;
            if (!args.empty()) profile.staticChallengeText = args[0];
            outLines.push_back(rawLine);
            continue;
        }
        if (key == "redirect-gateway" || key == "redirect-private") {
            profile.redirectGateway = true;
            outLines.push_back(rawLine);
            continue;
        }
        if (key == "dhcp-option") {
            if (args.size() >= 2) {
                const auto opt = str::lower(args[0]);
                if (opt == "dns" || opt == "dns6") profile.dns.push_back(args[1]);
                else if (opt == "domain" || opt == "domain-search" ||
                         opt == "adapter_domain_suffix") profile.searchDomains.push_back(args[1]);
            }
            outLines.push_back(rawLine);
            continue;
        }
        if (kFileRefDirectives.count(key) && !args.empty()) {
            // Inline forms were handled above, so reaching here means an external file path.
            warnings.push_back("directive '" + key +
                               "' points to an external file — inline it into the .ovpn (<" + key +
                               ">…</" + key + ">) so TunHub can store it securely");
            outLines.push_back(rawLine);
            continue;
        }

        outLines.push_back(rawLine);
    }

    if (!inlineTag.empty()) {
        if (error) *error = {0, "unterminated <" + inlineTag + "> block"};
        return std::nullopt;
    }
    if (remotes.empty()) {
        if (error) *error = {0, "no `remote` found in the OpenVPN profile"};
        return std::nullopt;
    }

    // Resolve each remote against the global proto/port defaults.
    for (auto& r : remotes) {
        if (r.port == 0) r.port = globalPort ? globalPort : 1194;
        if (r.proto.empty()) r.proto = globalProto.empty() ? "udp" : globalProto;
    }
    profile.remoteSummary = remotes[0].host + ":" + std::to_string(remotes[0].port) + "/" +
                            remotes[0].proto;
    if (remotes.size() > 1)
        profile.remoteSummary += " (+" + std::to_string(remotes.size() - 1) + ")";

    profile.configText = str::join(outLines, "\n");
    if (error) *error = {};
    return result;
}

}  // namespace tunhub::ovpn
