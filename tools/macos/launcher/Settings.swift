// Folders of the macOS app and the player's settings (settings.ini, same keys as the Windows launcher plus
// [Video] Scale and [Game] DiscFolder). tools/macos/README.md.
import AppKit

enum Paths {
    // $HOME first so a test run can point the app at a scratch home
    static let home = URL(fileURLWithPath: ProcessInfo.processInfo.environment["HOME"] ?? NSHomeDirectory())
    static let root = home.appendingPathComponent("Library/Application Support/God of War")
    static let data = root.appendingPathComponent("data")
    static let modules = data.appendingPathComponent("IOP_MOD")
    static let saves = root.appendingPathComponent("saves")
    static let save = saves.appendingPathComponent("GodOfWar.sav")
    static let backup = saves.appendingPathComponent("GodOfWar.sav.bak")
    static let settings = root.appendingPathComponent("settings.ini")
    static let logs = home.appendingPathComponent("Library/Logs/God of War")
    static let log = logs.appendingPathComponent("game.log")
    static let previousLog = logs.appendingPathComponent("game.log.1")
    static let elfName = "SCUS_973.99"

    // the runner sits next to the launcher in Contents/MacOS
    static var runner: URL {
        Bundle.main.executableURL!.deletingLastPathComponent().appendingPathComponent("GodOfWar")
    }

    // written by tools/macos/make_app.sh: the project's disc-usa/ and the build's test card
    static func bundled(_ key: String) -> URL? {
        guard let path = Bundle.main.object(forInfoDictionaryKey: key) as? String, !path.isEmpty else { return nil }
        return URL(fileURLWithPath: path)
    }

    static func isDisc(_ folder: URL) -> Bool {
        FileManager.default.fileExists(atPath: folder.appendingPathComponent(elfName).path)
    }
}

struct Settings {
    var width = 1920, height = 1080
    var fullscreen = false
    var scale = 2 // PS2X_GS_HW_SCALE, 1-4
    var discFolder = ""
    var importOffered = false
    var text = "en", voice = "en" // overlay lang/<text>_<voice>.layout made by tools/lang_pack.py (docs/settings.md)

    static let languages = [("en", "English"), ("es", "Español"), ("fr", "Français"), ("de", "Deutsch"),
                            ("it", "Italiano")]

    // the language overlay for the chosen pair, relative to the disc folder, when the disc folder has it
    var languageOverlay: String? {
        let name = "lang/\(text)_\(voice).layout"
        let path = URL(fileURLWithPath: discFolder).appendingPathComponent(name).path
        return !discFolder.isEmpty && FileManager.default.fileExists(atPath: path) ? name : nil
    }

    static func load() -> Settings {
        var s = Settings()
        let values = readIni(Paths.settings)
        if let v = values["Video.Width"].flatMap(Int.init), v >= 320 { s.width = v }
        if let v = values["Video.Height"].flatMap(Int.init), v >= 240 { s.height = v }
        if let v = values["Video.Fullscreen"] { s.fullscreen = v == "1" }
        if let v = values["Video.Scale"].flatMap(Int.init) { s.scale = min(4, max(1, v)) }
        if let v = values["Game.DiscFolder"] { s.discFolder = v }
        if let v = values["Game.ImportOffered"] { s.importOffered = v == "1" }
        let codes = Set(languages.map { $0.0 })
        if let v = values["Game.Text"], codes.contains(v) { s.text = v }
        if let v = values["Game.Voice"], codes.contains(v) { s.voice = v }
        if s.discFolder.isEmpty, let disc = Paths.bundled("GOWDefaultDiscFolder"), Paths.isDisc(disc) {
            s.discFolder = disc.path
        }
        return s
    }

    var exists: Bool { FileManager.default.fileExists(atPath: Paths.settings.path) }

    func save() {
        let text = """
        [Video]
        Width=\(width)
        Height=\(height)
        Fullscreen=\(fullscreen ? 1 : 0)
        Scale=\(scale)
        [Game]
        DiscFolder=\(discFolder)
        ImportOffered=\(importOffered ? 1 : 0)
        Text=\(text)
        Voice=\(voice)

        """
        try? FileManager.default.createDirectory(at: Paths.root, withIntermediateDirectories: true)
        try? text.write(to: Paths.settings, atomically: true, encoding: .utf8)
    }

    // "Section.Key" -> value
    private static func readIni(_ url: URL) -> [String: String] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [:] }
        var section = "", values: [String: String] = [:]
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("[") && line.hasSuffix("]") {
                section = String(line.dropFirst().dropLast())
            } else if let eq = line.firstIndex(of: "="), !line.hasPrefix(";") {
                let key = line[..<eq].trimmingCharacters(in: .whitespaces)
                values[section + "." + key] = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            }
        }
        return values
    }
}

// window sizes offered: the main display's modes (in points, as GOW_WINDOW is) from 1024x576 up, plus the saved one
func displayModes(including current: (Int, Int)) -> [(Int, Int)] {
    var set = Set<[Int]>([[current.0, current.1]])
    let options = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
    if let modes = CGDisplayCopyAllDisplayModes(CGMainDisplayID(), options) as? [CGDisplayMode] {
        for mode in modes where mode.width >= 1024 && mode.height >= 576 {
            set.insert([mode.width, mode.height])
        }
    }
    return set.sorted { $0[0] != $1[0] ? $0[0] > $1[0] : $0[1] > $1[1] }.map { ($0[0], $0[1]) }
}
