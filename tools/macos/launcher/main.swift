// God of War.app launcher. `God of War --play` (or `open "God of War.app" --args --play`) skips the window.
// While the game runs the launcher leaves the Dock; it comes back when the game exits (or quits with --play).
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    let playOnly = CommandLine.arguments.contains("--play")
    var settings = Settings.load()
    var launcher: LauncherWindowController?
    var game: Process?

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenu()
        if !settings.exists { settings.save() }
        if playOnly {
            start()
        } else {
            showLauncher()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        game == nil
    }

    private func showLauncher() {
        if launcher == nil {
            launcher = LauncherWindowController(settings: settings) { [weak self] in
                guard let self, let launcher = self.launcher else { return }
                self.settings = launcher.settings
                self.start()
            }
        }
        NSApp.setActivationPolicy(.regular)
        launcher!.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func start() {
        offerCardImport()
        // not fatal: without the keyboard overlays the game shows the PS2 buttons
        if let problem = GameRunner.prepareKeyPrompts(settings), !playOnly {
            fail("Keyboard button prompts are not available", problem)
        }
        do {
            try GameRunner.prepare(disc: URL(fileURLWithPath: settings.discFolder))
            game = try GameRunner.launch(settings) { [weak self] status, reason in
                self?.gameEnded(status: status, reason: reason)
            }
        } catch {
            let message = (error as? LaunchError)?.message ?? error.localizedDescription
            fail("The game could not start", message)
            if playOnly { NSApp.terminate(nil) }
            return
        }
        launcher?.window?.orderOut(nil)
        NSApp.setActivationPolicy(.accessory)
    }

    private func gameEnded(status: Int32, reason: Process.TerminationReason) {
        game = nil
        if playOnly {
            NSApp.terminate(nil)
            return
        }
        showLauncher()
        if reason == .exit && status != 0 {
            fail("The game stopped with an error (code \(status))", "Log: \(Paths.log.path)")
        }
    }

    // first start only: a card from the development runs (out/build-mac/run/Mcd001.ps2) can become the save file
    private func offerCardImport() {
        guard !settings.importOffered, !FileManager.default.fileExists(atPath: Paths.save.path),
              let card = Paths.bundled("GOWImportCard"), FileManager.default.fileExists(atPath: card.path) else { return }
        settings.importOffered = true
        settings.save()
        launcher?.settings.importOffered = true
        let alert = NSAlert()
        alert.messageText = "Import your existing saves?"
        alert.informativeText = "A memory card from an earlier run was found:\n\(card.path)\n\nCopy it to \(Paths.save.path)?"
        alert.addButton(withTitle: "Import")
        alert.addButton(withTitle: "Start empty")
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertFirstButtonReturn {
            do { try GameRunner.importCard(card) } catch { fail("The card could not be copied", error.localizedDescription) }
        }
    }

    private func fail(_ title: String, _ detail: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = detail
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    private func buildMenu() {
        let menu = NSMenu()
        let appItem = NSMenuItem()
        menu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Hide God of War", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit God of War", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        NSApp.mainMenu = menu
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
