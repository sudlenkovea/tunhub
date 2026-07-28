// Editor, settings, log viewer, import, conflicts and the credential prompt.
//
// Dialogs are built in code rather than from .rc templates: the layouts are simple, and this
// keeps the strings in one place for localisation instead of splitting them across resources.

#include <windows.h>    // must come before any other Windows header
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

#include "tunhub/conflicts.h"
#include "tunhub/log.h"      // LogLine, LogCaptureMode, log_settings
#include "tunhub/ovpn.h"
#include "tunhub/paths.h"
#include "tunhub/str.h"
#include "tunhub/util.h"
#include "tunhub/wgkey.h"
#include "tunhub/wgquick.h"
#include "ui.h"

#pragma comment(lib, "comdlg32.lib")

namespace tunhub::app {
namespace {

// All coordinates below are design-time (96 dpi) units. Every control-creation helper scales
// them through dpiScale, so one change covers the whole dialog layout — passing raw pixels
// produced cramped, overlapping dialogs on scaled displays.
constexpr int kLabelWidth = 150;
constexpr int kRowHeight = 30;
constexpr int kEditHeight = 26;

HWND label(HWND parent, const std::wstring& text, int x, int y, int w = kLabelWidth) {
    HWND h = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                             dpiScale(parent, x), dpiScale(parent, y + 5),
                             dpiScale(parent, w), dpiScale(parent, 20),
                             parent, nullptr, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return h;
}

HWND edit(HWND parent, const std::wstring& text, int x, int y, int w, int h = kEditHeight,
          DWORD extra = 0) {
    HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                             dpiScale(parent, x), dpiScale(parent, y),
                             dpiScale(parent, w), dpiScale(parent, h),
                             parent, nullptr, nullptr, nullptr);
    SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return e;
}

HWND checkbox(HWND parent, const std::wstring& text, int x, int y, int w, bool checked) {
    HWND c = CreateWindowExW(0, L"BUTTON", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                             dpiScale(parent, x), dpiScale(parent, y),
                             dpiScale(parent, w), dpiScale(parent, 24),
                             parent, nullptr, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}

HWND button(HWND parent, const std::wstring& text, int id, int x, int y, int w, int h = 30) {
    HWND b = CreateWindowExW(0, L"BUTTON", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             dpiScale(parent, x), dpiScale(parent, y),
                             dpiScale(parent, w), dpiScale(parent, h),
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return b;
}

HWND comboBox(HWND parent, int x, int y, int w) {
    HWND c = CreateWindowExW(0, L"COMBOBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                             dpiScale(parent, x), dpiScale(parent, y),
                             dpiScale(parent, w), dpiScale(parent, 200),
                             parent, nullptr, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return c;
}

std::wstring textOf(HWND control) {
    const int n = GetWindowTextLengthW(control);
    std::wstring out(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(control, out.data(), n + 1);
    out.resize(static_cast<size_t>(n));
    return out;
}

std::string utf8Of(HWND control) { return str::narrow(textOf(control)); }

bool isChecked(HWND control) {
    return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/// Modal window helper: runs a nested message loop until `done` is set.
void runModal(HWND dialog, bool& done) {
    EnableWindow(GetWindow(dialog, GW_OWNER), FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG msg;
    while (!done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    HWND owner = GetWindow(dialog, GW_OWNER);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DestroyWindow(dialog);
}

/// Each dialog gets its own window class, because the class carries the window procedure —
/// registering one shared class would route every dialog to whichever proc registered first.
HWND makeDialogWindow(AppContext& ctx, const std::wstring& className, const std::wstring& title,
                      int width, int height, WNDPROC proc) {
    static std::vector<std::wstring> registered;
    if (std::find(registered.begin(), registered.end(), className) == registered.end()) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = ctx.instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = className.c_str();
        if (!RegisterClassExW(&wc)) return nullptr;
        registered.push_back(className);
    }
    // Size is given in design units and scaled here, and the frame is added on top so the
    // client area really is as large as the layout expects.
    RECT rc{0, 0, dpiScale(ctx.mainWindow, width), dpiScale(ctx.mainWindow, height)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);

    // WS_EX_CONTROLPARENT lets IsDialogMessage walk into the child controls, which is what
    // makes Tab navigation and the default button work.
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                  className.c_str(), title.c_str(),
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rc.right - rc.left, rc.bottom - rc.top,
                                  ctx.mainWindow, nullptr, ctx.instance, nullptr);
    if (!dialog) return nullptr;

    // Centre on the owner rather than letting the shell cascade it off-screen.
    RECT owner{};
    if (GetWindowRect(ctx.mainWindow, &owner)) {
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        SetWindowPos(dialog, nullptr,
                     owner.left + ((owner.right - owner.left) - w) / 2,
                     owner.top + ((owner.bottom - owner.top) - h) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return dialog;
}

}  // namespace

// ── log viewer ───────────────────────────────────────────────────────────────

namespace {

struct LogWindowState {
    AppContext* ctx = nullptr;
    HWND text = nullptr;
    HWND pause = nullptr;
    bool paused = false;
    bool done = false;
    std::string lastSignature;
};

LogWindowState g_log;

constexpr UINT_PTR kLogTimer = 10;
constexpr int IDC_LOG_PAUSE = 1;
constexpr int IDC_LOG_COPY = 2;
constexpr int IDC_LOG_CLOSE = 3;

/// Pull the service log and render it. Only the last 1500 lines are kept: the file itself is
/// capped at 5 MB and an edit control gains nothing from more.
void refreshLog(HWND hwnd) {
    if (g_log.paused) return;

    Json payload = Json::object();
    payload.set("maxLines", Json(1500));
    ipc::Response response;
    std::string text;
    if (g_log.ctx->daemon.call(ipc::method::kRecentLog, payload, &response, 3000) && response.ok) {
        for (const auto& item : response.payload.items())
            text += LogLine::fromJson(item).formatted() + "\r\n";
    } else {
        text = "(the system component is not reachable)";
    }

    // Skip the rebuild when nothing changed — the common case for an idle tunnel.
    const auto signature = std::to_string(text.size());
    if (signature == g_log.lastSignature) return;
    g_log.lastSignature = signature;

    SetWindowTextW(g_log.text, str::widen(text).c_str());
    // Scroll to the newest line.
    SendMessageW(g_log.text, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(g_log.text, EM_SCROLLCARET, 0, 0);
    (void)hwnd;
}

/// The text area fills everything under the button row.
void layoutLogText(HWND dialog, const RECT& client) {
    const int pad = dpiScale(dialog, 10);
    const int barHeight = dpiScale(dialog, 30) + pad * 2;
    MoveWindow(g_log.text, pad, barHeight, std::max<int>(client.right - pad * 2, 0),
               std::max<int>(client.bottom - barHeight - pad, 0), TRUE);
}

LRESULT CALLBACK logProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            layoutLogText(hwnd, rc);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_LOG_PAUSE:
                    g_log.paused = !g_log.paused;
                    SetWindowTextW(g_log.pause, g_log.paused ? L"▶" : loc::w("Pause").c_str());
                    return 0;
                case IDC_LOG_COPY:
                    SendMessageW(g_log.text, EM_SETSEL, 0, -1);
                    SendMessageW(g_log.text, WM_COPY, 0, 0);
                    return 0;
                case IDC_LOG_CLOSE:
                    g_log.done = true;
                    return 0;
                default: break;
            }
            return 0;
        case WM_TIMER:
            if (wParam == kLogTimer) refreshLog(hwnd);
            return 0;
        case WM_CLOSE:
            g_log.done = true;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void showLogWindow(AppContext& ctx) {
    g_log = LogWindowState{};
    g_log.ctx = &ctx;

    HWND hwnd = makeDialogWindow(ctx, L"TunHubLogWindow", loc::w("Logs"), 1000, 640, logProc);
    if (!hwnd) return;

    g_log.pause = button(hwnd, loc::w("Pause"), IDC_LOG_PAUSE, 10, 10, 110);
    button(hwnd, loc::w("Copy all"), IDC_LOG_COPY, 128, 10, 130);
    button(hwnd, loc::w("Close"), IDC_LOG_CLOSE, 266, 10, 110);

    // A read-only multiline edit is the right control for a log: it handles large text,
    // selection and copy natively, and never re-lays-out per line the way a list would.
    g_log.text = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    HFONT mono = CreateFontW(-dpiScale(hwnd, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(g_log.text, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
    SendMessageW(g_log.text, EM_SETLIMITTEXT, 0, 0);   // no 32 KB cap

    RECT rc{};
    GetClientRect(hwnd, &rc);
    MoveWindow(g_log.text, 8, 44, rc.right - 16, rc.bottom - 52, TRUE);

    refreshLog(hwnd);
    SetTimer(hwnd, kLogTimer, 2000, nullptr);   // 2 s: live enough, half the work of 1 Hz
    runModal(hwnd, g_log.done);
    KillTimer(hwnd, kLogTimer);
    DeleteObject(mono);
}

// ── settings ─────────────────────────────────────────────────────────────────

namespace {

struct SettingsState {
    AppContext* ctx = nullptr;
    HWND language = nullptr;
    HWND launch = nullptr;
    HWND killSwitch = nullptr;
    HWND logMode = nullptr;
    bool done = false;
    bool save = false;
};

SettingsState g_settings;

constexpr int IDC_SET_OK = 1;
constexpr int IDC_SET_CANCEL = 2;

LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND) {
        if (LOWORD(wParam) == IDC_SET_OK) { g_settings.save = true; g_settings.done = true; }
        if (LOWORD(wParam) == IDC_SET_CANCEL) g_settings.done = true;
        return 0;
    }
    if (msg == WM_CLOSE) { g_settings.done = true; return 0; }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/// Register/unregister the Run key entry for launch-at-login.
void applyLaunchAtLogin(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, L"TunHub", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"TunHub");
    }
    RegCloseKey(key);
}

void restartApplication() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    ExitProcess(0);
}

}  // namespace

void showSettingsDialog(AppContext& ctx) {
    g_settings = SettingsState{};
    g_settings.ctx = &ctx;

    HWND hwnd = makeDialogWindow(ctx, L"TunHubSettings", loc::w("Settings"), 600, 430,
                                 settingsProc);
    if (!hwnd) return;

    int y = 18;
    label(hwnd, loc::w("Interface language"), 18, y);
    g_settings.language = comboBox(hwnd, 18 + kLabelWidth, y, 220);
    for (const auto* item : {L"System", L"English", L"Русский"})
        SendMessageW(g_settings.language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    const int languageIndex = ctx.settings.language == "en" ? 1
                            : ctx.settings.language == "ru" ? 2 : 0;
    SendMessageW(g_settings.language, CB_SETCURSEL, static_cast<WPARAM>(languageIndex), 0);
    y += kRowHeight + 10;

    g_settings.launch = checkbox(hwnd, loc::w("Launch TunHub at login"), 18, y, 460,
                                 ctx.settings.launchAtLogin);
    y += kRowHeight;
    g_settings.killSwitch = checkbox(hwnd, loc::w("Kill switch (global)"), 18, y, 460,
                                     ctx.settings.killSwitchGlobal);
    y += kRowHeight + 14;

    label(hwnd, loc::w("Log capture"), 18, y);
    g_settings.logMode = comboBox(hwnd, 18 + kLabelWidth, y, 220);
    SendMessageW(g_settings.logMode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(loc::w("Normal").c_str()));
    SendMessageW(g_settings.logMode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(loc::w("Verbose (debug)").c_str()));
    const auto currentMode = log_settings::read();
    SendMessageW(g_settings.logMode, CB_SETCURSEL,
                 currentMode == LogCaptureMode::Verbose ? 1 : 0, 0);
    y += kRowHeight + 6;

    HWND hint = CreateWindowExW(
        0, L"STATIC",
        loc::w("Verbose records every command and the tunnel core's debug output. Use it for "
               "troubleshooting only — it produces a lot of data and uses noticeably more CPU. "
               "Logs are kept in a single file, trimmed to the last 5 MB.").c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT, dpiScale(hwnd, 18), dpiScale(hwnd, y),
        dpiScale(hwnd, 560), dpiScale(hwnd, 90), hwnd, nullptr, nullptr, nullptr);
    SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont()), TRUE);

    button(hwnd, loc::w("OK"), IDC_SET_OK, 360, 348, 110);
    button(hwnd, loc::w("Cancel"), IDC_SET_CANCEL, 478, 348, 110);

    runModal(hwnd, g_settings.done);
    if (!g_settings.save) return;

    const auto languageChoice = SendMessageW(g_settings.language, CB_GETCURSEL, 0, 0);
    ctx.settings.language = languageChoice == 1 ? "en" : languageChoice == 2 ? "ru" : "system";
    ctx.settings.launchAtLogin = isChecked(g_settings.launch);
    ctx.settings.killSwitchGlobal = isChecked(g_settings.killSwitch);
    ctx.settings.save();
    loc::setLanguage(ctx.settings.language);
    applyLaunchAtLogin(ctx.settings.launchAtLogin);

    Json payload = Json::object();
    payload.set("enabled", Json(ctx.settings.killSwitchGlobal));
    ipc::Response ignored;
    ctx.daemon.call(ipc::method::kSetKillSwitch, payload, &ignored);

    // Capture mode only takes effect on a fresh start, so the log never mixes two levels.
    const auto chosen = SendMessageW(g_settings.logMode, CB_GETCURSEL, 0, 0) == 1
                            ? LogCaptureMode::Verbose
                            : LogCaptureMode::Normal;
    if (chosen != currentMode) {
        Json modePayload = Json::object();
        modePayload.set("mode", Json(modeToString(chosen)));
        ctx.daemon.call(ipc::method::kSetLogMode, modePayload, &ignored);
        if (MessageBoxW(ctx.mainWindow,
                        loc::w("The new log capture mode starts collecting after a restart. "
                               "Restart TunHub now?").c_str(),
                        loc::w("Restart required").c_str(),
                        MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            restartApplication();
        }
    }
}

// ── conflicts ────────────────────────────────────────────────────────────────

void showConflictsDialog(AppContext& ctx) {
    const auto findings = conflicts::checkAll(ctx.tunnels);
    if (findings.empty()) {
        messageBox(ctx.mainWindow, "Conflicts", loc::w("No conflicts found"));
        return;
    }
    std::string text;
    for (const auto& f : findings) {
        text += "[" + loc::t(conflicts::severityLabel(f.severity)) + "] " + f.message + "\r\n";
        if (!f.fixHint.empty()) text += "    → " + f.fixHint + "\r\n";
        text += "\r\n";
    }
    messageBox(ctx.mainWindow, "Conflicts", str::widen(text));
}

// ── import ───────────────────────────────────────────────────────────────────

void runImport(AppContext& ctx) {
    wchar_t files[8192]{};
    // Double-NUL terminated filter list; the pairs are description / pattern.
    std::wstring filter = loc::w("WireGuard / AmneziaWG configs (*.conf)") + L'\0' + L"*.conf" +
                          L'\0' + loc::w("OpenVPN profiles (*.ovpn)") + L'\0' + L"*.ovpn" + L'\0' +
                          loc::w("All files (*.*)") + L'\0' + L"*.*" + L'\0' + L'\0';

    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = ctx.mainWindow;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = files;
    ofn.nMaxFile = 8192;
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    // With multiselect the buffer is "dir\0name1\0name2\0\0"; a single file is just the path.
    std::vector<std::wstring> paths;
    const std::wstring first(files);
    if (files[first.size() + 1] == L'\0') {
        paths.push_back(first);
    } else {
        const std::wstring dir = first;
        for (const wchar_t* p = files + dir.size() + 1; *p; p += wcslen(p) + 1)
            paths.push_back(dir + L"\\" + p);
    }

    int imported = 0;
    std::string problems;
    for (const auto& path : paths) {
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        if (!f) continue;
        const std::string text((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        const auto stem = str::narrow(std::filesystem::path(path).stem().wstring());
        const auto extension = str::lower(str::narrow(std::filesystem::path(path).extension().wstring()));

        ParseError error;
        if (extension == ".ovpn") {
            auto parsed = ovpn::parse(text, &error);
            if (!parsed) {
                problems += stem + ": " + error.text() + "\r\n";
                continue;
            }
            TunnelConfig cfg;
            cfg.id = util::newGuid();
            cfg.name = stem;
            cfg.kind = TunnelKind::OpenVpn;
            cfg.meta.createdAt = util::nowUnix();
            cfg.openVpn = parsed->profile;
            cfg.options.killSwitch = parsed->profile.redirectGateway;
            std::string saveError;
            if (!ctx.store.saveTunnel(cfg, &saveError)) {
                problems += stem + ": " + saveError + "\r\n";
                continue;
            }
            ctx.store.saveSecrets(cfg.id, TunnelSecrets{});
            ++imported;
        } else {
            auto parsed = wgquick::parse(stem, text, &error);
            if (!parsed) {
                problems += stem + ": " + error.text() + "\r\n";
                continue;
            }
            std::string saveError;
            if (!ctx.store.saveTunnel(parsed->config, &saveError)) {
                problems += stem + ": " + saveError + "\r\n";
                continue;
            }
            TunnelSecrets secrets;
            secrets.privateKey = parsed->privateKey;
            secrets.presharedKeys = parsed->presharedKeys;
            ctx.store.saveSecrets(parsed->config.id, secrets);
            ++imported;
            for (const auto& w : parsed->warnings) problems += stem + ": " + w + "\r\n";
        }
    }

    ctx.reloadTunnels();
    std::wstring summary = loc::w("Imported") + L": " + std::to_wstring(imported);
    if (!problems.empty()) summary += L"\r\n\r\n" + str::widen(problems);
    messageBox(ctx.mainWindow, "Import configuration", summary);
}

// ── credential prompt ────────────────────────────────────────────────────────

namespace {

struct CredentialState {
    HWND username = nullptr;
    HWND password = nullptr;
    HWND otp = nullptr;
    HWND save = nullptr;
    bool done = false;
    bool accepted = false;
};

CredentialState g_creds;

constexpr int IDC_CRED_OK = 1;
constexpr int IDC_CRED_CANCEL = 2;

LRESULT CALLBACK credentialProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND) {
        if (LOWORD(wParam) == IDC_CRED_OK) { g_creds.accepted = true; g_creds.done = true; }
        if (LOWORD(wParam) == IDC_CRED_CANCEL) g_creds.done = true;
        return 0;
    }
    if (msg == WM_CLOSE) { g_creds.done = true; return 0; }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool promptCredentials(AppContext& ctx, const TunnelConfig& config, std::string& username,
                       std::string& password, std::string& otp, bool& save) {
    g_creds = CredentialState{};
    const bool needsOtp = config.openVpn && config.openVpn->staticChallenge;

    constexpr int kX = 18;
    const int fieldX = kX + kLabelWidth;

    HWND hwnd = makeDialogWindow(ctx, L"TunHubCredentials", loc::w("Sign in"), 520,
                                 needsOtp ? 300 : 250, credentialProc);
    if (!hwnd) return false;

    int y = 16;
    label(hwnd, str::widen(config.name), kX, y, 460);
    y += kRowHeight;

    label(hwnd, loc::w("Username"), kX, y);
    g_creds.username = edit(hwnd, str::widen(username), fieldX, y, 300);
    y += kRowHeight + 4;

    label(hwnd, loc::w("Password"), kX, y);
    g_creds.password = edit(hwnd, str::widen(password), fieldX, y, 300, kEditHeight, ES_PASSWORD);
    y += kRowHeight + 4;

    if (needsOtp) {
        const auto prompt = config.openVpn->staticChallengeText.empty()
                                ? loc::t("One-time code")
                                : config.openVpn->staticChallengeText;
        label(hwnd, str::widen(prompt), kX, y);
        g_creds.otp = edit(hwnd, L"", fieldX, y, 170);
        y += kRowHeight + 4;
    }

    g_creds.save = checkbox(hwnd, loc::w("Save login and password"), kX, y, 440, save);
    y += kRowHeight + 14;

    button(hwnd, loc::w("Connect"), IDC_CRED_OK, 262, y, 110);
    button(hwnd, loc::w("Cancel"), IDC_CRED_CANCEL, 380, y, 110);

    SetFocus(g_creds.username);
    runModal(hwnd, g_creds.done);
    if (!g_creds.accepted) return false;

    username = utf8Of(g_creds.username);
    password = utf8Of(g_creds.password);
    otp = g_creds.otp ? utf8Of(g_creds.otp) : "";
    save = isChecked(g_creds.save);
    return true;
}

// ── editor ───────────────────────────────────────────────────────────────────

namespace {

struct EditorState {
    AppContext* ctx = nullptr;
    TunnelConfig config;
    HWND name = nullptr;
    HWND addresses = nullptr;
    HWND dns = nullptr;
    HWND mtu = nullptr;
    HWND listenPort = nullptr;
    HWND privateKey = nullptr;
    HWND publicKey = nullptr;
    HWND peerPublicKey = nullptr;
    HWND endpoint = nullptr;
    HWND allowedIPs = nullptr;
    HWND keepalive = nullptr;
    HWND awg = nullptr;          // multiline "Key = value" block
    HWND killSwitch = nullptr;
    HWND autoConnect = nullptr;
    HWND ovpnUser = nullptr;
    HWND ovpnPassword = nullptr;
    bool done = false;
    bool save = false;
};

EditorState g_editor;

constexpr int IDC_ED_OK = 1;
constexpr int IDC_ED_CANCEL = 2;
constexpr int IDC_ED_GENKEY = 3;

LRESULT CALLBACK editorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
            case IDC_ED_OK:     g_editor.save = true; g_editor.done = true; return 0;
            case IDC_ED_CANCEL: g_editor.done = true; return 0;
            case IDC_ED_GENKEY: {
                if (auto kp = wgkey::generateKeyPair()) {
                    SetWindowTextW(g_editor.privateKey, str::widen(kp->privateKey).c_str());
                    SetWindowTextW(g_editor.publicKey, str::widen(kp->publicKey).c_str());
                }
                return 0;
            }
            default: break;
        }
        return 0;
    }
    if (msg == WM_CLOSE) { g_editor.done = true; return 0; }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/// AWG parameters are edited as text so every key (including I1–I5) is reachable without a
/// field per parameter.
std::string awgToText(const AwgParams& a) {
    std::string out;
    auto putInt = [&](const char* k, const std::optional<int>& v) {
        if (v) out += std::string(k) + " = " + std::to_string(*v) + "\r\n";
    };
    auto putU32 = [&](const char* k, const std::optional<uint32_t>& v) {
        if (v) out += std::string(k) + " = " + std::to_string(*v) + "\r\n";
    };
    auto putStr = [&](const char* k, const std::optional<std::string>& v) {
        if (v && !v->empty()) out += std::string(k) + " = " + *v + "\r\n";
    };
    putInt("Jc", a.jc); putInt("Jmin", a.jmin); putInt("Jmax", a.jmax);
    putInt("S1", a.s1); putInt("S2", a.s2); putInt("S3", a.s3); putInt("S4", a.s4);
    putU32("H1", a.h1); putU32("H2", a.h2); putU32("H3", a.h3); putU32("H4", a.h4);
    putStr("I1", a.i1); putStr("I2", a.i2); putStr("I3", a.i3);
    putStr("I4", a.i4); putStr("I5", a.i5);
    putInt("ITime", a.itime);
    return out;
}

AwgParams awgFromText(const std::string& text) {
    AwgParams a;
    std::string normalised = text;
    for (auto& c : normalised) if (c == '\r') c = '\n';
    for (const auto& line : str::split(normalised, '\n')) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = str::lower(str::trim(line.substr(0, eq)));
        const auto value = str::trim(line.substr(eq + 1));
        if (value.empty()) continue;
        auto toInt = [&]() -> std::optional<int> {
            try { return std::stoi(value); } catch (...) { return std::nullopt; }
        };
        auto toU32 = [&]() -> std::optional<uint32_t> {
            try { return static_cast<uint32_t>(std::stoul(value)); } catch (...) { return std::nullopt; }
        };
        if (key == "jc") a.jc = toInt();
        else if (key == "jmin") a.jmin = toInt();
        else if (key == "jmax") a.jmax = toInt();
        else if (key == "s1") a.s1 = toInt();
        else if (key == "s2") a.s2 = toInt();
        else if (key == "s3") a.s3 = toInt();
        else if (key == "s4") a.s4 = toInt();
        else if (key == "h1") a.h1 = toU32();
        else if (key == "h2") a.h2 = toU32();
        else if (key == "h3") a.h3 = toU32();
        else if (key == "h4") a.h4 = toU32();
        else if (key == "i1") a.i1 = value;
        else if (key == "i2") a.i2 = value;
        else if (key == "i3") a.i3 = value;
        else if (key == "i4") a.i4 = value;
        else if (key == "i5") a.i5 = value;
        else if (key == "itime") a.itime = toInt();
    }
    return a;
}

}  // namespace

void showEditorDialog(AppContext& ctx, const std::string& tunnelId) {
    const auto* existing = ctx.findTunnel(tunnelId);
    if (!existing) return;

    g_editor = EditorState{};
    g_editor.ctx = &ctx;
    g_editor.config = *existing;
    auto secrets = ctx.store.loadSecrets(tunnelId).value_or(TunnelSecrets{});

    // Require the profile too: a config of OpenVPN kind without one would otherwise be
    // dereferenced below.
    const bool isOpenVpn = g_editor.config.kind == TunnelKind::OpenVpn &&
                           g_editor.config.openVpn.has_value();
    // Design units; the frame and DPI are added by makeDialogWindow.
    constexpr int kX = 18;                       // left margin
    const int fieldX = kX + kLabelWidth;
    constexpr int kFieldWidth = 430;

    HWND hwnd = makeDialogWindow(ctx, L"TunHubEditor", str::widen(existing->name), 660,
                                 isOpenVpn ? 330 : 790, editorProc);
    if (!hwnd) return;

    int y = 16;
    label(hwnd, loc::w("Name"), kX, y);
    g_editor.name = edit(hwnd, str::widen(g_editor.config.name), fieldX, y, kFieldWidth);
    y += kRowHeight + 8;

    if (isOpenVpn) {
        label(hwnd, loc::w("Endpoint"), kX, y);
        HWND remote = edit(hwnd, str::widen(g_editor.config.openVpn->remoteSummary),
                           fieldX, y, kFieldWidth);
        SendMessageW(remote, EM_SETREADONLY, TRUE, 0);
        y += kRowHeight + 8;

        label(hwnd, loc::w("Username"), kX, y);
        g_editor.ovpnUser = edit(hwnd, str::widen(secrets.openVpnUsername), fieldX, y, 280);
        y += kRowHeight + 4;
        label(hwnd, loc::w("Password"), kX, y);
        g_editor.ovpnPassword = edit(hwnd, str::widen(secrets.openVpnPassword),
                                     fieldX, y, 280, kEditHeight, ES_PASSWORD);
        y += kRowHeight + 12;
    } else {
        label(hwnd, loc::w("Private key"), kX, y);
        g_editor.privateKey = edit(hwnd, str::widen(secrets.privateKey), fieldX, y, 380,
                                   kEditHeight, ES_PASSWORD);
        button(hwnd, L"⟳", IDC_ED_GENKEY, fieldX + 388, y, 42, kEditHeight);
        y += kRowHeight + 4;

        label(hwnd, loc::w("Public key"), kX, y);
        g_editor.publicKey = edit(hwnd, str::widen(g_editor.config.iface.publicKey),
                                  fieldX, y, kFieldWidth);
        SendMessageW(g_editor.publicKey, EM_SETREADONLY, TRUE, 0);
        y += kRowHeight + 4;

        label(hwnd, loc::w("Addresses"), kX, y);
        g_editor.addresses = edit(hwnd, str::widen(joinRanges(g_editor.config.iface.addresses)),
                                  fieldX, y, kFieldWidth);
        y += kRowHeight + 4;

        label(hwnd, loc::w("DNS"), kX, y);
        g_editor.dns = edit(hwnd, str::widen(str::join(g_editor.config.iface.dns, ", ")),
                            fieldX, y, kFieldWidth);
        y += kRowHeight + 4;

        label(hwnd, loc::w("MTU"), kX, y);
        g_editor.mtu = edit(hwnd,
                            g_editor.config.iface.mtu
                                ? std::to_wstring(*g_editor.config.iface.mtu) : L"",
                            fieldX, y, 100);
        label(hwnd, loc::w("Listen port"), fieldX + 116, y, 110);
        g_editor.listenPort = edit(hwnd,
                                   g_editor.config.iface.listenPort
                                       ? std::to_wstring(*g_editor.config.iface.listenPort) : L"",
                                   fieldX + 230, y, 100);
        y += kRowHeight + 12;

        label(hwnd, loc::w("Peers"), kX, y, 320);
        y += 26;
        const PeerConfig firstPeer =
            g_editor.config.peers.empty() ? PeerConfig{} : g_editor.config.peers.front();

        label(hwnd, loc::w("Public key"), kX, y);
        g_editor.peerPublicKey = edit(hwnd, str::widen(firstPeer.publicKey), fieldX, y, kFieldWidth);
        y += kRowHeight + 4;

        label(hwnd, loc::w("Endpoint"), kX, y);
        g_editor.endpoint = edit(hwnd, str::widen(firstPeer.endpoint.value_or("")),
                                 fieldX, y, kFieldWidth);
        y += kRowHeight + 4;

        label(hwnd, loc::w("Allowed IPs"), kX, y);
        g_editor.allowedIPs = edit(hwnd, str::widen(joinRanges(firstPeer.allowedIPs)),
                                   fieldX, y, kFieldWidth);
        y += kRowHeight + 4;

        label(hwnd, loc::w("Keepalive"), kX, y);
        g_editor.keepalive = edit(hwnd,
                                  firstPeer.persistentKeepalive
                                      ? std::to_wstring(*firstPeer.persistentKeepalive) : L"",
                                  fieldX, y, 100);
        y += kRowHeight + 12;

        label(hwnd, loc::w("Obfuscation (AmneziaWG)"), 18, y, 320);
        y += 26;
        // Free-form "Key = value" text so every AWG parameter (including I1–I5) is reachable
        // without a field per parameter.
        g_editor.awg = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT",
            str::widen(g_editor.config.awg ? awgToText(*g_editor.config.awg) : "").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN,
            dpiScale(hwnd, 18), dpiScale(hwnd, y), dpiScale(hwnd, 600), dpiScale(hwnd, 150),
            hwnd, nullptr, nullptr, nullptr);
        SendMessageW(g_editor.awg, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
        y += 162;
    }

    g_editor.killSwitch = checkbox(hwnd, loc::w("Kill switch (block traffic outside the tunnel)"),
                                   kX, y, 560, g_editor.config.options.killSwitch);
    y += kRowHeight;
    g_editor.autoConnect = checkbox(hwnd, loc::w("Connect on app launch"), kX, y, 560,
                                    g_editor.config.options.autoConnectOnLaunch);
    y += kRowHeight + 12;

    button(hwnd, loc::w("Save"), IDC_ED_OK, 400, y, 110);
    button(hwnd, loc::w("Cancel"), IDC_ED_CANCEL, 518, y, 110);

    runModal(hwnd, g_editor.done);
    if (!g_editor.save) return;

    auto& cfg = g_editor.config;
    cfg.name = utf8Of(g_editor.name);
    cfg.options.killSwitch = isChecked(g_editor.killSwitch);
    cfg.options.autoConnectOnLaunch = isChecked(g_editor.autoConnect);

    if (isOpenVpn) {
        secrets.openVpnUsername = utf8Of(g_editor.ovpnUser);
        secrets.openVpnPassword = utf8Of(g_editor.ovpnPassword);
    } else {
        secrets.privateKey = utf8Of(g_editor.privateKey);
        if (auto pub = wgkey::publicKeyFrom(secrets.privateKey)) cfg.iface.publicKey = *pub;
        cfg.iface.addresses = parseRangeList(utf8Of(g_editor.addresses));
        cfg.iface.dns.clear();
        cfg.iface.dnsSearchDomains.clear();
        for (const auto& d : str::splitList(utf8Of(g_editor.dns))) {
            if (isIpLiteral(d)) cfg.iface.dns.push_back(d);
            else cfg.iface.dnsSearchDomains.push_back(d);
        }
        auto parseOptionalInt = [](const std::string& s) -> std::optional<int> {
            if (s.empty()) return std::nullopt;
            try { return std::stoi(s); } catch (...) { return std::nullopt; }
        };
        cfg.iface.mtu = parseOptionalInt(utf8Of(g_editor.mtu));
        if (auto port = parseOptionalInt(utf8Of(g_editor.listenPort)))
            cfg.iface.listenPort = static_cast<uint16_t>(*port);
        else
            cfg.iface.listenPort.reset();

        if (cfg.peers.empty()) {
            PeerConfig p;
            p.id = util::newGuid();
            cfg.peers.push_back(p);
        }
        auto& peer = cfg.peers.front();
        peer.publicKey = utf8Of(g_editor.peerPublicKey);
        const auto endpointText = utf8Of(g_editor.endpoint);
        peer.endpoint = endpointText.empty() ? std::nullopt : std::optional<std::string>(endpointText);
        peer.allowedIPs = parseRangeList(utf8Of(g_editor.allowedIPs));
        if (auto ka = parseOptionalInt(utf8Of(g_editor.keepalive)))
            peer.persistentKeepalive = static_cast<uint16_t>(*ka);
        else
            peer.persistentKeepalive.reset();

        auto awg = awgFromText(utf8Of(g_editor.awg));
        if (awg.empty()) {
            cfg.awg.reset();
            if (cfg.kind == TunnelKind::AmneziaWg) cfg.kind = TunnelKind::WireGuard;
        } else {
            if (auto errors = awg.validate(); !errors.empty()) {
                messageBox(ctx.mainWindow, "Error", str::widen(str::join(errors, "\r\n")),
                           MB_OK | MB_ICONWARNING);
                return;
            }
            cfg.awg = awg;
            cfg.kind = TunnelKind::AmneziaWg;
        }
    }

    std::string error;
    if (!ctx.store.saveTunnel(cfg, &error)) {
        messageBox(ctx.mainWindow, "Error", str::widen(error), MB_OK | MB_ICONERROR);
        return;
    }
    ctx.store.saveSecrets(cfg.id, secrets);
}

}  // namespace tunhub::app
