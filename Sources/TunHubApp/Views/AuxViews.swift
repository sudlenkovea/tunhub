import SwiftUI
import Combine
import AppKit
import ServiceManagement
import TunHubShared

// MARK: - Import preview

struct ImportPreviewView: View {
    @EnvironmentObject var state: AppState
    @Environment(\.dismiss) var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Import tunnels").font(.headline)

            if !state.importErrors.isEmpty {
                GroupBox("Errors") {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(state.importErrors, id: \.self) { e in
                                Label(e, systemImage: "xmark.circle")
                                    .foregroundStyle(.red).font(.callout)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .frame(maxHeight: 100)
                }
            }

            if state.importCandidates.isEmpty {
                Text("Nothing to import").foregroundStyle(.secondary)
            } else {
                List {
                    ForEach($state.importCandidates) { $c in
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Toggle("", isOn: $c.include).labelsHidden()
                                Text(c.parsed.config.name).bold()
                                KindBadge(kind: c.parsed.config.kind)
                                if c.parsed.config.hasDefaultRoute {
                                    Text("default route").font(.caption2)
                                        .padding(.horizontal, 4)
                                        .background(Color.purple.opacity(0.2))
                                        .clipShape(Capsule())
                                }
                                Spacer()
                                Text(c.parsed.config.peers.first?.endpoint ?? "")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            ForEach(c.parsed.warnings, id: \.self) { w in
                                Label(w, systemImage: "exclamationmark.triangle")
                                    .font(.caption).foregroundStyle(.yellow)
                            }
                            ForEach(c.findings.filter { $0.severity != .info }) { f in
                                FindingRow(finding: f).font(.caption)
                            }
                        }
                        .padding(.vertical, 2)
                    }
                }
                .frame(minHeight: 220, maxHeight: 360)
            }

            HStack {
                Text("\(state.importCandidates.filter(\.include).count) of \(state.importCandidates.count) selected")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Cancel") {
                    state.importCandidates = []
                    dismiss()
                }
                .keyboardShortcut(.escape)
                Button("Import") { state.commitImport() }
                    .buttonStyle(.borderedProminent)
                    .disabled(state.importCandidates.filter(\.include).isEmpty)
            }
        }
        .padding(20)
        .frame(width: 640)
    }
}

// MARK: - Onboarding (daemon install)

struct OnboardingView: View {
    @EnvironmentObject var state: AppState
    @State private var status = DaemonManager.statusText

    var body: some View {
        VStack(spacing: 18) {
            Image(systemName: "shield.lefthalf.filled")
                .font(.system(size: 56)).foregroundStyle(.tint)
            Text("Welcome to TunHub").font(.title.bold())
            Text("""
            To manage tunnels TunHub needs a system component (a LaunchDaemon) that creates \
            utun interfaces and configures routes and DNS. It is installed once and only runs \
            on the app's request.
            """)
            .multilineTextAlignment(.center)
            .frame(maxWidth: 480)
            .foregroundStyle(.secondary)

            VStack(spacing: 8) {
                Text("Status: \(status)").font(.callout)

                Button {
                    Task { await state.restartDaemon() }
                } label: {
                    Label("Install system component (asks for password)", systemImage: "lock.shield")
                }
                .buttonStyle(.borderedProminent)

                Text("or in Terminal, from the project folder:")
                    .font(.caption2).foregroundStyle(.tertiary)
                HStack {
                    Text("sudo ./install-daemon.sh")
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .padding(6)
                        .background(Color.secondary.opacity(0.12))
                        .clipShape(RoundedRectangle(cornerRadius: 5))
                    Button {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString("sudo ./install-daemon.sh", forType: .string)
                    } label: { Image(systemName: "doc.on.doc") }
                    .buttonStyle(.plain)
                    .help("Copy")
                }
                Text("This window closes automatically once the daemon responds.")
                    .font(.caption2).foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center).frame(maxWidth: 420)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onReceive(Timer.publish(every: 2, on: .main, in: .common).autoconnect()) { _ in
            status = DaemonManager.statusText
        }
    }
}

// MARK: - Daemon version / update sheet (insistent)

struct DaemonUpdateSheet: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        VStack(spacing: 16) {
            switch state.daemonVersionStatus {
            case .ok(let v):
                Image(systemName: "checkmark.seal.fill")
                    .font(.system(size: 46)).foregroundStyle(.green)
                Text("System component is OK").font(.title3.bold())
                Text("Installed and running. Version matches.")
                    .foregroundStyle(.secondary)
                versionGrid(installed: v, expected: kDaemonFullVersion, ok: true)
                Button("Close") { state.showDaemonUpdateSheet = false }
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)

            case .mismatch(let installed, let expected):
                header(problem: "Component update required",
                       detail: "The running system component is older than the app. It must be reinstalled, otherwise tunnels may misbehave.")
                versionGrid(installed: installed, expected: expected, ok: false)
                actionButtons

            case .unreachable:
                header(problem: "System component not responding",
                       detail: "The component is installed but does not respond. It must be reinstalled / restarted.")
                actionButtons

            case .checking:
                ProgressView("Checking version…").padding(.top, 20)
                Button("Quit TunHub") {
                    state.showDaemonUpdateSheet = false
                    NSApp.terminate(nil)
                }
                .buttonStyle(.plain).font(.caption).foregroundStyle(.secondary)
                .padding(.bottom, 10)

            case .unknown:
                header(problem: "Component not installed",
                       detail: "Install the system component to manage tunnels.")
                actionButtons
            }
        }
        .padding(24)
        .frame(width: 460)
        .interactiveDismissDisabled(state.daemonVersionStatus.isProblem || state.daemonBusy)
    }

    func header(problem: String, detail: String) -> some View {
        VStack(spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 42)).foregroundStyle(.orange)
            Text(problem).font(.title3.bold()).multilineTextAlignment(.center)
            Text(detail).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
    }

    func versionGrid(installed: String, expected: String, ok: Bool) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
            GridRow {
                Text("Installed:").foregroundStyle(.secondary)
                Text(installed).font(.system(.callout, design: .monospaced))
                    .foregroundStyle(ok ? .green : .orange)
            }
            GridRow {
                Text("Expected:").foregroundStyle(.secondary)
                Text(expected).font(.system(.callout, design: .monospaced))
            }
        }
        .padding(10)
        .background(Color.secondary.opacity(0.08))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    @ViewBuilder var actionButtons: some View {
        if state.daemonBusy {
            ProgressView("Reinstalling… (confirm the password in the system dialog)")
                .padding(.vertical, 6)
        } else {
            VStack(spacing: 8) {
                Button {
                    Task { await state.restartDaemon() }
                } label: {
                    Label("Reinstall (asks for password)", systemImage: "lock.shield")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)

                Button("Check again") {
                    Task { await state.checkDaemonVersion() }
                }
                .controlSize(.small)

                HStack(spacing: 14) {
                    Button("Later") { state.showDaemonUpdateSheet = false }
                        .buttonStyle(.plain).font(.caption).foregroundStyle(.secondary)
                    Button("Quit TunHub") {
                        state.showDaemonUpdateSheet = false
                        NSApp.terminate(nil)
                    }
                    .buttonStyle(.plain).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }
}

// MARK: - Settings

/// Languages the app actually ships (discovered from the .lproj bundles at runtime),
/// plus a "System default" option. No hard-coding — add an .lproj and it appears here.
struct AppLanguageOption: Identifiable, Hashable {
    let code: String            // "system" or a locale code like "en", "ru"
    var id: String { code }
    var label: String {
        if code == "system" { return "System default" }
        let loc = Locale(identifier: code)
        // Native name of the language, capitalised (e.g. "Русский", "English").
        let native = loc.localizedString(forLanguageCode: code)
            ?? Locale.current.localizedString(forLanguageCode: code)
            ?? code
        return native.prefix(1).capitalized + native.dropFirst()
    }

    static var available: [AppLanguageOption] {
        let codes = Bundle.main.localizations
            .filter { $0.lowercased() != "base" }
            .sorted()
        return [AppLanguageOption(code: "system")] + codes.map { AppLanguageOption(code: $0) }
    }
}

/// "Launch at login" via SMAppService.mainApp (macOS 13+). No helper/plist needed —
/// macOS registers the app bundle itself as a login item.
enum LoginItem {
    static var isEnabled: Bool { SMAppService.mainApp.status == .enabled }

    static func setEnabled(_ on: Bool) throws {
        if on {
            if SMAppService.mainApp.status != .enabled { try SMAppService.mainApp.register() }
        } else {
            if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() }
        }
    }
}

struct SettingsView: View {
    @EnvironmentObject var state: AppState
    @AppStorage("killSwitchGlobal") private var killSwitchGlobal = true
    @AppStorage("appLanguage") private var appLanguage = "system"
    @State private var pendingLanguageChange = false
    @State private var launchAtLogin = LoginItem.isEnabled

    var body: some View {
        Form {
            Section("General") {
                Toggle("Launch TunHub at login", isOn: $launchAtLogin)
                    .onChange(of: launchAtLogin) { on in
                        do {
                            try LoginItem.setEnabled(on)
                        } catch {
                            launchAtLogin = LoginItem.isEnabled   // revert to the real state
                            state.lastError = "Login item: \(error.localizedDescription)"
                        }
                    }
                Text("Tunnels marked “Connect on app launch” connect automatically after start.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Language") {
                Picker("Interface language", selection: $appLanguage) {
                    ForEach(AppLanguageOption.available) { Text($0.label).tag($0.code) }
                }
                .onChange(of: appLanguage) { newValue in
                    applyLanguage(newValue)
                    pendingLanguageChange = true
                }
                if pendingLanguageChange {
                    HStack {
                        Text("Restart to apply the language.")
                            .font(.caption).foregroundStyle(.secondary)
                        Spacer()
                        Button("Restart now") { relaunch() }
                            .controlSize(.small)
                    }
                }
            }
            Section("Security") {
                Toggle("Allow kill switch globally", isOn: $killSwitchGlobal)
                    .onChange(of: killSwitchGlobal) { v in
                        Task { await state.daemon.setKillSwitchEnabled(v) }
                    }
                Text("PostUp/PreDown scripts from configs are never executed.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("System component") {
                LabeledContent("Status", value: DaemonManager.statusText)
                Button("Reinstall / restart daemon…") {
                    Task { await state.restartDaemon() }
                }
            }
            Section("Statistics") {
                Button("Export traffic to CSV…") { exportCSV() }
            }
        }
        .formStyle(.grouped)
        .frame(width: 480, height: 320)
    }

    func exportCSV() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "tunhub-traffic.csv"
        if panel.runModal() == .OK, let url = panel.url {
            var out = ""
            for t in state.tunnels {
                out += "# \(t.name)\n" + state.ledger.csv(id: t.id) + "\n"
            }
            try? out.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    /// Persist the language choice via AppleLanguages (applied by macOS on next launch).
    func applyLanguage(_ code: String) {
        LanguageManager.apply(code)
    }

    func relaunch() {
        let path = Bundle.main.bundlePath
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        task.arguments = ["-n", path]
        try? task.run()
        NSApp.terminate(nil)
    }
}

/// Applies/persists the interface language. AppleLanguages persists across launches
/// and macOS uses it to pick the .lproj bundle at startup.
enum LanguageManager {
    static func apply(_ code: String) {
        if code == "system" {
            UserDefaults.standard.removeObject(forKey: "AppleLanguages")
        } else {
            UserDefaults.standard.set([code], forKey: "AppleLanguages")
        }
        UserDefaults.standard.synchronize()
    }
    /// Re-assert the saved choice on launch (belt-and-suspenders in case it was cleared).
    static func applySavedOnLaunch() {
        let saved = UserDefaults.standard.string(forKey: "appLanguage") ?? "system"
        apply(saved)
    }
}
