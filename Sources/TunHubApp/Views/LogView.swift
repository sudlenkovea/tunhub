import SwiftUI
import AppKit
import TunHubShared

/// Log viewer: merges the daemon log (over XPC) and the app log, live-updating.
///
/// Performance notes (this view used to dominate the app's CPU profile):
///  * It polls ONLY while the window is actually on screen. A hidden-but-alive window kept
///    re-fetching and re-decoding the whole log forever.
///  * The log is rendered as ONE attributed `Text`, not one row per line. A `ForEach` over a
///    few thousand selectable rows made SwiftUI re-run layout for every row on every tick.
///  * Filtering happens when the data or the filter changes, never inside `body`.
struct LogView: View {
    @EnvironmentObject var state: AppState
    @State private var lines: [LogLine] = []
    @State private var rendered = AttributedString()
    @State private var minLevel: LogLevel = .info
    @State private var query = ""
    @State private var source: Source = .all
    @State private var autoScroll = true
    @State private var paused = false
    @State private var visible = false
    @State private var lastSignature = ""

    /// Upper bound on what we keep in memory and hand to the text engine. The file itself is
    /// capped at 5 MB; showing more than this in a scroll view helps nobody.
    private static let maxLines = 1500

    enum Source: String, CaseIterable, Identifiable {
        case all = "All", daemon = "Daemon", app = "App"
        var id: String { rawValue }
    }

    // 1s felt "live" but doubled the cost of every tick; 2s is still live and halves the work.
    private let timer = Timer.publish(every: 2, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            controls
            Divider()
            logBody
        }
        .frame(minWidth: 720, minHeight: 420)
        .onReceive(timer) { _ in
            guard visible, !paused else { return }
            Task { await refresh() }
        }
        .onAppear { visible = true; Task { await refresh() } }
        .onDisappear { visible = false; lines = []; rendered = AttributedString() }
    }

    var controls: some View {
        HStack(spacing: 10) {
            Picker("", selection: $source) {
                ForEach(Source.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .frame(width: 220)
            .onChange(of: source) { _ in Task { await refresh(force: true) } }

            Picker("Level", selection: $minLevel) {
                ForEach(LogLevel.allCases, id: \.self) { Text($0.rawValue).tag($0) }
            }
            .frame(width: 160)
            .onChange(of: minLevel) { _ in rebuild() }

            TextField("Search…", text: $query)
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 220)
                .onChange(of: query) { _ in rebuild() }

            Spacer()

            Toggle("Pause", isOn: $paused).toggleStyle(.button)
            Toggle("Auto-scroll", isOn: $autoScroll).toggleStyle(.button)

            Menu {
                Button("Copy all") { copyAll() }
                Button("Save to file…") { saveToFile() }
                Divider()
                Button("Reveal logs in Finder") { revealInFinder() }
                Button("Clear screen") { lines = []; rendered = AttributedString() }
            } label: { Image(systemName: "ellipsis.circle") }
            .frame(width: 44)
        }
        .padding(10)
    }

    var logBody: some View {
        ScrollViewReader { proxy in
            ScrollView {
                // One text node for the whole log: selection still works across the entire
                // buffer, and SwiftUI lays out a single view instead of thousands.
                Text(rendered)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .id("body")
                Color.clear.frame(height: 1).id("bottom")
            }
            .background(Color(nsColor: .textBackgroundColor))
            .onChange(of: rendered) { _ in
                if autoScroll { proxy.scrollTo("bottom", anchor: .bottom) }
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

    func refresh(force: Bool = false) async {
        var merged: [LogLine] = []
        if source != .app {
            merged += await state.daemon.recentLog(maxLines: Self.maxLines).map {
                var l = $0; l.category = "daemon:" + l.category; return l
            }
        }
        if source != .daemon {
            merged += applog.tail(maxLines: Self.maxLines).map {
                var l = $0; l.category = "app:" + l.category; return l
            }
        }
        merged.sort { $0.ts < $1.ts }
        if merged.count > Self.maxLines { merged = Array(merged.suffix(Self.maxLines)) }

        // Skip the (expensive) re-render when nothing actually changed — the common case
        // for an idle tunnel, where we'd otherwise rebuild the whole buffer every tick.
        let sig = "\(merged.count)|\(merged.last?.ts.timeIntervalSince1970 ?? 0)|\(merged.last?.message.count ?? 0)"
        guard force || sig != lastSignature else { return }
        lastSignature = sig
        lines = merged
        rebuild()
    }

    /// Rebuild the attributed buffer from `lines` + current filters.
    private func rebuild() {
        var out = AttributedString()
        for var line in lines {
            guard line.level >= minLevel else { continue }
            if !query.isEmpty,
               !line.message.localizedCaseInsensitiveContains(query),
               !line.category.localizedCaseInsensitiveContains(query) { continue }
            var piece = AttributedString(line.formatted + "\n")
            piece.foregroundColor = color(line.level)
            out += piece
        }
        rendered = out
    }

    private var visibleText: String {
        String(rendered.characters)
    }

    func copyAll() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(visibleText, forType: .string)
    }

    func saveToFile() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "tunhub-log.txt"
        if panel.runModal() == .OK, let url = panel.url {
            try? visibleText.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    func revealInFinder() {
        let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(TunHub.AppPath.logsDir)
        NSWorkspace.shared.open(dir)
    }
}
