// User settings, saved to %APPDATA%\LostIsland\config.json.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct Config {
    // Island
    std::wstring shape = L"island";      // island | notch
    std::wstring face = L"logo";         // logo (Windows mark + clock) | weather (clock + weather)
    std::wstring monitor = L"primary";   // primary | cursor
    int offsetY = 8;                     // gap from the top edge, island shape only
    int scale = 100;                     // percent, 80..150
    bool hoverOpen = false;              // open the card when the pointer rests on the pill
    bool hideFullscreen = true;          // hide the resting pill over fullscreen apps
    bool reducedMotion = false;          // no overshoot, short fades

    // Appearance
    std::wstring accent = L"blue";       // macOS accent names
    std::wstring cardStyle = L"black";   // black | graphite
    bool clock24 = false;
    bool showSeconds = false;
    bool showEq = true;

    // Live activities and peeks
    bool actMusic = true, actTimer = true, actClaude = true, actDownloads = true;
    bool peekVolume = true, peekCaps = true, peekCharger = true, peekTrack = true;
    bool timerChime = true;
    int timerMinutes = 25;

    // Weather
    std::wstring weatherCity;            // empty = locate by IP
    bool fahrenheit = false;

    // Wallpaper picker
    std::wstring wallpaperFolder;        // empty = Pictures\Wallpapers
    std::wstring wallpaperFit = L"fill"; // fill | fit | stretch | center | tile | span

    // Card layout
    std::vector<std::wstring> controls;
    std::vector<std::wstring> launchers;

    // Keyboard
    std::wstring hotkey = L"ctrl+alt+i"; // ctrl+alt+i | win+alt+i | ctrl+shift+space | off

    void Load();
    void Save() const;
    void Reset();
    static std::wstring Path();
};

struct ControlDef {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* icon;       // vector icon name (icons.cpp)
    bool toggle;               // has an on/off state
};
const std::vector<ControlDef>& AllControls();
const ControlDef* FindControl(const std::wstring& id);
const std::vector<std::wstring>& DefaultControls();

struct LauncherDef {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* command;    // run in Windows Terminal (or conhost)
    unsigned color;            // 0xRRGGBB brand tint for the tile
};
const std::vector<LauncherDef>& AllLaunchers();
const LauncherDef* FindLauncher(const std::wstring& id);
const std::vector<std::wstring>& DefaultLaunchers();

struct AccentDef { const wchar_t* id; const wchar_t* label; unsigned rgb; };
const std::vector<AccentDef>& AllAccents();
unsigned AccentRGB(const std::wstring& id);

extern Config g_config;
