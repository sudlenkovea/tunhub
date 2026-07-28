#include "tunhub/engine_host.h"

#include <windows.h>

#include "tunhub/constants.h"
#include "tunhub/paths.h"

namespace tunhub {

EngineHost::EngineHost(FileLog& log) : log_(log), supervisor_(log) {}

EngineHost::~EngineHost() { shutdown(); }

bool EngineHost::run(std::string* error) {
    paths::ensureDirectories();
    log_.info("helper", std::string("═══ TunHub helper ") + kProtocolVersion +
                            " started (pid=" + std::to_string(GetCurrentProcessId()) + ") ═══");

    // Before anything is spawned, so every match is by definition from a previous run.
    supervisor_.crashRecovery();
    supervisor_.startStatsLoop();

    server_ = std::make_unique<ipc::Server>([this](const ipc::Request& r) { return handle(r); });
    if (!server_->start(error)) return false;
    log_.info("helper", "IPC listening");
    return true;
}

void EngineHost::shutdown() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
    supervisor_.stopAll();
}

ipc::Response EngineHost::handle(const ipc::Request& request) {
    const auto& m = request.method;

    if (m == ipc::method::kVersion) return ipc::Response::success(Json(kProtocolVersion));

    if (m == ipc::method::kStartTunnel) {
        const auto spec = ResolvedTunnelSpec::fromJson(request.payload);
        if (spec.id.empty()) return ipc::Response::failure("missing tunnel spec");
        std::string error;
        if (!supervisor_.start(spec, &error)) return ipc::Response::failure(error);
        return ipc::Response::success();
    }

    if (m == ipc::method::kStopTunnel) {
        const auto id = request.payload["id"].asString("");
        if (id.empty()) return ipc::Response::failure("missing tunnel id");
        supervisor_.stop(id);
        return ipc::Response::success();
    }

    if (m == ipc::method::kStopAll) {
        supervisor_.stopAll();
        return ipc::Response::success();
    }

    if (m == ipc::method::kRuntimeStates) {
        Json arr = Json::array();
        for (const auto& s : supervisor_.states()) arr.push(s.toJson());
        return ipc::Response::success(arr);
    }

    if (m == ipc::method::kSetKillSwitch) {
        supervisor_.setKillSwitchEnabled(request.payload["enabled"].asBool(true));
        return ipc::Response::success();
    }

    if (m == ipc::method::kRecentLog) {
        const int maxLines = request.payload["maxLines"].asInt(500);
        Json arr = Json::array();
        for (const auto& line : log_.tail(static_cast<size_t>(maxLines > 0 ? maxLines : 500)))
            arr.push(line.toJson());
        return ipc::Response::success(arr);
    }

    if (m == ipc::method::kSetLogMode) {
        const auto mode = modeFromString(request.payload["mode"].asString("normal"));
        if (!log_settings::write(mode))
            return ipc::Response::failure("could not persist the log mode");
        log_.info("helper", "log capture mode set to " + modeToString(mode) +
                                " (applies on service restart)");
        return ipc::Response::success();
    }

    return ipc::Response::failure("unknown method: " + m);
}

}  // namespace tunhub
