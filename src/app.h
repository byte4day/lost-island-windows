// App-wide entry points shared by the island, the control panel and the tray.
#pragma once
#include <windows.h>

namespace app {
HINSTANCE Instance();
float DpiScaleFor(HWND hwnd);       // monitor DPI / 96
void ConfigChanged();               // call after editing g_config: saves, relayouts, re-registers the hotkey
void OpenSettings(const wchar_t* page = nullptr);
void ToggleCard();
void ToggleTimer();
void Quit();
}

// Control panel window (panel.cpp).
namespace panel {
void Show(const wchar_t* page = nullptr);   // page ids: general, island, appearance, controls, activities, claude, keyboard, about
void Close();
bool IsOpen();
}
