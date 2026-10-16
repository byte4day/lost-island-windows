// The island: one layered, top-most window that morphs between the resting
// pill, short peeks, and the card. Everything is drawn with Direct2D into a
// per-pixel-alpha surface, so transparent pixels click through to the
// desktop. Shapes move on springs, like Apple's Dynamic Island.
#include "island.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "app.h"
#include "config.h"
#include "gfx.h"
#include "services.h"

namespace {

// ------------------------------------------------------------ constants (DIPs)
constexpr float kWinW = 560;        // window width; content is centred in it
constexpr float kPillH = 37;        // Apple's island height (36.67 pt)
constexpr float kPeekH = 44;
constexpr float kCardW = 400;
constexpr float kCardPad = 18;
constexpr float kBubble = 37;
constexpr float kGap = 8;
constexpr float kShadow = 34;       // room below the shape for its shadow
constexpr UINT_PTR kTimerAnim = 1, kTimerTick = 2, kTimerLong = 3, kTimerHover = 4, kTimerOutside = 6;
constexpr int kHotkeyId = 7;

enum class Mode { Rest, Peek, Card };
enum class Page { Main, Clipboard, Power };
enum class Act { None, ClaudeNeeds, Music, ClaudeWork, ClaudeDone, Download, Timer };

struct Spring {
    float x = 0, v = 0, t = 0;
    void snap(float a) { x = t = a; v = 0; }
    bool step(float dt, float k, float d, float eps) {
        float a = -k * (x - t) - d * v;
        v += a * dt;
        x += v * dt;
        if (std::fabs(x - t) < eps && std::fabs(v) < eps * 4) { x = t; v = 0; return false; }
        return true;
    }
};

struct HitR {
    D2D1_RECT_F r;
    std::wstring id;
    int idx;
};

struct PeekInfo {
    std::wstring icon, title, detail;
    float bar = -1;
    Color swatch{0, 0, 0, 0};
    ULONGLONG until = 0;
};

// ------------------------------------------------------------ state
HWND g_hwnd = nullptr;
LayeredSurface g_surf;
Canvas g_meas;                       // text measurement only
float g_dpi = 1;                     // monitor DPI / 96
float g_k = 1;                       // DIP -> pixel (dpi * user scale)
RECT g_mon{};                        // target monitor, pixels
POINT g_winPos{};

Mode g_mode = Mode::Rest;
Page g_page = Page::Main;
bool g_edit = false;
bool g_hidden = false;               // fullscreen app in front
bool g_pinned = false;               // debug states: stay open for screenshots
bool g_demo = false;                 // --demo: sample music and Claude data for screenshots
bool g_wine = false;                 // Wine's D2D drops alpha on translucent fills
Spring g_w, g_h, g_r, g_alpha, g_bubble, g_shade;
ULONGLONG g_lastFrame = 0;
bool g_animating = false;
float g_phase = 0;                   // continuous animation clock (seconds)

std::vector<HitR> g_hits;
std::wstring g_hover, g_press;
int g_hoverIdx = -1, g_pressIdx = -1;
bool g_tracking = false;
POINT g_pressPt{};
bool g_dragging = false;
int g_dragFrom = -1;
float g_dragX = 0, g_dragY = 0;
std::wstring g_slider;               // "vol" or "bright" while dragging

PeekInfo g_peek;
std::wstring g_confirm;
ULONGLONG g_confirmUntil = 0;

// cached service state
MediaInfo g_media;
Image g_art;
uint64_t g_artVersion = 0;
std::wstring g_lastTitle;
float g_lastVol = -1;
bool g_lastMute = false;
bool g_lastPlugged = false;
bool g_powerKnown = false;
int g_brightness = -1;
float g_eq[4] = {0.3f, 0.6f, 0.4f, 0.7f};
float g_peak = 0;

// focus timer
bool g_timerRun = false;
ULONGLONG g_timerEnd = 0;
int g_timerLeftMs = 0;
int g_timerTotalMs = 0;

// ------------------------------------------------------------ helpers
ULONGLONG now() { return GetTickCount64(); }

// LOSTISLAND_LOG=1 writes a trace to %TEMP%\lostisland.log (for debugging)
void logf(const char* fmt, ...) {
    static int on = -1;
    if (on < 0) on = GetEnvironmentVariableA("LOSTISLAND_LOG", nullptr, 0) > 0;
    if (!on) return;
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "lostisland.log");
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%llu ", (unsigned long long)GetTickCount64());
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

Color accent() { return pal::accent(); }

void sendKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = k;
        in.push_back(i);
    }
    for (auto it = keys.end(); it != keys.begin();) {
        --it;
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = *it;
        i.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

std::wstring fmtClock(bool withAmPm = true) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    if (g_config.clock24) {
        if (g_config.showSeconds) swprintf(buf, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        else swprintf(buf, 32, L"%02d:%02d", st.wHour, st.wMinute);
    } else {
        int h = st.wHour % 12;
        if (!h) h = 12;
        if (g_config.showSeconds) swprintf(buf, 32, L"%d:%02d:%02d", h, st.wMinute, st.wSecond);
        else swprintf(buf, 32, L"%d:%02d", h, st.wMinute);
        if (withAmPm) wcscat(buf, st.wHour < 12 ? L" AM" : L" PM");
    }
    return buf;
}

std::wstring fmtDate() {
    wchar_t buf[80];
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"dddd, MMMM d", buf, 80, nullptr)) return buf;
    return L"";
}

std::wstring fmtMs(int ms) {
    int s = (ms + 999) / 1000;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d:%02d", s / 60, s % 60);
    return buf;
}

std::wstring fmtSecs(double s) {
    int t = (int)std::max(0.0, s);
    wchar_t buf[16];
    if (t >= 3600) swprintf(buf, 16, L"%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else swprintf(buf, 16, L"%d:%02d", t / 60, t % 60);
    return buf;
}

std::wstring fmtBytes(uint64_t b) {
    wchar_t buf[32];
    if (b >= (1ull << 30)) swprintf(buf, 32, L"%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) swprintf(buf, 32, L"%.1f MB", b / 1048576.0);
    else swprintf(buf, 32, L"%llu KB", (unsigned long long)(b / 1024));
    return buf;
}

std::wstring appName(const std::wstring& aumid) {
    std::wstring s = aumid;
    size_t bang = s.find(L'!');
    if (bang != std::wstring::npos) s = s.substr(bang + 1);
    size_t dot = s.rfind(L".exe");
    if (dot != std::wstring::npos) s = s.substr(0, dot);
    std::wstring low = s;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    if (low.find(L"spotify") != std::wstring::npos) return L"Spotify";
    if (low.find(L"chrome") != std::wstring::npos) return L"Chrome";
    if (low.find(L"msedge") != std::wstring::npos || low == L"microsoftedge") return L"Edge";
    if (low.find(L"firefox") != std::wstring::npos) return L"Firefox";
    if (low.find(L"zunemusic") != std::wstring::npos || low.find(L"music") != std::wstring::npos) return L"Media Player";
    if (low.find(L"vlc") != std::wstring::npos) return L"VLC";
    return s;
}

bool cardStyleGraphite() { return g_config.cardStyle == L"graphite"; }
bool notch() { return g_config.shape == L"notch"; }
float topY() { return notch() ? 0.f : (float)g_config.offsetY; }

// ------------------------------------------------------------ activities
std::vector<Act> activities() {
    std::vector<Act> out;
    auto sessions = g_config.actClaude ? claude::Visible() : std::vector<ClaudeSession>{};
    Act claudeAct = Act::None;
    if (!sessions.empty()) {
        const auto& s = sessions.front().state;
        claudeAct = s == L"needs" ? Act::ClaudeNeeds : s == L"done" ? Act::ClaudeDone : Act::ClaudeWork;
    }
    bool music = g_config.actMusic && g_media.active && g_media.playing && !g_media.title.empty();
    if (claudeAct == Act::ClaudeNeeds) out.push_back(claudeAct);
    if (music) out.push_back(Act::Music);
    if (claudeAct == Act::ClaudeWork || claudeAct == Act::ClaudeDone) out.push_back(claudeAct);
    if (g_config.actDownloads && !downloads::Active().empty()) out.push_back(Act::Download);
    if (g_config.actTimer && (g_timerRun || (g_timerLeftMs > 0 && g_timerLeftMs < g_timerTotalMs)))
        out.push_back(Act::Timer);
    // on the weather face, music moves to the bubble instead of replacing the clock
    if (g_config.face == L"weather" && !out.empty() && out.front() == Act::Music) {
        out.erase(out.begin());
        out.insert(out.begin(), Act::None);
        if (out.size() > 1) std::swap(out[1], out[0]), std::swap(out[0], out[1]);
    }
    return out;
}

Act primaryAct() {
    auto a = activities();
    return a.empty() ? Act::None : a[0];
}
Act secondaryAct() {
    auto a = activities();
    return a.size() > 1 ? a[1] : Act::None;
}

ClaudeSession topSession() {
    auto s = claude::Visible();
    return s.empty() ? ClaudeSession{} : s.front();
}

// ------------------------------------------------------------ sizing
float clockWidth(float size, DWRITE_FONT_WEIGHT w) { return g_meas.Measure(fmtClock(), size, w); }

float pillWidth(Act a) {
    float base = 0;
    switch (a) {
    case Act::None:
        if (g_config.face == L"weather") {
            WeatherInfo wx = weather::Get();
            float wxw = wx.valid ? 22 + g_meas.Measure(std::to_wstring(wx.tempC) + L"°", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) : 0;
            base = 12 + clockWidth(13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 60 + wxw + 14;
        } else {
            base = 12 + 16 + 64 + clockWidth(13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 14;
        }
        return std::max(base, 188.f);
    case Act::Music: {
        float tw = std::min(150.f, g_meas.Measure(g_media.title, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        return std::max(10 + 23 + 10 + tw + 18 + 20 + 12, 220.f);
    }
    case Act::ClaudeNeeds:
    case Act::ClaudeWork:
    case Act::ClaudeDone: {
        std::wstring d = topSession().detail;
        float tw = std::min(190.f, g_meas.Measure(d, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        return std::max(12 + 20 + 40 + tw + 14, 200.f);
    }
    case Act::Download: {
        auto t = downloads::Active();
        std::wstring s = t.empty() ? L"" : fmtBytes(t[0].bytes);
        return std::max(12 + 20 + 40 + g_meas.Measure(s, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 14, 200.f);
    }
    case Act::Timer: return 200;
    }
    return 200;
}

struct CardLayout {
    float header = 0, sessions = 0, controls = 0, tray = 0, sliders = 0, launchers = 0, info = 0;
    float yHeader = 0, ySessions = 0, yControls = 0, yTray = 0, ySliders = 0, yLaunchers = 0, yInfo = 0;
    float height = 0;
    int controlRows = 0, trayRows = 0;
    std::vector<std::wstring> trayItems;
};

constexpr int kCols = 6;
constexpr float kCellH = 72;

CardLayout cardLayout() {
    CardLayout L;
    float y = kCardPad;
    if (g_page == Page::Clipboard) {
        size_t n = std::min<size_t>(clipboard::History().size(), 8);
        L.height = 18 + 34 + 8 + (n ? n * 38.f : 64.f) + 16;
        return L;
    }
    if (g_page == Page::Power) {
        L.height = 18 + 34 + 8 + 5 * 44.f + 16;
        return L;
    }
    L.yHeader = y;
    L.header = g_media.active && !g_media.title.empty() ? (g_media.hasTimeline ? 158.f : 132.f) : 80.f;
    y += L.header + 12;
    auto sessions = claude::Visible();
    if (!sessions.empty()) {
        L.ySessions = y;
        L.sessions = std::min<size_t>(sessions.size(), 3) * 40.f + 8;
        y += L.sessions + 12;
    }
    L.yControls = y;
    size_t n = g_config.controls.size();
    L.controlRows = (int)std::max<size_t>(1, (n + kCols - 1) / kCols);
    L.controls = L.controlRows * kCellH + 16;
    y += L.controls + 12;
    if (g_edit) {
        for (auto& c : AllControls())
            if (std::find(g_config.controls.begin(), g_config.controls.end(), c.id) == g_config.controls.end())
                L.trayItems.push_back(c.id);
        for (auto& l : AllLaunchers())
            if (std::find(g_config.launchers.begin(), g_config.launchers.end(), l.id) == g_config.launchers.end())
                L.trayItems.push_back(std::wstring(L"launcher:") + l.id);
        L.trayRows = (int)std::max<size_t>(1, (L.trayItems.size() + kCols - 1) / kCols);
        L.yTray = y;
        L.tray = 26 + L.trayRows * kCellH + 10;
        y += L.tray + 12;
    }
    L.ySliders = y;
    L.sliders = brightness::Available() && g_brightness >= 0 ? 40 + 8 + 40 : 40;
    y += L.sliders + 12;
    if (!g_config.launchers.empty()) {
        L.yLaunchers = y;
        L.launchers = 44;
        y += L.launchers + 12;
    }
    L.yInfo = y;
    L.info = 30;
    y += L.info + 16;
    L.height = y;
    return L;
}

struct Target { float w, h, r; };

Target targetFor(Mode m) {
    if (m == Mode::Card) return {kCardW, cardLayout().height, notch() ? 30.f : 34.f};
    if (m == Mode::Peek) {
        float tw = g_meas.Measure(g_peek.title, 13, DWRITE_FONT_WEIGHT_BOLD);
        float dw = g_peek.bar >= 0 ? 130 + 40 : g_meas.Measure(g_peek.detail, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float w = std::clamp(20 + 24 + 12 + tw + 16 + dw + 20, 240.f, 380.f);
        return {w, kPeekH, kPeekH / 2};
    }
    return {pillWidth(primaryAct()), kPillH, kPillH / 2};
}

// ------------------------------------------------------------ monitor and placement
void pickMonitor() {
    HMONITOR mon;
    if (g_config.monitor == L"cursor") {
        POINT pt;
        GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    } else {
        mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_mon = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    g_dpi = dx / 96.f;
    g_k = g_dpi * g_config.scale / 100.f;
}

// ------------------------------------------------------------ animation
void kick();

void setMode(Mode m) {
    if (m != g_mode) logf("mode %d -> %d", (int)g_mode, (int)m);
    if (m != g_mode) {
        g_mode = m;
        g_alpha.x = 0;
        g_alpha.t = 1;
        if (m != Mode::Card) { g_page = Page::Main; g_edit = false; g_confirm.clear(); }
    }
    Target t = targetFor(m);
    g_w.t = t.w;
    g_h.t = t.h;
    g_r.t = t.r;
    g_shade.t = m == Mode::Card ? 1.f : 0.f;
    g_bubble.t = (m != Mode::Card && secondaryAct() != Act::None && !g_hidden) ? 1.f : 0.f;
    kick();
}

void relayout() { setMode(g_mode); }

bool stepAnim(float dt) {
    float k = g_config.reducedMotion ? 520.f : 380.f;
    float d = g_config.reducedMotion ? 2 * std::sqrt(k) : 27.f;   // < critical: a small overshoot
    bool a = false;
    a |= g_w.step(dt, k, d, 0.2f);
    a |= g_h.step(dt, k, d, 0.2f);
    a |= g_r.step(dt, k, d, 0.1f);
    a |= g_alpha.step(dt, 260, 32, 0.004f);
    a |= g_bubble.step(dt, 420, 22, 0.004f);
    a |= g_shade.step(dt, 300, 34, 0.004f);
    return a;
}

bool needsContinuous() {
    if (g_hidden) return false;
    Act p = primaryAct(), s = secondaryAct();
    bool eq = g_config.showEq && (p == Act::Music || (g_mode == Mode::Card && g_media.playing));
    bool spin = p == Act::ClaudeWork || s == Act::ClaudeWork || (g_mode == Mode::Card && !claude::Visible().empty());
    return eq || spin || g_edit || g_dragging || (p == Act::Download);
}

void kick() {
    if (!g_animating && g_hwnd) {
        g_animating = true;
        g_lastFrame = now();
        SetTimer(g_hwnd, kTimerAnim, 15, nullptr);
    }
}

// ------------------------------------------------------------ hit regions
void hit(D2D1_RECT_F r, const std::wstring& id, int idx = -1) { g_hits.push_back({r, id, idx}); }
bool hovered(const std::wstring& id, int idx = -1) { return g_hover == id && (idx < 0 || g_hoverIdx == idx); }
bool pressed(const std::wstring& id, int idx = -1) { return g_press == id && (idx < 0 || g_pressIdx == idx); }

// ------------------------------------------------------------ drawing: pill content
void drawEq(Canvas& c, float x, float cy, float h, Color col) {
    for (int i = 0; i < 4; i++) {
        float bh = std::max(3.f, h * g_eq[i]);
        c.FillRound(Rect(x + i * 5.f, cy - bh / 2, 3, bh), 1.5f, col);
    }
}

void drawRing(Canvas& c, float cx, float cy, float rad, float frac, Color col, float width = 2.6f) {
    c.StrokeCircle(cx, cy, rad, col.alpha(0.25f), width);
    c.Arc(cx, cy, rad, 0, 360 * std::clamp(frac, 0.f, 1.f), col, width);
}

Color artColor() {
    if (!g_art.src) return accent();
    Color a = g_art.average;
    float mx = std::max({a.r, a.g, a.b}), mn = std::min({a.r, a.g, a.b});
    if (mx <= 0 || (mx - mn) / mx < 0.35f) return accent();   // dull or brownish covers: use the accent
    // lift dark covers so the bars stay visible on black
    float lum = 0.3f * a.r + 0.59f * a.g + 0.11f * a.b;
    if (lum < 0.45f) a = a.mix(Color(1, 1, 1), (0.45f - lum) * 1.3f);
    return a;
}

void drawSparkAct(Canvas& c, float cx, float cy, float size, const std::wstring& state) {
    float angle = state == L"needs" || state == L"done" ? 0 : g_phase * 120.f;
    DrawSpark(c, cx, cy, size, pal::claude, angle);
}

void drawPillContent(Canvas& c, D2D1_RECT_F r, Act a) {
    float cy = (r.top + r.bottom) / 2;
    float L = r.left, R = r.right;
    switch (a) {
    case Act::None: {
        if (g_config.face == L"weather") {
            c.Text(fmtClock(), Rect(L + 14, r.top, 120, r.bottom - r.top), 13, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            WeatherInfo wx = weather::Get();
            if (wx.valid) {
                int t = g_config.fahrenheit ? (int)std::lround(wx.tempC * 9 / 5.0 + 32) : wx.tempC;
                std::wstring s = std::to_wstring(t) + L"°";
                float tw = c.Measure(s, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                c.Text(s, Rect(R - 14 - tw, r.top, tw + 2, r.bottom - r.top), 13, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                DrawIcon(c, wx.glyph, R - 14 - tw - 12, cy, 16, pal::ink2, 1.5f);
            }
        } else {
            DrawWindowsMark(c, L + 14 + 7, cy, 14, pal::ink);
            std::wstring clock = fmtClock();
            float tw = c.Measure(clock, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(clock, Rect(R - 14 - tw, r.top, tw + 2, r.bottom - r.top), 13, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
        break;
    }
    case Act::Music: {
        D2D1_RECT_F art = Rect(L + 7, cy - 12, 24, 24);
        if (g_art.src) c.DrawImage(g_art, art, 12);
        else { c.FillCircle(L + 19, cy, 12, pal::raisedHover); DrawIcon(c, L"music", L + 19, cy, 13, pal::ink2, 1.4f); }
        c.Text(g_media.title, D2D1::RectF(L + 40, r.top, R - 40, r.bottom), 13, Color(0.96f, 0.96f, 0.97f, 0.92f),
               DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (g_config.showEq) drawEq(c, R - 30, cy, 15, artColor());
        break;
    }
    case Act::ClaudeNeeds:
    case Act::ClaudeWork:
    case Act::ClaudeDone: {
        ClaudeSession s = topSession();
        drawSparkAct(c, L + 22, cy, 19, s.state);
        Color col = a == Act::ClaudeNeeds ? pal::warn : a == Act::ClaudeDone ? pal::ok : pal::ink2;
        float tw = std::min(190.f, c.Measure(s.detail, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 2);
        c.Text(s.detail, Rect(R - 14 - tw, r.top, tw, r.bottom - r.top), 13, col, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        break;
    }
    case Act::Download: {
        auto t = downloads::Active();
        float spin = std::fmod(g_phase * 200.f, 360.f);
        c.StrokeCircle(L + 22, cy, 9, accent().alpha(0.25f), 2.4f);
        c.Arc(L + 22, cy, 9, spin, 90, accent(), 2.4f);
        DrawIcon(c, L"download", L + 22, cy, 10, pal::ink, 1.6f);
        if (!t.empty()) {
            std::wstring s = fmtBytes(t[0].bytes);
            float tw = c.Measure(s, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 2;
            c.Text(s, Rect(R - 14 - tw, r.top, tw, r.bottom - r.top), 13, accent(), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
        break;
    }
    case Act::Timer: {
        float frac = g_timerTotalMs ? 1.f - (float)g_timerLeftMs / g_timerTotalMs : 0;
        drawRing(c, L + 22, cy, 8.5f, frac, accent());
        std::wstring s = fmtMs(g_timerLeftMs);
        float tw = c.Measure(s, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 2;
        c.Text(s, Rect(R - 14 - tw, r.top, tw, r.bottom - r.top), 13, g_timerRun ? accent() : pal::ink2,
               DWRITE_FONT_WEIGHT_SEMI_BOLD);
        break;
    }
    }
}

void drawBubbleContent(Canvas& c, float cx, float cy, float size, Act a) {
    float rad = size / 2;
    switch (a) {
    case Act::Music:
        if (g_art.src) c.DrawImage(g_art, Rect(cx - rad + 4, cy - rad + 4, size - 8, size - 8), rad - 4);
        else drawEq(c, cx - 9, cy, 14, artColor());
        break;
    case Act::ClaudeNeeds:
    case Act::ClaudeWork:
    case Act::ClaudeDone: drawSparkAct(c, cx, cy, 19, topSession().state); break;
    case Act::Download: {
        float spin = std::fmod(g_phase * 200.f, 360.f);
        c.Arc(cx, cy, rad - 7, spin, 90, accent(), 2.4f);
        DrawIcon(c, L"download", cx, cy, 11, pal::ink, 1.6f);
        break;
    }
    case Act::Timer: {
        float frac = g_timerTotalMs ? 1.f - (float)g_timerLeftMs / g_timerTotalMs : 0;
        drawRing(c, cx, cy, rad - 7, frac, accent(), 2.4f);
        break;
    }
    default: break;
    }
}

void drawPeek(Canvas& c, D2D1_RECT_F r) {
    float cy = (r.top + r.bottom) / 2;
    float x = r.left + 18;
    if (g_peek.swatch.a > 0) {
        c.FillCircle(x + 10, cy, 10, g_peek.swatch);
        c.StrokeCircle(x + 10, cy, 10, Color(1, 1, 1, 0.3f), 1);
    } else if (g_peek.icon == L"spark") {
        DrawSpark(c, x + 10, cy, 20, pal::claude);
    } else if (g_peek.icon == L"art" && g_art.src) {
        c.DrawImage(g_art, Rect(x, cy - 12, 24, 24), 12);
    } else {
        Color ic = g_peek.icon == L"bolt" ? pal::ok : pal::ink;
        DrawIcon(c, g_peek.icon, x + 10, cy, 20, ic, 1.6f);
    }
    x += 32;
    float tw = c.Measure(g_peek.title, 13, DWRITE_FONT_WEIGHT_BOLD) + 2;
    if (g_peek.bar >= 0) {
        float barW = r.right - 18 - 36 - x;
        c.FillRound(Rect(x, cy - 3, barW, 6), 3, Color(1, 1, 1, 0.18f));
        c.FillRound(Rect(x, cy - 3, std::max(6.f, barW * g_peek.bar), 6), 3, pal::ink);
        wchar_t pct[8];
        swprintf(pct, 8, L"%d", (int)std::lround(g_peek.bar * 100));
        c.Text(pct, Rect(r.right - 18 - 30, r.top, 30, r.bottom - r.top), 13, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD,
               Align::Right);
    } else {
        c.Text(g_peek.title, Rect(x, r.top, tw, r.bottom - r.top), 13, pal::ink, DWRITE_FONT_WEIGHT_BOLD);
        c.Text(g_peek.detail, D2D1::RectF(x + tw + 8, r.top, r.right - 18, r.bottom), 12, pal::ink2,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right);
    }
}

// ------------------------------------------------------------ drawing: card
struct ControlState {
    bool on = false;
    bool dim = false;
    std::wstring icon, sub;
};

ControlState controlState(const ControlDef& d) {
    ControlState s;
    s.icon = d.icon;
    std::wstring id = d.id;
    if (id == L"wifi") {
        s.on = radios::Available(RadioKind::WiFi) ? radios::IsOn(RadioKind::WiFi) : !network::WifiName().empty();
        s.sub = network::WifiName();
    } else if (id == L"bluetooth") {
        s.dim = !radios::Available(RadioKind::Bluetooth);
        s.on = radios::IsOn(RadioKind::Bluetooth);
    } else if (id == L"sound") {
        s.on = !audio::Muted();
        if (!s.on) s.icon = L"speaker-off";
    } else if (id == L"mic") {
        s.dim = !audio::HasMic();
        s.on = !s.dim && !audio::MicMuted();
        if (!s.on) s.icon = L"mic-off";
    } else if (id == L"dark") {
        s.on = sys::DarkMode();
    } else if (id == L"timer") {
        s.on = g_timerRun;
    }
    return s;
}

void drawControlCell(Canvas& c, float cx, float top, const ControlDef& d, int idx, bool trayItem) {
    ControlState st = controlState(d);
    float rad = 22;
    float cy = top + 8 + rad;
    float jig = 0;
    if (g_edit) jig = std::sin(g_phase * 14 + idx * 1.7f) * 2.2f;   // edit mode jiggles
    D2D1_MATRIX_3X2_F old = c.Transform();
    if (jig != 0) c.SetTransform(D2D1::Matrix3x2F::Rotation(jig, D2D1::Point2F(cx, cy)) * old);
    bool hov = hovered(trayItem ? L"tray" : L"ctl", idx);
    bool prs = pressed(trayItem ? L"tray" : L"ctl", idx) && !g_edit;
    float pr = prs ? 0.92f : 1.f;
    Color fill = st.on && d.toggle ? accent() : (hov ? pal::glassHi.mix(Color(1, 1, 1, 0.2f), 0.3f) : Color(1, 1, 1, 0.12f));
    if (st.dim) fill = Color(1, 1, 1, 0.06f);
    c.FillCircle(cx, cy, rad * pr, fill);
    Color ic = st.dim ? pal::ink3 : pal::ink;
    DrawIcon(c, st.icon, cx, cy, 20 * pr, ic, 1.6f);
    c.Text(d.label, Rect(cx - 30, cy + rad + 3, 60, 14), 10.5f, st.dim ? pal::ink3 : pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD,
           Align::Center);
    if (!st.sub.empty() && !g_edit)
        c.Text(st.sub, Rect(cx - 30, cy + rad + 16, 60, 12), 9.5f, pal::ink3, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    if (g_edit) {
        float bx = cx - rad + 3, by = cy - rad + 3;
        c.FillCircle(bx, by, 8, trayItem ? pal::ok : Color::Hex(0x636366));
        DrawIcon(c, trayItem ? L"plus" : L"minus", bx, by, 11, pal::ink, 2.2f);
        hit(Rect(bx - 9, by - 9, 18, 18), trayItem ? L"add" : L"remove", idx);
    }
    c.SetTransform(old);
}

Image g_brand[3];   // claude, codex, cursor (resources 3-5)

void drawLauncherIcon(Canvas& c, const LauncherDef& l, float cx, float cy) {
    std::wstring id = l.id;
    int i = id == L"claude" ? 0 : id == L"codex" ? 1 : id == L"cursor" ? 2 : -1;
    if (i >= 0) {
        if (!g_brand[i].src) g_brand[i].LoadResource(3 + i);
        if (g_brand[i].src) { c.DrawImage(g_brand[i], Rect(cx - 9, cy - 9, 18, 18), 0); return; }
    }
    DrawIcon(c, L"terminal", cx, cy, 18, Color::Hex(l.color), 1.6f);
}

void drawPlayer(Canvas& c, float x, float y, float w, float h) {
    // a plain platter like the rest of the card; the art carries the color
    c.FillRound(Rect(x, y, w, h), 22, pal::glass);
    c.StrokeRound(Rect(x + 0.5f, y + 0.5f, w - 1, h - 1), 22, pal::hairline, 1);
    D2D1_RECT_F art = Rect(x + 14, y + 14, 64, 64);
    if (g_art.src) {
        c.DrawImage(g_art, art, 32);
        c.StrokeCircle(x + 46, y + 46, 31.5f, Color(1, 1, 1, 0.12f), 1);
    } else {
        c.FillCircle(x + 46, y + 46, 32, Color(1, 1, 1, 0.1f));
        DrawIcon(c, L"music", x + 46, y + 46, 26, pal::ink2, 1.5f);
    }
    std::wstring app = appName(g_media.appId);
    c.Text(g_media.title, Rect(x + 92, y + 19, w - 92 - 50, 20), 15, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Left, false);
    c.Text(g_media.artist, Rect(x + 92, y + 40, w - 92 - 16, 18), 13, pal::ink2, DWRITE_FONT_WEIGHT_NORMAL, Align::Left, false);
    if (!app.empty()) c.Text(app, Rect(x + 92, y + 60, w - 92 - 16, 16), 11, pal::ink3, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Left, false);
    if (g_config.showEq && g_media.playing) drawEq(c, x + w - 34, y + 30, 16, artColor());
    float yy = y + 90;
    if (g_media.hasTimeline) {
        float bx = x + 16, bw = w - 32;
        float frac = g_media.duration > 0 ? (float)(g_media.position / g_media.duration) : 0;
        D2D1_RECT_F track = Rect(bx, yy + 4, bw, 5);
        c.FillRound(track, 2.5f, Color(1, 1, 1, 0.2f));
        c.FillRound(Rect(bx, yy + 4, std::max(5.f, bw * std::clamp(frac, 0.f, 1.f)), 5), 2.5f, Color(1, 1, 1, 0.85f));
        hit(Rect(bx, yy - 4, bw, 20), L"seek");
        c.Text(fmtSecs(g_media.position), Rect(bx, yy + 11, 60, 14), 10, pal::ink2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(L"-" + fmtSecs(g_media.duration - g_media.position), Rect(bx + bw - 60, yy + 11, 60, 14), 10, pal::ink2,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right);
        yy += 26;
    }
    float cx = x + w / 2, cy = yy + 20;
    struct B { const wchar_t* id; const wchar_t* icon; float dx, size; };
    B btns[3] = {{L"prev", L"prev", -64, 22}, {L"play", g_media.playing ? L"pause" : L"play", 0, 28}, {L"next", L"next", 64, 22}};
    for (auto& b : btns) {
        float bx = cx + b.dx;
        if (hovered(b.id)) c.FillCircle(bx, cy, 20, Color(1, 1, 1, 0.1f));
        float s = pressed(b.id) ? b.size * 0.88f : b.size;
        DrawIcon(c, b.icon, bx, cy, s, pal::ink, 1.6f);
        hit(Rect(bx - 20, cy - 20, 40, 40), b.id);
    }
}

void drawClockHeader(Canvas& c, float x, float y, float w) {
    std::wstring clock = fmtClock(false);
    c.Text(clock, Rect(x + 4, y, 220, 50), 44, pal::ink, DWRITE_FONT_WEIGHT_LIGHT, Align::Left, false);
    if (!g_config.clock24) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        float cw = c.Measure(clock, 44, DWRITE_FONT_WEIGHT_LIGHT);
        c.Text(st.wHour < 12 ? L"AM" : L"PM", Rect(x + 8 + cw, y + 24, 40, 20), 15, pal::ink2, DWRITE_FONT_WEIGHT_SEMI_BOLD,
               Align::Left, false);
    }
    c.Text(fmtDate(), Rect(x + 6, y + 54, 260, 20), 13, pal::ink2, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Left, false);
    WeatherInfo wx = weather::Get();
    if (wx.valid) {
        int t = g_config.fahrenheit ? (int)std::lround(wx.tempC * 9 / 5.0 + 32) : wx.tempC;
        std::wstring temp = std::to_wstring(t) + L"°";
        DrawIcon(c, wx.glyph, x + w - 92, y + 22, 26, pal::ink, 1.5f);
        c.Text(temp, Rect(x + w - 74, y + 6, 70, 34), 26, pal::ink, DWRITE_FONT_WEIGHT_NORMAL, Align::Right, false);
        c.Text(wx.condition, Rect(x + w - 160, y + 54, 156, 20), 12, pal::ink2, DWRITE_FONT_WEIGHT_NORMAL, Align::Right, false);
        hit(Rect(x + w - 160, y, 160, 76), L"weather");
    }
}

// Same as the Sway card: a platter with an icon, a thin track and the value.
void drawSlider(Canvas& c, D2D1_RECT_F r, float v, const std::wstring& icon, const std::wstring& id) {
    float h = r.bottom - r.top, cy = (r.top + r.bottom) / 2;
    c.FillRound(r, 18, pal::glass);
    c.StrokeRound(Inset(r, 0.5f, 0.5f), 18, pal::hairline, 1);
    DrawIcon(c, icon, r.left + 22, cy, 15, pal::ink2, 1.6f);
    D2D1_RECT_F track = D2D1::RectF(r.left + 44, cy - 2.5f, r.right - 58, cy + 2.5f);
    bool active = hovered(id) || g_slider == id;
    c.FillRound(track, 2.5f, Color(1, 1, 1, 0.16f));
    float fw = std::max(5.f, (track.right - track.left) * std::clamp(v, 0.f, 1.f));
    c.FillRound(D2D1::RectF(track.left, track.top, track.left + fw, track.bottom), 2.5f, pal::ink);
    if (active) c.FillCircle(track.left + fw, cy, 7, pal::ink);
    wchar_t pct[8];
    swprintf(pct, 8, L"%d%%", (int)std::lround(std::clamp(v, 0.f, 1.f) * 100));
    c.Text(pct, D2D1::RectF(r.right - 56, r.top, r.right - 16, r.bottom), 12, pal::ink2, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right);
    hit(D2D1::RectF(track.left, r.top, track.right, r.top + h), id);
}

void drawChip(Canvas& c, D2D1_RECT_F r, const std::wstring& id, const std::wstring& icon, const std::wstring& text, Color ic) {
    if (hovered(id)) c.FillRound(r, 15, Color(1, 1, 1, 0.1f));
    float y = (r.top + r.bottom) / 2;
    DrawIcon(c, icon, r.left + 15, y, 15, ic, 1.6f);
    c.Text(text, D2D1::RectF(r.left + 27, r.top, r.right - 6, r.bottom), 12, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hit(r, id);
}

void drawRoundButton(Canvas& c, float cx, float cy, const std::wstring& id, const std::wstring& icon, bool active = false) {
    Color bg = active ? accent() : (hovered(id) ? Color(1, 1, 1, 0.18f) : Color(1, 1, 1, 0.1f));
    c.FillCircle(cx, cy, 14, bg);
    DrawIcon(c, icon, cx, cy, 15, pal::ink, 1.7f);
    hit(Rect(cx - 15, cy - 15, 30, 30), id);
}

void drawMainPage(Canvas& c, float x0, float y0) {
    CardLayout L = cardLayout();
    float x = x0 + kCardPad, w = kCardW - 2 * kCardPad;
    // header
    if (g_media.active && !g_media.title.empty()) drawPlayer(c, x, y0 + L.yHeader, w, L.header);
    else drawClockHeader(c, x, y0 + L.yHeader, w);

    // Claude sessions
    if (L.sessions > 0) {
        auto sessions = claude::Visible();
        float y = y0 + L.ySessions;
        c.FillRound(Rect(x, y, w, L.sessions), 18, pal::glass);
        for (size_t i = 0; i < sessions.size() && i < 3; i++) {
            auto& s = sessions[i];
            D2D1_RECT_F row = Rect(x + 4, y + 4 + i * 40.f, w - 8, 40);
            if (hovered(L"session", (int)i)) c.FillRound(row, 14, Color(1, 1, 1, 0.07f));
            drawSparkAct(c, row.left + 20, row.top + 20, 18, s.state);
            c.Text(s.project.empty() ? L"Claude Code" : s.project, Rect(row.left + 40, row.top + 4, 160, 18), 13, pal::ink,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Left, false);
            Color col = s.state == L"needs" ? pal::warn : s.state == L"done" ? pal::ok : pal::ink2;
            c.Text(s.detail, Rect(row.left + 40, row.top + 21, w - 70, 16), 11.5f, col, DWRITE_FONT_WEIGHT_NORMAL, Align::Left, false);
            DrawIcon(c, L"chevron", row.right - 16, row.top + 20, 12, pal::ink3, 1.8f);
            hit(row, L"session", (int)i);
        }
    }

    // controls platter
    {
        float y = y0 + L.yControls;
        c.FillRound(Rect(x, y, w, L.controls), 22, pal::glass);
        float cellW = (w - 16) / kCols;
        for (size_t i = 0; i < g_config.controls.size(); i++) {
            const ControlDef* d = FindControl(g_config.controls[i]);
            if (!d) continue;
            int col = (int)(i % kCols), row = (int)(i / kCols);
            float cx = x + 8 + cellW * (col + 0.5f);
            float top = y + 8 + row * kCellH;
            if (g_dragging && g_dragFrom == (int)i) continue;
            drawControlCell(c, cx, top, *d, (int)i, false);
            hit(Rect(cx - cellW / 2, top, cellW, kCellH), L"ctl", (int)i);
        }
        if (g_config.controls.empty())
            c.Text(L"No controls. Add some below.", Rect(x, y, w, L.controls), 12, pal::ink3, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
    }

    // edit tray
    if (g_edit) {
        float y = y0 + L.yTray;
        c.FillRound(Rect(x, y, w, L.tray), 22, Color(1, 1, 1, 0.04f));
        c.StrokeRound(Rect(x + 0.5f, y + 0.5f, w - 1, L.tray - 1), 22, Color(1, 1, 1, 0.1f), 1);
        c.Text(L"Add to the card", Rect(x + 16, y + 6, 200, 18), 11, pal::ink3, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float cellW = (w - 16) / kCols;
        for (size_t i = 0; i < L.trayItems.size(); i++) {
            int col = (int)(i % kCols), row = (int)(i / kCols);
            float cx = x + 8 + cellW * (col + 0.5f);
            float top = y + 26 + row * kCellH;
            const std::wstring& item = L.trayItems[i];
            if (item.rfind(L"launcher:", 0) == 0) {
                const LauncherDef* l = FindLauncher(item.substr(9));
                if (!l) continue;
                float cy = top + 8 + 22;
                c.FillRound(Rect(cx - 22, cy - 22, 44, 44), 13, Color(1, 1, 1, 0.12f));
                drawLauncherIcon(c, *l, cx, cy);
                c.Text(l->label, Rect(cx - 30, cy + 25, 60, 14), 10.5f, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
                c.FillCircle(cx - 19, cy - 19, 8, pal::ok);
                DrawIcon(c, L"plus", cx - 19, cy - 19, 11, pal::ink, 2.2f);
                hit(Rect(cx - cellW / 2, top, cellW, kCellH), L"add", (int)i);
            } else if (const ControlDef* d = FindControl(item)) {
                drawControlCell(c, cx, top, *d, (int)i, true);
                hit(Rect(cx - cellW / 2, top, cellW, kCellH), L"add", (int)i);
            }
        }
    }

    // sliders
    {
        float y = y0 + L.ySliders;
        float v = audio::Muted() ? 0 : audio::Volume();
        drawSlider(c, Rect(x, y, w, 40), v, v <= 0.001f ? L"speaker-off" : L"speaker", L"vol");
        if (L.sliders > 40) drawSlider(c, Rect(x, y + 48, w, 40), g_brightness / 100.f, L"sun", L"bright");
    }

    // launchers
    if (L.launchers > 0) {
        float y = y0 + L.yLaunchers;
        size_t n = g_config.launchers.size();
        float gap = 8, tw = (w - gap * (n - 1)) / n;
        for (size_t i = 0; i < n; i++) {
            const LauncherDef* l = FindLauncher(g_config.launchers[i]);
            if (!l) continue;
            D2D1_RECT_F r = Rect(x + i * (tw + gap), y, tw, 44);
            float jig = g_edit ? std::sin(g_phase * 14 + i * 2.1f) * 1.2f : 0;
            D2D1_MATRIX_3X2_F old = c.Transform();
            if (jig != 0) c.SetTransform(D2D1::Matrix3x2F::Rotation(jig, D2D1::Point2F((r.left + r.right) / 2, y + 22)) * old);
            c.FillRound(r, 14, hovered(L"launch", (int)i) ? Color(1, 1, 1, 0.13f) : pal::glass);
            float lw = c.Measure(l->label, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            float cx = (r.left + r.right) / 2 - (lw + 26) / 2;
            drawLauncherIcon(c, *l, cx + 9, y + 22);
            c.Text(l->label, Rect(cx + 24, y, lw + 4, 44), 12.5f, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            if (g_edit) {
                c.FillCircle(r.left + 6, y + 6, 8, Color::Hex(0x636366));
                DrawIcon(c, L"minus", r.left + 6, y + 6, 11, pal::ink, 2.2f);
                hit(Rect(r.left - 3, y - 3, 18, 18), L"lremove", (int)i);
            }
            c.SetTransform(old);
            hit(r, L"launch", (int)i);
        }
    }

    // info line
    {
        float y = y0 + L.yInfo;
        float cx = x;
        std::wstring tt = g_timerRun || g_timerLeftMs > 0 ? fmtMs(g_timerLeftMs) : fmtMs(g_config.timerMinutes * 60000);
        float tw = c.Measure(tt, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 36;
        drawChip(c, Rect(cx, y, tw, 30), L"timerchip", L"timer", tt, g_timerRun ? accent() : pal::ink2);
        cx += tw + 2;
        WeatherInfo wx = weather::Get();
        if (wx.valid && !(g_media.active && !g_media.title.empty()) == false) {
            int t = g_config.fahrenheit ? (int)std::lround(wx.tempC * 9 / 5.0 + 32) : wx.tempC;
            std::wstring s = std::to_wstring(t) + L"°";
            float ww = c.Measure(s, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 36;
            drawChip(c, Rect(cx, y, ww, 30), L"weather", wx.glyph, s, pal::ink2);
            cx += ww + 2;
        }
        BatteryInfo b = power::Get();
        if (b.present && b.percent >= 0) {
            std::wstring s = std::to_wstring(b.percent) + L"%";
            float bw = c.Measure(s, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 36;
            Color bc = b.charging ? pal::ok : (b.percent <= 20 ? pal::danger : pal::ink2);
            drawChip(c, Rect(cx, y, bw, 30), L"battery", b.charging ? L"bolt" : L"battery", s, bc);
        }
        float rx = x + w - 15;
        drawRoundButton(c, rx, y + 15, L"settings", L"gear");
        drawRoundButton(c, rx - 36, y + 15, L"powerpage", L"power");
        drawRoundButton(c, rx - 72, y + 15, L"edit", g_edit ? L"check" : L"pencil", g_edit);
    }

    // the control being dragged follows the pointer
    if (g_dragging && g_dragFrom >= 0 && g_dragFrom < (int)g_config.controls.size()) {
        if (const ControlDef* d = FindControl(g_config.controls[(size_t)g_dragFrom])) {
            c.PushOpacity(0.9f);
            drawControlCell(c, g_dragX, g_dragY - 30, *d, g_dragFrom, false);
            c.PopOpacity();
        }
    }
}

void drawPageHeader(Canvas& c, float x, float y, float w, const std::wstring& title) {
    D2D1_RECT_F back = Rect(x, y, 34, 34);
    if (hovered(L"back")) c.FillCircle(x + 17, y + 17, 16, Color(1, 1, 1, 0.1f));
    D2D1_MATRIX_3X2_F old = c.Transform();
    c.SetTransform(D2D1::Matrix3x2F::Rotation(180, D2D1::Point2F(x + 17, y + 17)) * old);
    DrawIcon(c, L"chevron", x + 17, y + 17, 16, pal::ink, 2);
    c.SetTransform(old);
    hit(back, L"back");
    c.Text(title, Rect(x + 40, y, w - 120, 34), 16, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

void drawClipboardPage(Canvas& c, float x0, float y0) {
    float x = x0 + kCardPad, w = kCardW - 2 * kCardPad, y = y0 + 18;
    drawPageHeader(c, x, y, w, L"Clipboard");
    auto items = clipboard::History();
    if (!items.empty()) {
        D2D1_RECT_F clr = Rect(x + w - 64, y + 4, 64, 26);
        c.FillRound(clr, 13, hovered(L"clear") ? Color(1, 1, 1, 0.18f) : Color(1, 1, 1, 0.1f));
        c.Text(L"Clear", clr, 12, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Center);
        hit(clr, L"clear");
    }
    y += 42;
    if (items.empty()) {
        c.Text(L"Text you copy shows up here. It is kept in memory only.", Rect(x, y, w, 64), 12, pal::ink3,
               DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
        return;
    }
    for (size_t i = 0; i < items.size() && i < 8; i++) {
        std::wstring t = items[i];
        for (auto& ch : t) if (ch == L'\n' || ch == L'\r' || ch == L'\t') ch = L' ';
        D2D1_RECT_F r = Rect(x, y + i * 38.f, w, 34);
        c.FillRound(r, 12, hovered(L"clip", (int)i) ? Color(1, 1, 1, 0.12f) : pal::glass);
        c.Text(t, Inset(r, 14, 0), 12.5f, pal::ink);
        hit(r, L"clip", (int)i);
    }
}

void drawPowerPage(Canvas& c, float x0, float y0) {
    float x = x0 + kCardPad, w = kCardW - 2 * kCardPad, y = y0 + 18;
    drawPageHeader(c, x, y, w, L"Power");
    y += 42;
    struct Item { const wchar_t* id; const wchar_t* label; const wchar_t* icon; };
    Item items[5] = {{L"p-lock", L"Lock", L"lock"}, {L"p-signout", L"Sign out", L"close"}, {L"p-sleep", L"Sleep", L"moon"},
                     {L"p-restart", L"Restart", L"timer"}, {L"p-shutdown", L"Shut down", L"power"}};
    for (int i = 0; i < 5; i++) {
        D2D1_RECT_F r = Rect(x, y + i * 44.f, w, 40);
        bool confirm = g_confirm == items[i].id && now() < g_confirmUntil;
        Color bg = confirm ? pal::danger : (hovered(items[i].id) ? Color(1, 1, 1, 0.12f) : pal::glass);
        c.FillRound(r, 14, bg);
        DrawIcon(c, items[i].icon, r.left + 22, r.top + 20, 17, pal::ink, 1.7f);
        std::wstring label = confirm ? std::wstring(L"Click again to ") + items[i].label : items[i].label;
        if (confirm) {
            std::transform(label.begin() + 15, label.begin() + 16, label.begin() + 15, ::towlower);
        }
        c.Text(label, Rect(r.left + 44, r.top, w - 60, 40), 13, pal::ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hit(r, items[i].id);
    }
}

// ------------------------------------------------------------ render
D2D1_RECT_F shapeRect() {
    float w = g_w.x, h = g_h.x;
    return Rect(kWinW / 2 - w / 2, topY(), w, h);
}

void render() {
    if (!g_hwnd) return;
    g_hits.clear();
    D2D1_RECT_F s = shapeRect();
    float bubbleScale = std::max(0.f, g_bubble.x);
    float winH = std::max(s.bottom, topY() + kPillH) + kShadow;
    int pw = (int)std::ceil(kWinW * g_k), ph = (int)std::ceil(winH * g_k);
    if (!g_surf.Begin(pw, ph, g_k)) return;
    Canvas& c = g_surf.canvas();
    bool hide = g_hidden && g_mode == Mode::Rest;

    if (!hide && !g_wine) {
        // soft shadow, stronger for the card
        float sh = 0.035f + 0.05f * std::clamp(g_shade.x, 0.f, 1.f);
        for (int i = 1; i <= 5; i++) {
            float grow = i * 4.f;
            c.FillRound(D2D1::RectF(s.left - grow * 0.5f, s.top + grow * 0.6f, s.right + grow * 0.5f, s.bottom + grow * 1.1f),
                        g_r.x + grow * 0.7f, Color(0, 0, 0, sh * (6 - i) / 5.f));
        }
    }
    if (!hide) {
        Color bg = (g_mode == Mode::Card && cardStyleGraphite()) ? Color::Hex(0x1C1C1E) : Color(0, 0, 0, 1);
        if (notch()) c.FillNotch(s, g_r.x, 8, bg);
        else {
            c.FillRound(s, g_r.x, bg);
            c.StrokeRound(Inset(s, 0.5f, 0.5f), g_r.x, pal::hairline, 1);
        }

        // content, clipped to the moving shape and faded in after each change
        c.PushClipRect(s);
        c.PushOpacity(std::clamp(g_alpha.x, 0.f, 1.f));
        if (g_mode == Mode::Card) {
            float x0 = kWinW / 2 - kCardW / 2, y0 = topY();
            if (g_page == Page::Main) drawMainPage(c, x0, y0);
            else if (g_page == Page::Clipboard) drawClipboardPage(c, x0, y0);
            else drawPowerPage(c, x0, y0);
        } else if (g_mode == Mode::Peek) {
            Target t = targetFor(Mode::Peek);
            drawPeek(c, Rect(kWinW / 2 - t.w / 2, topY(), t.w, t.h));
        } else {
            Target t = targetFor(Mode::Rest);
            drawPillContent(c, Rect(kWinW / 2 - t.w / 2, topY(), t.w, t.h), primaryAct());
        }
        c.PopOpacity();
        c.PopClipRect();
        if (g_mode != Mode::Card) hit(s, L"pill");

        // the detached bubble for the second live activity
        Act second = secondaryAct();
        if (bubbleScale > 0.01f && second != Act::None && g_mode != Mode::Card) {
            float size = kBubble * bubbleScale;
            float cx = s.right + kGap + kBubble / 2, cy = topY() + kPillH / 2;
            c.FillCircle(cx, cy, size / 2, Color(0, 0, 0, 1));
            c.StrokeCircle(cx, cy, size / 2 - 0.5f, pal::hairline, 1);
            if (bubbleScale > 0.6f) {
                c.PushOpacity((bubbleScale - 0.6f) / 0.4f);
                drawBubbleContent(c, cx, cy, size, second);
                c.PopOpacity();
            }
            hit(Rect(cx - size / 2, cy - size / 2, size, size), L"bubble");
        }
    }

    int monW = g_mon.right - g_mon.left;
    g_winPos = POINT{g_mon.left + monW / 2 - pw / 2, g_mon.top};
    g_surf.End(g_hwnd, g_winPos);
}

// ------------------------------------------------------------ actions
void peek(const std::wstring& icon, const std::wstring& title, const std::wstring& detail, float bar, Color swatch = Color(0, 0, 0, 0)) {
    if (g_mode == Mode::Card) return;
    g_peek = PeekInfo{icon, title, detail, bar, swatch, now() + 1700};
    if (g_mode == Mode::Peek) {
        Target t = targetFor(Mode::Peek);
        g_w.t = t.w;
        kick();
    } else {
        setMode(Mode::Peek);
    }
}

void openCard() {
    if (g_mode == Mode::Card) return;
    pickMonitor();
    brightness::Refresh();
    g_brightness = brightness::Get();
    setMode(Mode::Card);
    SetForegroundWindow(g_hwnd);
    // also close on a click outside, in case Windows refused to activate us
    SetTimer(g_hwnd, kTimerOutside, 60, nullptr);
}

void closeCard() {
    if (g_mode != Mode::Card) return;
    setMode(Mode::Rest);
}

void timerReset() {
    g_timerRun = false;
    g_timerTotalMs = g_config.timerMinutes * 60000;
    g_timerLeftMs = 0;
}

void toggleTimer() {
    if (g_timerRun) {
        g_timerLeftMs = (int)std::max<LONGLONG>(0, (LONGLONG)g_timerEnd - (LONGLONG)now());
        g_timerRun = false;
    } else {
        if (g_timerLeftMs <= 0) { g_timerTotalMs = g_config.timerMinutes * 60000; g_timerLeftMs = g_timerTotalMs; }
        g_timerEnd = now() + (ULONGLONG)g_timerLeftMs;
        g_timerRun = true;
    }
    relayout();
}

void runLater(UINT ms, void (*fn)()) {
    static void (*pending)() = nullptr;
    pending = fn;
    SetTimer(g_hwnd, 50, ms, [](HWND h, UINT, UINT_PTR id, DWORD) {
        KillTimer(h, id);
        if (pending) { auto f = pending; pending = nullptr; f(); }
    });
}

void controlClick(const std::wstring& id) {
    if (id == L"wifi") {
        if (radios::Available(RadioKind::WiFi)) radios::Set(RadioKind::WiFi, !radios::IsOn(RadioKind::WiFi));
        else sys::OpenUri(L"ms-settings:network-wifi");
    } else if (id == L"bluetooth") {
        if (radios::Available(RadioKind::Bluetooth)) radios::Set(RadioKind::Bluetooth, !radios::IsOn(RadioKind::Bluetooth));
        else sys::OpenUri(L"ms-settings:bluetooth");
    } else if (id == L"sound") {
        audio::SetMuted(!audio::Muted());
    } else if (id == L"mic") {
        if (audio::HasMic()) audio::SetMicMuted(!audio::MicMuted());
    } else if (id == L"focus") {
        closeCard();
        sys::OpenUri(L"ms-settings:quiethours");
    } else if (id == L"dark") {
        sys::SetDarkMode(!sys::DarkMode());
    } else if (id == L"cast") {
        closeCard();
        sendKeys({VK_LWIN, 'K'});
    } else if (id == L"screenshot") {
        closeCard();
        runLater(450, [] {
            std::wstring path;
            if (sys::Screenshot(true, path)) peek(L"screen", L"Screenshot", L"Saved and copied", -1);
        });
    } else if (id == L"snip") {
        closeCard();
        runLater(300, [] { sys::SnipArea(); });
    } else if (id == L"color") {
        closeCard();
        runLater(350, [] {
            std::wstring hex = sys::PickColor();
            if (!hex.empty()) {
                clipboard::Copy(hex);
                unsigned v = (unsigned)wcstoul(hex.c_str() + 1, nullptr, 16);
                peek(L"dropper", hex, L"Copied", -1, Color::Hex(v));
            }
        });
    } else if (id == L"record") {
        closeCard();
        runLater(300, [] { sys::ToggleRecording(); });
    } else if (id == L"clipboard") {
        g_page = Page::Clipboard;
        g_alpha.x = 0;
        relayout();
    } else if (id == L"lock") {
        closeCard();
        sys::Lock();
    } else if (id == L"nightlight") {
        closeCard();
        sys::OpenUri(L"ms-settings:nightlight");
    } else if (id == L"power") {
        g_page = Page::Power;
        g_alpha.x = 0;
        relayout();
    } else if (id == L"timer") {
        toggleTimer();
    } else if (id == L"settings") {
        closeCard();
        panel::Show();
    } else if (id == L"files") {
        closeCard();
        sys::Launch(L"explorer.exe");
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
    kick();
}

void controlSettings(const std::wstring& id) {
    const wchar_t* uri = nullptr;
    if (id == L"wifi") uri = L"ms-settings:network-wifi";
    else if (id == L"bluetooth") uri = L"ms-settings:bluetooth";
    else if (id == L"sound" || id == L"mic") uri = L"ms-settings:sound";
    else if (id == L"dark") uri = L"ms-settings:colors";
    else if (id == L"focus") uri = L"ms-settings:quiethours";
    else if (id == L"nightlight") uri = L"ms-settings:nightlight";
    else if (id == L"cast") uri = L"ms-settings:display";
    if (uri) { closeCard(); sys::OpenUri(uri); }
}

void powerAction(const std::wstring& id) {
    if (id != L"p-lock" && !(g_confirm == id && now() < g_confirmUntil)) {
        g_confirm = id;
        g_confirmUntil = now() + 4000;
        kick();
        return;
    }
    g_confirm.clear();
    closeCard();
    if (id == L"p-lock") sys::Lock();
    else if (id == L"p-signout") sys::SignOut();
    else if (id == L"p-sleep") sys::Sleep();
    else if (id == L"p-restart") sys::Restart();
    else if (id == L"p-shutdown") sys::Shutdown();
}

void setSliderFromX(float x) {
    for (auto& h : g_hits) {
        if (h.id != g_slider) continue;
        float v = std::clamp((x - h.r.left) / (h.r.right - h.r.left), 0.f, 1.f);
        if (g_slider == L"vol") {
            audio::SetVolume(v);
            g_lastVol = v;
        } else if (g_slider == L"bright") {
            g_brightness = (int)std::lround(v * 100);
            brightness::Set(g_brightness);
        }
        kick();
        return;
    }
}

void click(const std::wstring& id, int idx, float x) {
    if (id == L"pill") {
        if (g_mode == Mode::Card) closeCard();
        else openCard();
    } else if (id == L"bubble") {
        Act a = secondaryAct();
        if (a == Act::ClaudeNeeds || a == Act::ClaudeWork || a == Act::ClaudeDone) claude::Focus(topSession());
        else if (a == Act::Music) openCard();
        else if (a == Act::Timer) toggleTimer();
        else if (a == Act::Download) sys::OpenUri(L"shell:Downloads");
    } else if (id == L"play") media::PlayPause();
    else if (id == L"next") media::Next();
    else if (id == L"prev") media::Previous();
    else if (id == L"seek") {
        for (auto& h : g_hits)
            if (h.id == L"seek") {
                float f = std::clamp((x - h.r.left) / (h.r.right - h.r.left), 0.f, 1.f);
                media::Seek(f * g_media.duration);
            }
    } else if (id == L"ctl") {
        if (!g_edit && idx >= 0 && idx < (int)g_config.controls.size()) controlClick(g_config.controls[(size_t)idx]);
    } else if (id == L"remove") {
        if (idx >= 0 && idx < (int)g_config.controls.size()) {
            g_config.controls.erase(g_config.controls.begin() + idx);
            g_config.Save();
            relayout();
        }
    } else if (id == L"add") {
        CardLayout L = cardLayout();
        if (idx >= 0 && idx < (int)L.trayItems.size()) {
            const std::wstring& item = L.trayItems[(size_t)idx];
            if (item.rfind(L"launcher:", 0) == 0) g_config.launchers.push_back(item.substr(9));
            else g_config.controls.push_back(item);
            g_config.Save();
            relayout();
        }
    } else if (id == L"lremove") {
        if (idx >= 0 && idx < (int)g_config.launchers.size()) {
            g_config.launchers.erase(g_config.launchers.begin() + idx);
            g_config.Save();
            relayout();
        }
    } else if (id == L"launch") {
        if (!g_edit && idx >= 0 && idx < (int)g_config.launchers.size()) {
            const LauncherDef* l = FindLauncher(g_config.launchers[(size_t)idx]);
            if (l) {
                closeCard();
                sys::LaunchInTerminal(l->command);
            }
        }
    } else if (id == L"session") {
        auto s = claude::Visible();
        if (idx >= 0 && idx < (int)s.size()) { closeCard(); claude::Focus(s[(size_t)idx]); }
    } else if (id == L"timerchip") toggleTimer();
    else if (id == L"weather") sys::OpenUri(L"https://wttr.in/");
    else if (id == L"battery") sys::OpenUri(L"ms-settings:batterysaver");
    else if (id == L"settings") { closeCard(); panel::Show(); }
    else if (id == L"powerpage") { g_page = Page::Power; g_alpha.x = 0; relayout(); }
    else if (id == L"edit") { g_edit = !g_edit; relayout(); }
    else if (id == L"back") { g_page = Page::Main; g_alpha.x = 0; relayout(); }
    else if (id == L"clear") { clipboard::Clear(); relayout(); }
    else if (id == L"clip") {
        auto items = clipboard::History();
        if (idx >= 0 && idx < (int)items.size()) {
            clipboard::Copy(items[(size_t)idx]);
            closeCard();
            peek(L"clipboard", L"Copied", L"", -1);
        }
    } else if (id.rfind(L"p-", 0) == 0) powerAction(id);
    kick();
}

void contextMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, g_mode == Mode::Card ? L"Close card" : L"Open card");
    AppendMenuW(m, MF_STRING, 2, g_timerRun ? L"Pause focus timer" : L"Start focus timer");
    AppendMenuW(m, MF_STRING, 3, L"Settings…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Quit Lost Island");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_CENTERALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) app::ToggleCard();
    else if (cmd == 2) toggleTimer();
    else if (cmd == 3) panel::Show();
    else if (cmd == 4) app::Quit();
}

// ------------------------------------------------------------ service updates
void demoMedia(MediaInfo& m) {
    m.active = true;
    m.playing = true;
    m.hasTimeline = true;
    m.title = L"Midnight City";
    m.artist = L"Sample Artist";
    m.album = L"Demo";
    m.appId = L"Spotify.exe";
    m.duration = 243;
    m.position = std::fmod(83 + g_phase, 243.0);
    m.artVersion = 1;
}

void refreshMedia() {
    MediaInfo m = media::Get();
    if (g_demo) {
        demoMedia(m);
        if (!g_art.src) g_art.LoadResource(2);
        g_artVersion = m.artVersion;
    }
    bool trackChanged = m.title != g_lastTitle && !m.title.empty();
    if (!g_demo && m.artVersion != g_artVersion) {
        g_artVersion = m.artVersion;
        if (m.art.empty()) g_art.Reset();
        else g_art.Decode(m.art, 256);
    }
    g_media = m;
    if (trackChanged && !g_lastTitle.empty() && g_config.peekTrack && m.playing && g_mode == Mode::Rest &&
        primaryAct() != Act::Music)
        peek(L"art", m.title, m.artist, -1);
    g_lastTitle = m.title;
    relayout();
}

void refreshAudio() {
    float v = audio::Volume();
    bool mute = audio::Muted();
    bool changed = g_lastVol >= 0 && (std::fabs(v - g_lastVol) > 0.004f || mute != g_lastMute);
    g_lastVol = v;
    g_lastMute = mute;
    if (changed && g_config.peekVolume && g_mode != Mode::Card)
        peek(mute || v < 0.005f ? L"speaker-off" : L"speaker", L"Volume", L"", mute ? 0.f : v);
    kick();
}

void refreshPower() {
    BatteryInfo b = power::Get();
    if (g_powerKnown && b.present && b.pluggedIn && !g_lastPlugged && g_config.peekCharger)
        peek(L"bolt", L"Charging", std::to_wstring(b.percent) + L"%", -1);
    g_lastPlugged = b.pluggedIn;
    g_powerKnown = true;
    kick();
}

void checkFullscreen() {
    QUERY_USER_NOTIFICATION_STATE st;
    bool full = false;
    if (g_config.hideFullscreen && SUCCEEDED(SHQueryUserNotificationState(&st)))
        full = st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
    if (full != g_hidden) {
        g_hidden = full;
        relayout();
    }
}

void tick() {
    // timer
    if (g_timerRun) {
        g_timerLeftMs = (int)std::max<LONGLONG>(0, (LONGLONG)g_timerEnd - (LONGLONG)now());
        if (g_timerLeftMs == 0) {
            g_timerRun = false;
            if (g_config.timerChime) MessageBeep(MB_ICONASTERISK);
            timerReset();
            peek(L"timer", L"Focus round done", L"Take a break", -1);
        }
    }
    if (g_config.monitor == L"cursor" && g_mode == Mode::Rest) pickMonitor();
    checkFullscreen();
    if (g_mode == Mode::Peek && now() > g_peek.until && g_slider.empty()) setMode(Mode::Rest);
    if (g_mode == Mode::Card) {
        g_media = media::Get();   // keeps the seek position moving
        if (g_demo) demoMedia(g_media);
    }
    relayout();
    if (!g_animating) render();
}

void stepEq(float dt) {
    float target = std::clamp(audio::Peak() * 1.6f, 0.f, 1.f);
    if (!g_media.playing) target = 0.12f;
    g_peak += (target - g_peak) * std::min(1.f, dt * 12);
    for (int i = 0; i < 4; i++) {
        float wobble = 0.55f + 0.45f * std::sin(g_phase * (7.f + i * 2.3f) + i * 1.9f);
        float v = std::clamp(0.18f + g_peak * wobble * 1.1f, 0.15f, 1.f);
        g_eq[i] += (v - g_eq[i]) * std::min(1.f, dt * 18);
    }
}

// ------------------------------------------------------------ hotkey
void registerHotkey() {
    UnregisterHotKey(g_hwnd, kHotkeyId);
    const std::wstring& h = g_config.hotkey;
    if (h == L"ctrl+alt+i") RegisterHotKey(g_hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'I');
    else if (h == L"win+alt+i") RegisterHotKey(g_hwnd, kHotkeyId, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'I');
    else if (h == L"ctrl+shift+space") RegisterHotKey(g_hwnd, kHotkeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_SPACE);
}

// ------------------------------------------------------------ tray
NOTIFYICONDATAW g_nid{};

void addTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(app::Instance(), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(g_nid.szTip, L"Lost Island");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// ------------------------------------------------------------ window procedure
std::wstring hitAt(float x, float y, int& idx) {
    // later regions sit on top (badges over cells), so search backwards
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (Hit(it->r, x, y)) { idx = it->idx; return it->id; }
    idx = -1;
    return L"";
}

int dropIndex(float x, float y) {
    for (auto& h : g_hits)
        if (h.id == L"ctl" && Hit(h.r, x, y)) return h.idx;
    return -1;
}

UINT g_taskbarCreated = 0;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float mx = GET_X_LPARAM(l) / g_k, my = GET_Y_LPARAM(l) / g_k;
    switch (m) {
    case WM_CREATE: return 0;
    case WM_MOUSEACTIVATE: return g_mode == Mode::Card ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && g_mode == Mode::Card && !g_dragging && !g_pinned) closeCard();
        return 0;
    case WM_TIMER:
        if (w == kTimerAnim) {
            ULONGLONG t = now();
            float dt = std::min(0.05f, (t - g_lastFrame) / 1000.f);
            g_lastFrame = t;
            g_phase += dt;
            bool moving = stepAnim(dt);
            if (primaryAct() == Act::Music || (g_mode == Mode::Card && g_media.playing)) stepEq(dt);
            render();
            if (!moving && !needsContinuous()) {
                KillTimer(h, kTimerAnim);
                g_animating = false;
            }
        } else if (w == kTimerTick) {
            tick();
        } else if (w == kTimerLong) {
            KillTimer(h, kTimerLong);
            if (g_press == L"ctl" && g_mode == Mode::Card && !g_edit) {
                g_edit = true;          // press and hold enters edit mode
                g_press.clear();
                relayout();
            }
        } else if (w == kTimerOutside) {
            if (g_mode != Mode::Card) { KillTimer(h, kTimerOutside); return 0; }
            bool down = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000;
            if (down && !g_pinned && GetCapture() != h) {
                POINT pt;
                GetCursorPos(&pt);
                D2D1_RECT_F sr = shapeRect();
                float x = (pt.x - g_winPos.x) / g_k, y = (pt.y - g_winPos.y) / g_k;
                if (!Hit(sr, x, y) && GetForegroundWindow() != h) closeCard();
            }
        } else if (w == kTimerHover) {
            KillTimer(h, kTimerHover);
            if (g_hover == L"pill" && g_mode == Mode::Rest && g_config.hoverOpen) openCard();
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        if (!g_slider.empty()) { setSliderFromX(mx); return 0; }
        if (g_edit && g_press == L"ctl" && g_pressIdx >= 0) {
            float dx = mx - g_pressPt.x / g_k, dy = my - g_pressPt.y / g_k;
            if (!g_dragging && dx * dx + dy * dy > 36) { g_dragging = true; g_dragFrom = g_pressIdx; }
        }
        if (g_dragging) { g_dragX = mx; g_dragY = my; kick(); return 0; }
        int idx;
        std::wstring id = hitAt(mx, my, idx);
        if (id != g_hover || idx != g_hoverIdx) {
            g_hover = id;
            g_hoverIdx = idx;
            if (id == L"pill" && g_config.hoverOpen && g_mode == Mode::Rest) SetTimer(h, kTimerHover, 450, nullptr);
            SetCursor(LoadCursor(nullptr, id.empty() ? IDC_ARROW : IDC_HAND));
            kick();
        }
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, g_hover.empty() ? IDC_ARROW : IDC_HAND));
        return TRUE;
    case WM_MOUSELEAVE:
        g_tracking = false;
        if (!g_hover.empty()) { g_hover.clear(); g_hoverIdx = -1; kick(); }
        return 0;
    case WM_LBUTTONDOWN: {
        int idx;
        g_press = hitAt(mx, my, idx);
        g_pressIdx = idx;
        g_pressPt = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        SetCapture(h);
        if (g_press == L"vol" || g_press == L"bright") { g_slider = g_press; setSliderFromX(mx); }
        if (g_press == L"ctl" && !g_edit) SetTimer(h, kTimerLong, 520, nullptr);
        kick();
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        KillTimer(h, kTimerLong);
        if (!g_slider.empty()) {
            g_slider.clear();
            g_press.clear();
            kick();
            return 0;
        }
        if (g_dragging) {
            int to = dropIndex(mx, my);
            if (to >= 0 && g_dragFrom >= 0 && to != g_dragFrom) {
                auto item = g_config.controls[(size_t)g_dragFrom];
                g_config.controls.erase(g_config.controls.begin() + g_dragFrom);
                g_config.controls.insert(g_config.controls.begin() + to, item);
                g_config.Save();
            }
            g_dragging = false;
            g_dragFrom = -1;
            g_press.clear();
            relayout();
            return 0;
        }
        int idx;
        std::wstring id = hitAt(mx, my, idx);
        std::wstring p = g_press;
        int pi = g_pressIdx;
        g_press.clear();
        g_pressIdx = -1;
        if (!p.empty() && p == id && pi == idx) click(id, idx, mx);
        else if (id.empty() && g_mode == Mode::Card && p.empty()) closeCard();
        kick();
        return 0;
    }
    case WM_RBUTTONUP: {
        int idx;
        std::wstring id = hitAt(mx, my, idx);
        if (id == L"ctl" && idx >= 0 && idx < (int)g_config.controls.size()) controlSettings(g_config.controls[(size_t)idx]);
        else if (id == L"pill" || id == L"bubble") contextMenu();
        return 0;
    }
    case WM_MBUTTONUP:
        if (g_hover == L"pill" || g_hover == L"bubble") media::PlayPause();
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if (g_hover == L"pill" || g_hover == L"vol") {
            audio::SetVolume(audio::Volume() + (delta > 0 ? 0.05f : -0.05f));
        } else if (g_hover == L"bright" && g_brightness >= 0) {
            g_brightness = std::clamp(g_brightness + (delta > 0 ? 5 : -5), 0, 100);
            brightness::Set(g_brightness);
        } else if (g_hover == L"timerchip" && !g_timerRun) {
            g_config.timerMinutes = std::clamp(g_config.timerMinutes + (delta > 0 ? 5 : -5), 5, 90);
            g_config.timerMinutes -= g_config.timerMinutes % 5;
            if (g_config.timerMinutes < 5) g_config.timerMinutes = 5;
            g_timerLeftMs = 0;
            g_config.Save();
        }
        kick();
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            if (g_edit) { g_edit = false; relayout(); }
            else if (g_page != Page::Main) { g_page = Page::Main; g_alpha.x = 0; relayout(); }
            else closeCard();
        }
        return 0;
    case WM_HOTKEY:
        if (w == kHotkeyId) app::ToggleCard();
        return 0;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEKEYBOARD && ri.data.keyboard.VKey == VK_CAPITAL &&
            (ri.data.keyboard.Flags & RI_KEY_BREAK) && g_config.peekCaps) {
            bool on = (GetKeyState(VK_CAPITAL) & 1) != 0;
            peek(L"capslock", on ? L"Caps Lock on" : L"Caps Lock off", L"", -1);
        }
        return DefWindowProcW(h, m, w, l);
    }
    case WM_CLIPBOARDUPDATE:
        clipboard::OnUpdate();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMPOWERSTATUSCHANGE) power::Refresh();
        return TRUE;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_SETTINGCHANGE:
        pickMonitor();
        relayout();
        return 0;
    case WM_APP_SERVICE:
        switch (w) {
        case SVC_MEDIA: refreshMedia(); break;
        case SVC_AUDIO:
            if (l == 1) audio::Rebind();
            refreshAudio();
            break;
        case SVC_POWER:
            g_brightness = brightness::Get();
            refreshPower();
            break;
        case SVC_CLAUDE: {
            Act before = primaryAct();
            relayout();
            Act after = primaryAct();
            // a fresh permission request deserves a little bounce
            if (after == Act::ClaudeNeeds && before != Act::ClaudeNeeds) { g_w.v += 260; g_h.v += 60; }
            break;
        }
        default: relayout(); break;
        }
        return 0;
    case WM_APP_CMD:
        switch (w) {
        case CMD_TOGGLE: app::ToggleCard(); break;
        case CMD_OPEN: openCard(); break;
        case CMD_CLOSE: closeCard(); break;
        case CMD_TIMER: toggleTimer(); break;
        case CMD_SETTINGS: panel::Show(); break;
        case CMD_QUIT: app::Quit(); break;
        }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) app::ToggleCard();
        else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) contextMenu();
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        RemoveClipboardFormatListener(h);
        UnregisterHotKey(h, kHotkeyId);
        return 0;
    }
    if (m == g_taskbarCreated && m) { addTray(); return 0; }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// ------------------------------------------------------------ public
namespace island {

bool Create(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LostIslandWindow";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc)) return false;
    g_wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    pickMonitor();
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"LostIslandWindow",
                             L"Lost Island", WS_POPUP, g_mon.left, g_mon.top, 10, 10, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    Target t = targetFor(Mode::Rest);
    g_w.snap(t.w * 0.4f);
    g_w.t = t.w;
    g_h.snap(t.h);
    g_r.snap(t.r);
    g_alpha.snap(0);
    g_alpha.t = 1;
    timerReset();

    media::Start(g_hwnd);
    radios::Start(g_hwnd);
    audio::Start(g_hwnd);
    power::Start(g_hwnd);
    network::Start(g_hwnd);
    weather::Start(g_hwnd);
    clipboard::Start(g_hwnd);
    claude::Start(g_hwnd);
    downloads::Start(g_hwnd);
    g_lastVol = audio::Volume();
    g_lastMute = audio::Muted();
    g_lastPlugged = power::Get().pluggedIn;
    g_powerKnown = true;

    RAWINPUTDEVICE rid{0x01, 0x06, RIDEV_INPUTSINK, g_hwnd};   // keyboard, for Caps Lock peeks
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    registerHotkey();
    addTray();

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_hwnd, kTimerTick, 1000, nullptr);
    kick();
    return true;
}

void Destroy() {
    if (!g_hwnd) return;
    KillTimer(g_hwnd, kTimerAnim);
    KillTimer(g_hwnd, kTimerTick);
    if (g_demo) DeleteFileW((sys::CacheDir() + L"\\claude-status\\demo.json").c_str());
    downloads::Stop();
    brightness::Stop();
    claude::Stop();
    weather::Stop();
    network::Stop();
    audio::Stop();
    radios::Stop();
    media::Stop();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
}

HWND Hwnd() { return g_hwnd; }

void ApplyConfig() {
    pickMonitor();
    registerHotkey();
    if (!g_timerRun && g_timerLeftMs == 0) timerReset();
    relayout();
}

void ToggleCard() {
    if (g_mode == Mode::Card) closeCard();
    else openCard();
}

void ToggleTimer() { toggleTimer(); }

void Peek(const std::wstring& icon, const std::wstring& title, const std::wstring& detail, float bar) {
    peek(icon, title, detail, bar);
}

void SetDemo(bool on) {
    g_demo = on;
    if (on) {
        // a sample Claude Code session waiting for permission
        std::wstring dir = sys::CacheDir() + L"\\claude-status";
        CreateDirectoryW(dir.c_str(), nullptr);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        double unix = ((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 1e7) - 11644473600.0;
        char buf[256];
        snprintf(buf, sizeof buf,
                 "{\"session\":\"demo\",\"state\":\"needs\",\"detail\":\"Allow Bash?\",\"project\":\"lost-island\",\"pid\":0,\"updated\":%.0f}",
                 unix + 3600);
        HANDLE f = CreateFileW((dir + L"\\demo.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(f, buf, (DWORD)strlen(buf), &n, nullptr);
            CloseHandle(f);
        }
        refreshMedia();
    }
}

void DebugState(const std::wstring& state) {
    logf("debug state %ls", state.c_str());
    g_pinned = true;
    if (state == L"card") openCard();
    else if (state == L"edit") { openCard(); g_edit = true; relayout(); }
    else if (state == L"clipboard") { openCard(); g_page = Page::Clipboard; relayout(); }
    else if (state == L"power") { openCard(); g_page = Page::Power; relayout(); }
    else if (state == L"volume") peek(L"speaker", L"Volume", L"", 0.62f);
    else if (state == L"timer") toggleTimer();
    else if (state == L"caps") peek(L"capslock", L"Caps Lock on", L"", -1);
    else if (state == L"timer-rest") { toggleTimer(); g_timerEnd -= 1000 * 60 * 7; }
    if (g_mode == Mode::Peek) g_peek.until = now() + 60000;   // keep it for the screenshot
}

}  // namespace island
