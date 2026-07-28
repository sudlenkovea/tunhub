#include "tunhub/supervisor.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "tunhub/constants.h"
#include "tunhub/paths.h"
#include "tunhub/str.h"
#include "tunhub/uapi.h"
#include "tunhub/util.h"

namespace tunhub {
namespace {

/// A tunnel that carries everything (prefix 0, or the /1 pair we install for it).
bool routesAreDefault(const std::vector<IpAddressRange>& routes) {
    return std::any_of(routes.begin(), routes.end(),
                       [](const IpAddressRange& r) { return r.prefix <= 1; });
}

/// Resolve a host to a literal address, preferring IPv4.
std::string resolveHost(const std::string& host) {
    if (isIpLiteral(host)) return host;

    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    PADDRINFOW result = nullptr;
    if (GetAddrInfoW(str::widen(host).c_str(), nullptr, &hints, &result) != 0 || !result)
        return {};

    std::string v4, v6;
    for (auto* it = result; it; it = it->ai_next) {
        char buf[INET6_ADDRSTRLEN]{};
        if (it->ai_family == AF_INET && v4.empty()) {
            auto* a = reinterpret_cast<sockaddr_in*>(it->ai_addr);
            inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
            v4 = buf;
        } else if (it->ai_family == AF_INET6 && v6.empty()) {
            auto* a = reinterpret_cast<sockaddr_in6*>(it->ai_addr);
            inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
            v6 = buf;
        }
    }
    FreeAddrInfoW(result);
    return !v4.empty() ? v4 : v6;
}

}  // namespace

bool TunnelSupervisor::RunningTunnel::isDefaultRoute() const {
    return routesAreDefault(spec.routes);
}

TunnelSupervisor::TunnelSupervisor(FileLog& log) : log_(log), net_(log) {}

TunnelSupervisor::~TunnelSupervisor() {
    stopping_ = true;
    if (statsThread_.joinable()) statsThread_.join();
    stopAll();
}

// ── endpoints ────────────────────────────────────────────────────────────────

std::map<int, std::string> TunnelSupervisor::resolveEndpoints(
    const ResolvedTunnelSpec& spec, std::vector<std::string>& hostsOut) const {
    std::map<int, std::string> out;
    for (size_t i = 0; i < spec.peers.size(); ++i) {
        const auto& p = spec.peers[i];
        if (!p.endpoint) continue;
        auto ep = parseEndpoint(*p.endpoint);
        if (!ep) continue;
        const auto ip = resolveHost(ep->host);
        if (ip.empty()) {
            log_.warn("dns", "could not resolve endpoint " + ep->host);
            continue;
        }
        out[static_cast<int>(i)] = ip + ":" + std::to_string(ep->port);
        hostsOut.push_back(ip);
    }
    return out;
}

// ── start ────────────────────────────────────────────────────────────────────

bool TunnelSupervisor::start(const ResolvedTunnelSpec& spec, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.count(spec.id) || openvpn_.count(spec.id) || starting_.count(spec.id)) {
            if (error) *error = "tunnel already running";
            return false;
        }
        starting_[spec.id] = {spec.name, util::nowUnix()};
        failed_.erase(spec.id);
    }

    log_.info("start", "▶ START \"" + spec.name + "\" [" + kindToString(spec.kind) +
                           "] peers=" + std::to_string(spec.peers.size()) +
                           " routes=" + std::to_string(spec.routes.size()) +
                           " killSwitch=" + (spec.killSwitch ? "true" : "false"));

    const bool ok = kindIsWireGuardFamily(spec.kind) ? startWireGuardFamily(spec, error)
                                                     : startOpenVpn(spec, error);
    std::lock_guard<std::mutex> lock(mutex_);
    starting_.erase(spec.id);
    if (!ok) {
        TunnelRuntimeState st;
        st.id = spec.id;
        st.name = spec.name;
        st.phase = TunnelPhase::Failed;
        st.errorMessage = error ? *error : "start failed";
        failed_[spec.id] = st;
        log_.error("start", "✕ FAIL \"" + spec.name + "\": " + st.errorMessage);
    }
    rebuildKillSwitchLocked();
    persistOwnershipLocked();
    return ok;
}

bool TunnelSupervisor::startWireGuardFamily(const ResolvedTunnelSpec& spec, std::string* error) {
    const auto coreName = kindCoreBinary(spec.kind);
    const auto corePath = paths::coreBinary(coreName);
    if (!std::filesystem::exists(std::filesystem::path(str::widen(corePath)))) {
        if (error) *error = "core binary not found: " + coreName;
        return false;
    }

    auto tunnel = std::make_unique<RunningTunnel>();
    tunnel->spec = spec;
    tunnel->since = util::nowUnix();
    // Adapter name is chosen by us and passed to the core, so we always know what to look for.
    tunnel->adapter = "TunHub-" + spec.id.substr(0, 8);
    tunnel->pipe = uapi::pipeName(tunnel->adapter, spec.kind);
    tunnel->resolvedEndpoints = resolveEndpoints(spec, tunnel->endpointHosts);

    const auto mode = log_settings::read();
    std::map<std::string, std::string> env{
        {"LOG_LEVEL", modeCoreLogLevel(mode)},
        {kOwnerEnvKey, spec.id},
    };

    tunnel->process = std::make_unique<ChildProcess>();
    const std::string tunnelName = spec.name;
    if (!tunnel->process->start(corePath, {tunnel->adapter}, env,
                                [this, tunnelName](const std::string& line) {
                                    log_.debug("core:" + tunnelName, line);
                                },
                                error)) {
        return false;
    }

    if (!uapi::waitForPipe(tunnel->pipe, 8000)) {
        if (error) *error = "the core did not create its UAPI pipe (timeout)";
        tunnel->process->terminate(1000);
        return false;
    }

    std::string renderError;
    const auto config = uapi::renderSetRequest(spec, tunnel->resolvedEndpoints, &renderError);
    if (config.empty()) {
        if (error) *error = renderError;
        tunnel->process->terminate(1000);
        return false;
    }
    if (!uapi::set(tunnel->pipe, config, error)) {
        tunnel->process->terminate(1000);
        return false;
    }
    log_.debug("start", "UAPI applied (" +
                            std::to_string(str::split(config, '\n').size()) + " lines, awg=" +
                            ((spec.awg && !spec.awg->empty()) ? "true" : "false") + ")");

    if (!net_.configureInterface(tunnel->adapter, spec.addresses, spec.mtu, error)) {
        tunnel->process->terminate(1000);
        return false;
    }
    if (!net_.applyRoutes(spec.id, tunnel->adapter, spec.routes, tunnel->endpointHosts, error)) {
        tunnel->process->terminate(1000);
        return false;
    }
    net_.applyDns(spec.id, tunnel->adapter, spec.dnsServers, spec.dnsSearchDomains,
                  spec.dnsMode, nullptr);

    tunnel->phase = TunnelPhase::Up;
    log_.info("start", "✔ UP \"" + spec.name + "\" on " + tunnel->adapter);

    std::lock_guard<std::mutex> lock(mutex_);
    running_[spec.id] = std::move(tunnel);
    return true;
}

bool TunnelSupervisor::startOpenVpn(const ResolvedTunnelSpec& spec, std::string* error) {
    auto session = std::make_unique<OpenVpnSession>(log_, spec);
    if (!session->start(error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    openvpn_[spec.id] = std::move(session);
    return true;
}

// ── stop ─────────────────────────────────────────────────────────────────────

void TunnelSupervisor::teardown(RunningTunnel& t) {
    net_.rollbackDns(t.spec.id);
    net_.rollbackRoutes(t.spec.id);
    if (t.process) t.process->terminate(3000);
}

void TunnelSupervisor::stop(const std::string& id) {
    std::unique_ptr<RunningTunnel> tunnel;
    std::unique_ptr<OpenVpnSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_.erase(id);
        if (auto it = running_.find(id); it != running_.end()) {
            it->second->intentionalStop = true;
            tunnel = std::move(it->second);
            running_.erase(it);
        }
        if (auto it = openvpn_.find(id); it != openvpn_.end()) {
            session = std::move(it->second);
            openvpn_.erase(it);
        }
    }
    if (tunnel) {
        log_.info("stop", "■ STOP \"" + tunnel->spec.name + "\"");
        teardown(*tunnel);
        log_.info("stop", "✔ STOPPED \"" + tunnel->spec.name + "\"");
    }
    if (session) {
        session->stop();
        log_.info("stop", "✔ STOPPED OpenVPN tunnel");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    rebuildKillSwitchLocked();
    persistOwnershipLocked();
}

void TunnelSupervisor::stopAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, _] : running_) ids.push_back(id);
        for (const auto& [id, _] : openvpn_) ids.push_back(id);
    }
    for (const auto& id : ids) stop(id);
    net_.disableKillSwitch();
}

// ── state ────────────────────────────────────────────────────────────────────

std::vector<TunnelRuntimeState> TunnelSupervisor::states() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TunnelRuntimeState> out;

    for (const auto& [id, t] : running_) {
        TunnelRuntimeState s;
        s.id = id;
        s.name = t->spec.name;
        s.phase = t->phase;
        s.interfaceName = t->adapter;
        s.errorMessage = t->lastError;
        s.peers = t->peers;
        s.since = t->since;
        for (const auto& r : t->spec.routes) s.routes.push_back(r.canonical());
        out.push_back(std::move(s));
    }
    for (const auto& [id, session] : openvpn_) out.push_back(session->snapshot());
    for (const auto& [id, info] : starting_) {
        if (running_.count(id) || openvpn_.count(id)) continue;
        TunnelRuntimeState s;
        s.id = id;
        s.name = info.first;
        s.phase = TunnelPhase::Starting;
        s.since = info.second;
        out.push_back(std::move(s));
    }
    for (const auto& [id, s] : failed_) out.push_back(s);
    return out;
}

void TunnelSupervisor::setKillSwitchEnabled(bool enabled) {
    killSwitchEnabled_ = enabled;
    std::lock_guard<std::mutex> lock(mutex_);
    rebuildKillSwitchLocked();
}

void TunnelSupervisor::rebuildKillSwitchLocked() {
    if (!killSwitchEnabled_) {
        net_.disableKillSwitch();
        return;
    }
    std::vector<NetConfig::ActiveTunnel> active;
    for (const auto& [id, t] : running_)
        if (t->spec.killSwitch) active.push_back({t->adapter, t->endpointHosts});
    net_.rebuildKillSwitch(active);
}

void TunnelSupervisor::persistOwnershipLocked() {
    Json owned = Json::array();
    for (const auto& [id, t] : running_) {
        Json e = Json::object();
        e.set("tunnelId", Json(id));
        e.set("name", Json(t->spec.name));
        e.set("adapter", Json(t->adapter));
        e.set("pid", Json(static_cast<long long>(t->process ? t->process->pid() : 0)));
        e.set("core", Json(kindCoreBinary(t->spec.kind)));
        owned.push(e);
    }
    Json root = Json::object();
    root.set("owned", owned);
    std::ofstream f(std::filesystem::path(str::widen(paths::ownershipFile())),
                    std::ios::binary | std::ios::trunc);
    if (f) {
        const auto text = root.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

// ── stats ────────────────────────────────────────────────────────────────────

void TunnelSupervisor::startStatsLoop() {
    statsThread_ = std::thread([this] {
        while (!stopping_) {
            pollStats();
            for (int i = 0; i < 10 && !stopping_; ++i) Sleep(100);   // ~1 s, responsive to stop
        }
    });
}

void TunnelSupervisor::pollStats() {
    std::vector<std::string> finishedOpenVpn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, session] : openvpn_)
            if (session->finished()) finishedOpenVpn.push_back(id);
    }
    for (const auto& id : finishedOpenVpn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = openvpn_.find(id); it != openvpn_.end()) {
            failed_[id] = it->second->snapshot();
            openvpn_.erase(it);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = util::nowUnix();

    for (auto& [id, t] : running_) {
        if (t->phase != TunnelPhase::Up && t->phase != TunnelPhase::Degraded) continue;

        // The core dying leaves the adapter and its routes behind — notice it promptly.
        if (t->process && !t->process->running()) {
            t->phase = TunnelPhase::Failed;
            t->lastError = "the tunnel core exited unexpectedly";
            log_.error("crash", "core for \"" + t->spec.name + "\" exited");
            continue;
        }

        // Loop monitor: only default-route tunnels can capture their own endpoint, and the
        // check costs route lookups, so run it sparingly.
        if (t->isDefaultRoute() && now - t->lastLoopCheck > 15) {
            t->lastLoopCheck = now;
            if (net_.ensureEndpointNotLooped(id, t->endpointHosts, t->adapter))
                log_.warn("stats", "\"" + t->spec.name + "\": endpoint loop detected and fixed");
        }

        auto peers = uapi::get(t->pipe);
        if (peers.empty()) continue;
        t->peers = peers;

        uint64_t rx = 0, tx = 0;
        int64_t lastHandshake = 0;
        for (const auto& p : peers) {
            rx += p.rxBytes;
            tx += p.txBytes;
            lastHandshake = std::max(lastHandshake, p.lastHandshake);
        }
        const bool fresh = lastHandshake > 0 && (now - lastHandshake) < 185;
        const bool sending = tx > t->lastTx;
        const bool receiving = rx > t->lastRx;

        // Health: a tunnel is healthy when it's fresh, receiving, or simply idle. It is
        // degraded only when it keeps SENDING with no replies — the real black-hole signal.
        TunnelPhase newPhase;
        if (fresh || receiving) {
            t->stalledSince = 0;
            newPhase = TunnelPhase::Up;
        } else if (sending && !receiving) {
            if (t->stalledSince == 0) t->stalledSince = now;
            newPhase = (now - t->stalledSince > 12) ? TunnelPhase::Degraded : TunnelPhase::Up;
        } else {
            t->stalledSince = 0;
            newPhase = TunnelPhase::Up;
        }

        if (newPhase != t->phase && newPhase == TunnelPhase::Degraded) {
            log_.error("diag", "\"" + t->spec.name + "\": sending but no replies for >12s — "
                               "server unreachable via this path, or AWG parameters "
                               "(Jc/S1/S2/H1–H4) do not match the server");
        }
        t->lastRx = rx;
        t->lastTx = tx;
        t->phase = newPhase;
    }
}

// ── recovery ─────────────────────────────────────────────────────────────────

void TunnelSupervisor::crashRecovery() {
    const auto dir = paths::executableDir();
    const std::vector<std::string> names{core_binary::kWireGuard, core_binary::kAmneziaWg,
                                         core_binary::kOpenVpn};
    // Matched by executable path inside OUR install directory, so another product's
    // WireGuard/OpenVPN is never touched. Killing a core also destroys its adapter, which
    // removes the stale routes it left behind — including a default route that would
    // otherwise hijack the next tunnel's endpoint.
    const auto orphans = findProcessesInDirectory(dir, names);
    for (auto pid : orphans) {
        log_.warn("recover", "reaping orphaned core pid=" + std::to_string(pid) +
                                 " from a previous run");
        killProcess(pid);
    }
    if (!orphans.empty())
        log_.info("recover", "reaped " + std::to_string(orphans.size()) + " orphaned core(s)");

    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(str::widen(paths::ownershipFile())), ec);
    net_.crashRecovery();
}

}  // namespace tunhub
