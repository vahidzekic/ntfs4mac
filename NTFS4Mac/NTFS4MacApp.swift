//
//  NTFS4MacApp.swift
//  NTFS4Mac — host app for the NTFSExtension FSKit module.
//
//  The app exists because an FSKit module must ship inside an app bundle
//  (Contents/Extensions/NTFSExtension.appex). It does no file-system work
//  itself: it explains how to enable the extension, shows whether FSKit sees
//  it as enabled, and displays the GPL notice for the bundled libntfs-3g.
//
//  Swift 6 language mode, strict concurrency: all UI state is @MainActor; the
//  only callback from FSKit is an explicitly @Sendable closure that hops back
//  to the main actor.
//

import AppKit
import FSKit
import SwiftUI

// MARK: - Constants

enum AppConstants {
    static let extensionBundleID = "com.vahidzekic.ntfs4mac.NTFSExtension"
    static let fsShortName = "ntfs4mac"

    /// Login Items & Extensions pane. Not a documented API; both forms are used
    /// by shipping FSKit apps. The first one scrolls to the extensions list on
    /// some releases, the second always opens the pane.
    static let settingsURLs = [
        "x-apple.systempreferences:com.apple.LoginItems-Settings.extension?ExtensionItems",
        "x-apple.systempreferences:com.apple.LoginItems-Settings.extension",
    ]

    static let gplURL = URL(string: "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html")!
    static let ntfs3gSourceURL = URL(string: "https://github.com/tuxera/ntfs-3g")!
    static let projectURL = URL(string: "https://github.com/vahidzekic/ntfs4mac")!
    static let author = "Vahid Zekic"
}

// MARK: - App

@main
struct NTFS4MacApp: App {
    @StateObject private var status = ExtensionStatusModel()

    var body: some Scene {
        Window("NTFS4Mac", id: "main") {
            ContentView()
                .environmentObject(status)
                .frame(minWidth: 560, idealWidth: 620, minHeight: 560, idealHeight: 680)
        }
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About NTFS4Mac") { AboutPanel.show() }
            }
        }
    }
}

// MARK: - About panel

/// Standard About panel with the author and project link in the credits.
@MainActor
enum AboutPanel {
    static func show() {
        let centered = NSMutableParagraphStyle()
        centered.alignment = .center
        let body: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11),
            .foregroundColor: NSColor.secondaryLabelColor,
            .paragraphStyle: centered,
        ]
        let credits = NSMutableAttributedString(
            string: "Developed by \(AppConstants.author)\n",
            attributes: [
                .font: NSFont.systemFont(ofSize: 12, weight: .semibold),
                .foregroundColor: NSColor.labelColor,
                .paragraphStyle: centered,
            ])
        credits.append(NSAttributedString(
            string: "github.com/vahidzekic/ntfs4mac",
            attributes: body.merging([.link: AppConstants.projectURL]) { $1 }))
        credits.append(NSAttributedString(
            string: "\n\nBuilt on Apple FSKit and libntfs-3g from the NTFS-3G project. "
                + "Free software under the GNU GPL v2 or later.",
            attributes: body))
        NSApplication.shared.orderFrontStandardAboutPanel(options: [.credits: credits])
        NSApplication.shared.activate()
    }
}

// MARK: - Extension status

enum ExtensionState: Equatable, Sendable {
    case checking
    case enabled
    case disabled
    case notInstalled
    case unknown(String)

    var title: String {
        switch self {
        case .checking: "Checking…"
        case .enabled: "Enabled"
        case .disabled: "Installed, not enabled"
        case .notInstalled: "Not registered with FSKit"
        case .unknown: "Status unknown"
        }
    }

    var symbol: String {
        switch self {
        case .checking: "hourglass"
        case .enabled: "checkmark.circle.fill"
        case .disabled: "exclamationmark.triangle.fill"
        case .notInstalled: "xmark.octagon.fill"
        case .unknown: "questionmark.circle"
        }
    }

    var tint: Color {
        switch self {
        case .enabled: .green
        case .disabled: .orange
        case .notInstalled: .red
        case .checking, .unknown: .secondary
        }
    }
}

@MainActor
final class ExtensionStatusModel: ObservableObject {
    @Published private(set) var state: ExtensionState = .checking

    init() {
        refresh()
    }

    func refresh() {
        state = .checking
        Self.queryFSKit(bundleID: AppConstants.extensionBundleID) { [weak self] result in
            Task { @MainActor in
                self?.state = result
            }
        }
    }

    /// Asks fskitd for the installed modules. Runs off the main actor; the
    /// completion handler is @Sendable and only carries a Sendable value.
    nonisolated private static func queryFSKit(
        bundleID: String,
        completion: @escaping @Sendable (ExtensionState) -> Void
    ) {
        FSClient.shared.fetchInstalledExtensions { @Sendable modules, error in
            if let error {
                completion(.unknown(error.localizedDescription))
                return
            }
            guard let module = modules?.first(where: { $0.bundleIdentifier == bundleID }) else {
                completion(.notInstalled)
                return
            }
            completion(module.isEnabled ? .enabled : .disabled)
        }
    }

    /// Opens System Settings at Login Items & Extensions.
    ///
    /// `NSWorkspace.open(_:)` returns `true` as soon as Launch Services accepts
    /// the URL, even when System Settings then ignores an unknown pane ID, and
    /// from a sandboxed app opening the .app bundle by file URL can fail
    /// silently. So: open the pane URL asynchronously with activation, and on
    /// any error fall back to launching System Settings by bundle identifier.
    func openSettings() {
        Self.openSettingsPane(AppConstants.settingsURLs[...])
    }

    private static func openSettingsPane(_ candidates: ArraySlice<String>) {
        guard let first = candidates.first, let url = URL(string: first) else {
            launchSystemSettings()
            return
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        NSWorkspace.shared.open(url, configuration: configuration) { @Sendable app, error in
            if app == nil || error != nil {
                Task { @MainActor in openSettingsPane(candidates.dropFirst()) }
            }
        }
    }

    private static func launchSystemSettings() {
        let workspace = NSWorkspace.shared
        guard let appURL = workspace.urlForApplication(withBundleIdentifier: "com.apple.systempreferences") else {
            return
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        workspace.openApplication(at: appURL, configuration: configuration) { @Sendable _, _ in }
    }
}

// MARK: - Views

struct ContentView: View {
    @EnvironmentObject private var status: ExtensionStatusModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header
                StatusCard()
                EnableSteps()
                UsageCard()
                LicenseCard()
            }
            .padding(24)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var header: some View {
        HStack(alignment: .center, spacing: 14) {
            Image(nsImage: NSApplication.shared.applicationIconImage)
                .resizable()
                .frame(width: 64, height: 64)
            VStack(alignment: .leading, spacing: 2) {
                Text("NTFS4Mac")
                    .font(.largeTitle.bold())
                Text("Read-write NTFS for macOS, in user space, via FSKit and libntfs-3g.")
                    .foregroundStyle(.secondary)
                HStack(spacing: 6) {
                    Text("by \(AppConstants.author)")
                    Text("·")
                    Link("GitHub", destination: AppConstants.projectURL)
                }
                .font(.callout)
                .foregroundStyle(.secondary)
            }
        }
    }
}

struct StatusCard: View {
    @EnvironmentObject private var status: ExtensionStatusModel

    var body: some View {
        GroupBox {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: status.state.symbol)
                    .font(.title2)
                    .foregroundStyle(status.state.tint)
                VStack(alignment: .leading, spacing: 4) {
                    Text("File system extension: \(status.state.title)")
                        .font(.headline)
                    if case .unknown(let reason) = status.state {
                        Text(reason)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                    if status.state == .notInstalled {
                        Text("Move NTFS4Mac.app to /Applications and open it once so macOS registers the extension.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 8) {
                    Button("Open System Settings…") { status.openSettings() }
                        .keyboardShortcut(.defaultAction)
                    Button("Refresh") { status.refresh() }
                }
            }
            .padding(6)
        } label: {
            Label("Status", systemImage: "info.circle")
        }
    }
}

struct EnableSteps: View {
    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                step(1, "Open System Settings → General → Login Items & Extensions.")
                step(2, "Scroll to Extensions and choose “By Category”.")
                step(3, "Click the ⓘ button next to “File System Extensions”.")
                step(4, "Turn on “NTFS (ntfs4mac)”, then click Done.")
                step(5, "Unplug and reconnect the NTFS disk (or remount it) so macOS probes it again.")
                Text("If the system's built-in read-only NTFS driver already mounted the disk, unmount it first; the extension is only chosen when a volume is probed.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .padding(.top, 4)
            }
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
        } label: {
            Label("Enable the extension", systemImage: "switch.2")
        }
    }

    private func step(_ n: Int, _ text: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text("\(n).")
                .monospacedDigit()
                .foregroundStyle(.secondary)
            Text(text)
        }
    }
}

struct UsageCard: View {
    private let commands = """
    # mount (as your user, not root)
    mkdir -p /tmp/ntfs
    mount -F -t \(AppConstants.fsShortName) /dev/diskXsY /tmp/ntfs
    mount -F -t \(AppConstants.fsShortName) -o rdonly /dev/diskXsY /tmp/ntfs

    # unmount
    umount /tmp/ntfs
    """

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                Text("Disks that macOS probes are mounted automatically once the extension is enabled. From Terminal:")
                Text(commands)
                    .font(.system(.callout, design: .monospaced))
                    .textSelection(.enabled)
                    .padding(8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 6))
                Text("Mount options: rdonly, remove_hiberfile, recover, show_sys_files, ignore_case, uid=N, gid=N.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            .padding(6)
        } label: {
            Label("Usage", systemImage: "terminal")
        }
    }
}

struct LicenseCard: View {
    private let notice = """
    NTFS4Mac — Copyright © 2026 Vahid Zekic.

    NTFS4Mac includes libntfs-3g and mkntfs from the NTFS-3G project \
    (https://github.com/tuxera/ntfs-3g), Copyright © the NTFS-3G and \
    Linux-NTFS developers, including Tuxera Inc.

    libntfs-3g and mkntfs are licensed under the GNU General Public License, \
    version 2 or (at your option) any later version. Because the file system \
    extension links them, NTFS4Mac as a whole is distributed under the GNU \
    GPL v2 or later.

    This program is distributed in the hope that it will be useful, but \
    WITHOUT ANY WARRANTY; without even the implied warranty of \
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General \
    Public License for more details.

    You are entitled to the complete corresponding source code of this \
    program and of the bundled libraries under the terms of the GPL.
    """

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                Text(notice)
                    .font(.callout)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 16) {
                    Link("GNU GPL v2", destination: AppConstants.gplURL)
                    Link("NTFS-3G source", destination: AppConstants.ntfs3gSourceURL)
                }
            }
            .padding(6)
            .frame(maxWidth: .infinity, alignment: .leading)
        } label: {
            Label("License", systemImage: "doc.text")
        }
    }
}
