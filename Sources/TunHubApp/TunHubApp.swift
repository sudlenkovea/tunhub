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
        popover.contentViewController = NSHostingController(
            rootView: MenuBarView().environmentObject(state))

        // React to state changes → update the status-item icon.
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
        let alert = NSAlert()
        alert.messageText = NSLocalizedString("Disconnect all tunnels before quitting?",
                                              comment: "quit prompt title")
        alert.informativeText = NSLocalizedString(
            "Some tunnels are still connected. You can disconnect them now or leave them running.",
            comment: "quit prompt body")
        alert.alertStyle = .warning
        alert.addButton(withTitle: NSLocalizedString("Disconnect and quit", comment: ""))     // 1000
        alert.addButton(withTitle: NSLocalizedString("Quit, keep running", comment: ""))       // 1001
        alert.addButton(withTitle: NSLocalizedString("Cancel", comment: ""))                   // 1002

        NSApp.activate(ignoringOtherApps: true)
        switch alert.runModal() {
        case .alertFirstButtonReturn:                 // Disconnect and quit
            Task { @MainActor in
                await state.stopAll()
                state.persistOnQuit()
                NSApp.reply(toApplicationShouldTerminate: true)
            }
            return .terminateLater
        case .alertSecondButtonReturn:                // Quit, keep tunnels running
            state.persistOnQuit()
            return .terminateNow
        default:                                      // Cancel
            return .terminateCancel
        }
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
        NSApp.activate(ignoringOtherApps: true)
        if let w = mainWindow {
            w.makeKeyAndOrderFront(nil); return
        }
        let w = makeWindow(title: "TunHub",
                           size: NSSize(width: 900, height: 560),
                           content: MainWindow().environmentObject(state))
        w.setFrameAutosaveName("TunHubMain")
        mainWindow = w
        w.makeKeyAndOrderFront(nil)
    }

    func showLogs() {
        guard let state else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in self?.showLogs() }
            return
        }
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        if let w = logsWindow {
            w.makeKeyAndOrderFront(nil); return
        }
        let w = makeWindow(title: "TunHub Logs",
                           size: NSSize(width: 820, height: 460),
                           content: LogView().environmentObject(state))
        w.setFrameAutosaveName("TunHubLogs")
        logsWindow = w
        w.makeKeyAndOrderFront(nil)
    }

    private func makeWindow(title: String, size: NSSize, content: some View) -> NSWindow {
        let w = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                         styleMask: [.titled, .closable, .miniaturizable, .resizable],
                         backing: .buffered, defer: false)
        w.title = title
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
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            let anyVisible = (self.mainWindow?.isVisible ?? false) || (self.logsWindow?.isVisible ?? false)
            if !anyVisible { NSApp.setActivationPolicy(.accessory) }
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
