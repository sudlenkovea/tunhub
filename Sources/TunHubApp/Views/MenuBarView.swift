import SwiftUI
import AppKit
import TunHubShared

struct MenuBarView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("TunHub").font(.headline)
                Spacer()
                Button {
                    WindowManager.shared.showMain()
                } label: {
                    Image(systemName: "arrow.up.forward.app")
                }
                .buttonStyle(.plain)
                .help("Open window")
            }
            .padding(.horizontal, 12).padding(.vertical, 8)

            Divider()

            if !state.daemonInstalled {
                VStack(spacing: 6) {
                    Text("System component not installed").font(.callout)
                    Button("Install…") { WindowManager.shared.showMain() }
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
            } else if state.tunnels.isEmpty {
                Text("No tunnels — import a .conf or ZIP")
                    .font(.callout).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 12)
            } else {
                // The menu grows to fit up to 10 tunnels; beyond that it scrolls.
                let rowHeight: CGFloat = 40
                let visible = min(state.tunnels.count, 10)
                ScrollView {
                    VStack(spacing: 2) {
                        ForEach(state.tunnels) { t in
                            MenuTunnelRow(config: t)
                        }
                    }
                    .padding(.vertical, 4)
                }
                .frame(height: CGFloat(visible) * rowHeight + 8)
                .scrollDisabled(state.tunnels.count <= 10)
            }

            Divider()

            HStack(spacing: 8) {
                Button {
                    Task { await state.stopAll() }
                } label: {
                    Label("Stop all", systemImage: "stop.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)
                .controlSize(.regular)
                .disabled(!state.anyUp)

                if let err = state.lastError {
                    Button {
                        WindowManager.shared.showLogs()
                    } label: {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.red)
                    }
                    .buttonStyle(.plain)
                    .help("Last error: \(err)\nClick to open logs")
                }

                Button {
                    WindowManager.shared.showLogs()
                } label: { Image(systemName: "text.alignleft") }
                .buttonStyle(.plain)
                .help("Logs")

                Menu {
                    Button("Check daemon status") {
                        Task {
                            await state.checkDaemonVersion(explicit: true)
                            state.showDaemonUpdateSheet = true
                            WindowManager.shared.showMain()
                        }
                    }
                    if state.daemonVersionStatus.isProblem {
                        Button("Reinstall system component…") {
                            Task { await state.restartDaemon() }
                        }
                    }
                    Divider()
                    Button("Quit TunHub") { NSApp.terminate(nil) }
                } label: { Image(systemName: "ellipsis.circle") }
                .menuStyle(.borderlessButton)
                .frame(width: 30)
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
        }
        .frame(width: 340)
    }
}

struct MenuTunnelRow: View {
    @EnvironmentObject var state: AppState
    let config: TunnelConfig

    var phase: TunnelPhase? { state.displayPhase(config) }
    var transitioning: Bool { phase == .starting || phase == .stopping }

    var body: some View {
        HStack(spacing: 8) {
            PhaseDot(phase: phase)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 5) {
                    Text(config.name).font(.callout).lineLimit(1)
                    KindBadge(kind: config.kind)
                }
                subtitle
            }
            Spacer()
            if transitioning {
                ProgressView().controlSize(.small).scaleEffect(0.7).frame(width: 28)
            } else {
                Toggle("", isOn: Binding(
                    get: { state.isRunning(config) },
                    set: { _ in state.toggle(config) }
                ))
                .toggleStyle(.switch)
                .controlSize(.mini)
                .labelsHidden()
            }
        }
        .padding(.horizontal, 12).padding(.vertical, 3)
    }

    @ViewBuilder var subtitle: some View {
        if phase == .starting {
            Text("starting…").font(.caption2).foregroundStyle(.yellow)
        } else if phase == .stopping {
            Text("stopping…").font(.caption2).foregroundStyle(.yellow)
        } else if let rt = state.runtime[config.id], rt.phase == .up || rt.phase == .degraded {
            let rate = state.currentRate(config.id)
            HStack(spacing: 6) {
                Text("▼ \(ByteFormat.rate(rate.rx))  ▲ \(ByteFormat.rate(rate.tx))")
                if let ip = state.externalIPs[config.id] { Text("· \(ip)") }
                if rt.phase == .degraded { Text("· no handshake").foregroundStyle(.yellow) }
            }
            .font(.caption2).foregroundStyle(.secondary)
        } else if let rt = state.runtime[config.id], rt.phase == .failed {
            Text(rt.errorMessage ?? "error")
                .font(.caption2).foregroundStyle(.red).lineLimit(1)
        } else {
            Text(config.peers.first?.endpoint ?? "—")
                .font(.caption2).foregroundStyle(.secondary).lineLimit(1)
        }
    }
}
