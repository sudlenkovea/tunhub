import SwiftUI
import AppKit
import TunHubShared

/// Log viewer: merges the daemon log (over XPC) and the app log, live-updating.
///
/// Performance notes (this view used to dominate the app's CPU profile):
///  * It polls ONLY while the window is on screen. A hidden-but-alive window kept
///    re-fetching and re-decoding the whole log forever.
///  * Text is rendered by AppKit's text system (NSTextView), not by SwiftUI. A `ForEach`
///    of selectable rows re-ran layout for thousands of views on every tick, and building
///    a SwiftUI `AttributedString` by appending was quadratic in attribute runs — switching
///    source or level stalled the UI for seconds.
///  * Filtering and attributed-string construction happen off the main thread.
struct LogView: View {
    @EnvironmentObject var state: AppState
    @State private var lines: [LogLine] = []
    @State private var rendered = NSAttributedString()
    @State private var minLevel: LogLevel = .info
    @State private var query = ""
    @State private var source: Source = .all
    @State private var autoScroll = true
    @State private var paused = false
    @State private var visible = false
    @State private var lastSignature = ""
    @State private var building = false
    @State private var rebuildToken = 0

    /// Upper bound on what we keep and hand to the text engine. The files themselves are
    /// capped at 5 MB; showing more than this in a scroll view helps nobody.
    private static let maxLines = 1500

    enum Source: String, CaseIterable, Identifiable {
        case all = "All", daemon = "Daemon", app = "App"
        var id: String { rawValue }
    }

    /// Levels that can actually appear in the file under the current capture mode.
    /// Offering `trace` while capturing at `info` just yields a confusing empty view.
    private var selectableLevels: [LogLevel] {
        LogLevel.allCases.filter { $0 >= appLogMode.minLevel }
    }

    // 1s felt "live" but doubled the cost of every tick; 2s is still live and halves the work.
    private let timer = Timer.publish(every: 2, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            controls
            Divider()
            LogTextView(attributed: rendered, scrollToEnd: autoScroll)
                .background(Color(nsColor: .textBackgroundColor))
        }
        .frame(minWidth: 720, minHeight: 420)
        .onReceive(timer) { _ in
            guard visible, !paused else { return }
            Task { await refresh() }
        }
        .onAppear {
            visible = true
            if minLevel < appLogMode.minLevel { minLevel = appLogMode.minLevel }
            Task { await refresh(force: true) }
        }
        .onDisappear {
            visible = false
            lines = []
            rendered = NSAttributedString()
            lastSignature = ""
        }
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
                ForEach(selectableLevels, id: \.self) { Text($0.rawValue).tag($0) }
            }
            .frame(width: 150)
            .onChange(of: minLevel) { _ in rebuild() }

            TextField("Search…", text: $query)
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 200)
                .onChange(of: query) { _ in rebuild() }

            if building { ProgressView().controlSize(.small) }

            Spacer()

            if appLogMode == .normal {
                Text("Normal capture")
                    .font(.caption).foregroundStyle(.secondary)
                    .help("Debug and trace are not recorded. Switch capture to Verbose in Settings (needs a restart).")
            }

            Toggle("Pause", isOn: $paused).toggleStyle(.button)
            Toggle("Auto-scroll", isOn: $autoScroll).toggleStyle(.button)

            Menu {
                Button("Copy all") { copyAll() }
                Button("Save to file…") { saveToFile() }
                Divider()
                Button("Reveal logs in Finder") { revealInFinder() }
                Button("Clear screen") { lines = []; rendered = NSAttributedString() }
            } label: { Image(systemName: "ellipsis.circle") }
            .frame(width: 44)
        }
        .padding(10)
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

        // Skip the re-render when nothing changed — the common case for an idle tunnel,
        // where we'd otherwise rebuild the whole buffer every tick.
        let sig = "\(merged.count)|\(merged.last?.ts.timeIntervalSince1970 ?? 0)|\(merged.last?.message.count ?? 0)"
        guard force || sig != lastSignature else { return }
        lastSignature = sig
        lines = merged
        rebuild()
    }

    /// Rebuild the text off the main thread. Only the final assignment touches the UI, so
    /// changing source/level/filter never blocks input.
    private func rebuild() {
        rebuildToken &+= 1
        let token = rebuildToken
        let snapshot = lines
        let level = minLevel
        let q = query
        building = true
        Task.detached(priority: .userInitiated) {
            // The costly part (filtering + string building) happens here; assembling the
            // attributed string is a handful of cheap attribute writes on the main actor.
            let doc = Self.renderText(snapshot, minLevel: level, query: q)
            await MainActor.run {
                // A newer rebuild may have started while we worked — drop this result.
                guard token == rebuildToken else { return }
                rendered = Self.attributed(doc)
                building = false
            }
        }
    }

    /// Plain text plus the colour spans to apply — both Sendable, so this can be produced
    /// off the main thread.
    private struct RenderedDoc: Sendable {
        var text: String
        var spans: [Span]
        struct Span: Sendable { var location: Int; var length: Int; var level: LogLevel }
    }

    nonisolated private static func renderText(_ lines: [LogLine],
                                               minLevel: LogLevel,
                                               query: String) -> RenderedDoc {
        var text = ""
        var spans: [RenderedDoc.Span] = []
        var utf16Offset = 0
        for var line in lines {
            guard line.level >= minLevel else { continue }
            if !query.isEmpty,
               !line.message.localizedCaseInsensitiveContains(query),
               !line.category.localizedCaseInsensitiveContains(query) { continue }
            let s = line.formatted + "\n"
            let len = s.utf16.count
            if line.level != .info {
                spans.append(.init(location: utf16Offset, length: len, level: line.level))
            }
            utf16Offset += len
            text += s
        }
        return RenderedDoc(text: text, spans: spans)
    }

    /// Build the attributed string once, then apply one colour attribute per line range.
    /// (Appending attributed pieces one by one is quadratic in attribute runs.)
    private static func attributed(_ doc: RenderedDoc) -> NSAttributedString {
        let font = NSFont.monospacedSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        let out = NSMutableAttributedString(string: doc.text, attributes: [
            .font: font,
            .foregroundColor: NSColor.labelColor
        ])
        for s in doc.spans {
            out.addAttribute(.foregroundColor, value: nsColor(s.level),
                             range: NSRange(location: s.location, length: s.length))
        }
        return out
    }

    nonisolated private static func nsColor(_ l: LogLevel) -> NSColor {
        switch l {
        case .trace: return .tertiaryLabelColor
        case .debug: return .secondaryLabelColor
        case .info:  return .labelColor
        case .warn:  return .systemOrange
        case .error: return .systemRed
        }
    }

    private var visibleText: String { rendered.string }

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

/// AppKit text view for the log body. NSTextView is built for large documents: it lays out
/// lazily, scrolls smoothly and gives selection/copy for free — none of which SwiftUI's
/// `Text` can do at this size.
private struct LogTextView: NSViewRepresentable {
    let attributed: NSAttributedString
    let scrollToEnd: Bool

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        guard let tv = scroll.documentView as? NSTextView else { return scroll }
        tv.isEditable = false
        tv.isSelectable = true
        tv.drawsBackground = false
        tv.textContainerInset = NSSize(width: 6, height: 6)
        tv.isHorizontallyResizable = false
        tv.textContainer?.widthTracksTextView = true
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let tv = scroll.documentView as? NSTextView else { return }
        guard tv.textStorage?.isEqual(to: attributed) != true else { return }
        tv.textStorage?.setAttributedString(attributed)
        if scrollToEnd {
            tv.scrollRangeToVisible(NSRange(location: attributed.length, length: 0))
        }
    }
}
