import Foundation

/// Single source of truth for app identity, bundle IDs, on-disk paths and core
/// binary names — shared by the app and the daemon. Avoids scattered string literals.
public enum TunHub {
    public static let appName        = "TunHub"
    public static let appBundleID    = "com.tunhub.app"
    public static let daemonLabel    = "com.tunhub.daemon"
    public static let machService    = "com.tunhub.daemon.xpc"

    /// Team ID for the XPC code-signing requirement. Empty = check disabled (dev/ad-hoc).
    public static let teamID = ""

    // Core binaries bundled in Contents/MacOS.
    public enum Core {
        public static let wireguard = "wireguard-go"
        public static let amneziawg = "amneziawg-go"   // v0.2.x — covers AmneziaWG 1.5 & 2.0
        public static let openvpn   = "openvpn"        // community 2.6.x, driven via management interface
    }

    // Keychain services.
    public enum Keychain {
        public static let legacyService  = "com.tunhub.keys"      // per-item (migrated)
        public static let secretsService = "com.tunhub.secrets"   // one item per tunnel
    }

    // Root-owned daemon paths.
    public enum DaemonPath {
        public static let varDir   = "/var/db/tunhub"
        public static let runDir   = "/var/run/tunhub"
        public static let logFile  = "/var/log/tunhub-daemon.log"
        public static let plist    = "/Library/LaunchDaemons/\(TunHub.daemonLabel).plist"

        /// Ownership registry: which utunN interfaces WE created (utun ↔ tunnel ↔ pid).
        /// Used to reclaim/clean up only our own interfaces and never touch a utun
        /// owned by another app (official WireGuard, Amnezia, etc.).
        public static let ownership = varDir + "/owned.json"
    }

    /// Env var stamped on every core process we spawn, so a running process can be
    /// positively identified as ours (macOS can't rename utun interfaces).
    public static let ownerEnvKey = "TUNHUB_OWNER"

    // Per-user Application Support layout (under ~/Library/Application Support/TunHub).
    public enum AppPath {
        public static let root       = "TunHub"
        public static let tunnels    = "TunHub/tunnels"
        public static let traffic    = "TunHub/traffic.json"
        public static let logsDir    = "Logs/TunHub"            // under ~/Library
        public static let logFile    = "Logs/TunHub/app.log"
    }
}
