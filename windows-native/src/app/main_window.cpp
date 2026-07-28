// Main window: a tunnel list on the left and a detail pane on the right, mirroring the macOS
// layout. The list carries only identity and state; everything about the *selected* tunnel —
// status, the connect action, statistics and the traffic strip — lives in the detail pane.

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
    IDC_DETAIL, IDC_CONNECT, IDC_GRAPH, IDC_HELPERTEXT, IDC_HELPERBTN,
    IDM_TRAY_OPEN = 2000, IDM_TRAY_STOPALL, IDM_TRAY_QUIT, IDM_TRAY_TUNNEL_BASE = 2100,
};

/// Design-time metrics, scaled to the window's DPI at layout time.
namespace metrics {
constexpr int kPad = 12;           // window padding
constexpr int kGap = 8;            // gap between controls
constexpr int kToolbarHeight = 32;
constexpr int kListWidth = 300;
constexpr int kGraphHeight = 132;
constexpr int kHelperBarHeight = 52;
constexpr int kRowHeight = 24;     // one statistic row in the detail pane
constexpr int kDotRadius = 5;
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
    HWND list = nullptr;
    HWND detail = nullptr;
    HWND connect = nullptr;
    HWND graph = nullptr;
    HWND helperText = nullptr;
    HWND helperButton = nullptr;
    std::vector<ToolbarButton> toolbar;
    bool helperBarVisible = false;

    NOTIFYICONDATAW tray{};
    bool trayAdded = false;
    HICON iconIdle = nullptr;
    HICON iconActive = nullptr;
    bool lastActive = false;
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

/// Filled circle used as the status indicator, the same idiom as the macOS list.
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

COLORREF dimText() {
    // Blend the window text toward the background: the shell has no dedicated "secondary
    // text" colour, and GrayText is reserved for disabled controls.
    const COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF bg = GetSysColor(COLOR_WINDOW);
    auto mix = [](BYTE a, BYTE b) { return static_cast<BYTE>((a * 45 + b * 55) / 100); };
    return RGB(mix(GetRValue(fg), GetRValue(bg)), mix(GetGValue(fg), GetGValue(bg)),
               mix(GetBValue(fg), GetBValue(bg)));
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

void setupList(HWND hwnd, HWND list) {
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    // Identity and state only. Everything else belongs to the detail pane, the way the macOS
    // sidebar works — a list crammed with statistics is unreadable at a glance.
    struct { const char* key; int width; } columns[] = {
        {"Name", 150}, {"Type", 80}, {"Status", 66},
    };
    int index = 0;
    for (const auto& c : columns) {
        auto title = loc::w(c.key);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = title.data();
        col.cx = dpiScale(hwnd, c.width);
        col.iSubItem = index;
        ListView_InsertColumn(list, index, &col);
        ++index;
    }
}

void refreshList(AppContext& ctx) {
    HWND list = g_state.list;
    // Rebuild only when the set of tunnels changed; otherwise update in place so the
    // selection, scroll position and focus survive the 1 Hz refresh.
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
        InvalidateRect(list, nullptr, TRUE);
        // Keep a selection so the detail pane is never empty for no reason.
        if (!ctx.tunnels.empty() && ctx.selectedId.empty()) {
            ctx.selectedId = ctx.tunnels.front().id;
            ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
    } else {
        InvalidateRect(list, nullptr, FALSE);
    }
}

/// Custom draw for the Status column: a coloured dot plus the phase, which reads far faster
/// than plain text and matches the macOS sidebar.
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

            RECT rc{};
            rc.left = LVIR_BOUNDS;
            rc.top = 2;   // subitem index
            SendMessageW(cd->nmcd.hdr.hwndFrom, LVM_GETSUBITEMRECT,
                         cd->nmcd.dwItemSpec, reinterpret_cast<LPARAM>(&rc));

            HDC dc = cd->nmcd.hdc;
            const int radius = dpiScale(cd->nmcd.hdr.hwndFrom, metrics::kDotRadius);
            const int centreY = (rc.top + rc.bottom) / 2;
            const int dotX = rc.left + radius + dpiScale(cd->nmcd.hdr.hwndFrom, 4);
            drawStatusDot(dc, dotX, centreY, radius, phaseColor(phase));

            RECT textRect = rc;
            textRect.left = dotX + radius + dpiScale(cd->nmcd.hdr.hwndFrom, 6);
            const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
            drawText(dc, phaseText(phase), textRect, uiFont(),
                     GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            return CDRF_SKIPDEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

// ── detail pane ──────────────────────────────────────────────────────────────

/// One "label / value" line.
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
    for (const auto& p : t.peers)
        if (p.endpoint) { add("Endpoint", str::widen(*p.endpoint)); break; }
    if (t.kind == TunnelKind::OpenVpn && t.openVpn)
        add("Endpoint", str::widen(t.openVpn->remoteSummary));

    add("Routes", t.hasDefaultRoute() ? loc::w("all traffic (default route)")
                                      : std::to_wstring(t.effectiveRoutes().size()));
    if (!t.iface.dns.empty()) add("DNS", str::widen(str::join(t.iface.dns, ", ")));
    if (rt && !rt->interfaceName.empty()) add("Interface", str::widen(rt->interfaceName));
    return rows;
}

/// Vertical offset of the connect button inside the detail pane. Shared by the painter and
/// the layout so the two can never disagree about where the button sits.
int connectButtonOffset(HWND hwnd) {
    return dpiScale(hwnd, metrics::kPad) +      // top padding
           dpiScale(hwnd, 30) +                 // title
           dpiScale(hwnd, 20) +                 // kind
           dpiScale(hwnd, metrics::kGap) +
           dpiScale(hwnd, 22) +                 // status line
           dpiScale(hwnd, metrics::kGap);
}

void paintDetail(HWND hwnd) {
    auto& ctx = *g_state.ctx;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

    const int pad = dpiScale(hwnd, metrics::kPad);
    const auto* t = ctx.findTunnel(ctx.selectedId);
    if (!t) {
        RECT rc = client;
        drawText(dc, loc::w("Select a tunnel"), rc, uiFont(), dimText(),
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        EndPaint(hwnd, &ps);
        return;
    }
    const auto* rt = ctx.findRuntime(t->id);
    const auto phase = rt ? rt->phase : TunnelPhase::Stopped;

    int y = pad;

    // Title: name over kind.
    RECT rc{pad, y, client.right - pad, y + dpiScale(hwnd, 30)};
    drawText(dc, str::widen(t->name), rc, titleFont(), GetSysColor(COLOR_WINDOWTEXT));
    y = rc.bottom;
    rc = {pad, y, client.right - pad, y + dpiScale(hwnd, 20)};
    drawText(dc, str::widen(kindLabel(t->kind)), rc, smallFont(), dimText());
    y = rc.bottom + dpiScale(hwnd, metrics::kGap);

    // Status line: coloured dot + phase.
    const int radius = dpiScale(hwnd, metrics::kDotRadius);
    const int lineHeight = dpiScale(hwnd, 22);
    drawStatusDot(dc, pad + radius, y + lineHeight / 2, radius, phaseColor(phase));
    rc = {pad + radius * 2 + dpiScale(hwnd, 8), y, client.right - pad, y + lineHeight};
    drawText(dc, phaseText(phase), rc, uiFont(), phaseColor(phase));

    // The connect button is a real child window placed by layout(); skip the band it occupies.
    y = connectButtonOffset(hwnd) + dpiScale(hwnd, 30) + dpiScale(hwnd, metrics::kGap);

    // Any error goes *below* the button, so its appearance never shifts the button.
    if (rt && !rt->errorMessage.empty()) {
        rc = {pad, y, client.right - pad, y + dpiScale(hwnd, 34)};
        drawText(dc, str::widen(rt->errorMessage), rc, smallFont(), phaseColor(TunnelPhase::Failed),
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        y = rc.bottom + dpiScale(hwnd, 4);
    }

    // Separator.
    RECT line{pad, y, client.right - pad, y + 1};
    FillRect(dc, &line, GetSysColorBrush(COLOR_BTNFACE));
    y += dpiScale(hwnd, metrics::kGap);

    // Statistics: labels in a fixed left column, values right of them.
    const int labelWidth = dpiScale(hwnd, 130);
    const int rowHeight = dpiScale(hwnd, metrics::kRowHeight);
    const int graphTop = client.bottom - pad - dpiScale(hwnd, metrics::kGraphHeight);
    for (const auto& row : buildStats(*t, rt)) {
        if (y + rowHeight > graphTop) break;   // never overlap the graph
        RECT labelRect{pad, y, pad + labelWidth, y + rowHeight};
        drawText(dc, row.label, labelRect, smallFont(), dimText());
        RECT valueRect{pad + labelWidth, y, client.right - pad, y + rowHeight};
        drawText(dc, row.value, valueRect, uiFont(), GetSysColor(COLOR_WINDOWTEXT));
        y += rowHeight;
    }

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK detailProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) { paintDetail(hwnd); return 0; }
    if (msg == WM_ERASEBKGND) return 1;   // painted in full by WM_PAINT
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
    FrameRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));

    const COLORREF downColour = RGB(58, 132, 214);
    const COLORREF upColour = RGB(214, 132, 48);
    const int pad = dpiScale(hwnd, 6);

    auto it = ctx.history.find(ctx.selectedId);
    const bool haveData = it != ctx.history.end() && it->second.size() >= 2;

    double peak = 1.0;
    if (haveData)
        for (const auto& s : it->second) peak = std::max({peak, s.rxRate, s.txRate});

    if (haveData) {
        const auto& samples = it->second;
        const int width = rc.right - rc.left - pad * 2;
        const int height = rc.bottom - rc.top - pad * 2 - dpiScale(hwnd, 14);
        const int top = rc.top + pad + dpiScale(hwnd, 14);
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

    // Legend: current rate per direction, so the strip is readable without axes.
    const Sample latest = haveData ? it->second.back() : Sample{};
    const int legendY = rc.top + pad;
    const int dot = dpiScale(hwnd, 3);
    int x = rc.left + pad;

    drawStatusDot(dc, x + dot, legendY + dpiScale(hwnd, 7), dot, downColour);
    RECT legend{x + dot * 2 + dpiScale(hwnd, 4), legendY, x + dpiScale(hwnd, 110),
                legendY + dpiScale(hwnd, 14)};
    drawText(dc, L"↓ " + str::widen(str::humanRate(latest.rxRate)), legend, smallFont(), dimText());

    x = legend.right + dpiScale(hwnd, 8);
    drawStatusDot(dc, x + dot, legendY + dpiScale(hwnd, 7), dot, upColour);
    legend = {x + dot * 2 + dpiScale(hwnd, 4), legendY, x + dpiScale(hwnd, 110),
              legendY + dpiScale(hwnd, 14)};
    drawText(dc, L"↑ " + str::widen(str::humanRate(latest.txRate)), legend, smallFont(), dimText());

    if (haveData) {
        RECT peakRect{rc.right - dpiScale(hwnd, 120), legendY, rc.right - pad,
                      legendY + dpiScale(hwnd, 14)};
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

// ── toolbar / layout ─────────────────────────────────────────────────────────

/// Ask each button how wide it wants to be, so labels are never clipped and the toolbar
/// stays proportionate in any language or DPI.
void measureToolbar(HWND hwnd) {
    const int minWidth = dpiScale(hwnd, 84);
    const int padding = dpiScale(hwnd, 20);
    for (auto& b : g_state.toolbar) {
        SIZE ideal{};
        if (SendMessageW(b.hwnd, BCM_GETIDEALSIZE, 0, reinterpret_cast<LPARAM>(&ideal)) &&
            ideal.cx > 0) {
            b.width = std::max(minWidth, static_cast<int>(ideal.cx) + padding);
        } else {
            b.width = minWidth;
        }
    }
}

void updateActionStates(AppContext& ctx) {
    const auto* rt = ctx.findRuntime(ctx.selectedId);
    const bool haveSelection = ctx.findTunnel(ctx.selectedId) != nullptr;
    const bool up = tunnelIsUp(rt);
    const bool busy = tunnelIsBusy(rt);

    EnableWindow(g_state.connect, haveSelection && !busy);
    ShowWindow(g_state.connect, haveSelection ? SW_SHOW : SW_HIDE);

    // Only touch the caption when it actually changes: this runs once a second, and
    // SetWindowText repaints unconditionally, which reads as a flicker.
    const std::wstring wanted = up ? loc::w("Stop") : loc::w("Start");
    wchar_t current[64]{};
    GetWindowTextW(g_state.connect, current, 64);
    if (wanted != current) SetWindowTextW(g_state.connect, wanted.c_str());

    for (const auto& b : g_state.toolbar) {
        if (b.id == IDC_EDIT || b.id == IDC_DELETE)
            EnableWindow(b.hwnd, haveSelection);
        else if (b.id == IDC_STOPALL)
            EnableWindow(b.hwnd, anyTunnelRunning(ctx));
    }
}

void layout(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int pad = dpiScale(hwnd, metrics::kPad);
    const int gap = dpiScale(hwnd, metrics::kGap);
    const int toolbarHeight = dpiScale(hwnd, metrics::kToolbarHeight);

    int y = pad;

    // Helper bar, shown only while the service is unreachable.
    ShowWindow(g_state.helperText, g_state.helperBarVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_state.helperButton, g_state.helperBarVisible ? SW_SHOW : SW_HIDE);
    if (g_state.helperBarVisible) {
        const int barHeight = dpiScale(hwnd, metrics::kHelperBarHeight);
        const int buttonWidth = dpiScale(hwnd, 200);
        MoveWindow(g_state.helperText, pad, y, client.right - pad * 2 - buttonWidth - gap,
                   barHeight, TRUE);
        MoveWindow(g_state.helperButton, client.right - pad - buttonWidth,
                   y + (barHeight - toolbarHeight) / 2, buttonWidth, toolbarHeight, TRUE);
        y += barHeight + gap;
    }

    // Toolbar: leading group on the left, utility group flushed right.
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

    // Split: list on the left, detail on the right.
    const int listWidth = std::min(dpiScale(hwnd, metrics::kListWidth),
                                   (client.right - pad * 2) / 2);
    const int bodyHeight = client.bottom - y - pad;
    MoveWindow(g_state.list, pad, y, listWidth, bodyHeight, TRUE);

    const int detailX = pad + listWidth + gap;
    const int detailWidth = client.right - pad - detailX;
    MoveWindow(g_state.detail, detailX, y, detailWidth, bodyHeight, TRUE);

    // The connect button sits over the detail pane, under the status line — the same offset
    // the painter skips.
    MoveWindow(g_state.connect, detailX + dpiScale(hwnd, metrics::kPad),
               y + connectButtonOffset(hwnd), dpiScale(hwnd, 132), dpiScale(hwnd, 30), TRUE);

    const int graphHeight = dpiScale(hwnd, metrics::kGraphHeight);
    MoveWindow(g_state.graph, detailX + dpiScale(hwnd, metrics::kPad),
               y + bodyHeight - pad - graphHeight,
               detailWidth - dpiScale(hwnd, metrics::kPad) * 2, graphHeight, TRUE);
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
            InvalidateRect(g_state.detail, nullptr, TRUE);
            break;
        case IDC_EDIT:
            if (!ctx.selectedId.empty()) {
                showEditorDialog(ctx, ctx.selectedId);
                ctx.reloadTunnels();
                refreshList(ctx);
                InvalidateRect(g_state.detail, nullptr, TRUE);
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
                InvalidateRect(g_state.detail, nullptr, TRUE);
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
    // Drop history for tunnels that stopped, so their graph doesn't freeze on the last value.
    for (auto it = ctx.history.begin(); it != ctx.history.end();)
        it = fresh.count(it->first) ? std::next(it) : ctx.history.erase(it);
    for (auto it = ctx.lastCounters.begin(); it != ctx.lastCounters.end();)
        it = fresh.count(it->first) ? std::next(it) : ctx.lastCounters.erase(it);

    ctx.runtime = std::move(fresh);
}

// ── window procedure ─────────────────────────────────────────────────────────

HWND makeButton(HWND parent, int id, const std::wstring& text, DWORD extraStyle = 0) {
    HWND b = CreateWindowExW(0, L"BUTTON", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extraStyle,
                             0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
    return b;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& ctx = *g_state.ctx;

    switch (msg) {
        case WM_CREATE: {
            g_state.helperText = CreateWindowExW(
                0, L"STATIC",
                (loc::w("System component not running") + L"\n" +
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
            setupList(hwnd, g_state.list);

            // WS_CLIPSIBLINGS: the detail pane paints its whole client area, and the connect
            // button and graph sit on top of it — without this they would be painted over.
            g_state.detail = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"TunHubDetailPane", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DETAIL)), nullptr, nullptr);

            // Primary action, placed next to the selected tunnel's status rather than in the
            // toolbar — it acts on that one tunnel, the way the macOS overview does it.
            g_state.connect = makeButton(hwnd, IDC_CONNECT, loc::w("Start"),
                                         BS_DEFPUSHBUTTON | WS_CLIPSIBLINGS);

            g_state.graph = CreateWindowExW(0, L"TunHubGraph", L"",
                                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                            hwnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GRAPH)),
                                            nullptr, nullptr);

            g_state.iconIdle = makeStatusIcon(false);
            g_state.iconActive = makeStatusIcon(true);
            updateTrayIcon(hwnd);

            SetTimer(hwnd, kPollTimer, 1000, nullptr);
            return 0;
        }

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case WM_DPICHANGED: {
            // Drop the cached fonts so they are rebuilt against the new scale, take the
            // suggested rectangle, then re-lay everything out.
            for (HFONT* font : {&g_uiFont, &g_titleFont, &g_smallFont}) {
                if (*font && *font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(*font);
                *font = nullptr;
            }
            for (const auto& b : g_state.toolbar)
                SendMessageW(b.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);
            for (HWND child : {g_state.list, g_state.connect, g_state.helperText,
                               g_state.helperButton})
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont()), TRUE);

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
                InvalidateRect(g_state.detail, nullptr, FALSE);
                InvalidateRect(g_state.graph, nullptr, FALSE);
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
                        InvalidateRect(g_state.detail, nullptr, TRUE);
                        InvalidateRect(g_state.graph, nullptr, TRUE);
                    }
                }
            } else if (header->code == NM_DBLCLK) {
                toggleSelected(ctx);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            // The helper bar sits on the window background, not on a white field.
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
    UINT dpi = 96;
    if (hwnd) {
        // GetDpiForWindow is per-monitor aware; the desktop DC is the fallback for callers
        // that run before the window exists.
        dpi = GetDpiForWindow(hwnd);
    }
    if (dpi == 0) {
        HDC screen = GetDC(nullptr);
        dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSY));
        ReleaseDC(nullptr, screen);
    }
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
HFONT derivedFont(int pointDelta, int weight) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        return uiFont();
    LOGFONTW lf = metrics.lfMessageFont;
    // lfHeight is negative (character height); grow away from zero.
    lf.lfHeight -= pointDelta;
    lf.lfWeight = weight;
    HFONT font = CreateFontIndirectW(&lf);
    return font ? font : uiFont();
}

HFONT titleFont() {
    if (!g_titleFont) g_titleFont = derivedFont(6, FW_SEMIBOLD);
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
        case TunnelPhase::Stopping: return RGB(215, 145, 40);
        case TunnelPhase::Failed:   return RGB(200, 60, 60);
        default:                    return RGB(140, 140, 140);
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

    // Child classes for the two owner-painted panes.
    WNDCLASSEXW detailClass{sizeof(detailClass)};
    detailClass.lpfnWndProc = detailProc;
    detailClass.hInstance = ctx.instance;
    detailClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    detailClass.lpszClassName = L"TunHubDetailPane";
    RegisterClassExW(&detailClass);

    WNDCLASSEXW graphClass{sizeof(graphClass)};
    graphClass.lpfnWndProc = graphProc;
    graphClass.hInstance = ctx.instance;
    graphClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    graphClass.lpszClassName = L"TunHubGraph";
    RegisterClassExW(&graphClass);

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
                                CW_USEDEFAULT, CW_USEDEFAULT, 960, 600,
                                nullptr, nullptr, ctx.instance, nullptr);
    if (!hwnd) return false;
    ctx.mainWindow = hwnd;

    ctx.reloadTunnels();
    refreshList(ctx);
    updateActionStates(ctx);
    layout(hwnd);
    return true;
}

void showMainWindow(AppContext& ctx) {
    if (!ctx.mainWindow) return;
    ShowWindow(ctx.mainWindow, SW_SHOW);
    SetForegroundWindow(ctx.mainWindow);
}

}  // namespace tunhub::app
