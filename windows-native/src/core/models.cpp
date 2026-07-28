#include "tunhub/models.h"

#include <algorithm>
#include <ctime>
#include <random>
#include <set>
#include <unordered_set>

#include "tunhub/constants.h"
#include "tunhub/str.h"

namespace tunhub {
namespace {

std::mt19937& rng() {
    static std::mt19937 gen{std::random_device{}()};
    return gen;
}

int randomInt(int lo, int hiInclusive) {
    return std::uniform_int_distribution<int>(lo, hiInclusive)(rng());
}

// ── optional<T> ↔ JSON. Absent stays absent, so we never write nulls into configs. ──

template <typename T>
void putOpt(Json& j, const char* key, const std::optional<T>& v) {
    if (v) j.set(key, Json(*v));
}

void putOptStr(Json& j, const char* key, const std::optional<std::string>& v) {
    if (v && !v->empty()) j.set(key, Json(*v));
}

std::optional<int> getOptInt(const Json& j, const char* key) {
    if (!j.has(key) || !j[key].isNumber()) return std::nullopt;
    return j[key].asInt();
}

std::optional<uint32_t> getOptU32(const Json& j, const char* key) {
    if (!j.has(key) || !j[key].isNumber()) return std::nullopt;
    return static_cast<uint32_t>(j[key].asInt64());
}

std::optional<uint16_t> getOptU16(const Json& j, const char* key) {
    if (!j.has(key) || !j[key].isNumber()) return std::nullopt;
    return static_cast<uint16_t>(j[key].asInt());
}

std::optional<std::string> getOptStr(const Json& j, const char* key) {
    if (!j.has(key) || !j[key].isString() || j[key].asString().empty()) return std::nullopt;
    return j[key].asString();
}

Json rangesToJson(const std::vector<IpAddressRange>& v) {
    Json a = Json::array();
    for (const auto& r : v) a.push(Json(r.canonical()));
    return a;
}

std::vector<IpAddressRange> rangesFromJson(const Json& j) {
    std::vector<IpAddressRange> out;
    for (const auto& item : j.items())
        if (auto r = IpAddressRange::parse(item.asString())) out.push_back(*r);
    return out;
}

Json stringsToJson(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push(Json(s));
    return a;
}

std::vector<std::string> stringsFromJson(const Json& j) {
    std::vector<std::string> out;
    for (const auto& item : j.items())
        if (item.isString()) out.push_back(item.asString());
    return out;
}

Json secretRefToJson(const std::optional<SecretRef>& ref) {
    if (!ref || ref->account.empty()) return Json();
    Json j = Json::object();
    j.set("account", Json(ref->account));
    return j;
}

std::optional<SecretRef> secretRefFromJson(const Json& j) {
    if (!j.isObject()) return std::nullopt;
    auto account = j["account"].asString("");
    if (account.empty()) return std::nullopt;
    return SecretRef{account};
}

const char* dnsModeName(DnsModeKind k) {
    switch (k) {
        case DnsModeKind::Split: return "split";
        case DnsModeKind::Disabled: return "disabled";
        default: return "global";
    }
}

DnsModeKind dnsModeFromName(const std::string& s) {
    if (str::iequals(s, "split")) return DnsModeKind::Split;
    if (str::iequals(s, "disabled")) return DnsModeKind::Disabled;
    return DnsModeKind::Global;
}

}  // namespace

// ── TunnelKind ───────────────────────────────────────────────────────────────

std::string kindLabel(TunnelKind k) {
    switch (k) {
        case TunnelKind::AmneziaWg: return "AmneziaWG";
        case TunnelKind::OpenVpn:   return "OpenVPN";
        default:                    return "WireGuard";
    }
}

std::string kindCoreBinary(TunnelKind k) {
    switch (k) {
        case TunnelKind::AmneziaWg: return core_binary::kAmneziaWg;
        case TunnelKind::OpenVpn:   return core_binary::kOpenVpn;
        default:                    return core_binary::kWireGuard;
    }
}

bool kindIsWireGuardFamily(TunnelKind k) {
    return k == TunnelKind::WireGuard || k == TunnelKind::AmneziaWg;
}

std::string kindToString(TunnelKind k) {
    switch (k) {
        case TunnelKind::AmneziaWg: return "amneziawg";
        case TunnelKind::OpenVpn:   return "openvpn";
        default:                    return "wireguard";
    }
}

TunnelKind kindFromString(const std::string& s, TunnelKind fallback) {
    if (str::iequals(s, "amneziawg")) return TunnelKind::AmneziaWg;
    if (str::iequals(s, "openvpn"))   return TunnelKind::OpenVpn;
    if (str::iequals(s, "wireguard")) return TunnelKind::WireGuard;
    return fallback;
}

// ── AwgParams ────────────────────────────────────────────────────────────────

bool AwgParams::empty() const {
    return !jc && !jmin && !jmax && !s1 && !s2 && !s3 && !s4 &&
           !h1 && !h2 && !h3 && !h4 && !i1 && !i2 && !i3 && !i4 && !i5 && !itime;
}

std::vector<std::string> AwgParams::validate() const {
    std::vector<std::string> e;
    if (jc && (*jc < 0 || *jc > 128)) e.push_back("Jc must be 0…128");
    if (jmin && jmax && *jmin > *jmax) e.push_back("Jmin > Jmax");
    if (jmin && (*jmin < 0 || *jmin > 1280)) e.push_back("Jmin must be 0…1280");
    if (jmax && (*jmax < 0 || *jmax > 1280)) e.push_back("Jmax must be 0…1280");
    if (s1 && (*s1 < 0 || *s1 > 1132)) e.push_back("S1 out of range (0…1132)");
    if (s2 && (*s2 < 0 || *s2 > 1188)) e.push_back("S2 out of range (0…1188)");

    std::vector<uint32_t> hs;
    for (const auto& h : {h1, h2, h3, h4}) if (h) hs.push_back(*h);
    if (!hs.empty()) {
        if (hs.size() != 4) e.push_back("H1–H4 must all be set together");
        if (std::set<uint32_t>(hs.begin(), hs.end()).size() != hs.size())
            e.push_back("H1–H4 must be pairwise distinct");
    }
    return e;
}

AwgParams AwgParams::amneziaDefault() {
    AwgParams p;
    p.jc = randomInt(3, 10);
    p.jmin = 50;
    p.jmax = 1000;
    p.s1 = 0;
    p.s2 = 0;
    return p;
}

AwgParams AwgParams::fullObfuscation() {
    AwgParams p;
    p.jc = randomInt(4, 12);
    p.jmin = 40;
    p.jmax = 70;
    p.s1 = randomInt(15, 150);
    p.s2 = randomInt(15, 150);
    // H1–H4 must be distinct and above the four real WireGuard message types.
    std::set<uint32_t> hs;
    while (hs.size() < 4) hs.insert(static_cast<uint32_t>(randomInt(5, 2147483646)));
    auto it = hs.begin();
    p.h1 = *it++; p.h2 = *it++; p.h3 = *it++; p.h4 = *it;
    return p;
}

Json AwgParams::toJson() const {
    Json j = Json::object();
    putOpt(j, "jc", jc); putOpt(j, "jmin", jmin); putOpt(j, "jmax", jmax);
    putOpt(j, "s1", s1); putOpt(j, "s2", s2); putOpt(j, "s3", s3); putOpt(j, "s4", s4);
    if (h1) j.set("h1", Json(static_cast<long long>(*h1)));
    if (h2) j.set("h2", Json(static_cast<long long>(*h2)));
    if (h3) j.set("h3", Json(static_cast<long long>(*h3)));
    if (h4) j.set("h4", Json(static_cast<long long>(*h4)));
    putOptStr(j, "i1", i1); putOptStr(j, "i2", i2); putOptStr(j, "i3", i3);
    putOptStr(j, "i4", i4); putOptStr(j, "i5", i5);
    putOpt(j, "itime", itime);
    return j;
}

AwgParams AwgParams::fromJson(const Json& j) {
    AwgParams p;
    p.jc = getOptInt(j, "jc"); p.jmin = getOptInt(j, "jmin"); p.jmax = getOptInt(j, "jmax");
    p.s1 = getOptInt(j, "s1"); p.s2 = getOptInt(j, "s2");
    p.s3 = getOptInt(j, "s3"); p.s4 = getOptInt(j, "s4");
    p.h1 = getOptU32(j, "h1"); p.h2 = getOptU32(j, "h2");
    p.h3 = getOptU32(j, "h3"); p.h4 = getOptU32(j, "h4");
    p.i1 = getOptStr(j, "i1"); p.i2 = getOptStr(j, "i2"); p.i3 = getOptStr(j, "i3");
    p.i4 = getOptStr(j, "i4"); p.i5 = getOptStr(j, "i5");
    p.itime = getOptInt(j, "itime");
    return p;
}

// ── TunnelConfig ─────────────────────────────────────────────────────────────

std::vector<IpAddressRange> TunnelConfig::effectiveRoutes() const {
    if (options.routeMode.kind == RouteModeKind::Custom) return options.routeMode.custom;

    std::vector<IpAddressRange> out;
    std::unordered_set<std::string> seen;
    for (const auto& p : peers)
        for (const auto& r : p.allowedIPs)
            if (seen.insert(r.canonical()).second) out.push_back(r);
    return out;
}

bool TunnelConfig::hasDefaultRoute() const {
    const auto routes = effectiveRoutes();
    return std::any_of(routes.begin(), routes.end(),
                       [](const IpAddressRange& r) { return r.prefix <= 1; });
}

DnsMode TunnelConfig::effectiveDnsMode() const {
    if (options.dnsMode.kind == DnsModeKind::Global) {
        DnsMode m;
        m.kind = hasDefaultRoute() ? DnsModeKind::Global : DnsModeKind::Disabled;
        return m;
    }
    return options.dnsMode;
}

Json TunnelConfig::toJson() const {
    Json j = Json::object();
    j.set("id", Json(id));
    j.set("name", Json(name));
    j.set("kind", Json(kindToString(kind)));
    j.set("schemaVersion", Json(schemaVersion));

    Json ifc = Json::object();
    if (auto ref = secretRefToJson(iface.privateKeyRef); !ref.isNull())
        ifc.set("privateKeyRef", ref);
    if (!iface.publicKey.empty()) ifc.set("publicKey", Json(iface.publicKey));
    ifc.set("addresses", rangesToJson(iface.addresses));
    putOpt(ifc, "listenPort", iface.listenPort);
    if (!iface.dns.empty()) ifc.set("dns", stringsToJson(iface.dns));
    if (!iface.dnsSearchDomains.empty())
        ifc.set("dnsSearchDomains", stringsToJson(iface.dnsSearchDomains));
    putOpt(ifc, "mtu", iface.mtu);
    // Preserved verbatim so an exported config round-trips, but never executed.
    if (!iface.preUp.empty())    ifc.set("preUp", stringsToJson(iface.preUp));
    if (!iface.postUp.empty())   ifc.set("postUp", stringsToJson(iface.postUp));
    if (!iface.preDown.empty())  ifc.set("preDown", stringsToJson(iface.preDown));
    if (!iface.postDown.empty()) ifc.set("postDown", stringsToJson(iface.postDown));
    j.set("interface", ifc);

    Json ps = Json::array();
    for (const auto& p : peers) {
        Json pj = Json::object();
        pj.set("id", Json(p.id));
        pj.set("publicKey", Json(p.publicKey));
        if (auto ref = secretRefToJson(p.presharedKeyRef); !ref.isNull())
            pj.set("presharedKeyRef", ref);
        putOptStr(pj, "endpoint", p.endpoint);
        pj.set("allowedIPs", rangesToJson(p.allowedIPs));
        putOpt(pj, "persistentKeepalive", p.persistentKeepalive);
        ps.push(pj);
    }
    j.set("peers", ps);

    if (awg && !awg->empty()) j.set("awg", awg->toJson());

    if (openVpn) {
        Json o = Json::object();
        o.set("configText", Json(openVpn->configText));
        o.set("authUserPass", Json(openVpn->authUserPass));
        o.set("staticChallenge", Json(openVpn->staticChallenge));
        if (!openVpn->staticChallengeText.empty())
            o.set("staticChallengeText", Json(openVpn->staticChallengeText));
        if (auto ref = secretRefToJson(openVpn->credentialsRef); !ref.isNull())
            o.set("credentialsRef", ref);
        if (!openVpn->remoteSummary.empty()) o.set("remoteSummary", Json(openVpn->remoteSummary));
        if (!openVpn->dns.empty()) o.set("dns", stringsToJson(openVpn->dns));
        if (!openVpn->searchDomains.empty())
            o.set("searchDomains", stringsToJson(openVpn->searchDomains));
        o.set("redirectGateway", Json(openVpn->redirectGateway));
        j.set("openVpn", o);
    }

    Json opt = Json::object();
    Json dm = Json::object();
    dm.set("kind", Json(dnsModeName(options.dnsMode.kind)));
    if (!options.dnsMode.matchDomains.empty())
        dm.set("matchDomains", stringsToJson(options.dnsMode.matchDomains));
    opt.set("dnsMode", dm);
    Json rm = Json::object();
    rm.set("kind", Json(options.routeMode.kind == RouteModeKind::Custom ? "custom" : "fromAllowedIPs"));
    if (!options.routeMode.custom.empty()) rm.set("custom", rangesToJson(options.routeMode.custom));
    opt.set("routeMode", rm);
    opt.set("autoConnectOnLaunch", Json(options.autoConnectOnLaunch));
    opt.set("killSwitch", Json(options.killSwitch));
    j.set("options", opt);

    Json meta_ = Json::object();
    meta_.set("createdAt", Json(static_cast<long long>(meta.createdAt)));
    if (meta.lastConnectedAt)
        meta_.set("lastConnectedAt", Json(static_cast<long long>(*meta.lastConnectedAt)));
    if (!meta.group.empty()) meta_.set("group", Json(meta.group));
    if (!meta.notes.empty()) meta_.set("notes", Json(meta.notes));
    meta_.set("sortOrder", Json(meta.sortOrder));
    j.set("meta", meta_);

    return j;
}

TunnelConfig TunnelConfig::fromJson(const Json& j) {
    TunnelConfig c;
    c.id = j["id"].asString("");
    c.name = j["name"].asString("");
    c.kind = kindFromString(j["kind"].asString(""));
    c.schemaVersion = j.has("schemaVersion") ? j["schemaVersion"].asInt(1) : 1;

    const Json& ifc = j["interface"];
    c.iface.privateKeyRef = secretRefFromJson(ifc["privateKeyRef"]);
    c.iface.publicKey = ifc["publicKey"].asString("");
    c.iface.addresses = rangesFromJson(ifc["addresses"]);
    c.iface.listenPort = getOptU16(ifc, "listenPort");
    c.iface.dns = stringsFromJson(ifc["dns"]);
    c.iface.dnsSearchDomains = stringsFromJson(ifc["dnsSearchDomains"]);
    c.iface.mtu = getOptInt(ifc, "mtu");
    c.iface.preUp = stringsFromJson(ifc["preUp"]);
    c.iface.postUp = stringsFromJson(ifc["postUp"]);
    c.iface.preDown = stringsFromJson(ifc["preDown"]);
    c.iface.postDown = stringsFromJson(ifc["postDown"]);

    for (const auto& pj : j["peers"].items()) {
        PeerConfig p;
        p.id = pj["id"].asString("");
        p.publicKey = pj["publicKey"].asString("");
        p.presharedKeyRef = secretRefFromJson(pj["presharedKeyRef"]);
        p.endpoint = getOptStr(pj, "endpoint");
        p.allowedIPs = rangesFromJson(pj["allowedIPs"]);
        p.persistentKeepalive = getOptU16(pj, "persistentKeepalive");
        c.peers.push_back(std::move(p));
    }

    if (j.has("awg")) c.awg = AwgParams::fromJson(j["awg"]);

    if (j.has("openVpn")) {
        const Json& o = j["openVpn"];
        OpenVpnProfile p;
        p.configText = o["configText"].asString("");
        p.authUserPass = o["authUserPass"].asBool(false);
        p.staticChallenge = o["staticChallenge"].asBool(false);
        p.staticChallengeText = o["staticChallengeText"].asString("");
        p.credentialsRef = secretRefFromJson(o["credentialsRef"]);
        p.remoteSummary = o["remoteSummary"].asString("");
        p.dns = stringsFromJson(o["dns"]);
        p.searchDomains = stringsFromJson(o["searchDomains"]);
        p.redirectGateway = o["redirectGateway"].asBool(false);
        c.openVpn = std::move(p);
    }

    const Json& opt = j["options"];
    c.options.dnsMode.kind = dnsModeFromName(opt["dnsMode"]["kind"].asString("global"));
    c.options.dnsMode.matchDomains = stringsFromJson(opt["dnsMode"]["matchDomains"]);
    c.options.routeMode.kind = str::iequals(opt["routeMode"]["kind"].asString(""), "custom")
                                   ? RouteModeKind::Custom
                                   : RouteModeKind::FromAllowedIPs;
    c.options.routeMode.custom = rangesFromJson(opt["routeMode"]["custom"]);
    c.options.autoConnectOnLaunch = opt["autoConnectOnLaunch"].asBool(false);
    c.options.killSwitch = opt["killSwitch"].asBool(false);

    const Json& m = j["meta"];
    c.meta.createdAt = m["createdAt"].asInt64(0);
    if (m.has("lastConnectedAt")) c.meta.lastConnectedAt = m["lastConnectedAt"].asInt64(0);
    c.meta.group = m["group"].asString("");
    c.meta.notes = m["notes"].asString("");
    c.meta.sortOrder = m["sortOrder"].asInt(0);

    return c;
}

// ── ResolvedTunnelSpec ───────────────────────────────────────────────────────

Json ResolvedTunnelSpec::toJson() const {
    Json j = Json::object();
    j.set("id", Json(id));
    j.set("name", Json(name));
    j.set("kind", Json(kindToString(kind)));
    j.set("privateKey", Json(privateKey));
    j.set("addresses", rangesToJson(addresses));
    putOpt(j, "listenPort", listenPort);
    putOpt(j, "mtu", mtu);
    j.set("dnsServers", stringsToJson(dnsServers));
    j.set("dnsSearchDomains", stringsToJson(dnsSearchDomains));
    Json dm = Json::object();
    dm.set("kind", Json(dnsModeName(dnsMode.kind)));
    dm.set("matchDomains", stringsToJson(dnsMode.matchDomains));
    j.set("dnsMode", dm);
    j.set("routes", rangesToJson(routes));
    if (awg && !awg->empty()) j.set("awg", awg->toJson());
    j.set("killSwitch", Json(killSwitch));

    Json ps = Json::array();
    for (const auto& p : peers) {
        Json pj = Json::object();
        pj.set("publicKey", Json(p.publicKey));
        putOptStr(pj, "presharedKey", p.presharedKey);
        putOptStr(pj, "endpoint", p.endpoint);
        pj.set("allowedIPs", rangesToJson(p.allowedIPs));
        putOpt(pj, "keepalive", p.keepalive);
        ps.push(pj);
    }
    j.set("peers", ps);

    if (openVpn) {
        Json o = Json::object();
        o.set("configText", Json(openVpn->configText));
        o.set("username", Json(openVpn->username));
        o.set("password", Json(openVpn->password));
        o.set("otp", Json(openVpn->otp));
        j.set("openVpn", o);
    }
    return j;
}

ResolvedTunnelSpec ResolvedTunnelSpec::fromJson(const Json& j) {
    ResolvedTunnelSpec s;
    s.id = j["id"].asString("");
    s.name = j["name"].asString("");
    s.kind = kindFromString(j["kind"].asString(""));
    s.privateKey = j["privateKey"].asString("");
    s.addresses = rangesFromJson(j["addresses"]);
    s.listenPort = getOptU16(j, "listenPort");
    s.mtu = getOptInt(j, "mtu");
    s.dnsServers = stringsFromJson(j["dnsServers"]);
    s.dnsSearchDomains = stringsFromJson(j["dnsSearchDomains"]);
    s.dnsMode.kind = dnsModeFromName(j["dnsMode"]["kind"].asString("global"));
    s.dnsMode.matchDomains = stringsFromJson(j["dnsMode"]["matchDomains"]);
    s.routes = rangesFromJson(j["routes"]);
    if (j.has("awg")) s.awg = AwgParams::fromJson(j["awg"]);
    s.killSwitch = j["killSwitch"].asBool(false);

    for (const auto& pj : j["peers"].items()) {
        ResolvedPeer p;
        p.publicKey = pj["publicKey"].asString("");
        p.presharedKey = getOptStr(pj, "presharedKey");
        p.endpoint = getOptStr(pj, "endpoint");
        p.allowedIPs = rangesFromJson(pj["allowedIPs"]);
        p.keepalive = getOptU16(pj, "keepalive");
        s.peers.push_back(std::move(p));
    }

    if (j.has("openVpn")) {
        ResolvedOpenVpn o;
        o.configText = j["openVpn"]["configText"].asString("");
        o.username = j["openVpn"]["username"].asString("");
        o.password = j["openVpn"]["password"].asString("");
        o.otp = j["openVpn"]["otp"].asString("");
        s.openVpn = std::move(o);
    }
    return s;
}

// ── Runtime state ────────────────────────────────────────────────────────────

std::string phaseToString(TunnelPhase p) {
    switch (p) {
        case TunnelPhase::Starting: return "starting";
        case TunnelPhase::Up:       return "up";
        case TunnelPhase::Degraded: return "degraded";
        case TunnelPhase::Failed:   return "failed";
        case TunnelPhase::Stopping: return "stopping";
        default:                    return "stopped";
    }
}

TunnelPhase phaseFromString(const std::string& s) {
    if (s == "starting") return TunnelPhase::Starting;
    if (s == "up")       return TunnelPhase::Up;
    if (s == "degraded") return TunnelPhase::Degraded;
    if (s == "failed")   return TunnelPhase::Failed;
    if (s == "stopping") return TunnelPhase::Stopping;
    return TunnelPhase::Stopped;
}

uint64_t TunnelRuntimeState::rxTotal() const {
    uint64_t t = 0;
    for (const auto& p : peers) t += p.rxBytes;
    return t;
}

uint64_t TunnelRuntimeState::txTotal() const {
    uint64_t t = 0;
    for (const auto& p : peers) t += p.txBytes;
    return t;
}

int64_t TunnelRuntimeState::lastHandshake() const {
    int64_t max = 0;
    for (const auto& p : peers) max = std::max(max, p.lastHandshake);
    return max;
}

bool TunnelRuntimeState::handshakeFresh() const {
    const int64_t h = lastHandshake();
    if (h == 0) return false;
    return (static_cast<int64_t>(std::time(nullptr)) - h) < 185;
}

Json TunnelRuntimeState::toJson() const {
    Json j = Json::object();
    j.set("id", Json(id));
    j.set("name", Json(name));
    j.set("phase", Json(phaseToString(phase)));
    if (!interfaceName.empty()) j.set("interfaceName", Json(interfaceName));
    if (!errorMessage.empty()) j.set("errorMessage", Json(errorMessage));
    if (since) j.set("since", Json(static_cast<long long>(since)));
    if (!routes.empty()) j.set("routes", stringsToJson(routes));

    Json ps = Json::array();
    for (const auto& p : peers) {
        Json pj = Json::object();
        pj.set("publicKey", Json(p.publicKey));
        if (!p.endpoint.empty()) pj.set("endpoint", Json(p.endpoint));
        if (p.lastHandshake) pj.set("lastHandshake", Json(static_cast<long long>(p.lastHandshake)));
        pj.set("rxBytes", Json(static_cast<long long>(p.rxBytes)));
        pj.set("txBytes", Json(static_cast<long long>(p.txBytes)));
        ps.push(pj);
    }
    j.set("peers", ps);
    return j;
}

TunnelRuntimeState TunnelRuntimeState::fromJson(const Json& j) {
    TunnelRuntimeState s;
    s.id = j["id"].asString("");
    s.name = j["name"].asString("");
    s.phase = phaseFromString(j["phase"].asString("stopped"));
    s.interfaceName = j["interfaceName"].asString("");
    s.errorMessage = j["errorMessage"].asString("");
    s.since = j["since"].asInt64(0);
    s.routes = stringsFromJson(j["routes"]);
    for (const auto& pj : j["peers"].items()) {
        PeerRuntime p;
        p.publicKey = pj["publicKey"].asString("");
        p.endpoint = pj["endpoint"].asString("");
        p.lastHandshake = pj["lastHandshake"].asInt64(0);
        p.rxBytes = pj["rxBytes"].asUInt64(0);
        p.txBytes = pj["txBytes"].asUInt64(0);
        s.peers.push_back(std::move(p));
    }
    return s;
}

}  // namespace tunhub
