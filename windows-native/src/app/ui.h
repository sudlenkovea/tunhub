#pragma once
// Shared UI declarations.

#include <windows.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "loc.h"
#include "store.h"
#include "tunhub/ipc.h"
#include "tunhub/models.h"

namespace tunhub::app {

/// One traffic sample; the overview graph plots a short window of these.
struct Sample {
    double rxRate = 0;
    double txRate = 0;
};

/// Everything the windows share. One instance, owned by WinMain.
struct AppContext {
    Store store;
    AppSettings settings;
    ipc::Client daemon;

    std::vector<TunnelConfig> tunnels;
    std::map<std::string, TunnelRuntimeState> runtime;
    std::map<std::string, std::deque<Sample>> history;
    std::map<std::string, std::pair<uint64_t, uint64_t>> lastCounters;  // id → (rx, tx)

    bool helperReachable = false;
    std::string selectedId;

    HWND mainWindow = nullptr;
    HINSTANCE instance = nullptr;

    /// Chart window: at 1 Hz this is two minutes, which is all a strip this size can show.
    static constexpr size_t kMaxSamples = 120;

    const TunnelConfig* findTunnel(const std::string& id) const;
    const TunnelRuntimeState* findRuntime(const std::string& id) const;
    void reloadTunnels();
};

// ── windows ──────────────────────────────────────────────────────────────────

bool createMainWindow(AppContext& ctx);
void showMainWindow(AppContext& ctx);

void showLogWindow(AppContext& ctx);
void showSettingsDialog(AppContext& ctx);
void showEditorDialog(AppContext& ctx, const std::string& tunnelId);
void showConflictsDialog(AppContext& ctx);
void runImport(AppContext& ctx);

/// Modal credential prompt for OpenVPN. Returns false when the user cancels.
bool promptCredentials(AppContext& ctx, const TunnelConfig& config, std::string& username,
                       std::string& password, std::string& otp, bool& save);

// ── helpers shared by the windows ────────────────────────────────────────────

void startTunnel(AppContext& ctx, const std::string& id);
void stopTunnel(AppContext& ctx, const std::string& id);
void stopAllTunnels(AppContext& ctx);
bool installHelper(AppContext& ctx);

std::wstring phaseText(TunnelPhase phase);
COLORREF phaseColor(TunnelPhase phase);

void messageBox(HWND owner, const std::string& titleKey, const std::wstring& text,
                UINT flags = MB_OK | MB_ICONINFORMATION);

// ── theming ──────────────────────────────────────────────────────────────────
// Everything is derived from the shell's own message font and DPI, so the app matches the
// rest of the system instead of hard-coding sizes that break on scaled displays.

/// Body text (the shell's message font).
HFONT uiFont();
/// Larger, semibold — the selected tunnel's name.
HFONT titleFont();
/// Smaller, dimmed — captions and secondary values.
HFONT smallFont();

/// Scale a design-time pixel value to the window's DPI.
int dpiScale(HWND hwnd, int value);

}  // namespace tunhub::app
