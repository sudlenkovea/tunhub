import SwiftUI
import Charts
import UniformTypeIdentifiers
import TunHubShared

struct MainWindow: View {
    @EnvironmentObject var state: AppState
    @State private var selection: UUID?
    @State private var showConflictsAll = false
    @State private var allFindings: [ConflictFinding] = []

    var body: some View {
        Group {
            if !state.daemonInstalled {
                OnboardingView()
            } else {
                NavigationSplitView {
                    sidebar
                } detail: {
                    if let id = selection, let cfg = state.tunnels.first(where: { $0.id == id }) {
                        TunnelDetailView(config: cfg)
                            .id(cfg.id)
                    } else {
                        ContentUnavailableCompat()
                    }
                }
            }
        }
        .toolbar { toolbarContent }
        .sheet(isPresented: $state.showDaemonUpdateSheet) {
            DaemonUpdateSheet().environmentObject(state)
        }
        .sheet(isPresented: $state.showImportSheet) {
            ImportPreviewView().environmentObject(state)
        }
        .sheet(isPresented: $state.showConflictSheet) {
            ConflictsSheet(findings: state.blockedFindings,
                           title: "Conflicts blocking start")
        }
        .sheet(item: $state.credentialRequest) { req in
            OVPNCredentialSheet(request: req).environmentObject(state)
        }
        .sheet(isPresented: $showConflictsAll) {
            ConflictsSheet(findings: allFindings, title: "Check all tunnels")
        }
        .onDrop(of: [UTType.fileURL], isTargeted: nil) { providers in
            handleDrop(providers)
        }
    }

    var sidebar: some View {
        List(selection: $selection) {
            Section("Tunnels") {
                ForEach(state.tunnels) { t in
                    HStack {
                        PhaseDot(phase: state.displayPhase(t))
                        Text(t.name).lineLimit(1)
                        Spacer()
                        if state.displayPhase(t) == .starting || state.displayPhase(t) == .stopping {
                            ProgressView().controlSize(.small).scaleEffect(0.6)
                        }
                        KindBadge(kind: t.kind)
                    }
                    .tag(t.id)
                    .contextMenu {
                        Button(state.isRunning(t) ? "Stop" : "Start") { state.toggle(t) }
                        Button("Duplicate") { state.duplicate(t) }
                        Divider()
                        Button("Export .conf…") { exportSingle(t) }
                        Divider()
                        Button("Delete", role: .destructive) { state.delete(t) }
                    }
                }
            }
        }
        .navigationSplitViewColumnWidth(min: 200, ideal: 240)
    }

    @ToolbarContentBuilder var toolbarContent: some ToolbarContent {
        ToolbarItemGroup {
            Button {
                newTunnel()
            } label: { Label("New", systemImage: "plus") }

            Button {
                openImportPanel()
            } label: { Label("Import", systemImage: "square.and.arrow.down") }

            Button {
                exportAll()
            } label: { Label("Export all (ZIP)", systemImage: "square.and.arrow.up") }
            .disabled(state.tunnels.isEmpty)

            Button {
                allFindings = state.checkAllConflicts()
                showConflictsAll = true
            } label: { Label("Check conflicts", systemImage: "checkmark.shield") }
            .disabled(state.tunnels.count < 2)

            Button {
                WindowManager.shared.showLogs()
            } label: { Label("Logs", systemImage: "text.alignleft") }
        }
    }

    func newTunnel() {
        var cfg = TunnelConfig()
        cfg.name = ImportService.uniqueName("New tunnel", existing: state.tunnels)
        let pk = WGKey.generatePrivateKey()
        KeychainService.saveSecrets(tunnelID: cfg.id, .init(privateKey: pk))
        cfg.interface.privateKeyRef = KeychainService.interfaceRef(cfg.id)
        cfg.interface.publicKey = WGKey.publicKey(fromPrivate: pk) ?? ""
        cfg.peers = [PeerConfig()]
        state.save(cfg)
        selection = cfg.id
    }

    func openImportPanel() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [UTType(filenameExtension: "conf") ?? .plainText, .zip]
        panel.message = "Select .conf files or a ZIP archive of configs"
        if panel.runModal() == .OK {
            state.importFiles(panel.urls)
        }
    }

    func exportSingle(_ config: TunnelConfig) {
        let includeSecrets = confirmSecrets()
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "\(config.name).conf"
        if panel.runModal() == .OK, let url = panel.url {
            let text = ExportService.confText(config, includeSecrets: includeSecrets)
            try? text.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    func exportAll() {
        let includeSecrets = confirmSecrets()
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "tunhub-export.zip"
        if panel.runModal() == .OK, let url = panel.url {
            if let data = try? ExportService.zipData(state.tunnels, includeSecrets: includeSecrets) {
                try? data.write(to: url)
            }
        }
    }

    func confirmSecrets() -> Bool {
        let alert = NSAlert()
        alert.messageText = "Include secrets in export?"
        alert.informativeText = "With private keys the config works on another machine. Without them it is reference-only."
        alert.addButton(withTitle: "With secrets")
        alert.addButton(withTitle: "Without secrets")
        return alert.runModal() == .alertFirstButtonReturn
    }

    func handleDrop(_ providers: [NSItemProvider]) -> Bool {
        var urls: [URL] = []
        let group = DispatchGroup()
        for p in providers where p.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
            group.enter()
            p.loadItem(forTypeIdentifier: UTType.fileURL.identifier) { item, _ in
                if let data = item as? Data,
                   let url = URL(dataRepresentation: data, relativeTo: nil) {
                    urls.append(url)
                }
                group.leave()
            }
        }
        group.notify(queue: .main) {
            if !urls.isEmpty { state.importFiles(urls) }
        }
        return true
    }
}

struct ContentUnavailableCompat: View {
    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: "shield.slash").font(.system(size: 40)).foregroundStyle(.secondary)
            Text("Select a tunnel or import configs").foregroundStyle(.secondary)
            Text("Drag & drop of .conf and .zip is supported").font(.caption).foregroundStyle(.tertiary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Detail

struct TunnelDetailView: View {
    @EnvironmentObject var state: AppState
    let config: TunnelConfig

    var body: some View {
        TabView {
            OverviewView(config: config).tabItem { Text("Overview") }
            EditorView(original: config).tabItem { Text("Editor") }
            RawConfigView(config: config).tabItem { Text("Raw") }
        }
        .padding()
    }
}

struct OverviewView: View {
    @EnvironmentObject var state: AppState
    let config: TunnelConfig

    var rt: TunnelRuntimeState? { state.runtime[config.id] }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                HStack(spacing: 12) {
                    PhaseDot(phase: state.displayPhase(config))
                    Text(config.name).font(.title2.bold())
                    KindBadge(kind: config.kind)
                    Spacer()
                    if state.displayPhase(config) == .starting || state.displayPhase(config) == .stopping {
                        ProgressView().controlSize(.small)
                        Text(state.displayPhase(config) == .starting ? "starting…" : "stopping…")
                            .font(.callout).foregroundStyle(.secondary)
                    } else {
                        Button(state.isRunning(config) ? "Stop" : "Start") {
                            state.toggle(config)
                        }
                        .keyboardShortcut(.return)
                    }
                }

                if let err = rt?.errorMessage {
                    Label(err, systemImage: "xmark.octagon").foregroundStyle(.red)
                }

                GroupBox("Status") {
                    Grid(alignment: .leading, horizontalSpacing: 24, verticalSpacing: 6) {
                        GridRow {
                            Text("Interface").foregroundStyle(.secondary)
                            Text(rt?.utunName ?? "—")
                        }
                        GridRow {
                            Text("Handshake").foregroundStyle(.secondary)
                            Text(handshakeText)
                        }
                        GridRow {
                            Text("Endpoint").foregroundStyle(.secondary)
                            Text(rt?.peers.first?.endpoint ?? config.peers.first?.endpoint ?? "—")
                        }
                        GridRow {
                            Text("External IP").foregroundStyle(.secondary)
                            HStack(spacing: 6) {
                                Text(state.externalIPs[config.id] ?? "—")
                                    .textSelection(.enabled)
                                if let detail = state.externalIPDetails[config.id], !detail.isEmpty {
                                    Image(systemName: "info.circle")
                                        .foregroundStyle(.secondary)
                                        .help(detail)   // hover shows the full explanation
                                }
                                Button("Check") { state.fetchExternalIP(config) }
                                    .controlSize(.small)
                                    .disabled(!state.isRunning(config))
                            }
                            .help(state.externalIPDetails[config.id] ?? "")
                        }
                        GridRow {
                            Text("Traffic (session)").foregroundStyle(.secondary)
                            Text("▼ \(ByteFormat.human(rt?.rxTotal ?? 0))  ▲ \(ByteFormat.human(rt?.txTotal ?? 0))")
                        }
                        GridRow {
                            Text("Traffic (month)").foregroundStyle(.secondary)
                            let m = state.ledger.monthTotals(id: config.id)
                            Text("▼ \(ByteFormat.human(m.rx))  ▲ \(ByteFormat.human(m.tx))")
                        }
                        GridRow {
                            Text("System DNS").foregroundStyle(.secondary)
                            Text(TunnelProbe.systemPrimaryDNS().joined(separator: ", "))
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(4)
                }

                if config.kind == .openvpn {
                    OVPNCredentialsBox(config: config)
                }

                RoutesBox(config: config)

                GroupBox("Speed (10 min)") {
                    SpeedChart(samples: Array((state.history[config.id] ?? []).suffix(300)))
                        .frame(height: 160)
                }

                if let peers = rt?.peers, !peers.isEmpty {
                    GroupBox("Peers") {
                        VStack(spacing: 6) {
                            // header
                            HStack {
                                Text("Public key").frame(maxWidth: .infinity, alignment: .leading)
                                Text("Handshake").frame(width: 110, alignment: .leading)
                                Text("RX").frame(width: 80, alignment: .trailing)
                                Text("TX").frame(width: 80, alignment: .trailing)
                            }
                            .font(.caption2).foregroundStyle(.secondary)
                            Divider()
                            ForEach(peers) { p in
                                HStack {
                                    Text(p.publicKey.prefix(20) + "…")
                                        .font(.system(.caption, design: .monospaced))
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                    Text(p.lastHandshake.map { rel($0) } ?? "never")
                                        .font(.caption).frame(width: 110, alignment: .leading)
                                    Text(ByteFormat.human(p.rxBytes))
                                        .font(.caption).frame(width: 80, alignment: .trailing)
                                    Text(ByteFormat.human(p.txBytes))
                                        .font(.caption).frame(width: 80, alignment: .trailing)
                                }
                            }
                        }
                        .padding(4)
                    }
                }
            }
            .padding(.vertical, 6)
        }
    }

    var handshakeText: String {
        guard let h = rt?.lastHandshake else { return "none yet" }
        return rel(h) + (rt?.handshakeFresh == true ? " · ok" : " · stale")
    }

    func rel(_ d: Date) -> String {
        let f = RelativeDateTimeFormatter()
        f.unitsStyle = .abbreviated
        return f.localizedString(for: d, relativeTo: Date())
    }
}

/// OpenVPN credentials editor: save the login/password to the Keychain ahead of time so
/// you aren't prompted at every connect.
struct OVPNCredentialsBox: View {
    @EnvironmentObject var state: AppState
    let config: TunnelConfig
    @State private var username = ""
    @State private var password = ""
    @State private var justSaved = false

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Credentials").bold()
                    Spacer()
                    if justSaved { Text("saved").font(.caption).foregroundStyle(.green) }
                }
                TextField("Username", text: $username).textFieldStyle(.roundedBorder)
                SecureField("Password", text: $password).textFieldStyle(.roundedBorder)
                HStack(spacing: 8) {
                    Button("Save") {
                        state.saveOVPNCredentials(config, username: username, password: password)
                        justSaved = true
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(username.isEmpty)
                    Button("Forget password") {
                        state.forgetOVPNPassword(config); password = ""; justSaved = false
                    }
                    Spacer()
                    if config.openvpn?.staticChallenge != nil {
                        Text("OTP is asked at connect").font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            .padding(4)
            .onChange(of: username) { _ in justSaved = false }
            .onChange(of: password) { _ in justSaved = false }
        }
        .onAppear {
            if let s = KeychainService.loadSecrets(tunnelID: config.id) {
                username = s.openvpn["username"] ?? ""
                password = s.openvpn["password"] ?? ""
            }
        }
    }
}

/// Collapsible list of the tunnel's effective routes — collapsed by default so a large
/// route set (e.g. dozens of subnets) doesn't clutter the overview.
struct RoutesBox: View {
    let config: TunnelConfig
    @State private var expanded = false

    var routes: [IPAddressRange] { config.effectiveRoutes() }

    var body: some View {
        GroupBox {
            DisclosureGroup(isExpanded: $expanded) {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(routes, id: \.canonical) { r in
                        Text(r.canonical)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.top, 4)
            } label: {
                HStack {
                    Text("Routes")
                    Spacer()
                    Text(config.hasDefaultRoute
                         ? String(localized: "all traffic (default route)")
                         : "\(routes.count)")
                        .foregroundStyle(.secondary)
                }
            }
            .padding(4)
        }
    }
}

struct SpeedChart: View {
    let samples: [StatSample]
    struct Point: Identifiable {
        let id = UUID(); let t: Date; let v: Double; let series: String
    }
    var points: [Point] {
        samples.flatMap {
            [Point(t: $0.t, v: $0.rxRate, series: "RX"),
             Point(t: $0.t, v: $0.txRate, series: "TX")]
        }
    }
    var body: some View {
        Chart(points) { p in
            LineMark(x: .value("Time", p.t), y: .value("B/s", p.v))
                .foregroundStyle(by: .value("Series", p.series))
        }
        .chartYAxis {
            AxisMarks { value in
                AxisValueLabel {
                    if let v = value.as(Double.self) {
                        Text(ByteFormat.rate(v))
                    }
                }
            }
        }
    }
}

/// A group of findings of the same type (code) — so identical ones don't spam the list.
struct FindingGroup: Identifiable {
    let id: String            // code
    let severity: FindingSeverity
    let items: [ConflictFinding]
    var count: Int { items.count }
    var code: String { items.first?.code ?? id }
    var summary: String {
        let names = Set(items.flatMap { $0.tunnelNames })
        if count == 1 { return items[0].message }
        return "\(count)× · tunnels: \(names.sorted().joined(separator: ", "))"
    }
    var fixHint: String? { items.compactMap { $0.fixHint }.first }
}

extension Array where Element == ConflictFinding {
    /// Collapse by code, sort by severity.
    var grouped: [FindingGroup] {
        let byCode = Dictionary(grouping: self, by: { $0.code })
        return byCode.map { code, items in
            FindingGroup(id: code, severity: items.map(\.severity).max() ?? .info, items: items)
        }
        .sorted { ($0.severity, $0.count) > ($1.severity, $1.count) }
    }
}

/// Conflicts sheet: one row per type, expandable into details.
struct ConflictsSheet: View {
    @Environment(\.dismiss) var dismiss
    let findings: [ConflictFinding]
    let title: String
    @State private var expanded: Set<String> = []

    var groups: [FindingGroup] { findings.grouped }
    var errors: Int { findings.filter { $0.severity == .error }.count }
    var warnings: Int { findings.filter { $0.severity == .warning }.count }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(title).font(.headline)
                Spacer()
                if !findings.isEmpty {
                    Text("\(errors) errors · \(warnings) warnings")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }

            if findings.isEmpty {
                Label("No conflicts found", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.vertical, 20)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(groups) { group in
                            FindingGroupRow(
                                group: group,
                                isExpanded: expanded.contains(group.id),
                                toggle: {
                                    if expanded.contains(group.id) { expanded.remove(group.id) }
                                    else { expanded.insert(group.id) }
                                }
                            )
                        }
                    }
                    .padding(.trailing, 4)
                }
                .frame(maxHeight: 340)
            }

            HStack {
                if groups.contains(where: { $0.count > 1 }) {
                    Button(expanded.isEmpty ? "Expand all" : "Collapse all") {
                        expanded = expanded.isEmpty ? Set(groups.map(\.id)) : []
                    }
                    .controlSize(.small)
                }
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.escape)
            }
        }
        .padding(20)
        .frame(width: 580)
    }
}

/// Collapsed group row; a click reveals the details of each item.
struct FindingGroupRow: View {
    let group: FindingGroup
    let isExpanded: Bool
    let toggle: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button(action: toggle) {
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: icon).foregroundStyle(color)
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: 6) {
                            Text(group.code).font(.callout.bold())
                            if group.count > 1 {
                                Text("×\(group.count)").font(.caption2.bold())
                                    .padding(.horizontal, 5).padding(.vertical, 1)
                                    .background(color.opacity(0.18)).clipShape(Capsule())
                            }
                        }
                        Text(group.summary).font(.caption).foregroundStyle(.secondary)
                            .lineLimit(isExpanded ? nil : 2)
                            .multilineTextAlignment(.leading)
                    }
                    Spacer()
                    Image(systemName: isExpanded ? "chevron.down" : "chevron.right")
                        .font(.caption2).foregroundStyle(.tertiary)
                }
                .contentShape(Rectangle())
                .padding(.vertical, 6).padding(.horizontal, 8)
            }
            .buttonStyle(.plain)

            if isExpanded {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(group.items) { item in
                        HStack(alignment: .top, spacing: 6) {
                            Circle().fill(color.opacity(0.5)).frame(width: 5, height: 5).padding(.top, 6)
                            VStack(alignment: .leading, spacing: 1) {
                                Text(item.message).font(.caption)
                                if let fix = item.fixHint {
                                    Text("→ \(fix)").font(.caption2).foregroundStyle(.blue)
                                }
                            }
                        }
                    }
                }
                .padding(.leading, 30).padding(.trailing, 8).padding(.bottom, 6)
            }
        }
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.secondary.opacity(0.06)))
    }

    var icon: String {
        switch group.severity {
        case .error: return "xmark.octagon.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .info: return "info.circle"
        }
    }
    var color: Color {
        switch group.severity {
        case .error: return .red
        case .warning: return .yellow
        case .info: return .blue
        }
    }
}
