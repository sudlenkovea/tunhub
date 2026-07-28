// TunHub privileged helper: a LocalSystem Windows service hosting the tunnel engine.
//
//   tunhub-helper.exe            run as a service (invoked by the SCM)
//   tunhub-helper.exe --console  run in the foreground (development)
//   tunhub-helper.exe --install  register the service
//   tunhub-helper.exe --uninstall

#include <winsock2.h>
#include <windows.h>

#include <cstdio>    // wprintf
#include <memory>
#include <string>

#include "tunhub/constants.h"
#include "tunhub/engine_host.h"
#include "tunhub/log.h"
#include "tunhub/paths.h"
#include "tunhub/str.h"
#include "tunhub/util.h"

using namespace tunhub;

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
std::unique_ptr<FileLog> g_log;
std::unique_ptr<EngineHost> g_engine;

void reportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;
    static DWORD checkPoint = 1;
    g_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

void WINAPI serviceCtrlHandler(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_stopEvent) SetEvent(g_stopEvent);
    }
}

/// Shared startup for both the service and --console.
bool startEngine() {
    util::initSockets();   // endpoint resolution and the OpenVPN management socket need it
    paths::ensureDirectories();
    const auto mode = log_settings::read();
    g_log = std::make_unique<FileLog>(paths::helperLogFile());
    g_log->setMinLevel(modeMinLevel(mode));

    g_engine = std::make_unique<EngineHost>(*g_log);
    std::string error;
    if (!g_engine->run(&error)) {
        g_log->error("helper", "failed to start: " + error);
        return false;
    }
    return true;
}

void stopEngine() {
    if (g_engine) {
        g_engine->shutdown();
        g_engine.reset();
    }
    g_log.reset();
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerW(ipc::kServiceName, serviceCtrlHandler);
    if (!g_statusHandle) return;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    reportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent || !startEngine()) {
        reportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    reportStatus(SERVICE_RUNNING);
    WaitForSingleObject(g_stopEvent, INFINITE);

    stopEngine();
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    reportStatus(SERVICE_STOPPED);
}

int installService() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return 1;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        wprintf(L"OpenSCManager failed (%lu) — run elevated\n", GetLastError());
        return 1;
    }
    const std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
    SC_HANDLE svc = CreateServiceW(
        scm, ipc::kServiceName, L"TunHub Helper", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        quoted.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            wprintf(L"service already installed\n");
            CloseServiceHandle(scm);
            return 0;
        }
        wprintf(L"CreateService failed (%lu)\n", err);
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_DESCRIPTIONW desc{
        const_cast<LPWSTR>(L"Privileged tunnel manager for TunHub (WireGuard / AmneziaWG / OpenVPN).")};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Restart automatically if the service ever dies, so tunnels can be recovered.
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000},
                            {SC_ACTION_RESTART, 10000},
                            {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    StartServiceW(svc, 0, nullptr);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    wprintf(L"service installed and started\n");
    return 0;
}

int uninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 1;
    SC_HANDLE svc = OpenServiceW(scm, ipc::kServiceName, SERVICE_STOP | DELETE);
    if (!svc) {
        CloseServiceHandle(scm);
        return 0;   // already gone
    }
    SERVICE_STATUS status{};
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    wprintf(L"service removed\n");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring arg = argc > 1 ? argv[1] : L"";

    if (arg == L"--install")   return installService();
    if (arg == L"--uninstall") return uninstallService();

    if (arg == L"--console") {
        if (!startEngine()) return 1;
        wprintf(L"TunHub helper running in the console. Press Ctrl+C to stop.\n");
        // Ctrl+C ends the process; the engine's destructor tears the tunnels down.
        while (true) Sleep(1000);
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(ipc::kServiceName), serviceMain},
        {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        // Launched from a console without --console: explain rather than silently failing.
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            wprintf(L"This is a Windows service. Use --console to run it in the foreground, "
                    L"or --install to register it.\n");
        return 1;
    }
    return 0;
}
