#pragma once

// GOW-Port: the in-game settings panel of the PC build (docs/settings.md "In-game settings").
// F1, the pad's guide button or Select+Start open it over the running game; while it is open the game sees a released
// pad. Resolution and fullscreen apply at once; text and voice languages are read from the disc at start, so they
// apply through "Restart": the runner exits with kRestartExitCode and the launcher starts it again.
// The launcher passes the state through the environment:
//   GOW_SETTINGS   settings.ini to write ([Video] Width/Height/Fullscreen, [Game] Text/Voice); unset: nothing is saved
//   GOW_TEXT, GOW_VOICE   the languages of this run (en fr de it es ja)
//   GOW_TEXTS, GOW_VOICES the text / voice codes the folder has overlays for, comma separated; unset: English only
namespace settings_menu
{
constexpr int kRestartExitCode = 3;

// host thread, once per host frame before drawing: open/close keys, navigation, applying changes. Returns true when
// the runner has to stop (Restart or Quit chosen).
bool update();
// host thread, inside the frame's drawing (after the game picture)
void draw();
// any thread: true while the panel takes the input
bool isOpen();
// the process exit code main() returns (kRestartExitCode after "Restart")
int exitCode();
} // namespace settings_menu
