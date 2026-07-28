// Main window: a tunnel list on the left and a detail pane on the right, mirroring the macOS
// layout. The list carries identity and state only; everything about the *selected* tunnel —
// status, the connect action, statistics and the traffic strip — lives in the detail pane.
//
// The detail pane is a container window that owns its own controls (title, status, the
// Start/Stop button, the stats block and the graph). An earlier version drew the pane and
// placed the button over it as a sibling of the main window, which depended on sibling
// clipping and z-order to stay visible — and did not.

#include <windows.h>    // must come before any other Windows header
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>     // wcscpy_s
#include <iterator>   // std::next
#include <vector>

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

enum : int {
    IDC_LIST = 1000,
    IDC_IMPORT, IDC_EDIT, IDC_DELETE, IDC_CONFLICTS, IDC_LOGS, IDC_SETTINGS, IDC_STOPALL,
    IDC_DETAIL, IDC_HELPERTEXT, IDC_HELPERBTN,
    // Children of the detail pane.
    IDC_TITLE = 1100, IDC_SUBTITLE, IDC_STATUS, IDC_CONNECT, IDC_STATS, IDC_GRAPH, IDC_ERROR,
    IDM_TRAY_OPEN = 2000, IDM_TRAY_STOPALL, IDM_TRAY_QUIT, IDM_TRAY_TUNNEL_BASE = 2100,
};

/// Design-time metrics in 96-dpi units; every use goes through dpiScale.
namespace metrics {
constexpr int kPad = 14;
constexpr int kGap = 10;
constexpr int kToolbarHeight = 34;
constexpr int kListWidth = 330;
constexpr int kGraphHeight = 120;
constexpr int kHelperBarHeight = 44;
constexpr int kStatRow = 26;
constexpr int kDotRadius = 5;
constexpr int kTitleHeight = 34;
constexpr int kSubtitleHeight = 22;
constexpr int kStatusHeight = 26;
constexpr int kButtonHeight = 34;
constexpr int kButtonWidth = 150;
constexpr int kErrorHeight = 38;
}  // namespace metrics

struct ToolbarButton {
    int id;
    const char* key;
    HWND hwnd = nullptr;
    int width = 0;
    bool trailing = false;   // pushed to the right-hand end of the toolbar
};

struct MainWindowState {
    AppContext* ctx = nullptr;
    HWND main = nullptr;
    HWND list = nullptr;
    HWND detail = nullptr;
    // Detail-pane children.
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND status = nullptr;
    HWND connect = nullptr;
    HWND error = nullptr;
    HWND stats = nullptr;
    HWND graph = nullptr;

    HWND helperText = nullptr;
    HWND helperButton = nullptr;
    std::vector<ToolbarButton> toolbar;
    bool helperBarVisible = false;
    bool errorVisible = false;

    NOTIFYICONDATAW tray{};
    bool trayAdded = false;
    HICON iconIdle = nullptr;
    HICON iconActive = nullptr;
    bool lastActive = false;

    COLORREF statusColour = RGB(0, 0, 0);
};

MainWindowState g_state;
HFONT g_uiFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_smallFont = nullptr;

// ── small helpers ────────────────────────────────────────────────────────────

bool tunnelIsUp(const TunnelRuntimeState* rt) {
    return rt && (rt->phase == TunnelPhase::Up || rt->phase == TunnelPhase::Degraded);
}

bool tunnelIsBusy(const TunnelRuntimeState* rt) {
    return rt && (rt->phase == TunnelPhase::Starting || rt->phase == TunnelPhase::Stopping);
}

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

void drawStatusDot(HDC dc, int centreX, int centreY, int radius, COLORREF colour) {
    HBRUSH brush = CreateSolidBrush(colour);
    HPEN pen = CreatePen(PS_SOLID, 1, colour);
    auto* oldBrush = static_cast<HBRUSH>(SelectObject(dc, brush));
    auto* oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    Ellipse(dc, centreX - radius, centreY - radius, centreX + radius, centreY + radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void drawText(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF colour,
              UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    auto* oldFont = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, colour);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc, format);
    SelectObject(dc, oldFont);
}

/// Secondary text: the shell has no such system colour, so blend toward the background.
COLORREF dimText() {
    const COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF bg = GetSysColor(COLOR_WINDOW);
    auto mix = [](BYTE a, BYTE b) { return static_cast<BYTE>((a * 45 + b * 55) / 100); };
    return RGB(mix(GetRValue(fg), GetRValue(bg)), mix(GetGValue(fg), GetGValue(bg)),
               mix(GetBValue(fg), GetBValue(bg)));
}

void setTextIfChanged(HWND control, const std::wstring& text) {
    // SetWindowText repaints unconditionally; at 1 Hz that reads as flicker.
    const int length = GetWindowTextLengthW(control);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, current.data(), length + 1);
    current.resize(static_cast<size_t>(length));
    if (current != text) SetWindowTextW(control, text.c_str());
}

// ── tray ─────────────────────────────────────────────────────────────────────

HICON makeStatusIcon(bool active) {
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

    const COLORREF fill = active ? RGB(46, 160, 90) : RGB(140, 140, 140);
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

    int index = 0;
    for (const auto& t : ctx.tunnels) {
        const auto* rt = ctx.findRuntime(t.id);
        const std::wstring label = str::widen(t.name) + L"\t" +
                                   phaseText(rt ? rt->phase : TunnelPhase::Stopped);
        AppendMenuW(menu, MF_STRING | (tunnelIsUp(rt) ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(IDM_TRAY_TUNNEL_BASE + index), label.c_str());
        ++index;
    }
    if (index > 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, loc::w("Open window").c_str());
    if (anyTunnelRunning(ctx))
        AppendMenuW(menu, MF_STRING, IDM_TRAY_STOPALL, loc::w("Stop all").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, loc::w("Quit TunHub").c_str());

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);     // otherwise the menu won't dismiss on click-away
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ── tunnel list ──────────────────────────────────────────────────────────────

void setupList(HWND list) {
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    // Identity and state only — everything else belongs to the detail pane. Widths are set
    // from the real client width in sizeListColumns(); fixed widths overflowed at 150% DPI
    // and produced a horizontal scrollbar.
    const char* titles[] = {"Name", "Type", "Status"};
    for (int i = 0; i < 3; ++i) {
        auto text = loc::w(titles[i]);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = text.data();
        col.cx = 100;
        col.iSubItem = i;
        ListView_InsertColumn(list, i, &col);
    }
}

/// Proportional columns that always add up to the visible width, so nothing is clipped and
/// no horizontal scrollbar appears.
void sizeListColumns(HWND list) {
    RECT rc{};
    GetClientRect(list, &rc);
    int width = rc.right - rc.left;
    if (width <= 0) return;
    width -= GetSystemMetrics(SM_CXVSCROLL);   // leave room for the vertical scrollbar

    const int status = std::max<int>(width * 30 / 100, 90);
    const int type = std::max<int>(width * 24 / 100, 70);
    const int name = std::max<int>(width - status - type, 90);
    ListView_SetColumnWidth(list, 0, name);
    ListView_SetColumnWidth(list, 1, type);
    ListView_SetColumnWidth(list, 2, status);
}

void refreshList(AppContext& ctx) {
    HWND list = g_state.list;
    const int existing = ListView_GetItemCount(list);
    const bool rebuild = existing != static_cast<int>(ctx.tunnels.size());

    if (rebuild) {
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
    }

    int row = 0;
    for (const auto& t : ctx.tunnels) {
        auto name = str::widen(t.name);
        auto kind = str::widen(kindLabel(t.kind));
        const auto* rt = ctx.findRuntime(t.id);
        auto status = phaseText(rt ? rt->phase : TunnelPhase::Stopped);

        if (rebuild) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = name.data();
            ListView_InsertItem(list, &item);
        } else {
            ListView_SetItemText(list, row, 0, name.data());
        }
        ListView_SetItemText(list, row, 1, kind.data());
        ListView_SetItemText(list, row, 2, status.data());
        ++row;
    }

    if (rebuild) {
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        sizeListColumns(list);
        InvalidateRect(list, nullptr, TRUE);
        if (!ctx.tunnels.empty() && ctx.selectedId.empty()) {
            ctx.selectedId = ctx.tunnels.front().id;
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
    } else {
        InvalidateRect(list, nullptr, FALSE);
    }
}

/// Status column: a coloured dot plus the phase, which reads far faster than plain text.
LRESULT handleListCustomDraw(AppContext& ctx, LPNMLVCUSTOMDRAW cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            if (cd->iSubItem != 2) return CDRF_DODEFAULT;
            const size_t index = static_cast<size_t>(cd->nmcd.dwItemSpec);
            if (index >= ctx.tunnels.size()) return CDRF_DODEFAULT;

            const auto* rt = ctx.findRuntime(ctx.tunnels[index].id);
            const auto phase = rt ? rt->phase : TunnelPhase::Stopped;
            HWND list = cd->nmcd.hdr.hwndFrom;

            RECT rc{};
            rc.left = LVIR_BOUNDS;
            rc.top = 2;   // subitem index
            SendMessageW(list, LVM_GETSUBITEMRECT, cd->nmcd.dwItemSpec,
                         reinterpret_cast<LPARAM>(&rc));

            HDC dc = cd->nmcd.hdc;
            const int radius = dpiScale(list, metrics::kDotRadius);
            const int centreY = (rc.top + rc.bottom) / 2;
            const int dotX = rc.left + radius + dpiScale(list, 4);
            drawStatusDot(dc, dotX, centreY, radius, phaseColor(phase));

            RECT textRect = rc;
            textRect.left = dotX + radius + dpiScale(list, 6);
            const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
            drawText(dc, phaseText(phase), textRect, uiFont(),
                     GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            return CDRF_SKIPDEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

// ── statistics block (owner-drawn child of the detail pane) ──────────────────

struct StatRow {
    std::wstring label;
    std::wstring value;
};

std::vector<StatRow> buildStats(const TunnelConfig& t, const TunnelRuntimeState* rt) {
    std::vector<StatRow> rows;
    auto add = [&](const char* key, std::wstring value) {
        if (!value.empty()) rows.push_back({loc::w(key), std::move(value)});
    };

    if (tunnelIsUp(rt)) {
        add("Uptime", str::widen(util::formatDuration(util::nowUnix() - rt->since)));
        add("Received", str::widen(str::humanBytes(rt->rxTotal())));
        add("Sent", str::widen(str::humanBytes(rt->txTotal())));
        const auto hs = rt->lastHandshake();
        add("Last handshake", hs ? str::widen(util::formatDuration(util::nowUnix() - hs))
                                 : loc::w("never"));
    }
    if (t.kind == TunnelKind::OpenVpn && t.openVpn) {
        add("Endpoint", str::widen(t.openVpn->remoteSummary));
    } else {
        for (const auto& p : t.peers)
            if (p.endpoint) { add("Endpoint", str::widen(*p.endpoint)); break; }
    }
    add("Routes", t.hasDefaultRoute() ? loc::w("all traffic (default route)")
                                      : std::to_wstring(t.effectiveRoutes().size()));
    if (!t.iface.dns.empty()) add("DNS", str::widen(str::join(t.iface.dns, ", ")));
    if (rt && !rt->interfaceName.empty()) add("Interface", str::widen(rt->interfaceName));
    return rows;
}

void paintStats(HWND hwnd) {
    auto& ctx = *g_state.ctx;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

    const auto* t = ctx.findTunnel(ctx.selectedId);
    if (!t) { EndPaint(hwnd, &ps); return; }
    const auto* rt = ctx.findRuntime(t->id);

    const int labelWidth = dpiScale(hwnd, 150);
    const int rowHeight = dpiScale(hwnd, metrics::kStatRow);
    int y = 0;
    for (const auto& row : buildStats(*t, rt)) {
        if (y + rowHeight > client.bottom) break;
        RECT labelRect{0, y, labelWidth, y + rowHeight};
        drawText(dc, row.label, labelRect, smallFont(), dimText());
        RECT valueRect{labelWidth, y, client.right, y + rowHeight};
        drawText(dc, row.value, valueRect, uiFont(), GetSysColor(COLOR_WINDOWTEXT));
        y += rowHeight;
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK statsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) { paintStats(hwnd); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── traffic strip ────────────────────────────────────────────────────────────

void paintGraph(HWND hwnd) {
    auto& ctx = *g_state.ctx;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    FrameRect(dc, &rc, GetSysColorBrush(COLOR_BTNSHADOW));

    const COLORREF downColour = RGB(58, 132, 214);
    const COLORREF upColour = RGB(214, 132, 48);
    const int pad = dpiScale(hwnd, 8);
    const int legendHeight = dpiScale(hwnd, 18);

    auto it = ctx.history.find(ctx.selectedId);
    const bool haveData = it != ctx.history.end() && it->second.size() >= 2;

    double peak = 1.0;
    if (haveData)
        for (const auto& s : it->second) peak = std::max({peak, s.rxRate, s.txRate});

    if (haveData) {
        const auto& samples = it->second;
        const int width = rc.right - pad * 2;
        const int top = rc.top + pad + legendHeight;
        const int height = rc.bottom - pad - top;
        const size_t count = samples.size();

        auto drawSeries = [&](bool rx, COLORREF colour) {
            HPEN pen = CreatePen(PS_SOLID, dpiScale(hwnd, 2), colour);
            auto* oldPen = static_cast<HPEN>(SelectObject(dc, pen));
            for (size_t i = 0; i < count; ++i) {
                const double value = rx ? samples[i].rxRate : samples[i].txRate;
                const int x = rc.left + pad + static_cast<int>(width * i / (count - 1));
                const int y = top + height - static_cast<int>(height * (value / peak));
                if (i == 0) MoveToEx(dc, x, y, nullptr);
                else LineTo(dc, x, y);
            }
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        };
        drawSeries(true, downColour);
        drawSeries(false, upColour);
    }

    // Legend with the live rates, so the strip is readable without axes.
    const Sample latest = haveData ? it->second.back() : Sample{};
    const int legendY = rc.top + pad / 2;
    const int dot = dpiScale(hwnd, 4);
    const int columnWidth = dpiScale(hwnd, 130);
    int x = rc.left + pad;

    drawStatusDot(dc, x + dot, legendY + legendHeight / 2, dot, downColour);
    RECT legend{x + dot * 2 + dpiScale(hwnd, 6), legendY, x + columnWidth, legendY + legendHeight};
    drawText(dc, L"↓ " + str::widen(str::humanRate(latest.rxRate)), legend, smallFont(),
             GetSysColor(COLOR_WINDOWTEXT));

    x = legend.right + dpiScale(hwnd, 10);
    drawStatusDot(dc, x + dot, legendY + legendHeight / 2, dot, upColour);
    legend = {x + dot * 2 + dpiScale(hwnd, 6), legendY, x + columnWidth, legendY + legendHeight};
    drawText(dc, L"↑ " + str::widen(str::humanRate(latest.txRate)), legend, smallFont(),
             GetSysColor(COLOR_WINDOWTEXT));

    if (haveData) {
        RECT peakRect{rc.right - dpiScale(hwnd, 150), legendY, rc.right - pad,
                      legendY + legendHeight};
        drawText(dc, str::widen("peak " + str::humanRate(peak)), peakRect, smallFont(), dimText(),
                 DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK graphProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) { paintGraph(hwnd); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── detail pane ──────────────────────────────────────────────────────────────

/// Lays out the pane's own children. Everything here is a real control, so nothing depends on
/// paint order to be visible.
void layoutDetail(HWND pane) {
    // WM_SIZE reaches the pane while it is still being created, before its children exist.
    if (!g_state.title || !g_state.graph) return;

    RECT client{};
    GetClientRect(pane, &client);
    const int pad = dpiScale(pane, metrics::kPad);
    const int gap = dpiScale(pane, metrics::kGap);
    const int width = client.right - pad * 2;
    if (width <= 0) return;

    int y = pad;
    auto place = [&](HWND control, int height, int controlWidth = 0) {
        MoveWindow(control, pad, y, controlWidth ? controlWidth : width, height, TRUE);
        y += height;
    };

    place(g_state.title, dpiScale(pane, metrics::kTitleHeight));
    place(g_state.subtitle, dpiScale(pane, metrics::kSubtitleHeight));
    y += gap;
    place(g_state.status, dpiScale(pane, metrics::kStatusHeight));
    y += gap;
    place(g_state.connect, dpiScale(pane, metrics::kButtonHeight),
          dpiScale(pane, metrics::kButtonWidth));
    y += gap;

    if (g_state.errorVisible) {
        place(g_state.error, dpiScale(pane, metrics::kErrorHeight));
        y += gap;
    } else {
        MoveWindow(g_state.error, pad, y, width, 0, TRUE);
    }

    const int graphHeight = dpiScale(pane, metrics::kGraphHeight);
    const int graphTop = client.bottom - pad - graphHeight;
    const int statsHeight = std::max<int>(graphTop - gap - y, 0);
    MoveWindow(g_state.stats, pad, y, width, statsHeight, TRUE);
    MoveWindow(g_state.graph, pad, graphTop, width, graphHeight, TRUE);
}

void paintDetail(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

    auto& ctx = *g_state.ctx;
    if (!g_state.stats || !ctx.findTunnel(ctx.selectedId)) {
        if (!ctx.findTunnel(ctx.selectedId))
            drawText(dc, loc::w("Select a tunnel"), client, uiFont(), dimText(),
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        EndPaint(hwnd, &ps);
        return;
    }

    // Separator above the statistics block, matching the macOS overview.
    RECT statsRect{};
    GetWindowRect(g_state.stats, &statsRect);
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&statsRect), 2);
    const int pad = dpiScale(hwnd, metrics::kPad);
    RECT line{pad, statsRect.top - dpiScale(hwnd, 6), client.right - pad,
              statsRect.top - dpiScale(hwnd, 6) + 1};
    FillRect(dc, &line, GetSysColorBrush(COLOR_BTNFACE));

    // The coloured status dot, drawn to the left of the status label.
    RECT statusRect{};
    GetWindowRect(g_state.status, &statusRect);
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&statusRect), 2);
    const int radius = dpiScale(hwnd, metrics::kDotRadius);
    drawStatusDot(dc, statusRect.left + radius, (statusRect.top + statusRect.bottom) / 2, radius,
                  g_state.statusColour);

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK detailProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            paintDetail(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            layoutDetail(hwnd);
            return 0;
        case WM_COMMAND:
            // The pane owns the button; the main window owns the behaviour.
            return SendMessageW(GetParent(hwnd), WM_COMMAND, wParam, lParam);
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            if (control == g_state.status)        SetTextColor(dc, g_state.statusColour);
            else if (control == g_state.subtitle) SetTextColor(dc, dimText());
            else if (control == g_state.error)    SetTextColor(dc, phaseColor(TunnelPhase::Failed));
            else                                  SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/// Push the selected tunnel's data into the pane's controls.
void refreshDetail(AppContext& ctx) {
    const auto* t = ctx.findTunnel(ctx.selectedId);
    const int show = t ? SW_SHOW : SW_HIDE;
    for (HWND control : {g_state.title, g_state.subtitle, g_state.status, g_state.connect,
                         g_state.stats, g_state.graph})
        ShowWindow(control, show);

    if (!t) {
        ShowWindow(g_state.error, SW_HIDE);
        InvalidateRect(g_state.detail, nullptr, TRUE);
        return;
    }

    const auto* rt = ctx.findRuntime(t->id);
    const auto phase = rt ? rt->phase : TunnelPhase::Stopped;

    setTextIfChanged(g_state.title, str::widen(t->name));
    setTextIfChanged(g_state.subtitle, str::widen(kindLabel(t->kind)));
    // Leading spaces leave room for the dot the pane paints.
    setTextIfChanged(g_state.status, L"    " + phaseText(phase));
    setTextIfChanged(g_state.connect, tunnelIsUp(rt) ? loc::w("Stop") : loc::w("Start"));
    EnableWindow(g_state.connect, !tunnelIsBusy(rt));

    if (const COLORREF colour = phaseColor(phase); colour != g_state.statusColour) {
        g_state.statusColour = colour;
        InvalidateRect(g_state.status, nullptr, TRUE);
        InvalidateRect(g_state.detail, nullptr, FALSE);
    }

    const bool showError = rt && !rt->errorMessage.empty();
    if (showError) setTextIfChanged(g_state.error, str::widen(rt->errorMessage));
    if (showError != g_state.errorVisible) {
        g_state.errorVisible = showError;
        ShowWindow(g_state.error, showError ? SW_SHOW : SW_HIDE);
        layoutDetail(g_state.detail);
    }

    InvalidateRect(g_state.stats, nullptr, FALSE);
    InvalidateRect(g_state.graph, nullptr, FALSE);
}

// ── toolbar / layout ─────────────────────────────────────────────────────────

/// Ask each button how wide it wants to be, so labels are never clipped in any language.
void measureToolbar(HWND hwnd) {
    const int minWidth = dpiScale(hwnd, 90);
    const int padding = dpiScale(hwnd, 24);
    for (auto& b : g_state.toolbar) {
        SIZE ideal{};
        if (SendMessageW(b.hwnd, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&ideal)) &&
            ideal.cx > 0)
            b.width = std::max<int>(minWidth, static_cast<int>(ideal.cx) + padding);
        else
            b.width = minWidth;
    }
}

void updateActionStates(AppContext& ctx) {
    const bool haveSelection = ctx.findTunnel(ctx.selectedId) != nullptr;
    for (const auto& b : g_state.toolbar) {
        if (b.id == IDC_EDIT || b.id == IDC_DELETE) EnableWindow(b.hwnd, haveSelection);
        else if (b.id == IDC_STOPALL) EnableWindow(b.hwnd, anyTunnelRunning(ctx));
    }
}

void layout(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int pad = dpiScale(hwnd, metrics::kPad);
    const int gap = dpiScale(hwnd, metrics::kGap);
    const int toolbarHeight = dpiScale(hwnd, metrics::kToolbarHeight);
    int y = pad;

    ShowWindow(g_state.helperText, g_state.helperBarVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_state.helperButton, g_state.helperBarVisible ? SW_SHOW : SW_HIDE);
    if (g_state.helperBarVisible) {
        const int buttonWidth = dpiScale(hwnd, 230);
        const int barHeight = dpiScale(hwnd, metrics::kHelperBarHeight);
        MoveWindow(g_state.helperText, pad, y,
                   std::max<int>(client.right - pad * 2 - buttonWidth - gap, 0), barHeight, TRUE);
        MoveWindow(g_state.helperButton, client.right - pad - buttonWidth,
                   y + (barHeight - toolbarHeight) / 2, buttonWidth, toolbarHeight, TRUE);
        y += barHeight + gap;
    }

    measureToolbar(hwnd);
    int leftX = pad;
    int rightX = client.right - pad;
    for (auto it = g_state.toolbar.rbegin(); it != g_state.toolbar.rend(); ++it) {
        if (!it->trailing) continue;
        rightX -= it->width;
        MoveWindow(it->hwnd, rightX, y, it->width, toolbarHeight, TRUE);
        rightX -= gap;
    }
    for (auto& b : g_state.toolbar) {
        if (b.trailing) continue;
        MoveWindow(b.hwnd, leftX, y, b.width, toolbarHeight, TRUE);
        leftX += b.width + gap;
    }
    y += toolbarHeight + gap;

    const int listWidth = std::min<int>(dpiScale(hwnd, metrics::kListWidth),
                                        static_cast<int>(client.right - pad * 2) * 2 / 5);
    const int bodyHeight = std::max<int>(client.bottom - y - pad, 0);
    MoveWindow(g_state.list, pad, y, listWidth, bodyHeight, TRUE);
    sizeListColumns(g_state.list);

    const int detailX = pad + listWidth + gap;
    MoveWindow(g_state.detail, detailX, y, std::max<int>(client.right - pad - detailX, 0),
               bodyHeight, TRUE);
}

// ── actions ──────────────────────────────────────────────────────────────────

void toggleSelected(AppContext& ctx) {
    if (ctx.selectedId.empty()) return;
    const auto* rt = ctx.findRuntime(ctx.selectedId);
    if (tunnelIsBusy(rt)) return;
    if (tunnelIsUp(rt)) stopTunnel(ctx, ctx.selectedId);
    else startTunnel(ctx, ctx.selectedId);
}

void onCommand(HWND hwnd, int id) {
    auto& ctx = *g_state.ctx;

    if (id >= IDM_TRAY_TUNNEL_BASE) {
        const size_t index = static_cast<size_t>(id - IDM_TRAY_TUNNEL_BASE);
        if (index >= ctx.tunnels.size()) return;
        const auto& t = ctx.tunnels[index];
        const auto* rt = ctx.findRuntime(t.id);
        if (tunnelIsUp(rt) || tunnelIsBusy(rt)) stopTunnel(ctx, t.id);
        else startTunnel(ctx, t.id);
        return;
    }

    switch (id) {
        case IDC_CONNECT:
            toggleSelected(ctx);
            break;
        case IDC_STOPALL:
        case IDM_TRAY_STOPALL:
            stopAllTunnels(ctx);
            break;
        case IDC_IMPORT:
            runImport(ctx);
            refreshList(ctx);
            refreshDetail(ctx);
            break;
        case IDC_EDIT:
            if (!ctx.selectedId.empty()) {
                showEditorDialog(ctx, ctx.selectedId);
                ctx.reloadTunnels();
                refreshList(ctx);
                refreshDetail(ctx);
            }
            break;
        case IDC_DELETE: {
            const auto* t = ctx.findTunnel(ctx.selectedId);
            if (!t) break;
            const auto text = loc::w("Delete this tunnel?") + L"\n\n" + str::widen(t->name) +
                              L"\n\n" + loc::w("This removes its configuration and stored keys.");
            if (MessageBoxW(hwnd, text.c_str(), loc::w("Delete").c_str(),
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                stopTunnel(ctx, t->id);
                ctx.store.deleteTunnel(t->id);
                ctx.selectedId.clear();
                ctx.reloadTunnels();
                refreshList(ctx);
                refreshDetail(ctx);
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
    updateActionStates(ctx);
    refreshDetail(ctx);
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
    for (auto it = ctx.history.begin(); it != ctx.history.end();)
        it = fresh.count(it->first) ? std::next(it) : ctx.history.erase(it);
    for (auto it = ctx.lastCounters.begin(); it != ctx.lastCounters.end();)
        it = fresh.count(it->first) ? std::next(it) : ctx.lastCounters.erase(it);

    ctx.runtime = std::move(fresh);
}

// ── window creation ──────────────────────────────────────────────────────────

HWND makeButton(HWND parent, int id, const std::wstring& text, DWORD extraStyle = 0) {
    HWND b = CreateWindowExW(0, L"BUTTON", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extraStyle,
                             0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return b;
}

HWND makeLabel(HWND parent, int id, HFONT font, DWORD extraStyle = 0) {
    HWND s = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | extraStyle,
                             0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(s, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return s;
}

void createDetailChildren(HWND pane) {
    g_state.title = makeLabel(pane, IDC_TITLE, titleFont(), SS_ENDELLIPSIS);
    g_state.subtitle = makeLabel(pane, IDC_SUBTITLE, smallFont(), SS_ENDELLIPSIS);
    g_state.status = makeLabel(pane, IDC_STATUS, uiFont(), SS_ENDELLIPSIS);
    g_state.connect = makeButton(pane, IDC_CONNECT, loc::w("Start"), BS_DEFPUSHBUTTON);
    g_state.error = makeLabel(pane, IDC_ERROR, smallFont());
    ShowWindow(g_state.error, SW_HIDE);

    g_state.stats = CreateWindowExW(0, L"TunHubStats", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                    pane, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATS)),
                                    nullptr, nullptr);
    g_state.graph = CreateWindowExW(0, L"TunHubGraph", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                    pane, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GRAPH)),
                                    nullptr, nullptr);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& ctx = *g_state.ctx;

    switch (msg) {
        case WM_CREATE: {
            g_state.main = hwnd;

            g_state.helperText = CreateWindowExW(
                0, L"STATIC",
                (loc::w("System component not running") + L" — " +
                 loc::w("TunHub needs a background service to manage tunnels.")).c_str(),
                WS_CHILD | SS_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HELPERTEXT)), nullptr, nullptr);
            SendMessageW(g_state.helperText, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
            g_state.helperButton = makeButton(hwnd, IDC_HELPERBTN,
                                              loc::w("Install system component"));
            ShowWindow(g_state.helperButton, SW_HIDE);

            g_state.toolbar = {
                {IDC_IMPORT,    "Import…"},
                {IDC_EDIT,      "Edit"},
                {IDC_DELETE,    "Delete"},
                {IDC_STOPALL,   "Stop all"},
                {IDC_CONFLICTS, "Check conflicts", nullptr, 0, true},
                {IDC_LOGS,      "Logs",            nullptr, 0, true},
                {IDC_SETTINGS,  "Settings",        nullptr, 0, true},
            };
            for (auto& b : g_state.toolbar) b.hwnd = makeButton(hwnd, b.id, loc::w(b.key));

            g_state.list = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                    LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                nullptr, nullptr);
            SendMessageW(g_state.list, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
            setupList(g_state.list);

            g_state.detail = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"TunHubDetailPane", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DETAIL)), nullptr, nullptr);
            createDetailChildren(g_state.detail);

            g_state.iconIdle = makeStatusIcon(false);
            g_state.iconActive = makeStatusIcon(true);
            updateTrayIcon(hwnd);

            SetTimer(hwnd, kPollTimer, 1000, nullptr);
            return 0;
        }

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            // Below this the panes stop making sense and controls start overlapping.
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = dpiScale(hwnd, 820);
            info->ptMinTrackSize.y = dpiScale(hwnd, 520);
            return 0;
        }

        case WM_DPICHANGED: {
            for (HFONT* font : {&g_uiFont, &g_titleFont, &g_smallFont}) {
                if (*font && *font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(*font);
                *font = nullptr;
            }
            for (const auto& b : g_state.toolbar)
                SendMessageW(b.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
            for (HWND child : {g_state.list, g_state.helperText, g_state.helperButton,
                               g_state.status, g_state.connect})
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
            SendMessageW(g_state.title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont()), TRUE);
            SendMessageW(g_state.subtitle, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont()), TRUE);
            SendMessageW(g_state.error, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont()), TRUE);

            auto* target = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, target->left, target->top,
                         target->right - target->left, target->bottom - target->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kPollTimer) {
                poll(ctx);
                if (const bool showBar = !ctx.helperReachable; showBar != g_state.helperBarVisible) {
                    g_state.helperBarVisible = showBar;
                    layout(hwnd);
                }
                refreshList(ctx);
                updateActionStates(ctx);
                refreshDetail(ctx);
                updateTrayIcon(hwnd);
            }
            return 0;

        case WM_COMMAND:
            onCommand(hwnd, LOWORD(wParam));
            return 0;

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<LPNMHDR>(lParam);
            if (header->idFrom != IDC_LIST) return 0;

            if (header->code == NM_CUSTOMDRAW)
                return handleListCustomDraw(ctx, reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam));

            if (header->code == LVN_ITEMCHANGED) {
                auto* view = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if (view->uNewState & LVIS_SELECTED) {
                    const size_t index = static_cast<size_t>(view->iItem);
                    if (index < ctx.tunnels.size()) {
                        ctx.selectedId = ctx.tunnels[index].id;
                        updateActionStates(ctx);
                        refreshDetail(ctx);
                        InvalidateRect(g_state.detail, nullptr, TRUE);
                    }
                }
            } else if (header->code == NM_DBLCLK) {
                toggleSelected(ctx);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));

        case WM_TUNHUB_TRAY:
            if (LOWORD(lParam) == WM_LBUTTONUP) showMainWindow(ctx);
            else if (LOWORD(lParam) == WM_RBUTTONUP) showTrayMenu(hwnd);
            return 0;

        case WM_CLOSE: {
            if (!anyTunnelRunning(ctx)) { DestroyWindow(hwnd); return 0; }
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

// ── theming ──────────────────────────────────────────────────────────────────

int dpiScale(HWND hwnd, int value) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0) {
        HDC screen = GetDC(nullptr);
        dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSY));
        ReleaseDC(nullptr, screen);
    }
    if (dpi == 0) dpi = 96;
    return MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT uiFont() {
    if (!g_uiFont) {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            g_uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!g_uiFont) g_uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return g_uiFont;
}

/// Derive a variant of the shell font rather than naming a family, so the app follows the
/// system's font choice and any accessibility overrides.
HFONT derivedFont(int heightDelta, int weight) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        return uiFont();
    LOGFONTW lf = metrics.lfMessageFont;
    lf.lfHeight -= heightDelta;   // lfHeight is negative; grow away from zero
    lf.lfWeight = weight;
    HFONT font = CreateFontIndirectW(&lf);
    return font ? font : uiFont();
}

HFONT titleFont() {
    if (!g_titleFont) g_titleFont = derivedFont(7, FW_SEMIBOLD);
    return g_titleFont;
}

HFONT smallFont() {
    if (!g_smallFont) g_smallFont = derivedFont(-1, FW_NORMAL);
    return g_smallFont;
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
        case TunnelPhase::Stopping: return RGB(200, 135, 30);
        case TunnelPhase::Failed:   return RGB(200, 60, 60);
        default:                    return RGB(130, 130, 130);
    }
}

void messageBox(HWND owner, const std::string& titleKey, const std::wstring& text, UINT flags) {
    MessageBoxW(owner, text.c_str(), loc::w(titleKey).c_str(), flags);
}

// ── construction ─────────────────────────────────────────────────────────────

bool createMainWindow(AppContext& ctx) {
    g_state.ctx = &ctx;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    struct { const wchar_t* name; WNDPROC proc; } childClasses[] = {
        {L"TunHubDetailPane", detailProc},
        {L"TunHubStats", statsProc},
        {L"TunHubGraph", graphProc},
    };
    for (const auto& c : childClasses) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = c.proc;
        wc.hInstance = ctx.instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = c.name;
        RegisterClassExW(&wc);
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ctx.instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TunHubMainWindow";
    wc.hIcon = LoadIconW(ctx.instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return false;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"TunHub", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1040, 660,
                                nullptr, nullptr, ctx.instance, nullptr);
    if (!hwnd) return false;
    ctx.mainWindow = hwnd;

    ctx.reloadTunnels();
    refreshList(ctx);
    updateActionStates(ctx);
    layout(hwnd);
    refreshDetail(ctx);
    return true;
}

void showMainWindow(AppContext& ctx) {
    if (!ctx.mainWindow) return;
    ShowWindow(ctx.mainWindow, SW_SHOW);
    SetForegroundWindow(ctx.mainWindow);
}

}  // namespace tunhub::app
