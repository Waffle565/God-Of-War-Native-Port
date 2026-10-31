// The launcher window: disc folder, resolution, fullscreen, internal resolution, controls, Play/Quit.
// Text and voice language come from the disc folder's lang/ overlays (tools/lang_pack.py with the European disc).
// Graphics: anti-aliasing, anisotropic filtering, MetalFX, sharpening, wider view (docs/macos.md). 120 Hz (S7) is
// shown greyed out until it exists.
import AppKit

final class LauncherWindowController: NSWindowController {
    var settings: Settings
    let onPlay: () -> Void
    private var modes: [(Int, Int)] = []
    private let resolution = NSPopUpButton()
    private let fullscreen = NSButton(checkboxWithTitle: "Fullscreen", target: nil, action: nil)
    private let scale = NSPopUpButton()
    private let textLanguage = NSPopUpButton()
    private let voiceLanguage = NSPopUpButton()
    private let languageNote = NSTextField(labelWithString: "")
    private let fxaa = NSButton(checkboxWithTitle: "Anti-aliasing (FXAA)", target: nil, action: nil)
    private let aniso = NSPopUpButton()
    private let metalFX = NSButton(checkboxWithTitle: "MetalFX upscaling (Retina/4K)", target: nil, action: nil)
    private let sharpen = NSSlider(value: 0.5, minValue: 0, maxValue: 1, target: nil, action: nil)
    private let wideView = NSButton(checkboxWithTitle: "Wider field of view", target: nil, action: nil)
    private let hdTextures = NSButton(checkboxWithTitle: "HD texture pack", target: nil, action: nil)
    private let texturesPath = NSTextField(labelWithString: "")
    private let discPath = NSTextField(labelWithString: "")
    private let playButton = NSButton(title: "Play", target: nil, action: nil)

    static let controlsHelp = """
    Keyboard + mouse (a gamepad works at the same time)
    WASD  move            Arrows  evade (right stick)
    Left click  □ Square    Right click  △ Triangle
    Space  ✕ Cross          E  ○ Circle
    Q / R  L1 / R1          F / G  L2 / R2
    1-4  d-pad (magic)     Enter  Start    Backspace  Select
    F11  window / fullscreen
    """

    init(settings: Settings, onPlay: @escaping () -> Void) {
        self.settings = settings
        self.onPlay = onPlay
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 770),
                              styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "God of War"
        super.init(window: window)
        build()
        window.center()
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private func build() {
        let title = NSTextField(labelWithString: "God of War")
        title.font = .systemFont(ofSize: 22, weight: .semibold)

        discPath.lineBreakMode = .byTruncatingMiddle
        discPath.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let choose = NSButton(title: "Choose…", target: self, action: #selector(chooseDisc))
        let discRow = NSStackView(views: [discPath, choose])
        discRow.spacing = 8
        updateDiscLabel()

        modes = displayModes(including: (settings.width, settings.height))
        for (w, h) in modes { resolution.addItem(withTitle: "\(w) × \(h)") }
        resolution.selectItem(at: modes.firstIndex { $0 == (settings.width, settings.height) } ?? 0)
        resolution.target = self
        resolution.action = #selector(changed)

        fullscreen.state = settings.fullscreen ? .on : .off
        fullscreen.target = self
        fullscreen.action = #selector(changed)

        scale.addItems(withTitles: ["1× (PS2 native)", "2×", "3×", "4×"])
        scale.selectItem(at: settings.scale - 1)
        scale.target = self
        scale.action = #selector(changed)

        for popup in [textLanguage, voiceLanguage] {
            popup.addItems(withTitles: Settings.languages.map { $0.1 })
            popup.target = self
            popup.action = #selector(changed)
        }
        textLanguage.selectItem(at: Settings.languages.firstIndex { $0.0 == settings.text } ?? 0)
        voiceLanguage.selectItem(at: Settings.languages.firstIndex { $0.0 == settings.voice } ?? 0)
        languageNote.textColor = .tertiaryLabelColor
        languageNote.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        let languageRow = NSStackView(views: [label("Text"), textLanguage, label("Voice"), voiceLanguage])
        languageRow.spacing = 6
        updateLanguageState()
        let hz = NSButton(checkboxWithTitle: "120 Hz frame rate", target: nil, action: nil)
        hz.isEnabled = false
        fxaa.state = settings.fxaa ? .on : .off
        aniso.addItems(withTitles: Settings.anisoLevels.map { $0 == 0 ? "Off" : "\($0)×" })
        aniso.selectItem(at: Settings.anisoLevels.firstIndex(of: settings.aniso) ?? 0)
        metalFX.state = settings.metalFX ? .on : .off
        sharpen.doubleValue = max(0, settings.sharpen)
        wideView.state = settings.wideView ? .on : .off
        hdTextures.state = settings.hdTextures ? .on : .off
        texturesPath.lineBreakMode = .byTruncatingMiddle
        texturesPath.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        texturesPath.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let chooseTextures = NSButton(title: "Choose…", target: self, action: #selector(chooseTextures))
        let texturesRow = NSStackView(views: [hdTextures, texturesPath, chooseTextures])
        texturesRow.spacing = 6
        updateTexturesLabel()
        for control in [fxaa, aniso, metalFX, sharpen, wideView, hdTextures] as [NSControl] {
            control.target = self
            control.action = #selector(changed)
        }
        sharpen.widthAnchor.constraint(equalToConstant: 140).isActive = true
        let anisoRow = NSStackView(views: [label("Texture filtering"), aniso])
        let sharpenRow = NSStackView(views: [label("Sharpen"), sharpen])

        let grid = NSGridView(views: [
            [label("Disc folder:"), discRow],
            [label("Resolution:"), resolution],
            [NSGridCell.emptyContentView, fullscreen],
            [label("Internal resolution:"), scale],
            [label("Language:"), languageRow],
            [NSGridCell.emptyContentView, languageNote],
            [label("Frame rate:"), soon(hz)],
            [label("Graphics:"), fxaa],
            [NSGridCell.emptyContentView, anisoRow],
            [NSGridCell.emptyContentView, sharpenRow],
            [NSGridCell.emptyContentView, metalFX],
            [NSGridCell.emptyContentView, wideView],
            [label("Textures:"), texturesRow],
        ])
        grid.rowSpacing = 10
        grid.columnSpacing = 10
        grid.column(at: 0).xPlacement = .trailing
        grid.rowAlignment = .firstBaseline

        let controls = NSTextField(wrappingLabelWithString: Self.controlsHelp)
        controls.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        controls.textColor = .secondaryLabelColor
        let controlsBox = NSBox()
        controlsBox.title = "Controls"
        controlsBox.contentView = controls

        let quit = NSButton(title: "Quit", target: NSApp, action: #selector(NSApplication.terminate(_:)))
        quit.keyEquivalent = "\u{1b}"
        playButton.target = self
        playButton.action = #selector(play)
        playButton.keyEquivalent = "\r"
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let buttons = NSStackView(views: [spacer, quit, playButton])

        let stack = NSStackView(views: [title, grid, controlsBox, buttons])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 16
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 24, bottom: 20, right: 24)
        stack.translatesAutoresizingMaskIntoConstraints = false
        let content = window!.contentView!
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            stack.topAnchor.constraint(equalTo: content.topAnchor),
            stack.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            controlsBox.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -48),
            buttons.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -48),
            discRow.widthAnchor.constraint(lessThanOrEqualToConstant: 330),
        ])
    }

    private func label(_ text: String) -> NSTextField {
        NSTextField(labelWithString: text)
    }

    private func soon(_ control: NSView) -> NSView {
        let note = NSTextField(labelWithString: "coming soon")
        note.textColor = .tertiaryLabelColor
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        return NSStackView(views: [control, note])
    }

    private func updateDiscLabel() {
        let ok = !settings.discFolder.isEmpty && Paths.isDisc(URL(fileURLWithPath: settings.discFolder))
        discPath.stringValue = settings.discFolder.isEmpty ? "(not set)" : settings.discFolder
        discPath.textColor = ok ? .labelColor : .systemRed
        discPath.toolTip = settings.discFolder
        playButton.isEnabled = ok
        updateLanguageState()
    }

    // languages other than English need the overlays tools/lang_pack.py writes into <disc folder>/lang/
    private func updateLanguageState() {
        let langFolder = URL(fileURLWithPath: settings.discFolder).appendingPathComponent("lang").path
        let available = !settings.discFolder.isEmpty && FileManager.default.fileExists(atPath: langFolder)
        textLanguage.isEnabled = available
        voiceLanguage.isEnabled = available
        languageNote.stringValue = available
            ? "Menus and HUD screens stay in English."
            : "Other languages need the European disc: tools/lang_pack.py"
    }

    @objc private func changed() {
        let mode = modes[max(0, resolution.indexOfSelectedItem)]
        settings.width = mode.0
        settings.height = mode.1
        settings.fullscreen = fullscreen.state == .on
        settings.scale = scale.indexOfSelectedItem + 1
        settings.text = Settings.languages[max(0, textLanguage.indexOfSelectedItem)].0
        settings.voice = Settings.languages[max(0, voiceLanguage.indexOfSelectedItem)].0
        settings.fxaa = fxaa.state == .on
        settings.aniso = Settings.anisoLevels[max(0, aniso.indexOfSelectedItem)]
        settings.metalFX = metalFX.state == .on
        settings.sharpen = sharpen.doubleValue
        settings.wideView = wideView.state == .on
        settings.hdTextures = hdTextures.state == .on
        settings.save()
    }

    @objc private func chooseDisc() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = "Choose the folder with the extracted disc (it holds \(Paths.elfName))."
        if !settings.discFolder.isEmpty { panel.directoryURL = URL(fileURLWithPath: settings.discFolder) }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if !Paths.isDisc(url) {
            let alert = NSAlert()
            alert.messageText = "Not a God of War disc folder"
            alert.informativeText = "\(url.path) does not hold \(Paths.elfName)."
            alert.runModal()
            return
        }
        settings.discFolder = url.path
        settings.save()
        updateDiscLabel()
    }

    private func updateTexturesLabel() {
        texturesPath.stringValue = settings.texturesFolder.isEmpty ? "(no pack folder)"
            : URL(fileURLWithPath: settings.texturesFolder).lastPathComponent
        texturesPath.toolTip = settings.texturesFolder
        hdTextures.isEnabled = !settings.texturesFolder.isEmpty
    }

    // a PCSX2 pack for SCUS-97399: choose the folder with its replacement files (.dds/.png), not the pack's root
    @objc private func chooseTextures() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = "Choose the texture pack's replacements folder for SCUS-97399 (PCSX2 format)."
        if !settings.texturesFolder.isEmpty { panel.directoryURL = URL(fileURLWithPath: settings.texturesFolder) }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        settings.texturesFolder = url.path
        settings.hdTextures = true
        hdTextures.state = .on
        settings.save()
        updateTexturesLabel()
    }

    @objc private func play() {
        changed()
        onPlay()
    }
}
