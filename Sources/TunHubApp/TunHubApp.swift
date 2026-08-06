import SwiftUI
import AppKit
import Combine
import TunHubShared

@main
struct TunHubApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        // Windows are managed by AppKit (WindowManager) so the status item can distinguish
        // left-click (open window) from right-click (menu). Settings stays a SwiftUI scene.
        Settings {
            SettingsView()
                .environmentObject(appDelegate.state)
        }
    }
}

/// Owns the menu-bar status item, popover, and the main/logs windows.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let state = AppState()
    private var statusItem: NSStatusItem!
    private let popover = NSPopover()
    private var cancellables = Set<AnyCancellable>()
    /// Last symbol name shown in the status item — `objectWillChange` fires many times per
    /// second (history tick, runtime refresh…) and `NSImage(systemSymbolName:)` is not free,
    /// so we skip the work when the icon would not actually change.
    private var lastIconName: String?

    func applicationDidFinishLaunching(_ notification: Notification) {
        LanguageManager.applySavedOnLaunch()   // re-assert saved interface language
        WindowManager.shared.state = state

        // Menu-bar status item — left click opens window, right click shows the menu popover.
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "shield", accessibilityDescription: "TunHub")
            button.image?.isTemplate = true
            button.target = self
            button.action = #selector(statusClicked(_:))
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        }

        popover.behavior = .transient

        // React to state changes → update the status-item icon.
        // NB: updateIcon() is a no-op when the symbol wouldn't change, which is the common
        // case — the icon only depends on (anyUp, anyDegraded, daemonVersionStatus) and not
        // on the per-second runtime/history churn that drives most objectWillChange fires.
        state.objectWillChange
            .receive(on: RunLoop.main)
            .sink { [weak self] in DispatchQueue.main.async { self?.updateIcon() } }
            .store(in: &cancellables)
        updateIcon()
    }

    private func updateIcon() {
        guard let button = statusItem.button else { return }
        let name: String
        if state.daemonVersionStatus.isProblem { name = "exclamationmark.shield.fill" }
        else if state.anyDegraded { name = "exclamationmark.shield" }
        else if state.anyUp { name = "shield.fill" }
        else { name = "shield" }
        guard name != lastIconName else { return }
        lastIconName = name
        button.image = NSImage(systemSymbolName: name, accessibilityDescription: "TunHub")
        button.image?.isTemplate = true
    }

    @objc private func statusClicked(_ sender: NSStatusBarButton) {
        let event = NSApp.currentEvent
        let isRight = event?.type == .rightMouseUp
            || event?.modifierFlags.contains(.control) == true
        if isRight {
            togglePopover(sender)
        } else {
            popover.performClose(nil)
            WindowManager.shared.showMain()
        }
    }

    private func togglePopover(_ sender: NSStatusBarButton) {
        if popover.isShown {
            popover.performClose(nil)
        } else {
            // Rebuild the popover's SwiftUI tree at open time. The popover is hidden for
            // the vast majority of the app's life; keeping a live NSHostingController bound
            // to AppState meant every objectWillChange fire (per-poll runtime/history churn)
            // drove a full SwiftUI diff of the menu contents — for nothing, since nobody
            // was looking. Recreating on open is essentially free and gives a fresh snapshot.
            popover.contentViewController = NSHostingController(
                rootView: MenuBarView().environmentObject(state))
            NSApp.activate(ignoringOtherApps: true)
            popover.show(relativeTo: sender.bounds, of: sender, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }

    func closePopover() { popover.performClose(nil) }

    /// On quit, if any tunnel is connected, ask whether to disconnect them first.
    /// Yes → stop all, then quit. No → quit and leave the tunnels running. Cancel → stay.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard state.anyRunning else {
            state.persistOnQuit()
            return .terminateNow
        }
        // Present the prompt on the NEXT runloop tick. terminate() is often triggered from
        // inside menu/popover tracking (the tray "Quit" item); running an NSAlert modal in that
        // context silently fails to show and the quit appears to do nothing. Deferring lets the
        // menu tracking end first, so the alert reliably appears. We hold termination with
        // .terminateLater and reply once the user chooses.
        DispatchQueue.main.async { [weak self] in
            guard let self else { NSApp.reply(toApplicationShouldTerminate: true); return }
            self.closePopover()
            NSApp.setActivationPolicy(.regular)
            NSApp.activate(ignoringOtherApps: true)

            let alert = NSAlert()
            alert.messageText = NSLocalizedString("Disconnect all tunnels before quitting?",
                                                  comment: "quit prompt title")
            alert.informativeText = NSLocalizedString(
                "Some tunnels are still connected. You can disconnect them now or leave them running.",
                comment: "quit prompt body")
            alert.alertStyle = .warning
            alert.addButton(withTitle: NSLocalizedString("Disconnect and quit", comment: ""))
            alert.addButton(withTitle: NSLocalizedString("Quit, keep running", comment: ""))
            alert.addButton(withTitle: NSLocalizedString("Cancel", comment: ""))

            switch alert.runModal() {
            case .alertFirstButtonReturn:             // Disconnect and quit
                Task { @MainActor in
                    await self.state.stopAll()
                    self.state.persistOnQuit()
                    NSApp.reply(toApplicationShouldTerminate: true)
                }
            case .alertSecondButtonReturn:            // Quit, keep tunnels running
                self.state.persistOnQuit()
                NSApp.reply(toApplicationShouldTerminate: true)
            default:                                  // Cancel
                NSApp.reply(toApplicationShouldTerminate: false)
            }
        }
        return .terminateLater
    }
}

/// Central place to show AppKit-hosted SwiftUI windows (main + logs).
@MainActor
final class WindowManager: NSObject, NSWindowDelegate {
    static let shared = WindowManager()
    var state: AppState?

    private var mainWindow: NSWindow?
    private var logsWindow: NSWindow?

    func showMain() {
        // state is wired in AppState.init(); if a very early caller beats it, retry shortly.
        guard let state else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in self?.showMain() }
            return
        }
        NSApp.setActivationPolicy(.regular)   // show a Dock icon while a window is open
        if let w = mainWindow {
            activate(w); return
        }
        let w = makeWindow(title: "TunHub",
                           size: NSSize(width: 900, height: 560),
                           content: MainWindow().environmentObject(state))
        w.setFrameAutosaveName("TunHubMain")
        mainWindow = w
        activate(w)
        windowVisibilityChanged(state: state)
    }

    func showLogs() {
        guard let state else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in self?.showLogs() }
            return
        }
        NSApp.setActivationPolicy(.regular)
        if let w = logsWindow {
            activate(w); return
        }
        let w = makeWindow(title: "TunHub Logs",
                           size: NSSize(width: 820, height: 460),
                           content: LogView().environmentObject(state))
        w.setFrameAutosaveName("TunHubLogs")
        logsWindow = w
        activate(w)
        windowVisibilityChanged(state: state)
    }

    /// Central place to recalculate `anyWindowVisible` and resume/suspend the poll loop.
    /// Called on every show / close. Driving the flag from here (rather than KVO on
    /// NSWindow.isVisible) keeps the logic in one place and lets us wake the loop the
    /// instant a window opens, so the user never waits for fresh state.
    private func windowVisibilityChanged(state: AppState) {
        let visible = (mainWindow?.isVisible ?? false) || (logsWindow?.isVisible ?? false)
        let wasVisible = state.anyWindowVisible
        state.anyWindowVisible = visible
        if visible {
            // A window just appeared — poll now so the user sees current state instead of a
            // stale snapshot, and make sure the loop is running at the visible-window cadence.
            state.pollNowAndResume()
        } else if wasVisible {
            // The last window closed. If there are no active tunnels, this transitions the
            // app into fully-suspended idle (the loop will cancel itself); if there are, the
            // loop simply slows down to the "background watch" cadence.
            state.reevaluatePolling()
        }
    }

    /// Bring the app+window fully to the foreground. From an accessory (menu-bar) app the
    /// activation right after switching to `.regular` often doesn't "stick" — the window becomes
    /// key (colored traffic lights) but the app stays inactive, so NSVisualEffectView materials
    /// render in their desaturated inactive state (the window looks greyed-out). Re-asserting the
    /// activation on the next runloop tick fixes it.
    private func activate(_ w: NSWindow) {
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        DispatchQueue.main.async {
            NSApp.activate(ignoringOtherApps: true)
            w.makeKeyAndOrderFront(nil)
        }
    }

    private func makeWindow(title: String, size: NSSize, content: some View) -> NSWindow {
        let w = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                         styleMask: [.titled, .closable, .miniaturizable, .resizable],
                         backing: .buffered, defer: false)
        w.title = title
        // Unified toolbar style merges the SwiftUI `.toolbar` with the titlebar. We deliberately
        // avoid the legacy `.unifiedTitleAndToolbar` style mask and opaque-titlebar tweaks — on
        // macOS 26 (Liquid Glass) those opt the window out of the system's glass chrome.
        w.toolbarStyle = .unified
        w.isReleasedWhenClosed = false      // keep it so we can re-show on next click
        w.center()
        w.contentViewController = NSHostingController(rootView: content)
        w.delegate = self
        return w
    }

    // Close just hides the window; the reference is kept for re-showing.
    func windowWillClose(_ notification: Notification) {
        state?.persistOnQuit()
        // Hide the Dock icon again once no TunHub window remains open (back to menu-bar-only).
        // Also recompute anyWindowVisible so the poll loop can slow back down to the idle
        // heartbeat — every 30 s instead of every 2 s — when nobody is watching.
        DispatchQueue.main.async { [weak self] in
            guard let self, let state = self.state else { return }
            let anyVisible = (self.mainWindow?.isVisible ?? false) || (self.logsWindow?.isVisible ?? false)
            if !anyVisible { NSApp.setActivationPolicy(.accessory) }
            self.windowVisibilityChanged(state: state)
        }
    }
}

// MARK: - Small shared views

struct PhaseDot: View {
    let phase: TunnelPhase?
    var body: some View {
        Circle().fill(color).frame(width: 9, height: 9)
    }
    var color: Color {
        switch phase {
        case .up: return .green
        case .degraded, .starting, .stopping: return .yellow
        case .failed: return .red
        default: return .secondary.opacity(0.4)
        }
    }
}

struct KindBadge: View {
    let kind: TunnelKind
    var label: String {
        switch kind {
        case .wireguard: return "WG"
        case .amneziawg: return "AWG"
        case .openvpn:   return "OVPN"
        }
    }
    var color: Color {
        switch kind {
        case .wireguard: return .blue
        case .amneziawg: return .orange
        case .openvpn:   return .green
        }
    }
    var body: some View {
        Text(label)
            .font(.caption2.bold())
            .padding(.horizontal, 5).padding(.vertical, 1)
            .background(color.opacity(0.2))
            .clipShape(Capsule())
    }
}

struct FindingRow: View {
    let finding: ConflictFinding
    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: icon).foregroundStyle(color)
            VStack(alignment: .leading, spacing: 2) {
                Text("\(finding.code): \(finding.message)").font(.callout)
                if let fix = finding.fixHint {
                    Text(fix).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }
    var icon: String {
        switch finding.severity {
        case .error: return "xmark.octagon.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .info: return "info.circle"
        }
    }
    var color: Color {
        switch finding.severity {
        case .error: return .red
        case .warning: return .yellow
        case .info: return .blue
        }
    }
}
