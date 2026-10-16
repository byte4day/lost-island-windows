// The island window: resting pill, live activities, peeks and the card.
#pragma once
#include <windows.h>
#include <string>

constexpr UINT WM_APP_CMD = WM_APP + 41;   // wParam = IslandCmd, from a second instance or the tray
constexpr UINT WM_APP_TRAY = WM_APP + 42;

enum IslandCmd : WPARAM { CMD_TOGGLE = 1, CMD_TIMER, CMD_SETTINGS, CMD_QUIT, CMD_OPEN, CMD_CLOSE };

namespace island {
bool Create(HINSTANCE inst);
void Destroy();
HWND Hwnd();
void ApplyConfig();          // relayout, re-register the hotkey, refresh services that read config
void ToggleCard();
void ToggleTimer();
// A short message in the pill: icon name from icons.cpp, optional 0..1 bar (-1 = none).
void Peek(const std::wstring& icon, const std::wstring& title, const std::wstring& detail = L"", float bar = -1);
// For screenshots and tests: open a given state without user input.
void DebugState(const std::wstring& state);
void SetDemo(bool on);      // --demo: sample music and a Claude request, for screenshots
}
