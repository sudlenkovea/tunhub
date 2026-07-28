#include "tunhub/wgquick.h"

#include <charconv>

#include "tunhub/str.h"
#include "tunhub/util.h"
#include "tunhub/wgkey.h"

namespace tunhub::wgquick {
namespace {

enum class Section { None, Interface, Peer };

std::optional<int> parseInt(const std::string& s) {
    int v = 0;
    auto* begin = s.data();
    auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return v;
}

std::optional<uint32_t> parseU32(const std::string& s) {
    uint32_t v = 0;
    auto* begin = s.data();
    auto* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return v;
}

std::string prefix8(const std::string& s) { return s.size() <= 8 ? s : s.substr(0, 8); }

struct ParseState {
    TunnelConfig cfg;
    AwgParams awg;
    std::string privateKey;
    bool sawPrivateKey = false;
    std::vector<std::string> warnings;
};

/// Returns false and fills `error` on a fatal problem.
bool parseInterfaceKey(const std::string& key, const std::string& value,
                       const std::vector<std::string>& list, int lineNo,
                       ParseState& st, ParseError* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = {lineNo, msg};
        return false;
    };

    if (key == "privatekey") {
        if (!wgkey::isValid(value)) return fail("PrivateKey is not a 32-byte base64 key");
        st.privateKey = value;
        st.sawPrivateKey = true;
    } else if (key == "address") {
        for (const auto& a : list) {
            auto r = IpAddressRange::parse(a);
            if (!r) return fail("invalid Address: " + a);
            st.cfg.iface.addresses.push_back(*r);
        }
    } else if (key == "listenport") {
        auto v = parseInt(value);
        if (!v || *v < 0 || *v > 65535) return fail("invalid ListenPort");
        st.cfg.iface.listenPort = static_cast<uint16_t>(*v);
    } else if (key == "dns") {
        // wg-quick allows resolvers and search domains in the same list.
        for (const auto& d : list) {
            if (isIpLiteral(d)) st.cfg.iface.dns.push_back(d);
            else st.cfg.iface.dnsSearchDomains.push_back(d);
        }
    } else if (key == "mtu") {
        auto v = parseInt(value);
        if (!v || *v < 576 || *v > 9200) return fail("invalid MTU");
        st.cfg.iface.mtu = *v;
    } else if (key == "table" || key == "fwmark" || key == "saveconfig") {
        st.warnings.push_back("line " + std::to_string(lineNo) + ": " + key +
                              " is not supported on this platform, ignored");
    } else if (key == "preup") {
        st.cfg.iface.preUp.push_back(value);
    } else if (key == "postup") {
        st.cfg.iface.postUp.push_back(value);
    } else if (key == "predown") {
        st.cfg.iface.preDown.push_back(value);
    } else if (key == "postdown") {
        st.cfg.iface.postDown.push_back(value);
    }
    // ── AmneziaWG ──
    else if (key == "jc")    { st.awg.jc = parseInt(value); }
    else if (key == "jmin")  { st.awg.jmin = parseInt(value); }
    else if (key == "jmax")  { st.awg.jmax = parseInt(value); }
    else if (key == "s1")    { st.awg.s1 = parseInt(value); }
    else if (key == "s2")    { st.awg.s2 = parseInt(value); }
    else if (key == "s3")    { st.awg.s3 = parseInt(value); }
    else if (key == "s4")    { st.awg.s4 = parseInt(value); }
    else if (key == "h1")    { st.awg.h1 = parseU32(value); }
    else if (key == "h2")    { st.awg.h2 = parseU32(value); }
    else if (key == "h3")    { st.awg.h3 = parseU32(value); }
    else if (key == "h4")    { st.awg.h4 = parseU32(value); }
    else if (key == "i1")    { st.awg.i1 = value; }
    else if (key == "i2")    { st.awg.i2 = value; }
    else if (key == "i3")    { st.awg.i3 = value; }
    else if (key == "i4")    { st.awg.i4 = value; }
    else if (key == "i5")    { st.awg.i5 = value; }
    else if (key == "itime") { st.awg.itime = parseInt(value); }
    else if (key == "j1" || key == "j2" || key == "j3") {
        st.warnings.push_back("line " + std::to_string(lineNo) + ": parameter " +
                              str::lower(key) + " is not yet supported by the core, ignored");
    } else {
        st.warnings.push_back("line " + std::to_string(lineNo) + ": unknown [Interface] key " + key);
    }
    return true;
}

bool parsePeerKey(const std::string& key, const std::string& value,
                  const std::vector<std::string>& list, int lineNo,
                  PeerConfig& peer, std::optional<std::string>& psk,
                  std::vector<std::string>& warnings, ParseError* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = {lineNo, msg};
        return false;
    };

    if (key == "publickey") {
        if (!wgkey::isValid(value)) return fail("PublicKey is not a 32-byte base64 key");
        peer.publicKey = value;
    } else if (key == "presharedkey") {
        if (!wgkey::isValid(value)) return fail("PresharedKey is not a 32-byte base64 key");
        psk = value;
    } else if (key == "allowedips") {
        for (const auto& a : list) {
            auto r = IpAddressRange::parse(a);
            if (!r) return fail("invalid AllowedIPs: " + a);
            peer.allowedIPs.push_back(*r);
        }
    } else if (key == "endpoint") {
        if (!parseEndpoint(value)) return fail("invalid Endpoint (expected host:port)");
        peer.endpoint = value;
    } else if (key == "persistentkeepalive") {
        auto v = parseInt(value);
        if (!v || *v < 0 || *v > 65535) return fail("invalid PersistentKeepalive");
        peer.persistentKeepalive = static_cast<uint16_t>(*v);
    } else {
        warnings.push_back("line " + std::to_string(lineNo) + ": unknown [Peer] key " + key);
    }
    return true;
}

}  // namespace

std::optional<ParsedTunnel> parse(const std::string& name, const std::string& text,
                                  ParseError* error) {
    ParseState st;
    st.cfg.id = util::newGuid();
    st.cfg.name = name;
    st.cfg.meta.createdAt = util::nowUnix();

    Section section = Section::None;
    std::optional<PeerConfig> currentPeer;
    std::optional<std::string> currentPsk;
    std::map<std::string, std::string> peerPsks;

    auto flushPeer = [&](int lineNo) -> bool {
        if (!currentPeer) return true;
        if (!wgkey::isValid(currentPeer->publicKey)) {
            if (error) *error = {lineNo, "[Peer] has no valid PublicKey"};
            return false;
        }
        if (currentPsk) peerPsks[currentPeer->id] = *currentPsk;
        if (currentPeer->allowedIPs.empty())
            st.warnings.push_back("peer " + prefix8(currentPeer->publicKey) + "…: empty AllowedIPs");
        st.cfg.peers.push_back(*currentPeer);
        currentPeer.reset();
        currentPsk.reset();
        return true;
    };

    // Normalise line endings, then walk line by line.
    std::string normalised = text;
    for (auto& c : normalised) if (c == '\r') c = '\n';
    const auto lines = str::split(normalised, '\n', /*keepEmpty=*/true);

    for (size_t i = 0; i < lines.size(); ++i) {
        const int lineNo = static_cast<int>(i) + 1;
        std::string line = lines[i];
        if (auto hash = line.find('#'); hash != std::string::npos) line = line.substr(0, hash);
        line = str::trim(line);
        if (line.empty()) continue;

        const auto lower = str::lower(line);
        if (lower == "[interface]") {
            if (!flushPeer(lineNo)) return std::nullopt;
            section = Section::Interface;
            continue;
        }
        if (lower == "[peer]") {
            if (!flushPeer(lineNo)) return std::nullopt;
            section = Section::Peer;
            currentPeer = PeerConfig{};
            currentPeer->id = util::newGuid();
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = {lineNo, "expected key = value"};
            return std::nullopt;
        }
        const auto key = str::lower(str::trim(line.substr(0, eq)));
        const auto value = str::trim(line.substr(eq + 1));
        const auto list = str::splitList(value);

        if (section == Section::Interface) {
            if (!parseInterfaceKey(key, value, list, lineNo, st, error)) return std::nullopt;
        } else if (section == Section::Peer) {
            if (!parsePeerKey(key, value, list, lineNo, *currentPeer, currentPsk,
                              st.warnings, error)) return std::nullopt;
        } else {
            if (error) *error = {lineNo, "key outside an [Interface]/[Peer] section"};
            return std::nullopt;
        }
    }
    if (!flushPeer(static_cast<int>(lines.size()))) return std::nullopt;

    if (!st.sawPrivateKey) {
        if (error) *error = {0, "no PrivateKey in [Interface]"};
        return std::nullopt;
    }
    if (st.cfg.peers.empty()) {
        if (error) *error = {0, "no [Peer] section found"};
        return std::nullopt;
    }
    if (auto awgErrors = st.awg.validate(); !awgErrors.empty()) {
        if (error) *error = {0, str::join(awgErrors, "; ")};
        return std::nullopt;
    }

    // Any AWG parameter present ⇒ this is an AmneziaWG tunnel.
    if (!st.awg.empty()) {
        st.cfg.kind = TunnelKind::AmneziaWg;
        st.cfg.awg = st.awg;
    }
    if (auto pub = wgkey::publicKeyFrom(st.privateKey)) st.cfg.iface.publicKey = *pub;

    if (!st.cfg.iface.postUp.empty() || !st.cfg.iface.preUp.empty())
        st.warnings.push_back(
            "config contains PreUp/PostUp scripts — TunHub stores but never executes them (security)");
    if (st.cfg.iface.addresses.empty())
        st.warnings.push_back("no Address in [Interface]");

    ParsedTunnel out;
    out.config = std::move(st.cfg);
    out.privateKey = std::move(st.privateKey);
    out.presharedKeys = std::move(peerPsks);
    out.warnings = std::move(st.warnings);
    if (error) *error = {};
    return out;
}

std::string serialize(const TunnelConfig& config, const std::string& privateKey,
                      const std::map<std::string, std::string>& presharedKeys,
                      bool redactSecrets) {
    std::string out = "[Interface]\n";
    out += "PrivateKey = ";
    out += redactSecrets ? "<REDACTED>" : (privateKey.empty() ? "<MISSING>" : privateKey);
    out += "\n";

    if (!config.iface.addresses.empty())
        out += "Address = " + joinRanges(config.iface.addresses) + "\n";
    if (config.iface.listenPort)
        out += "ListenPort = " + std::to_string(*config.iface.listenPort) + "\n";

    auto dnsAll = config.iface.dns;
    dnsAll.insert(dnsAll.end(), config.iface.dnsSearchDomains.begin(),
                  config.iface.dnsSearchDomains.end());
    if (!dnsAll.empty()) out += "DNS = " + str::join(dnsAll, ", ") + "\n";
    if (config.iface.mtu) out += "MTU = " + std::to_string(*config.iface.mtu) + "\n";

    if (config.awg && config.kind == TunnelKind::AmneziaWg) {
        const auto& a = *config.awg;
        auto putInt = [&](const char* k, const std::optional<int>& v) {
            if (v) out += std::string(k) + " = " + std::to_string(*v) + "\n";
        };
        auto putU32 = [&](const char* k, const std::optional<uint32_t>& v) {
            if (v) out += std::string(k) + " = " + std::to_string(*v) + "\n";
        };
        auto putStr = [&](const char* k, const std::optional<std::string>& v) {
            if (v && !v->empty()) out += std::string(k) + " = " + *v + "\n";
        };
        putInt("Jc", a.jc); putInt("Jmin", a.jmin); putInt("Jmax", a.jmax);
        putInt("S1", a.s1); putInt("S2", a.s2); putInt("S3", a.s3); putInt("S4", a.s4);
        putU32("H1", a.h1); putU32("H2", a.h2); putU32("H3", a.h3); putU32("H4", a.h4);
        putStr("I1", a.i1); putStr("I2", a.i2); putStr("I3", a.i3);
        putStr("I4", a.i4); putStr("I5", a.i5);
        putInt("ITime", a.itime);
    }

    for (const auto& p : config.peers) {
        out += "\n[Peer]\n";
        out += "PublicKey = " + p.publicKey + "\n";
        const auto psk = presharedKeys.find(p.id);
        if (p.presharedKeyRef || psk != presharedKeys.end()) {
            out += "PresharedKey = ";
            if (redactSecrets) out += "<REDACTED>";
            else out += psk != presharedKeys.end() ? psk->second : "<MISSING>";
            out += "\n";
        }
        if (!p.allowedIPs.empty()) out += "AllowedIPs = " + joinRanges(p.allowedIPs) + "\n";
        if (p.endpoint) out += "Endpoint = " + *p.endpoint + "\n";
        if (p.persistentKeepalive)
            out += "PersistentKeepalive = " + std::to_string(*p.persistentKeepalive) + "\n";
    }
    return out;
}

}  // namespace tunhub::wgquick
