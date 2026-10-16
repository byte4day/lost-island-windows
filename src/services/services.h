// Lost Island for Windows: system services.
// Every service is a small namespace with Start/Stop and plain getters.
// Services that change on their own post `WM_APP_SERVICE` to the island
// window with wParam = ServiceId, so the UI repaints only when needed.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

constexpr UINT WM_APP_SERVICE = WM_APP + 40;

enum ServiceId : WPARAM {
    SVC_MEDIA = 1,
    SVC_RADIOS,
    SVC_AUDIO,
    SVC_POWER,
    SVC_WEATHER,
    SVC_CLIPBOARD,
    SVC_CLAUDE,
    SVC_DOWNLOADS,
    SVC_NETWORK,
    SVC_WALLPAPER,
};

// ---------------------------------------------------------------- media
// Now playing, from Windows' Global System Media Transport Controls
// (the same source as the volume flyout's media card).
struct MediaInfo {
    bool active = false;        // a session exists
    bool playing = false;
    bool hasTimeline = false;   // false when the player publishes no length
    std::wstring title, artist, album;
    std::wstring appId;         // SourceAppUserModelId, e.g. "Spotify.exe"
    double position = 0;        // seconds, extrapolated while playing
    double duration = 0;        // seconds
    uint64_t artVersion = 0;    // bumps whenever `art` changes
    std::vector<uint8_t> art;   // encoded image bytes (PNG/JPEG), may be empty
};

namespace media {
void Start(HWND notify);
void Stop();
MediaInfo Get();
void PlayPause();
void Next();
void Previous();
void Seek(double seconds);
}

// ---------------------------------------------------------------- radios
// Wi-Fi and Bluetooth on/off through Windows.Devices.Radios.
enum class RadioKind { WiFi, Bluetooth };

namespace radios {
void Start(HWND notify);
void Stop();
bool Available(RadioKind kind);
bool IsOn(RadioKind kind);
void Set(RadioKind kind, bool on);   // async; posts SVC_RADIOS when done
}

// ---------------------------------------------------------------- audio
namespace audio {
void Start(HWND notify);
void Stop();
float Volume();            // 0..1 master output
bool Muted();
void SetVolume(float v);
void SetMuted(bool m);
bool MicMuted();
void SetMicMuted(bool m);
bool HasMic();
std::wstring OutputName(); // friendly name of the default output
void Rebind();             // default device changed (SVC_AUDIO with lParam=1)
float Peak();              // 0..1 current output level, drives the EQ bars
}

// ---------------------------------------------------------------- power
struct BatteryInfo {
    bool present = false;
    bool charging = false;
    bool pluggedIn = false;
    int percent = -1;
};
namespace power {
void Start(HWND notify);
BatteryInfo Get();
void Refresh();            // call on WM_POWERBROADCAST
}

// ---------------------------------------------------------------- brightness
namespace brightness {
void Start();              // probes on a worker thread; Available() is false until it answers
void Stop();
bool Available();
int Get();                 // cached 0..100, -1 if unknown
void Set(int percent);     // async
void Refresh();            // re-read in the background, posts SVC_POWER on change
}

// ---------------------------------------------------------------- network
namespace network {
void Start(HWND notify);
void Stop();
std::wstring WifiName();   // connected SSID or empty
bool Online();
}

// ---------------------------------------------------------------- weather
struct WeatherInfo {
    bool valid = false;
    int tempC = 0;
    std::wstring condition; // "Sunny", "Light rain"...
    std::wstring glyph;     // emoji-free icon key: sun, cloud, rain, snow, storm, fog, night
    std::wstring place;
};
namespace weather {
void Start(HWND notify);
void Stop();
WeatherInfo Get();
void Refresh();
}

// ---------------------------------------------------------------- clipboard
// Text history kept in memory only, never written to disk.
namespace clipboard {
void Start(HWND listener);           // call from the island window
void OnUpdate();                     // forward WM_CLIPBOARDUPDATE
std::vector<std::wstring> History(); // newest first, max 25
void Copy(const std::wstring& text);
void Clear();
}

// ---------------------------------------------------------------- claude
struct ClaudeSession {
    std::wstring session, state, detail, project;
    DWORD pid = 0;
    double updated = 0;      // unix seconds
};
namespace claude {
void Start(HWND notify);
void Stop();
std::vector<ClaudeSession> Visible();   // most urgent first
int RunHook();                          // `--claude-hook`: reads stdin JSON
bool Setup(std::wstring& message);      // add hooks to ~/.claude/settings.json
bool Remove(std::wstring& message);
bool Installed();
void Focus(const ClaudeSession& s);     // bring its terminal to the front
}

// ---------------------------------------------------------------- downloads
struct Transfer {
    std::wstring name;
    uint64_t bytes = 0;
    double rate = 0;          // bytes/sec
};
namespace downloads {
void Start(HWND notify);
void Stop();
std::vector<Transfer> Active();
}

// ---------------------------------------------------------------- wallpaper
// The card's wallpaper picker. Thumbnails decode on a worker thread and
// post SVC_WALLPAPER as each one lands.
namespace wallpaper {
constexpr UINT kThumbW = 320, kThumbH = 184;
struct Thumb {
    UINT w = 0, h = 0;               // 0 when the file could not be decoded
    std::vector<uint8_t> pixels;     // 32bpp premultiplied BGRA
};
void Start(HWND notify);
std::wstring Folder();               // config, else Pictures\Wallpapers, else Pictures
std::vector<std::wstring> List();    // image files in Folder(), sorted by name
std::wstring Name(const std::wstring& path);
bool Matches(const std::wstring& path, const std::wstring& query);
std::wstring Current();
const Thumb* Get(const std::wstring& path);   // nullptr until ready (queues it)
void Prioritise(const std::vector<std::wstring>& first);
void Release();                      // drop thumbnails while the picker is closed
bool Set(const std::wstring& path, std::wstring& error);
}

// ---------------------------------------------------------------- system
namespace sys {
bool DarkMode();
void SetDarkMode(bool dark);
bool NightLightSupported();
void OpenUri(const wchar_t* uri);
void Launch(const std::wstring& exe, const std::wstring& args = L"");
void LaunchInTerminal(const std::wstring& command);
void Lock();
void SignOut();
void Sleep();
void Restart();
void Shutdown();
bool Screenshot(bool toClipboard, std::wstring& savedPath);  // full virtual screen
void SnipArea();                   // Windows' own snipping overlay
void ToggleRecording();            // Snipping Tool screen recorder
std::wstring PickColor();          // returns "#RRGGBB" or empty
bool StartWithWindows();
void SetStartWithWindows(bool on);
std::wstring ExePath();
std::wstring ConfigDir();          // %APPDATA%\LostIsland
std::wstring CacheDir();           // %LOCALAPPDATA%\LostIsland
}
