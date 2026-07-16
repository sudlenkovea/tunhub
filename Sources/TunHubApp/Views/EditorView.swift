import SwiftUI
import TunHubShared

/// Tunnel editor form. Fields are string drafts; committed with validation on save.
struct EditorView: View {
    @EnvironmentObject var state: AppState
    let original: TunnelConfig
    var onDone: () -> Void = {}    // called after Revert/Save to jump back to the status tab

    @State private var name = ""
    @State private var kind: TunnelKind = .wireguard
    @State private var addresses = ""
    @State private var dns = ""
    @State private var searchDomains = ""
    @State private var listenPort = ""
    @State private var mtu = ""
    @State private var newPrivateKey = ""       // empty = keep current
    @State private var publicKey = ""
    // AmneziaWG obfuscation
    @State private var jc = ""; @State private var jmin = ""; @State private var jmax = ""
    @State private var s1 = ""; @State private var s2 = ""; @State private var s3 = ""; @State private var s4 = ""
    @State private var h1 = ""; @State private var h2 = ""; @State private var h3 = ""; @State private var h4 = ""
    @State private var i1 = ""; @State private var i2 = ""; @State private var i3 = ""
    @State private var i4 = ""; @State private var i5 = ""
    @State private var showAdvancedAWG = false
    // Options
    @State private var dnsModeIsSplit = false
    @State private var splitDomains = ""
    @State private var autoConnect = false
    @State private var killSwitch = false
    @State private var hcEnabled = false
    @State private var hcHost = ""; @State private var hcPort = "443"
    @State private var hcInterval = "30"; @State private var hcAction: HealthAction = .notify
    @State private var failoverGroup = ""; @State private var failoverPriority = "0"
    // Peers
    @State private var peers: [PeerDraft] = []
    @State private var errors: [String] = []
    @State private var savedFlash = false

    struct PeerDraft: Identifiable {
        var id = UUID()
        var publicKey = ""
        var newPSK = ""            // empty = keep current
        var hasStoredPSK = false
        var removePSK = false
        var endpoint = ""
        var allowedIPs = ""
        var keepalive = ""
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                interfaceBox
                if kind == .amneziawg { awgBox }
                peersBox
                optionsBox

                if !errors.isEmpty {
                    GroupBox {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(errors, id: \.self) { e in
                                Label(e, systemImage: "xmark.circle").foregroundStyle(.red).font(.callout)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }

                HStack {
                    if savedFlash { Label("Saved", systemImage: "checkmark").foregroundStyle(.green) }
                    Spacer()
                    Button("Cancel") { load(); onDone() }   // discard changes → back to status
                        .keyboardShortcut(.cancelAction)
                    Button("Save") { save(); onDone() }
                        .buttonStyle(.borderedProminent)
                        .keyboardShortcut("s")
                }
            }
            .padding(.vertical, 6)
        }
        .onAppear { load() }
    }

    // MARK: Interface

    var interfaceBox: some View {
        GroupBox("Interface") {
            Grid(alignment: .leading, verticalSpacing: 8) {
                row("Name") { TextField("", text: $name) }
                row("Type") {
                    Picker("", selection: $kind) {
                        ForEach(TunnelKind.allCases) { Text($0.label).tag($0) }
                    }.labelsHidden().frame(width: 180)
                }
                row("Public key") {
                    Text(publicKey.isEmpty ? "—" : publicKey)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                }
                row("Private key") {
                    HStack {
                        SecureField("keep current / paste new", text: $newPrivateKey)
                        Button("Generate") {
                            newPrivateKey = WGKey.generatePrivateKey()
                            publicKey = WGKey.publicKey(fromPrivate: newPrivateKey) ?? ""
                        }
                    }
                }
                row("Address") { TextField("10.0.0.2/32, fd00::2/128", text: $addresses) }
                row("DNS") { TextField("1.1.1.1, 8.8.8.8", text: $dns) }
                row("Search domains") { TextField("corp.example.com", text: $searchDomains) }
                row("ListenPort") { TextField("auto", text: $listenPort).frame(width: 120) }
                row("MTU") { TextField("auto (1420)", text: $mtu).frame(width: 120) }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        }
    }

    // MARK: AmneziaWG obfuscation (full width, matches other boxes)

    var awgBox: some View {
        GroupBox("AmneziaWG obfuscation") {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Menu("Presets") {
                        Button("Amnezia default (junk packets)") { applyPreset(.amneziaDefault()) }
                        Button("Full obfuscation (junk + magic headers)") { applyPreset(.fullObfuscation()) }
                        Button("Clear all") { applyPreset(AWGParams()) }
                    }
                    .frame(width: 150)
                    Text("Jc / S1–S4 / H1–H4 / I1–I5 must match the server. On mismatch the handshake still succeeds but no data flows.")
                        .font(.caption).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer()
                }
                Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                    GridRow {
                        field("Jc", $jc, help: "junk packet count, 0–128")
                        field("Jmin", $jmin, help: "min junk size, ≤1280")
                        field("Jmax", $jmax, help: "max junk size, ≤1280")
                    }
                    GridRow {
                        field("S1", $s1, help: "init packet junk size")
                        field("S2", $s2, help: "response packet junk size")
                        field("S3", $s3, help: "cookie-reply junk size (2.x)")
                    }
                    GridRow {
                        field("S4", $s4, help: "transport junk size (2.x)")
                        Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                        Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                    }
                    GridRow {
                        field("H1", $h1); field("H2", $h2); field("H3", $h3)
                    }
                    GridRow {
                        field("H4", $h4)
                        Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                        Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                    }
                }
                DisclosureGroup(isExpanded: $showAdvancedAWG) {
                    Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                        GridRow { wideField("I1", $i1) }
                        GridRow { wideField("I2", $i2) }
                        GridRow { wideField("I3", $i3) }
                        GridRow { wideField("I4", $i4) }
                        GridRow { wideField("I5", $i5) }
                    }
                    .padding(.top, 4)
                } label: {
                    Text("Signature packets I1–I5 (AmneziaWG 2.0)").font(.callout)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        }
    }

    // MARK: Peers

    var peersBox: some View {
        GroupBox("Peers") {
            VStack(alignment: .leading, spacing: 10) {
                ForEach($peers) { $peer in
                    VStack(alignment: .leading, spacing: 6) {
                        Grid(alignment: .leading, verticalSpacing: 6) {
                            row("Public key") { TextField("base64", text: $peer.publicKey) }
                            row("PSK") {
                                HStack {
                                    SecureField(peer.hasStoredPSK ? "stored — paste new to replace" : "none",
                                                text: $peer.newPSK)
                                    Button("Generate") { peer.newPSK = WGKey.generatePSK() }
                                    if peer.hasStoredPSK {
                                        Toggle("remove", isOn: $peer.removePSK).controlSize(.small)
                                    }
                                }
                            }
                            row("Endpoint") { TextField("host:port", text: $peer.endpoint) }
                            row("AllowedIPs") { TextField("0.0.0.0/0, ::/0", text: $peer.allowedIPs) }
                            row("Keepalive") { TextField("25", text: $peer.keepalive).frame(width: 120) }
                        }
                        HStack {
                            Spacer()
                            Button(role: .destructive) {
                                peers.removeAll { $0.id == peer.id }
                            } label: { Label("Remove peer", systemImage: "trash") }
                            .controlSize(.small)
                            .disabled(peers.count == 1)
                        }
                        Divider()
                    }
                }
                Button { peers.append(PeerDraft()) } label: {
                    Label("Add peer", systemImage: "plus")
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        }
    }

    // MARK: Options

    var optionsBox: some View {
        GroupBox("Options") {
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Split DNS (only for the domains below)", isOn: $dnsModeIsSplit)
                if dnsModeIsSplit {
                    TextField("comma-separated: corp.local, internal.example.com", text: $splitDomains)
                }
                Toggle("Connect on app launch", isOn: $autoConnect)
                Toggle("Kill switch (block traffic outside the tunnel)", isOn: $killSwitch)
                Divider()
                Toggle("Health check", isOn: $hcEnabled)
                if hcEnabled {
                    HStack {
                        TextField("host inside the tunnel", text: $hcHost)
                        TextField("port", text: $hcPort).frame(width: 70)
                        TextField("interval, s", text: $hcInterval).frame(width: 90)
                        Picker("on failure:", selection: $hcAction) {
                            Text("notify").tag(HealthAction.notify)
                            Text("restart").tag(HealthAction.restart)
                            Text("failover").tag(HealthAction.failover)
                        }
                    }
                }
                HStack {
                    TextField("failover group (empty = none)", text: $failoverGroup)
                    TextField("priority", text: $failoverPriority).frame(width: 90)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        }
    }

    // MARK: helpers

    @ViewBuilder
    func row(_ label: String, @ViewBuilder content: () -> some View) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary).frame(width: 110, alignment: .trailing)
            content()
        }
    }

    @ViewBuilder
    func field(_ label: String, _ binding: Binding<String>, help: String = "") -> some View {
        HStack(spacing: 4) {
            Text(label).foregroundStyle(.secondary).frame(width: 40, alignment: .trailing)
            TextField("", text: binding).frame(width: 100).help(help)
        }
    }

    @ViewBuilder
    func wideField(_ label: String, _ binding: Binding<String>) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary).frame(width: 40, alignment: .trailing)
            TextField("hex signature", text: binding)
                .font(.system(.caption, design: .monospaced))
        }
    }

    func applyPreset(_ p: AWGParams) {
        func s(_ i: Int?) -> String { i.map(String.init) ?? "" }
        func s32(_ i: UInt32?) -> String { i.map(String.init) ?? "" }
        jc = s(p.jc); jmin = s(p.jmin); jmax = s(p.jmax)
        s1 = s(p.s1); s2 = s(p.s2); s3 = s(p.s3); s4 = s(p.s4)
        h1 = s32(p.h1); h2 = s32(p.h2); h3 = s32(p.h3); h4 = s32(p.h4)
        i1 = p.i1 ?? ""; i2 = p.i2 ?? ""; i3 = p.i3 ?? ""; i4 = p.i4 ?? ""; i5 = p.i5 ?? ""
        if !i1.isEmpty || !i2.isEmpty { showAdvancedAWG = true }
    }

    func load() {
        let c = original
        name = c.name; kind = c.kind
        publicKey = c.interface.publicKey
        newPrivateKey = ""
        addresses = c.interface.addresses.map { "\($0.addressString)/\($0.prefix)" }.joined(separator: ", ")
        dns = c.interface.dns.joined(separator: ", ")
        searchDomains = c.interface.dnsSearchDomains.joined(separator: ", ")
        listenPort = c.interface.listenPort.map(String.init) ?? ""
        mtu = c.interface.mtu.map(String.init) ?? ""
        applyPreset(c.awg ?? AWGParams())
        if case .split(let d) = c.options.dnsMode { dnsModeIsSplit = true; splitDomains = d.joined(separator: ", ") }
        else { dnsModeIsSplit = false; splitDomains = "" }
        autoConnect = c.options.autoConnectOnLaunch
        killSwitch = c.options.killSwitch
        if let hc = c.options.healthCheck {
            hcEnabled = true; hcHost = hc.host; hcPort = String(hc.port)
            hcInterval = String(hc.intervalSec); hcAction = hc.action
        } else { hcEnabled = false }
        failoverGroup = c.options.failoverGroup ?? ""
        failoverPriority = String(c.options.failoverPriority)
        peers = c.peers.map { p in
            var d = PeerDraft()
            d.id = p.id
            d.publicKey = p.publicKey
            d.hasStoredPSK = p.presharedKeyRef != nil
            d.endpoint = p.endpoint ?? ""
            d.allowedIPs = p.allowedIPs.map { "\($0.addressString)/\($0.prefix)" }.joined(separator: ", ")
            d.keepalive = p.persistentKeepalive.map(String.init) ?? ""
            return d
        }
        errors = []
    }

    func save() {
        errors = []
        var cfg = original
        var errs: [String] = []

        let trimmedName = name.trimmingCharacters(in: .whitespaces)
        if trimmedName.isEmpty { errs.append("name cannot be empty") }
        if state.tunnels.contains(where: { $0.id != cfg.id && $0.name == trimmedName }) {
            errs.append("name “\(trimmedName)” is already taken")
        }
        cfg.name = trimmedName
        cfg.kind = kind

        func parseCIDRList(_ str: String, _ label: String) -> [IPAddressRange] {
            str.split(separator: ",").compactMap {
                let t = $0.trimmingCharacters(in: .whitespaces)
                if t.isEmpty { return nil }
                if let r = IPAddressRange(string: t) { return r }
                errs.append("\(label): invalid CIDR “\(t)”"); return nil
            }
        }

        cfg.interface.addresses = parseCIDRList(addresses, "Address")
        if cfg.interface.addresses.isEmpty { errs.append("at least one Address is required") }
        cfg.interface.dns = dns.split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        for d in cfg.interface.dns where !EndpointUtil.isIPLiteral(d) {
            errs.append("DNS “\(d)” is not an IP address")
        }
        cfg.interface.dnsSearchDomains = searchDomains.split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        cfg.interface.listenPort = listenPort.isEmpty ? nil : UInt16(listenPort)
        if !listenPort.isEmpty && cfg.interface.listenPort == nil { errs.append("invalid ListenPort") }
        cfg.interface.mtu = mtu.isEmpty ? nil : Int(mtu)

        // Secrets are collected into one object and written once at the end.
        var secrets = KeychainService.loadSecrets(tunnelID: cfg.id)
            ?? KeychainService.TunnelSecrets(privateKey: "")

        if !newPrivateKey.isEmpty {
            guard WGKey.isValidKey(newPrivateKey) else {
                errors = errs + ["private key: not a 32-byte base64 key"]; return
            }
            secrets.privateKey = newPrivateKey
            cfg.interface.privateKeyRef = KeychainService.interfaceRef(cfg.id)
            cfg.interface.publicKey = WGKey.publicKey(fromPrivate: newPrivateKey) ?? ""
            publicKey = cfg.interface.publicKey
        }

        if kind == .amneziawg {
            var a = AWGParams()
            a.jc = Int(jc); a.jmin = Int(jmin); a.jmax = Int(jmax)
            a.s1 = Int(s1); a.s2 = Int(s2); a.s3 = Int(s3); a.s4 = Int(s4)
            a.h1 = UInt32(h1); a.h2 = UInt32(h2); a.h3 = UInt32(h3); a.h4 = UInt32(h4)
            a.i1 = i1.isEmpty ? nil : i1; a.i2 = i2.isEmpty ? nil : i2; a.i3 = i3.isEmpty ? nil : i3
            a.i4 = i4.isEmpty ? nil : i4; a.i5 = i5.isEmpty ? nil : i5
            errs += a.validate()
            cfg.awg = a.isEmpty ? nil : a
        } else {
            cfg.awg = nil
        }

        var newPeers: [PeerConfig] = []
        for d in peers {
            var p = PeerConfig()
            p.id = d.id
            p.publicKey = d.publicKey.trimmingCharacters(in: .whitespaces)
            if !WGKey.isValidKey(p.publicKey) { errs.append("peer: invalid PublicKey") }
            p.allowedIPs = parseCIDRList(d.allowedIPs, "AllowedIPs")
            let ep = d.endpoint.trimmingCharacters(in: .whitespaces)
            if !ep.isEmpty {
                if EndpointUtil.split(ep) == nil { errs.append("peer: invalid Endpoint “\(ep)”") }
                p.endpoint = ep
            }
            if !d.keepalive.isEmpty {
                if let k = UInt16(d.keepalive) { p.persistentKeepalive = k }
                else { errs.append("peer: invalid Keepalive") }
            }
            let oldPeer = original.peers.first { $0.id == d.id }
            if d.removePSK {
                secrets.psks[p.id.uuidString] = nil
                p.presharedKeyRef = nil
            } else if !d.newPSK.isEmpty {
                guard WGKey.isValidKey(d.newPSK) else { errs.append("peer: PSK is not a base64 key"); continue }
                secrets.psks[p.id.uuidString] = d.newPSK
                p.presharedKeyRef = KeychainService.pskRef(cfg.id, peerID: p.id)
            } else {
                p.presharedKeyRef = oldPeer?.presharedKeyRef
            }
            newPeers.append(p)
        }
        if newPeers.isEmpty { errs.append("at least one peer is required") }
        cfg.peers = newPeers

        cfg.options.dnsMode = dnsModeIsSplit
            ? .split(splitDomains.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty })
            : .global
        if dnsModeIsSplit, case .split(let d) = cfg.options.dnsMode, d.isEmpty {
            errs.append("split DNS: specify at least one domain")
        }
        cfg.options.autoConnectOnLaunch = autoConnect
        cfg.options.killSwitch = killSwitch
        if hcEnabled {
            var hc = HealthCheckConfig()
            hc.host = hcHost.trimmingCharacters(in: .whitespaces)
            hc.port = UInt16(hcPort) ?? 443
            hc.intervalSec = Int(hcInterval) ?? 30
            hc.action = hcAction
            if hc.host.isEmpty { errs.append("health check: specify a host") }
            cfg.options.healthCheck = hc
        } else {
            cfg.options.healthCheck = nil
        }
        cfg.options.failoverGroup = failoverGroup.isEmpty ? nil : failoverGroup
        cfg.options.failoverPriority = Int(failoverPriority) ?? 0

        let findings = ConflictChecker.check(candidate: cfg, against: state.tunnels.filter { $0.id != cfg.id })
        errs += findings.filter { $0.severity == .error && $0.code != "GlobalDNSClash" && $0.code != "DefaultRouteClash" }
            .map(\.message)

        guard errs.isEmpty else { errors = errs; return }
        if secrets.privateKey.isEmpty {
            errors = ["the tunnel has no private key — set one"]; return
        }
        KeychainService.saveSecrets(tunnelID: cfg.id, secrets)
        state.save(cfg)
        savedFlash = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { savedFlash = false }

        if state.isRunning(cfg) {
            let alert = NSAlert()
            alert.messageText = "Tunnel is running"
            alert.informativeText = "Restart “\(cfg.name)” to apply changes?"
            alert.addButton(withTitle: "Restart")
            alert.addButton(withTitle: "Later")
            if alert.runModal() == .alertFirstButtonReturn {
                Task {
                    await state.stop(cfg)
                    try? await state.start(cfg, force: true)
                }
            }
        }
    }
}

// MARK: - Raw .conf editor

struct RawConfigView: View {
    @EnvironmentObject var state: AppState
    let config: TunnelConfig
    @State private var text = ""
    @State private var showSecrets = false
    @State private var error: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Toggle("Show secrets", isOn: $showSecrets)
                    .onChange(of: showSecrets) { _ in reload() }
                Spacer()
                Button("Reload") { reload() }
                Button("Apply") { apply() }
                    .buttonStyle(.borderedProminent)
                    .disabled(!showSecrets)
                    .help(showSecrets ? "" : "Apply is only available with secrets shown (otherwise the PrivateKey is lost)")
            }
            TextEditor(text: $text)
                .font(.system(.body, design: .monospaced))
            if let error {
                Label(error, systemImage: "xmark.circle").foregroundStyle(.red)
            }
        }
        .onAppear { reload() }
    }

    func reload() {
        text = ExportService.confText(config, includeSecrets: showSecrets)
        error = nil
    }

    func apply() {
        do {
            let parsed = try WGQuickParser.parse(name: config.name, text: text)
            var cfg = parsed.config
            cfg.id = config.id
            cfg.options = config.options
            cfg.meta = config.meta
            var secrets = KeychainService.TunnelSecrets(privateKey: parsed.privateKey)
            cfg.interface.privateKeyRef = KeychainService.interfaceRef(cfg.id)
            for i in cfg.peers.indices {
                if let psk = parsed.presharedKeys[cfg.peers[i].id] {
                    secrets.psks[cfg.peers[i].id.uuidString] = psk
                    cfg.peers[i].presharedKeyRef = KeychainService.pskRef(cfg.id, peerID: cfg.peers[i].id)
                }
            }
            KeychainService.saveSecrets(tunnelID: cfg.id, secrets)
            state.save(cfg)
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }
}
