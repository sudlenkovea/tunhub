// TunHub for Windows — entry point and the actions the windows share.

#include <shellapi.h>

#include "tunhub/conflicts.h"
#include "tunhub/paths.h"
#include "tunhub/str.h"
#include "ui.h"

namespace tunhub::app {

void startTunnel(AppContext& ctx, const std::string& id) {
    const auto* config = ctx.findTunnel(id);
    if (!config) return;

    if (!ctx.helperReachable) {
        if (!installHelper(ctx)) return;
    }

    // Refuse a start that would fight with what is already running, and explain why.
    std::vector<TunnelConfig> active;
    for (const auto& [runningId, state] : ctx.runtime) {
        if (state.phase != TunnelPhase::Up && state.phase != TunnelPhase::Degraded) continue;
        if (const auto* t = ctx.findTunnel(runningId)) active.push_back(*t);
    }
    const auto findings = conflicts::check(*config, active);
    if (conflicts::hasErrors(findings)) {
        std::string text;
        for (const auto& f : findings) {
            if (f.severity != FindingSeverity::Error) continue;
            text += f.message + "\r\n";
            if (!f.fixHint.empty()) text += "    → " + f.fixHint + "\r\n";
        }
        messageBox(ctx.mainWindow, "Conflicts", str::widen(text), MB_OK | MB_ICONWARNING);
        return;
    }

    std::string otp;
    if (config->kind == TunnelKind::OpenVpn && config->openVpn) {
        auto secrets = ctx.store.loadSecrets(id).value_or(TunnelSecrets{});
        const bool needsPrompt = config->openVpn->authUserPass &&
                                 (secrets.openVpnUsername.empty() ||
                                  secrets.openVpnPassword.empty() ||
                                  config->openVpn->staticChallenge);
        if (needsPrompt) {
            bool save = !secrets.openVpnUsername.empty();
            if (!promptCredentials(ctx, *config, secrets.openVpnUsername,
                                   secrets.openVpnPassword, otp, save))
                return;
            if (save) ctx.store.saveSecrets(id, secrets);
        }
    }

    std::string error;
    auto spec = ctx.store.resolve(*config, otp, &error);
    if (!spec) {
        messageBox(ctx.mainWindow, "Error", str::widen(error), MB_OK | MB_ICONERROR);
        return;
    }

    ipc::Response response;
    if (!ctx.daemon.call(ipc::method::kStartTunnel, spec->toJson(), &response, 20000)) {
        messageBox(ctx.mainWindow, "Error",
                   loc::w("TunHub needs a background service to manage tunnels."),
                   MB_OK | MB_ICONERROR);
        return;
    }
    if (!response.ok)
        messageBox(ctx.mainWindow, "Connection failed", str::widen(response.error),
                   MB_OK | MB_ICONERROR);
}

void stopTunnel(AppContext& ctx, const std::string& id) {
    Json payload = Json::object();
    payload.set("id", Json(id));
    ipc::Response response;
    ctx.daemon.call(ipc::method::kStopTunnel, payload, &response, 15000);
}

void stopAllTunnels(AppContext& ctx) {
    ipc::Response response;
    ctx.daemon.call(ipc::method::kStopAll, Json(), &response, 20000);
}

bool installHelper(AppContext& ctx) {
    // The service must be registered by an administrator; ShellExecute with "runas" raises
    // the standard UAC prompt instead of a console window flashing past.
    const auto helper = str::widen(paths::coreBinary("tunhub-helper.exe"));

    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = ctx.mainWindow;
    info.lpVerb = L"runas";
    info.lpFile = helper.c_str();
    info.lpParameters = L"--install";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return false;   // cancelled at the UAC prompt

    if (info.hProcess) {
        WaitForSingleObject(info.hProcess, 30000);
        CloseHandle(info.hProcess);
    }

    // Give the service a moment to come up and answer.
    for (int i = 0; i < 20; ++i) {
        if (ctx.daemon.ping()) {
            ctx.helperReachable = true;
            return true;
        }
        Sleep(500);
    }
    messageBox(ctx.mainWindow, "Install system component",
               loc::w("The component was installed but isn't responding yet."),
               MB_OK | MB_ICONWARNING);
    return false;
}

}  // namespace tunhub::app

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    using namespace tunhub;
    using namespace tunhub::app;

    // A second instance would fight over the tray icon and the tunnel list.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Global\\TunHubAppSingleton");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"TunHubMainWindow", nullptr)) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    paths::ensureDirectories();

    AppContext ctx;
    ctx.instance = instance;
    ctx.settings.load();
    loc::setLanguage(ctx.settings.language);

    if (!createMainWindow(ctx)) return 1;
    ShowWindow(ctx.mainWindow, SW_SHOW);

    // Auto-connect happens once the service has been reached, which the poll timer handles.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (single) CloseHandle(single);
    return static_cast<int>(msg.wParam);
}
