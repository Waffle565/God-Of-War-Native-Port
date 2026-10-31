// The launcher window: disc folder, resolution, fullscreen, internal resolution, controls, Play/Quit.
// Text and voice language come from the disc folder's lang/ overlays (tools/lang_pack.py with the European disc).
// 120 Hz (S7) and graphics options (S8) are shown greyed out until they exist.
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
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 580),
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
        let graphics = NSButton(title: "Graphics options…", target: nil, action: nil)
        graphics.isEnabled = false

        let grid = NSGridView(views: [
            [label("Disc folder:"), discRow],
            [label("Resolution:"), resolution],
            [NSGridCell.emptyContentView, fullscreen],
            [label("Internal resolution:"), scale],
            [label("Language:"), languageRow],
            [NSGridCell.emptyContentView, languageNote],
            [label("Frame rate:"), soon(hz)],
            [label("Graphics:"), soon(graphics)],
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

    @objc private func play() {
        changed()
        onPlay()
    }
}
