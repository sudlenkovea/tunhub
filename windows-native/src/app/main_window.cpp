// Main window: tunnel list, details pane with a traffic strip, toolbar and tray icon.

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>

#include "tunhub/constants.h"
#include "tunhub/str.h"
#include "tunhub/util.h"
#include "ui.h"

#pragma comment(lib, "comctl32.lib")

namespace tunhub::app {
namespace {

constexpr UINT WM_TUNHUB_TRAY = WM_APP + 1;
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT kTrayId = 1;

// Control ids.
enum : int {
    IDC_LIST = 1000,
    IDC_START, IDC_STOP, IDC_STOPALL, IDC_IMPORT, IDC_EDIT, IDC_DELETE,
    IDC_LOGS, IDC_SETTINGS, IDC_CONFLICTS,
    IDC_DETAILS, IDC_GRAPH, IDC_HELPERBAR, IDC_HELPERBTN,
    // Tray menu commands start well above the control ids.
    IDM_TRAY_OPEN = 2000, IDM_TRAY_STOPALL, IDM_TRAY_QUIT, IDM_TRAY_TUNNEL_BASE = 2100,
};

struct MainWindowState {
    AppContext* ctx = nullptr;
    HWND list = nullptr;
    HWND details = nullptr;
    HWND graph = nullptr;
    HWND helperBar = nullptr;
    HWND helperButton = nullptr;
    std::vector<HWND> buttons;
    bool helperBarVisible = false;
    NOTIFYICONDATAW tray{};
    bool trayAdded = false;
    HICON iconIdle = nullptr;
    HICON iconActive = nullptr;
    bool lastActive = false;
    std::string lastTraySignature;
};

MainWindowState g_state;

HFONT g_font = nullptr;

HWND makeButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND b = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return b;
}

/// Any tunnel actually carrying traffic right now.
bool anyTunnelActive(const AppContext& ctx) {
    for (const auto& [id, state] : ctx.runtime)
        if (state.phase == TunnelPhase::Up || state.phase == TunnelPhase::Degraded) return true;
    return false;
}

bool anyTunnelRunning(const AppContext& ctx) {
    for (const auto& [id, state] : ctx.runtime)
        if (state.phase != TunnelPhase::Stopped && state.phase != TunnelPhase::Failed) return true;
    return false;
}

// ── tray ─────────────────────────────────────────────────────────────────────

HICON makeStatusIcon(bool active) {
    // Drawn rather than shipped as a resource: two 16×16 shields differing only in colour.
    const int size = 16;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP colour = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    auto* old = static_cast<HBITMAP>(SelectObject(dc, colour));

    RECT r{0, 0, size, size};
    HBRUSH back = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &r, back);
    DeleteObject(back);

    const COLORREF fill = active ? RGB(64, 190, 110) : RGB(150, 150, 150);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    auto* oldBrush = static_cast<HBRUSH>(SelectObject(dc, brush));
    auto* oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    const POINT shield[] = {{8, 1}, {14, 4}, {14, 9}, {8, 15}, {2, 9}, {2, 4}};
    Polygon(dc, shield, 6);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SelectObject(dc, mask);
    PatBlt(dc, 0, 0, size, size, WHITENESS);
    auto* oldMaskBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(BLACK_BRUSH)));
    Polygon(dc, shield, 6);
    SelectObject(dc, oldMaskBrush);

    SelectObject(dc, old);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colour;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);

    DeleteObject(colour);
    DeleteObject(mask);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

void updateTrayIcon(HWND hwnd) {
    const bool active = anyTunnelActive(*g_state.ctx);
    if (g_state.trayAdded && active == g_state.lastActive) return;
    g_state.lastActive = active;

    g_state.tray.cbSize = sizeof(g_state.tray);
    g_state.tray.hWnd = hwnd;
    g_state.tray.uID = kTrayId;
    g_state.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_state.tray.uCallbackMessage = WM_TUNHUB_TRAY;
    g_state.tray.hIcon = active ? g_state.iconActive : g_state.iconIdle;
    wcscpy_s(g_state.tray.szTip, active ? L"TunHub — connected" : L"TunHub");

    Shell_NotifyIconW(g_state.trayAdded ? NIM_MODIFY : NIM_ADD, &g_state.tray);
    g_state.trayAdded = true;
}

void showTrayMenu(HWND hwnd) {
    auto& ctx = *g_state.ctx;
    HMENU menu = CreatePopupMenu();

    // Tunnel list, mirroring the macOS menu bar: click one to toggle it.
    int index = 0;
    for (const auto& t : ctx.tunnels) {
        const auto* rt = ctx.findRuntime(t.id);
        const bool up = rt && (rt->phase == TunnelPhase::Up || rt->phase == TunnelPhase::Degraded);
        std::wstring label = str::widen(t.name) + L"\t" + phaseText(rt ? rt->phase : TunnelPhase::Stopped);
        AppendMenuW(menu, MF_STRING | (up ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(IDM_TRAY_TUNNEL_BASE + index), label.c_str());
        ++index;
    }
    if (index > 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, loc::w("Open window").c_str());
    // Only offer "Stop all" when there is something to stop.
    if (anyTunnelRunning(ctx))
        AppendMenuW(menu, MF_STRING, IDM_TRAY_STOPALL, loc::w("Stop all").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, loc::w("Quit TunHub").c_str());

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);     // required, else the menu won't dismiss on click-away
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ── list ─────────────────────────────────────────────────────────────────────

void setupList(HWND list) {
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    struct { const char* key; int width; } columns[] = {
        {"Name", 170}, {"Type", 100}, {"Status", 110}, {"Traffic", 150},
    };
    int index = 0;
    for (const auto& c : columns) {
        auto title = loc::w(c.key);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = title.data();
        col.cx = c.width;
        col.iSubItem = index;
        ListView_InsertColumn(list, index, &col);
        ++index;
    }
}

void refreshList(AppContext& ctx) {
    HWND list = g_state.list;
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);

    int row = 0;
    for (const auto& t : ctx.tunnels) {
        const auto* rt = ctx.findRuntime(t.id);
        auto name = str::widen(t.name);

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = name.data();
        item.lParam = row;
        ListView_InsertItem(list, &item);

        auto kind = str::widen(kindLabel(t.kind));
        ListView_SetItemText(list, row, 1, kind.data());

        auto status = phaseText(rt ? rt->phase : TunnelPhase::Stopped);
        ListView_SetItemText(list, row, 2, status.data());

        std::wstring traffic = L"—";
        if (rt && (rt->phase == TunnelPhase::Up || rt->phase == TunnelPhase::Degraded)) {
            traffic = str::widen("↓ " + str::humanBytes(rt->rxTotal()) +
                                 "   ↑ " + str::humanBytes(rt->txTotal()));
        }
        ListView_SetItemText(list, row, 3, traffic.data());
        ++row;
    }

    if (selected >= 0 && selected < row)
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void refreshDetails(AppContext& ctx) {
    const auto* t = ctx.findTunnel(ctx.selectedId);
    if (!t) {
        SetWindowTextW(g_state.details, loc::w("Select a tunnel").c_str());
        return;
    }
    const auto* rt = ctx.findRuntime(t->id);

    std::string text = t->name + "  ·  " + kindLabel(t->kind) + "\r\n";
    if (rt) {
        text += loc::t("Status") + ": " + str::narrow(phaseText(rt->phase));
        if (!rt->errorMessage.empty()) text += " — " + rt->errorMessage;
        text += "\r\n";
        if (rt->phase == TunnelPhase::Up || rt->phase == TunnelPhase::Degraded) {
            text += loc::t("Uptime") + ": " +
                    util::formatDuration(util::nowUnix() - rt->since) + "\r\n";
            text += loc::t("Received") + ": " + str::humanBytes(rt->rxTotal()) + "   " +
                    loc::t("Sent") + ": " + str::humanBytes(rt->txTotal()) + "\r\n";
            const auto hs = rt->lastHandshake();
            text += loc::t("Last handshake") + ": " +
                    (hs ? util::formatDuration(util::nowUnix() - hs) : loc::t("never")) + "\r\n";
        }
        if (!rt->interfaceName.empty()) text += "Adapter: " + rt->interfaceName + "\r\n";
    }

    const auto routes = t->effectiveRoutes();
    text += loc::t("Routes") + ": ";
    text += t->hasDefaultRoute() ? loc::t("all traffic (default route)")
                                 : std::to_string(routes.size());
    text += "\r\n";
    if (!t->iface.dns.empty()) text += "DNS: " + str::join(t->iface.dns, ", ") + "\r\n";
    for (const auto& p : t->peers)
        if (p.endpoint) text += loc::t("Endpoint") + ": " + *p.endpoint + "\r\n";

    SetWindowTextW(g_state.details, str::widen(text).c_str());
}

// ── traffic strip ────────────────────────────────────────────────────────────

void paintGraph(HWND hwnd, AppContext& ctx) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    FrameRect(dc, &rc, GetSysColorBrush(COLOR_BTNSHADOW));

    auto it = ctx.history.find(ctx.selectedId);
    if (it == ctx.history.end() || it->second.size() < 2) {
        EndPaint(hwnd, &ps);
        return;
    }
    const auto& samples = it->second;

    double peak = 1.0;
    for (const auto& s : samples) peak = std::max({peak, s.rxRate, s.txRate});

    const int width = rc.right - rc.left - 2;
    const int height = rc.bottom - rc.top - 2;
    const size_t count = samples.size();

    auto drawSeries = [&](bool rx, COLORREF colour) {
        HPEN pen = CreatePen(PS_SOLID, 2, colour);
        auto* oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        for (size_t i = 0; i < count; ++i) {
            const double value = rx ? samples[i].rxRate : samples[i].txRate;
            const int x = rc.left + 1 + static_cast<int>(width * i / (count - 1));
            const int y = rc.bottom - 1 - static_cast<int>(height * (value / peak));
            if (i == 0) MoveToEx(dc, x, y, nullptr);
            else LineTo(dc, x, y);
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    };
    drawSeries(true, RGB(64, 150, 220));    // download
    drawSeries(false, RGB(220, 140, 60));   // upload

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, uiFont());
    const auto label = str::widen("peak " + str::humanRate(peak));
    TextOutW(dc, rc.left + 6, rc.top + 4, label.c_str(), static_cast<int>(label.size()));

    EndPaint(hwnd, &ps);
}

// ── polling ──────────────────────────────────────────────────────────────────

void poll(AppContext& ctx) {
    ipc::Response response;
    const bool reachable =
        ctx.daemon.call(ipc::method::kRuntimeStates, Json(), &response, 2000) && response.ok;

    ctx.helperReachable = reachable;
    std::map<std::string, TunnelRuntimeState> fresh;
    if (reachable)
        for (const auto& item : response.payload.items()) {
            auto s = TunnelRuntimeState::fromJson(item);
            if (!s.id.empty()) fresh[s.id] = std::move(s);
        }

    // Traffic rates from the delta between polls.
    for (const auto& [id, state] : fresh) {
        const auto rx = state.rxTotal(), tx = state.txTotal();
        Sample sample;
        if (auto prev = ctx.lastCounters.find(id); prev != ctx.lastCounters.end()) {
            if (rx >= prev->second.first) sample.rxRate = static_cast<double>(rx - prev->second.first);
            if (tx >= prev->second.second) sample.txRate = static_cast<double>(tx - prev->second.second);
        }
        ctx.lastCounters[id] = {rx, tx};
        auto& series = ctx.history[id];
        series.push_back(sample);
        while (series.size() > AppContext::kMaxSamples) series.pop_front();
    }
    // Forget tunnels that stopped, so their graph doesn't freeze on the last value.
    for (auto it = ctx.history.begin(); it != ctx.history.end();)
        it = fresh.count(it->first) ? std::next(it) : ctx.history.erase(it);
    for (auto it = ctx.lastCounters.begin(); it != ctx.lastCounters.end();)
        it = fresh.count(it->first) ? std::next(it) : ctx.lastCounters.erase(it);

    ctx.runtime = std::move(fresh);
}

void layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right, height = rc.bottom;
    const int barHeight = g_state.helperBarVisible ? 56 : 0;
    const int toolbarHeight = 40;

    if (g_state.helperBar) {
        ShowWindow(g_state.helperBar, g_state.helperBarVisible ? SW_SHOW : SW_HIDE);
        ShowWindow(g_state.helperButton, g_state.helperBarVisible ? SW_SHOW : SW_HIDE);
        MoveWindow(g_state.helperBar, 12, 8, width - 220, 40, TRUE);
        MoveWindow(g_state.helperButton, width - 200, 12, 188, 30, TRUE);
    }

    int x = 12;
    const int top = barHeight + 8;
    for (HWND b : g_state.buttons) {
        MoveWindow(b, x, top, 108, 30, TRUE);
        x += 114;
    }

    const int listTop = top + toolbarHeight;
    const int listWidth = static_cast<int>(width * 0.55);
    MoveWindow(g_state.list, 12, listTop, listWidth - 18, height - listTop - 12, TRUE);

    const int rightX = listWidth + 4;
    const int rightWidth = width - rightX - 12;
    const int graphHeight = 120;
    MoveWindow(g_state.details, rightX, listTop, rightWidth,
               height - listTop - graphHeight - 24, TRUE);
    MoveWindow(g_state.graph, rightX, height - graphHeight - 12, rightWidth, graphHeight, TRUE);
}

void onCommand(HWND hwnd, int id) {
    auto& ctx = *g_state.ctx;

    if (id >= IDM_TRAY_TUNNEL_BASE) {
        const size_t index = static_cast<size_t>(id - IDM_TRAY_TUNNEL_BASE);
        if (index >= ctx.tunnels.size()) return;
        const auto& t = ctx.tunnels[index];
        const auto* rt = ctx.findRuntime(t.id);
        const bool up = rt && (rt->phase == TunnelPhase::Up || rt->phase == TunnelPhase::Degraded ||
                               rt->phase == TunnelPhase::Starting);
        if (up) stopTunnel(ctx, t.id);
        else startTunnel(ctx, t.id);
        return;
    }

    switch (id) {
        case IDC_START:
            if (!ctx.selectedId.empty()) startTunnel(ctx, ctx.selectedId);
            break;
        case IDC_STOP:
            if (!ctx.selectedId.empty()) stopTunnel(ctx, ctx.selectedId);
            break;
        case IDC_STOPALL:
        case IDM_TRAY_STOPALL:
            stopAllTunnels(ctx);
            break;
        case IDC_IMPORT:
            runImport(ctx);
            refreshList(ctx);
            break;
        case IDC_EDIT:
            if (!ctx.selectedId.empty()) {
                showEditorDialog(ctx, ctx.selectedId);
                ctx.reloadTunnels();
                refreshList(ctx);
                refreshDetails(ctx);
            }
            break;
        case IDC_DELETE: {
            if (ctx.selectedId.empty()) break;
            const auto* t = ctx.findTunnel(ctx.selectedId);
            if (!t) break;
            const auto text = loc::w("Delete this tunnel?") + L"\n\n" +
                              str::widen(t->name) + L"\n\n" +
                              loc::w("This removes its configuration and stored keys.");
            if (MessageBoxW(hwnd, text.c_str(), loc::w("Delete").c_str(),
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                stopTunnel(ctx, t->id);
                ctx.store.deleteTunnel(t->id);
                ctx.selectedId.clear();
                ctx.reloadTunnels();
                refreshList(ctx);
                refreshDetails(ctx);
            }
            break;
        }
        case IDC_LOGS:      showLogWindow(ctx); break;
        case IDC_SETTINGS:  showSettingsDialog(ctx); break;
        case IDC_CONFLICTS: showConflictsDialog(ctx); break;
        case IDC_HELPERBTN: installHelper(ctx); break;
        case IDM_TRAY_OPEN: showMainWindow(ctx); break;
        case IDM_TRAY_QUIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        default: break;
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& ctx = *g_state.ctx;

    switch (msg) {
        case WM_CREATE: {
            g_state.helperBar = CreateWindowExW(
                0, L"STATIC", loc::w("TunHub needs a background service to manage tunnels.").c_str(),
                WS_CHILD | SS_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(IDC_HELPERBAR), nullptr, nullptr);
            SendMessageW(g_state.helperBar, WM_SETFONT,
                         reinterpret_cast<WPARAM>(uiFont()), TRUE);
            g_state.helperButton = makeButton(hwnd, IDC_HELPERBTN,
                                              loc::w("Install system component").c_str(),
                                              0, 0, 0, 0);

            struct { int id; const char* key; } buttons[] = {
                {IDC_START, "Start"}, {IDC_STOP, "Stop"}, {IDC_STOPALL, "Stop all"},
                {IDC_IMPORT, "Import…"}, {IDC_EDIT, "Edit"}, {IDC_DELETE, "Delete"},
                {IDC_CONFLICTS, "Check conflicts"}, {IDC_LOGS, "Logs"}, {IDC_SETTINGS, "Settings"},
            };
            for (const auto& b : buttons)
                g_state.buttons.push_back(makeButton(hwnd, b.id, loc::w(b.key).c_str(), 0, 0, 0, 0));

            g_state.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                               LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                           0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(IDC_LIST), nullptr, nullptr);
            SendMessageW(g_state.list, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
            setupList(g_state.list);

            g_state.details = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 0, 0, 0, 0,
                hwnd, reinterpret_cast<HMENU>(IDC_DETAILS), nullptr, nullptr);
            SendMessageW(g_state.details, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);

            // Plain static: the subclass below does the painting, so no owner-draw plumbing
            // through the parent is needed.
            g_state.graph = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                            0, 0, 0, 0, hwnd,
                                            reinterpret_cast<HMENU>(IDC_GRAPH), nullptr, nullptr);

            g_state.iconIdle = makeStatusIcon(false);
            g_state.iconActive = makeStatusIcon(true);
            updateTrayIcon(hwnd);

            SetTimer(hwnd, kPollTimer, 1000, nullptr);
            return 0;
        }

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case WM_TIMER:
            if (wParam == kPollTimer) {
                poll(ctx);
                const bool showBar = !ctx.helperReachable;
                if (showBar != g_state.helperBarVisible) {
                    g_state.helperBarVisible = showBar;
                    layout(hwnd);
                }
                refreshList(ctx);
                refreshDetails(ctx);
                InvalidateRect(g_state.graph, nullptr, FALSE);
                updateTrayIcon(hwnd);
            }
            return 0;

        case WM_COMMAND:
            onCommand(hwnd, LOWORD(wParam));
            return 0;

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<LPNMHDR>(lParam);
            if (header->idFrom == IDC_LIST && header->code == LVN_ITEMCHANGED) {
                auto* view = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if (view->uNewState & LVIS_SELECTED) {
                    const size_t index = static_cast<size_t>(view->iItem);
                    if (index < ctx.tunnels.size()) {
                        ctx.selectedId = ctx.tunnels[index].id;
                        refreshDetails(ctx);
                        InvalidateRect(g_state.graph, nullptr, TRUE);
                    }
                }
            } else if (header->idFrom == IDC_LIST && header->code == NM_DBLCLK) {
                if (!ctx.selectedId.empty()) {
                    const auto* rt = ctx.findRuntime(ctx.selectedId);
                    const bool up = rt && (rt->phase == TunnelPhase::Up ||
                                           rt->phase == TunnelPhase::Degraded);
                    if (up) stopTunnel(ctx, ctx.selectedId);
                    else startTunnel(ctx, ctx.selectedId);
                }
            }
            return 0;
        }

        case WM_TUNHUB_TRAY:
            if (LOWORD(lParam) == WM_LBUTTONUP) showMainWindow(ctx);
            else if (LOWORD(lParam) == WM_RBUTTONUP) showTrayMenu(hwnd);
            return 0;

        case WM_CLOSE: {
            // Closing the window hides it; quitting happens from the tray.
            if (!anyTunnelRunning(ctx)) {
                DestroyWindow(hwnd);
                return 0;
            }
            const int answer = MessageBoxW(
                hwnd, (loc::w("Some tunnels are still connected.") + L"\n\n" +
                       loc::w("Disconnect all tunnels before quitting?")).c_str(),
                loc::w("Quit TunHub").c_str(), MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL) return 0;
            if (answer == IDYES) stopAllTunnels(ctx);
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, kPollTimer);
            if (g_state.trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_state.tray);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/// Subclass for the graph child so it can paint itself.
LRESULT CALLBACK graphProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                           UINT_PTR, DWORD_PTR) {
    if (msg == WM_PAINT) {
        paintGraph(hwnd, *g_state.ctx);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

}  // namespace

// ── AppContext ───────────────────────────────────────────────────────────────

const TunnelConfig* AppContext::findTunnel(const std::string& id) const {
    for (const auto& t : tunnels)
        if (t.id == id) return &t;
    return nullptr;
}

const TunnelRuntimeState* AppContext::findRuntime(const std::string& id) const {
    auto it = runtime.find(id);
    return it == runtime.end() ? nullptr : &it->second;
}

void AppContext::reloadTunnels() { tunnels = store.loadTunnels(); }

// ── shared helpers ───────────────────────────────────────────────────────────

HFONT uiFont() {
    if (!g_font) {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            g_font = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!g_font) g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return g_font;
}

std::wstring phaseText(TunnelPhase phase) {
    switch (phase) {
        case TunnelPhase::Up:       return loc::w("running");
        case TunnelPhase::Degraded: return loc::w("degraded");
        case TunnelPhase::Starting: return loc::w("starting…");
        case TunnelPhase::Stopping: return loc::w("stopping…");
        case TunnelPhase::Failed:   return loc::w("failed");
        default:                    return loc::w("stopped");
    }
}

COLORREF phaseColor(TunnelPhase phase) {
    switch (phase) {
        case TunnelPhase::Up:       return RGB(46, 160, 90);
        case TunnelPhase::Degraded:
        case TunnelPhase::Starting:
        case TunnelPhase::Stopping: return RGB(215, 145, 40);
        case TunnelPhase::Failed:   return RGB(200, 60, 60);
        default:                    return RGB(120, 120, 120);
    }
}

void messageBox(HWND owner, const std::string& titleKey, const std::wstring& text, UINT flags) {
    MessageBoxW(owner, text.c_str(), loc::w(titleKey).c_str(), flags);
}

bool createMainWindow(AppContext& ctx) {
    g_state.ctx = &ctx;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ctx.instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TunHubMainWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return false;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"TunHub",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1020, 620,
                                nullptr, nullptr, ctx.instance, nullptr);
    if (!hwnd) return false;
    ctx.mainWindow = hwnd;

    SetWindowSubclass(g_state.graph, graphProc, 1, 0);
    ctx.reloadTunnels();
    refreshList(ctx);
    refreshDetails(ctx);
    layout(hwnd);
    return true;
}

void showMainWindow(AppContext& ctx) {
    if (!ctx.mainWindow) return;
    ShowWindow(ctx.mainWindow, SW_SHOW);
    SetForegroundWindow(ctx.mainWindow);
}

}  // namespace tunhub::app
