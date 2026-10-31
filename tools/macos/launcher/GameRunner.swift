// Prepares ~/Library/Application Support/God of War/ and runs the game (the runner, Contents/MacOS/GodOfWar):
//   data/      symlinks to the disc folder's files + IOP_MOD/<name>.irx (the game loads its modules from IOP_MOD/
//              next to the ELF); the runner's working directory
//   saves/     GodOfWar.sav (GOW_MC0, a PCSX2 .ps2 card image), copied to GodOfWar.sav.bak before every start
//   ~/Library/Logs/God of War/game.log (the previous run's log is kept as game.log.1)
import Foundation

struct LaunchError: Error {
    let message: String
}

enum GameRunner {
    // the disc folder's test cards stay out of data/ (as in tools/pc_release/make_pc_folder.ps1)
    static let skipped: Set<String> = ["Mcd001.ps2", "play.mcd", "mc0", "mc1", "IOP_MOD", ".DS_Store", "imgui.ini"]

    static func prepare(disc: URL) throws {
        let fm = FileManager.default
        guard Paths.isDisc(disc) else {
            throw LaunchError(message: "\(disc.path) does not hold \(Paths.elfName). Choose the extracted disc folder.")
        }
        try fm.createDirectory(at: Paths.modules, withIntermediateDirectories: true)
        try fm.createDirectory(at: Paths.saves, withIntermediateDirectories: true)
        // links from a previous start (maybe to another disc folder); real files the game wrote stay
        removeLinks(in: Paths.data)
        removeLinks(in: Paths.modules)
        for name in try fm.contentsOfDirectory(atPath: disc.path) where !skipped.contains(name) {
            let target = disc.appendingPathComponent(name)
            let link = Paths.data.appendingPathComponent(name)
            if (try? fm.attributesOfItem(atPath: link.path)) == nil {
                try fm.createSymbolicLink(at: link, withDestinationURL: target)
            }
            if name.uppercased().hasSuffix(".IRX") {
                try fm.createSymbolicLink(at: Paths.modules.appendingPathComponent(name.lowercased()),
                                          withDestinationURL: target)
            }
        }
        // a broken save never costs the previous one
        if fm.fileExists(atPath: Paths.save.path) {
            try? fm.removeItem(at: Paths.backup)
            try fm.copyItem(at: Paths.save, to: Paths.backup)
        }
    }

    // copies the build's test card (out/build-mac/run/Mcd001.ps2) as the save file, once
    static func importCard(_ card: URL) throws {
        try FileManager.default.createDirectory(at: Paths.saves, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: card, to: Paths.save)
    }

    static func environment(_ settings: Settings) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        // release settings of play.cmd / the Windows launcher: GS worker thread, FINISH without waiting
        env["PS2X_GS_WORKER"] = "1"
        env["GOW_GS_FINISH_ASINCRONO"] = "1"
        if env["GOW_AUDIO"] == nil { // test runs mute with GOW_AUDIO=0
            env["GOW_AUDIO_DEVICE"] = "default"
        }
        env["GOW_MC0"] = Paths.save.path
        env["GOW_WINDOW"] = "\(settings.width)x\(settings.height)"
        env["GOW_FULLSCREEN"] = settings.fullscreen ? "1" : "0"
        env["PS2X_GS_HW_SCALE"] = String(settings.scale)
        env["GOW_GS_MTL_AA"] = settings.fxaa ? "fxaa" : "off"
        env["GOW_GS_MTL_ANISO"] = String(settings.aniso)
        env["GOW_GS_MTL_METALFX"] = settings.metalFX ? "spatial" : "off"
        env["GOW_SHARPEN"] = settings.sharpen < 0 ? "off" : String(format: "%.2f", settings.sharpen)
        if settings.wideView {
            env["GOW_PANORAMICO"] = "ampliado"
        }
        if settings.hdTextures && !settings.texturesFolder.isEmpty {
            env["GOW_TEXTURES_DIR"] = settings.texturesFolder
        } else {
            env["GOW_GS_MTL_TEXREPL"] = "0"
        }
        // text and voice language + the port's save wording (lang/ is linked into data/ with the disc's files)
        if let overlay = settings.languageOverlay {
            env["PS2X_DISC_OVERLAY"] = overlay
        }
        return env
    }

    // starts the game; onExit runs on the main thread with the runner's exit status
    static func launch(_ settings: Settings, onExit: @escaping (Int32, Process.TerminationReason) -> Void) throws -> Process {
        let fm = FileManager.default
        guard fm.isExecutableFile(atPath: Paths.runner.path) else {
            throw LaunchError(message: "The game program is missing: \(Paths.runner.path). Run tools/macos/make_app.sh again.")
        }
        try fm.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        if fm.fileExists(atPath: Paths.log.path) {
            try? fm.removeItem(at: Paths.previousLog)
            try? fm.moveItem(at: Paths.log, to: Paths.previousLog)
        }
        fm.createFile(atPath: Paths.log.path, contents: nil)
        let log = try FileHandle(forWritingTo: Paths.log)

        let elf = Paths.data.appendingPathComponent(Paths.elfName).path
        let env = environment(settings)
        let keys = ["GOW_MC0", "GOW_WINDOW", "GOW_FULLSCREEN", "PS2X_GS_HW_SCALE", "PS2X_GS_WORKER",
                    "GOW_GS_FINISH_ASINCRONO", "GOW_AUDIO_DEVICE", "PS2X_DISC_OVERLAY", "GOW_GS_MTL_AA",
                    "GOW_GS_MTL_ANISO", "GOW_GS_MTL_METALFX", "GOW_SHARPEN", "GOW_PANORAMICO",
                    "GOW_TEXTURES_DIR", "GOW_GS_MTL_TEXREPL"]
        var header = "[launcher] \(Date()) disc=\(settings.discFolder)\n[launcher] cwd=\(Paths.data.path) elf=\(elf)\n"
        for key in keys { header += "[launcher] \(key)=\(env[key] ?? "(unset)")\n" }
        log.write(header.data(using: .utf8)!)

        let process = Process()
        process.executableURL = Paths.runner
        process.arguments = [elf]
        process.currentDirectoryURL = Paths.data
        process.environment = env
        process.standardOutput = log
        process.standardError = log
        process.terminationHandler = { p in
            try? log.close()
            DispatchQueue.main.async { onExit(p.terminationStatus, p.terminationReason) }
        }
        try process.run()
        return process
    }

    private static func removeLinks(in folder: URL) {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: folder.path) else { return }
        for name in names {
            let item = folder.appendingPathComponent(name)
            if let type = (try? fm.attributesOfItem(atPath: item.path))?[.type] as? FileAttributeType,
               type == .typeSymbolicLink {
                try? fm.removeItem(at: item)
            }
        }
    }
}
