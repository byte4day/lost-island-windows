#include "config.h"

#include <shlobj.h>
#include <algorithm>

#include "json.h"
#include "services.h"

Config g_config;

const std::vector<ControlDef>& AllControls() {
    static const std::vector<ControlDef> v = {
        {L"wifi", L"Wi-Fi", L"wifi", true},
        {L"bluetooth", L"Bluetooth", L"bluetooth", true},
        {L"sound", L"Sound", L"speaker", true},
        {L"mic", L"Mic", L"mic", true},
        {L"focus", L"Focus", L"moon", false},
        {L"dark", L"Dark", L"contrast", true},
        {L"cast", L"Cast", L"cast", false},
        {L"screenshot", L"Screen", L"screen", false},
        {L"snip", L"Area", L"crop", false},
        {L"color", L"Color", L"dropper", false},
        {L"record", L"Record", L"record", false},
        {L"clipboard", L"Clipboard", L"clipboard", false},
        {L"lock", L"Lock", L"lock", false},
        {L"nightlight", L"Night light", L"sun", false},
        {L"power", L"Power", L"power", false},
        {L"timer", L"Timer", L"timer", false},
        {L"settings", L"Settings", L"gear", false},
        {L"files", L"Files", L"folder", false},
        {L"wallpaper", L"Wallpaper", L"wallpaper", false},
    };
    return v;
}

const ControlDef* FindControl(const std::wstring& id) {
    for (auto& c : AllControls()) if (id == c.id) return &c;
    return nullptr;
}

const std::vector<std::wstring>& DefaultControls() {
    static const std::vector<std::wstring> v = {L"wifi", L"bluetooth", L"sound", L"mic", L"focus", L"dark",
                                                L"cast", L"screenshot", L"snip", L"color", L"clipboard", L"lock"};
    return v;
}

const std::vector<LauncherDef>& AllLaunchers() {
    static const std::vector<LauncherDef> v = {
        {L"claude", L"Claude", L"claude", 0xD97757},
        {L"codex", L"Codex", L"codex", 0xF5F5F7},
        {L"cursor", L"Cursor", L"cursor-agent", 0xA0A0AA},
        {L"terminal", L"Terminal", L"", 0x30D158},
        {L"powershell", L"PowerShell", L"pwsh", 0x2F7FD6},
    };
    return v;
}

const LauncherDef* FindLauncher(const std::wstring& id) {
    for (auto& l : AllLaunchers()) if (id == l.id) return &l;
    return nullptr;
}

const std::vector<std::wstring>& DefaultLaunchers() {
    static const std::vector<std::wstring> v = {L"claude", L"codex", L"cursor"};
    return v;
}

// macOS System Settings accent colors (dark appearance values).
const std::vector<AccentDef>& AllAccents() {
    static const std::vector<AccentDef> v = {
        {L"blue", L"Blue", 0x0A84FF},   {L"purple", L"Purple", 0xBF5AF2}, {L"pink", L"Pink", 0xFF375F},
        {L"red", L"Red", 0xFF453A},     {L"orange", L"Orange", 0xFF9F0A}, {L"yellow", L"Yellow", 0xFFD60A},
        {L"green", L"Green", 0x30D158}, {L"graphite", L"Graphite", 0x98989D},
    };
    return v;
}

unsigned AccentRGB(const std::wstring& id) {
    for (auto& a : AllAccents()) if (id == a.id) return a.rgb;
    return 0x0A84FF;
}

std::wstring Config::Path() { return sys::ConfigDir() + L"\\config.json"; }

static std::vector<std::wstring> readList(const json::Value& v, const std::vector<std::wstring>& def,
                                          bool (*valid)(const std::wstring&)) {
    if (!v.isArray()) return def;
    std::vector<std::wstring> out;
    for (auto& item : v.items()) {
        std::wstring s = item.asWString();
        if (valid(s) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    return out;
}

void Config::Reset() { *this = Config(); controls = DefaultControls(); launchers = DefaultLaunchers(); }

void Config::Load() {
    Reset();
    std::string text;
    json::Value v;
    if (!json::ReadFile(Path(), text) || !json::Parse(text, v) || !v.isObject()) return;
    auto str = [&](const char* k, std::wstring& field, std::initializer_list<const wchar_t*> allowed) {
        std::wstring s = v[k].asWString(field);
        if (allowed.size() == 0) { field = s; return; }
        for (auto a : allowed) if (s == a) { field = s; return; }
    };
    auto num = [&](const char* k, int& field, int lo, int hi) {
        if (v[k].isNumber()) field = std::clamp(v[k].asInt(), lo, hi);
    };
    auto flag = [&](const char* k, bool& field) { if (v[k].isBool()) field = v[k].asBool(); };

    str("shape", shape, {L"island", L"notch"});
    str("face", face, {L"logo", L"weather"});
    str("monitor", monitor, {L"primary", L"cursor"});
    num("offset_y", offsetY, 0, 60);
    num("scale", scale, 80, 150);
    flag("hover_open", hoverOpen);
    flag("hide_fullscreen", hideFullscreen);
    flag("reduced_motion", reducedMotion);
    str("accent", accent, {L"blue", L"purple", L"pink", L"red", L"orange", L"yellow", L"green", L"graphite"});
    str("card_style", cardStyle, {L"black", L"graphite"});
    flag("clock_24h", clock24);
    flag("show_seconds", showSeconds);
    flag("show_eq", showEq);
    flag("activity_music", actMusic);
    flag("activity_timer", actTimer);
    flag("activity_claude", actClaude);
    flag("activity_downloads", actDownloads);
    flag("peek_volume", peekVolume);
    flag("peek_caps", peekCaps);
    flag("peek_charger", peekCharger);
    flag("peek_track", peekTrack);
    flag("timer_chime", timerChime);
    num("timer_minutes", timerMinutes, 1, 120);
    str("weather_city", weatherCity, {});
    flag("fahrenheit", fahrenheit);
    str("hotkey", hotkey, {L"ctrl+alt+i", L"win+alt+i", L"ctrl+shift+space", L"off"});
    str("wallpaper_folder", wallpaperFolder, {});
    str("wallpaper_fit", wallpaperFit, {L"fill", L"fit", L"stretch", L"center", L"tile", L"span"});
    if (wallpaperFolder.size() > 260) wallpaperFolder.clear();
    if (weatherCity.size() > 80) weatherCity.resize(80);
    controls = readList(v["controls"], DefaultControls(), [](const std::wstring& s) { return FindControl(s) != nullptr; });
    launchers = readList(v["launchers"], DefaultLaunchers(), [](const std::wstring& s) { return FindLauncher(s) != nullptr; });
}

void Config::Save() const {
    json::Value v = json::Value::Object();
    v.set("shape", shape);
    v.set("face", face);
    v.set("monitor", monitor);
    v.set("offset_y", offsetY);
    v.set("scale", scale);
    v.set("hover_open", hoverOpen);
    v.set("hide_fullscreen", hideFullscreen);
    v.set("reduced_motion", reducedMotion);
    v.set("accent", accent);
    v.set("card_style", cardStyle);
    v.set("clock_24h", clock24);
    v.set("show_seconds", showSeconds);
    v.set("show_eq", showEq);
    v.set("activity_music", actMusic);
    v.set("activity_timer", actTimer);
    v.set("activity_claude", actClaude);
    v.set("activity_downloads", actDownloads);
    v.set("peek_volume", peekVolume);
    v.set("peek_caps", peekCaps);
    v.set("peek_charger", peekCharger);
    v.set("peek_track", peekTrack);
    v.set("timer_chime", timerChime);
    v.set("timer_minutes", timerMinutes);
    v.set("weather_city", weatherCity);
    v.set("fahrenheit", fahrenheit);
    v.set("hotkey", hotkey);
    v.set("wallpaper_folder", wallpaperFolder);
    v.set("wallpaper_fit", wallpaperFit);
    json::Value c = json::Value::Array();
    for (auto& s : controls) c.push(json::Value(s));
    v.set("controls", c);
    json::Value l = json::Value::Array();
    for (auto& s : launchers) l.push(json::Value(s));
    v.set("launchers", l);
    CreateDirectoryW(sys::ConfigDir().c_str(), nullptr);
    json::WriteFileAtomic(Path(), v.dump());
}
