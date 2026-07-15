import SwiftUI
import AppKit
import TunHubShared

/// Log viewer: merges the daemon log (over XPC) and the app log, live-updating.
struct LogView: View {
    @EnvironmentObject var state: AppState
    @State private var lines: [LogLine] = []
    @State private var minLevel: LogLevel = .debug
    @State private var query = ""
    @State private var source: Source = .all
    @State private var autoScroll = true
    @State private var paused = false

    enum Source: String, CaseIterable, Identifiable {
        case all = "All", daemon = "Daemon", app = "App"
        var id: String { rawValue }
    }

    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var filtered: [LogLine] {
        lines.filter { l in
            l.level >= minLevel &&
            (query.isEmpty || l.message.localizedCaseInsensitiveContains(query)
                || l.category.localizedCaseInsensitiveContains(query))
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            controls
            Divider()
            logBody
        }
        .frame(minWidth: 720, minHeight: 420)
        .onReceive(timer) { _ in if !paused { Task { await refresh() } } }
        .task { await refresh() }
    }

    var controls: some View {
        HStack(spacing: 10) {
            Picker("", selection: $source) {
                ForEach(Source.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .frame(width: 220)
            .onChange(of: source) { _ in Task { await refresh() } }

            Picker("Level", selection: $minLevel) {
                ForEach(LogLevel.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }
            .frame(width: 160)

            TextField("Search…", text: $query)
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 220)

            Spacer()

            Toggle("Pause", isOn: $paused).toggleStyle(.button)
            Toggle("Auto-scroll", isOn: $autoScroll).toggleStyle(.button)

            Menu {
                Button("Copy all") { copyAll() }
                Button("Save to file…") { saveToFile() }
                Divider()
                Button("Reveal logs in Finder") { revealInFinder() }
                Button("Clear screen") { lines = [] }
            } label: { Image(systemName: "ellipsis.circle") }
            .frame(width: 44)
        }
        .padding(10)
    }

    var logBody: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(filtered) { line in
                        HStack(alignment: .top, spacing: 6) {
                            Text(line.formatted)
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(color(line.level))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .id(line.id)
                        .padding(.horizontal, 10)
                    }
                    Color.clear.frame(height: 1).id("bottom")
                }
                .padding(.vertical, 6)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .onChange(of: lines.count) { _ in
                if autoScroll { withAnimation(.none) { proxy.scrollTo("bottom", anchor: .bottom) } }
            }
        }
    }

    func color(_ l: LogLevel) -> Color {
        switch l {
        case .trace: return .secondary
        case .debug: return .primary.opacity(0.8)
        case .info: return .primary
        case .warn: return .orange
        case .error: return .red
        }
    }

    // MARK: data

    func refresh() async {
        var merged: [LogLine] = []
        if source != .app {
            merged += await state.daemon.recentLog(maxLines: 2000).map {
                var l = $0; l.category = "daemon:" + l.category; return l
            }
        }
        if source != .daemon {
            merged += applog.tail(maxLines: 2000).map {
                var l = $0; l.category = "app:" + l.category; return l
            }
        }
        merged.sort { $0.ts < $1.ts }
        lines = merged
    }

    func copyAll() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(filtered.map(\.formatted).joined(separator: "\n"), forType: .string)
    }

    func saveToFile() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "tunhub-log.txt"
        if panel.runModal() == .OK, let url = panel.url {
            try? filtered.map(\.formatted).joined(separator: "\n").write(to: url, atomically: true, encoding: .utf8)
        }
    }

    func revealInFinder() {
        let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(TunHub.AppPath.logsDir)
        NSWorkspace.shared.open(dir)
    }
}
