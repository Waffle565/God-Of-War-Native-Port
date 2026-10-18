// GOW-Port: in-game settings panel (runtime/settings_menu.h, docs/settings.md "In-game settings").
#include "runtime/settings_menu.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
struct Language
{
    const char *code;
    const char *name;
};
// every language a pack can carry; GOW_LANGUAGES says which ones this folder has
constexpr Language kLanguages[] = {{"en", "English"}, {"fr", "Fran\xC3\xA7" "ais"}, {"de", "Deutsch"}, {"it", "Italiano"},
                                   {"es", "Espa\xC3\xB1ol"}, {"ja", "Japanese"}, {"ko", "Korean"}, {"zh", "Chinese"}};

enum Row
{
    kResolution,
    kFullscreen,
    kText,
    kVoice,
    kRestart,
    kResume,
    kQuit,
    kRows
};

std::atomic<bool> g_open{false};
// after closing, the game keeps a released pad until the closing key or button is let go (Esc and Enter are also
// the game's circle and Start)
std::atomic<bool> g_holding{false};
int g_exitCode = 0;

struct State
{
    bool ready = false;
    std::vector<std::pair<int, int>> modes;
    std::vector<int> languages; // indices into kLanguages offered in this folder
    int mode = 0;
    int text = 0, voice = 0;       // indices into languages: chosen
    int runText = 0, runVoice = 0; // indices into languages: what this run uses
    int row = 0;
    std::string ini;
} g;

int languageIndex(const char *code)
{
    for (size_t i = 0; i < g.languages.size(); ++i)
        if (code && std::strcmp(kLanguages[g.languages[i]].code, code) == 0)
            return static_cast<int>(i);
    return 0;
}

void setup()
{
    g.ready = true;
    if (const char *ini = std::getenv("GOW_SETTINGS"))
        g.ini = ini;
    g.languages.push_back(0);
    if (const char *list = std::getenv("GOW_LANGUAGES"))
    {
        std::stringstream codes(list);
        std::string code;
        while (std::getline(codes, code, ','))
            for (int i = 1; i < static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0])); ++i)
                if (code == kLanguages[i].code && std::find(g.languages.begin(), g.languages.end(), i) == g.languages.end())
                    g.languages.push_back(i);
    }
    g.text = g.runText = languageIndex(std::getenv("GOW_TEXT"));
    g.voice = g.runVoice = languageIndex(std::getenv("GOW_VOICE"));

    // 16:9 sizes up to the monitor, plus the monitor's own and the current window's
    const int monitor = GetCurrentMonitor();
    const int maxWidth = std::max(GetMonitorWidth(monitor), 1280), maxHeight = std::max(GetMonitorHeight(monitor), 720);
    static const std::pair<int, int> kSizes[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080},
                                                 {2560, 1440}, {3200, 1800}, {3840, 2160}};
    for (const auto &size : kSizes)
        if (size.first <= maxWidth && size.second <= maxHeight)
            g.modes.push_back(size);
    const std::pair<int, int> extra[] = {{maxWidth, maxHeight}, {GetScreenWidth(), GetScreenHeight()}};
    for (const auto &size : extra)
        if (std::find(g.modes.begin(), g.modes.end(), size) == g.modes.end())
            g.modes.push_back(size);
    std::sort(g.modes.begin(), g.modes.end());
    const auto current = std::find(g.modes.begin(), g.modes.end(), std::make_pair(GetScreenWidth(), GetScreenHeight()));
    g.mode = static_cast<int>(current - g.modes.begin());
}

// settings.ini: replaces key=value in [section] (adds both when missing), keeps every other line
void writeIni(const char *section, const char *key, const std::string &value)
{
    if (g.ini.empty())
        return;
    std::vector<std::string> lines;
    {
        std::ifstream in(g.ini);
        for (std::string line; std::getline(in, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }
    const std::string header = std::string("[") + section + "]", prefix = std::string(key) + "=";
    auto sectionAt = std::find(lines.begin(), lines.end(), header);
    if (sectionAt == lines.end())
    {
        lines.push_back(header);
        lines.push_back(prefix + value);
    }
    else
    {
        auto end = std::find_if(sectionAt + 1, lines.end(), [](const std::string &l) { return !l.empty() && l[0] == '['; });
        auto keyAt = std::find_if(sectionAt + 1, end, [&](const std::string &l) { return l.compare(0, prefix.size(), prefix) == 0; });
        if (keyAt != end)
            *keyAt = prefix + value;
        else
            lines.insert(end, prefix + value);
    }
    std::ofstream out(g.ini, std::ios::binary | std::ios::trunc);
    for (const auto &line : lines)
        out << line << "\r\n";
}

void applyMode()
{
    const auto [width, height] = g.modes[g.mode];
    const bool fullscreen = IsWindowFullscreen();
    if (fullscreen)
        ToggleFullscreen(); // the monitor mode follows the window size when entering fullscreen
    SetWindowSize(width, height);
    if (fullscreen)
        ToggleFullscreen();
    else
    {
        const int monitor = GetCurrentMonitor();
        SetWindowPosition(std::max(0, (GetMonitorWidth(monitor) - width) / 2), std::max(0, (GetMonitorHeight(monitor) - height) / 2));
    }
    writeIni("Video", "Width", std::to_string(width));
    writeIni("Video", "Height", std::to_string(height));
}

// press edges from the held state, sampled once per update(): raylib's IsKeyPressed only reports a press in the one
// frame between two input polls and misses short presses when the host loop does not draw every poll
std::map<int, bool> g_wasDown;

bool edge(int id, bool down)
{
    bool &was = g_wasDown[id];
    const bool pressed = down && !was;
    was = down;
    return pressed;
}

bool padDown(int button)
{
    for (int i = 0; i < 4; ++i)
        if (IsGamepadAvailable(i) && IsGamepadButtonDown(i, button))
            return true;
    return false;
}

bool keyPressed(int key)
{
    return edge(key, IsKeyDown(key));
}

bool padPressed(int button)
{
    return edge(0x10000 + button, padDown(button));
}

bool anyHeld()
{
    for (int key : {KEY_ESCAPE, KEY_ENTER, KEY_F1, KEY_C, KEY_X})
        if (IsKeyDown(key))
            return true;
    for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
        if (padDown(button))
            return true;
    return false;
}

void close()
{
    g_open.store(false, std::memory_order_relaxed);
    g_holding.store(true, std::memory_order_relaxed);
}

bool toggleRequested()
{
    if (keyPressed(KEY_F1) || padPressed(GAMEPAD_BUTTON_MIDDLE))
        return true;
    // Select+Start: the second of the two pressed while the first is held
    return (padPressed(GAMEPAD_BUTTON_MIDDLE_LEFT) && padDown(GAMEPAD_BUTTON_MIDDLE_RIGHT)) ||
           (padPressed(GAMEPAD_BUTTON_MIDDLE_RIGHT) && padDown(GAMEPAD_BUTTON_MIDDLE_LEFT));
}

bool rowEnabled(int row)
{
    if (row == kText || row == kVoice)
        return g.languages.size() > 1;
    if (row == kRestart)
        return g.text != g.runText || g.voice != g.runVoice;
    return true;
}

std::string rowValue(int row)
{
    switch (row)
    {
    case kResolution:
        return std::to_string(g.modes[g.mode].first) + " x " + std::to_string(g.modes[g.mode].second);
    case kFullscreen:
        return IsWindowFullscreen() ? "On" : "Off";
    case kText:
        return kLanguages[g.languages[g.text]].name;
    case kVoice:
        return kLanguages[g.languages[g.voice]].name;
    default:
        return {};
    }
}

const char *rowLabel(int row)
{
    static const char *const kLabels[kRows] = {"Resolution", "Fullscreen", "Text", "Voices", "Restart to apply languages",
                                               "Resume", "Quit game"};
    return kLabels[row];
}

// Left/Right on a row
void change(int row, int step)
{
    const int languages = static_cast<int>(g.languages.size());
    switch (row)
    {
    case kResolution:
        g.mode = (g.mode + step + static_cast<int>(g.modes.size())) % static_cast<int>(g.modes.size());
        applyMode();
        break;
    case kFullscreen:
        ToggleFullscreen();
        writeIni("Video", "Fullscreen", IsWindowFullscreen() ? "1" : "0");
        break;
    case kText:
        g.text = (g.text + step + languages) % languages;
        writeIni("Game", "Text", kLanguages[g.languages[g.text]].code);
        break;
    case kVoice:
        g.voice = (g.voice + step + languages) % languages;
        writeIni("Game", "Voice", kLanguages[g.languages[g.voice]].code);
        break;
    default:
        break;
    }
}
} // namespace

namespace settings_menu
{
bool isOpen()
{
    return g_open.load(std::memory_order_relaxed) || g_holding.load(std::memory_order_relaxed);
}

int exitCode()
{
    return g_exitCode;
}

bool update()
{
    if (!g.ready)
        setup();
    if (g_holding.load(std::memory_order_relaxed) && !anyHeld())
        g_holding.store(false, std::memory_order_relaxed);
    const bool open = g_open.load(std::memory_order_relaxed);
    if (toggleRequested())
    {
        if (open)
            close();
        else
            g_open.store(true, std::memory_order_relaxed);
        g.row = kResolution;
        return false;
    }
    if (!open)
        return false;

    const auto move = [](int step)
    {
        do
            g.row = (g.row + step + kRows) % kRows;
        while (!rowEnabled(g.row));
    };
    if (keyPressed(KEY_UP) || keyPressed(KEY_W) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP))
        move(-1);
    if (keyPressed(KEY_DOWN) || keyPressed(KEY_S) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
        move(1);
    if (!rowEnabled(g.row))
        move(1);
    if (keyPressed(KEY_LEFT) || keyPressed(KEY_A) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
        change(g.row, -1);
    if (keyPressed(KEY_RIGHT) || keyPressed(KEY_D) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
        change(g.row, 1);
    // Esc is the pad's circle in the game's keyboard map: back, like circle on a pad
    if (keyPressed(KEY_ESCAPE) || keyPressed(KEY_C) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
    {
        close();
        return false;
    }
    if (keyPressed(KEY_ENTER) || keyPressed(KEY_X) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
    {
        switch (g.row)
        {
        case kRestart:
            g_exitCode = kRestartExitCode;
            return true;
        case kResume:
            close();
            break;
        case kQuit:
            return true;
        default:
            change(g.row, 1);
            break;
        }
    }
    return false;
}

void draw()
{
    if (!g_open.load(std::memory_order_relaxed))
        return;
    const int width = GetScreenWidth(), height = GetScreenHeight();
    DrawRectangle(0, 0, width, height, Color{0, 0, 0, 170});
    const int font = std::max(16, height / 28), line = font * 2;
    const int panelWidth = std::min(width - 2 * font, font * 26);
    const int panelHeight = line * (kRows + 3);
    const int x = (width - panelWidth) / 2, y = (height - panelHeight) / 2;
    DrawRectangle(x, y, panelWidth, panelHeight, Color{24, 18, 14, 235});
    DrawRectangleLinesEx(Rectangle{float(x), float(y), float(panelWidth), float(panelHeight)}, 2.0f, Color{170, 40, 30, 255});
    DrawText("SETTINGS", x + font, y + font / 2 + 4, font + font / 3, Color{230, 200, 150, 255});
    for (int row = 0; row < kRows; ++row)
    {
        const int rowY = y + line * (row + 1) + font / 2;
        const bool selected = row == g.row, enabled = rowEnabled(row);
        if (selected)
            DrawRectangle(x + 4, rowY - font / 3, panelWidth - 8, font + font * 2 / 3, Color{120, 30, 22, 200});
        const Color color = !enabled ? Color{110, 100, 90, 255} : selected ? WHITE : Color{215, 205, 190, 255};
        DrawText(rowLabel(row), x + font, rowY, font, color);
        const std::string value = rowValue(row);
        if (!value.empty())
        {
            const std::string shown = enabled ? "< " + value + " >" : value;
            DrawText(shown.c_str(), x + panelWidth - font - MeasureText(shown.c_str(), font), rowY, font, color);
        }
    }
    const char *hint = g.languages.size() <= 1 ? "Other languages need the European disc (see README.txt)"
                       : rowEnabled(kRestart) ? "Languages change after a restart; unsaved progress is lost"
                       : g.ini.empty()        ? "Changes are not saved (no settings.ini)"
                                              : "Up/Down choose, Left/Right change, Esc closes";
    DrawText(hint, x + font, y + panelHeight - line + font / 4, font * 3 / 4, Color{180, 170, 155, 255});
}
} // namespace settings_menu
