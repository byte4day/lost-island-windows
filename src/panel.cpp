// Lost Island Settings: the control panel window.
// Everything is custom drawn with Direct2D in the style of macOS System
// Settings (dark appearance). The UI is immediate mode: each paint draws the
// page and records clickable regions ("hots"); mouse and keyboard handlers act
// on the regions from the last paint.
#include "app.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "gfx.h"
#include "services.h"

#ifndef LI_VERSION
#define LI_VERSION "dev"
#endif

namespace {

// ------------------------------------------------------------ tokens
constexpr float kSidebarW = 216;
constexpr float kHeaderH = 52;
constexpr float kMargin = 20;
constexpr float kStartW = 720, kStartH = 560, kMinW = 640, kMinH = 480;
constexpr UINT_PTR kAnimTimer = 1, kCaretTimer = 2;
constexpr float kSwitchMs = 120;

constexpr Color kSidebarBg = Color::Hex(0x262628);
constexpr Color kContentBg = Color::Hex(0x1E1E1E);
constexpr Color kBoxFill(1, 1, 1, 0.05f);
constexpr Color kBoxLine(1, 1, 1, 0.08f);
constexpr Color kSep(1, 1, 1, 0.08f);
constexpr Color kWhite(1, 1, 1, 1);

constexpr DWRITE_FONT_WEIGHT kRegular = DWRITE_FONT_WEIGHT_NORMAL;
constexpr DWRITE_FONT_WEIGHT kSemibold = DWRITE_FONT_WEIGHT_SEMI_BOLD;
constexpr DWRITE_FONT_WEIGHT kBold = DWRITE_FONT_WEIGHT_BOLD;

enum class Cur { Arrow, Hand, IBeam };
enum class Kind { Button, Slider };
enum class Focus { None, Sidebar, Search, City };

struct SliderInfo {
    int* value = nullptr;
    int lo = 0, hi = 1, step = 1;
    float x0 = 0, x1 = 1;   // knob centre range
    std::function<void()> changed;
};

struct Hot {
    D2D1_RECT_F r;
    D2D1_RECT_F clip;
    std::wstring key;
    Kind kind;
    Cur cur;
    std::function<void()> click;
    SliderInfo slider;
};

struct PageDef {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* icon;      // DrawIcon name; "spark" = DrawSpark
    unsigned tile;
    const wchar_t* keywords;  // row titles, for the sidebar search
    int group;
};

const PageDef kPages[] = {
    {L"general", L"General", L"gear", 0x8E8E93,
     L"start with windows open card on hover hide over fullscreen apps reduce motion reset all settings config folder", 0},
    {L"island", L"Island", L"island", 0x0A84FF,
     L"shape island notch pill face windows logo clock weather display primary follows pointer monitor distance from top size scale", 0},
    {L"appearance", L"Appearance", L"palette", 0xBF5AF2,
     L"accent color card style black graphite 24-hour clock show seconds eq bars music", 0},
    {L"controls", L"Controls", L"grid", 0xFF9F0A,
     L"in the card available launchers wi-fi bluetooth sound mic focus dark cast screenshot clipboard lock timer wallpaper wallpapers folder fill fit", 0},
    {L"activities", L"Live Activities", L"sparkles", 0xFF375F,
     L"music timer claude downloads peeks volume caps lock charger track focus timer length chime weather city units", 1},
    {L"claude", L"Claude Code", L"spark", 0xD97757, L"hooks install remove status permission terminal", 1},
    {L"keyboard", L"Keyboard", L"keyboard", 0x8E8E93,
     L"open card shortcut hotkey gestures click scroll volume middle-click play pause right-click menu", 1},
    {L"about", L"About", L"info", 0x636366, L"version github report an issue credits", 2},
};
constexpr int kPageCount = (int)(sizeof(kPages) / sizeof(kPages[0]));

struct Row {
    std::wstring title, subtitle;
    std::function<void(D2D1_RECT_F)> trailing;   // gets the row rect minus the right padding
    std::function<void(D2D1_RECT_F)> leading;    // 22x22 tile rect
    float height = 0;
    Color titleColor = pal::ink;
};

using Options = std::vector<std::pair<std::wstring, std::wstring>>;   // label, value

// ------------------------------------------------------------ state
struct State {
    HWND hwnd = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    Canvas* cv = nullptr;
    float scale = 1;
    float w = 0, h = 0;              // client size in DIPs

    int page = 0;
    float scrollY = 0, contentH = 0, viewH = 0;

    std::vector<Hot> hots;
    std::wstring hover, press;
    float mx = -1, my = -1;
    bool tracking = false;
    bool dragging = false;
    SliderInfo drag;
    std::wstring dragKey;

    Focus focus = Focus::None;
    std::wstring search, city;
    bool caretOn = true;

    std::map<std::wstring, float> knob, knobTarget;
    bool animating = false;
    DWORD lastTick = 0;

    bool startWithWindows = false;
    bool claudeInstalled = false;
    std::wstring claudeMsg;
    bool claudeOk = true;

    // per-paint layout
    float y = 0;
    float cx0 = 0, cw = 0;
    D2D1_RECT_F view{};
    D2D1_RECT_F clip{};

    std::map<std::tuple<int, int, int>, IDWriteTextFormat*> wrapFormats;
};
State S;

Canvas& C() { return *S.cv; }

void Invalidate() {
    if (S.hwnd) InvalidateRect(S.hwnd, nullptr, FALSE);
}

void Changed() {
    app::ConfigChanged();
    Invalidate();
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool PageVisible(int i) {
    if (S.search.empty()) return true;
    std::wstring q = Lower(S.search);
    return Lower(kPages[i].label).find(q) != std::wstring::npos ||
           Lower(kPages[i].keywords).find(q) != std::wstring::npos;
}

// ------------------------------------------------------------ hots
void AddHot(D2D1_RECT_F r, const std::wstring& key, Cur cur, std::function<void()> click,
            Kind kind = Kind::Button, SliderInfo sl = {}) {
    if (r.right <= S.clip.left || r.left >= S.clip.right || r.bottom <= S.clip.top || r.top >= S.clip.bottom) return;
    S.hots.push_back({r, S.clip, key, kind, cur, std::move(click), std::move(sl)});
}

const Hot* HitTest(float x, float y) {
    for (auto it = S.hots.rbegin(); it != S.hots.rend(); ++it)
        if (Hit(it->r, x, y) && Hit(it->clip, x, y)) return &*it;
    return nullptr;
}

bool Hovered(const std::wstring& key) { return S.hover == key && (S.press.empty() || S.press == key); }
bool Pressed(const std::wstring& key) { return S.press == key && S.hover == key; }

// ------------------------------------------------------------ animation
void StartAnim() {
    if (S.animating || !S.hwnd) return;
    S.animating = true;
    S.lastTick = GetTickCount();
    SetTimer(S.hwnd, kAnimTimer, 15, nullptr);
}

void StepAnim() {
    DWORD now = GetTickCount();
    float dt = (float)(now - S.lastTick);
    S.lastTick = now;
    float step = g_config.reducedMotion ? 1.f : dt / kSwitchMs;
    bool busy = false;
    for (auto& [key, pos] : S.knob) {
        float target = S.knobTarget[key];
        if (pos < target) pos = std::min(target, pos + step);
        else if (pos > target) pos = std::max(target, pos - step);
        if (pos != target) busy = true;
    }
    if (!busy) {
        KillTimer(S.hwnd, kAnimTimer);
        S.animating = false;
    }
    Invalidate();
}

void CaretTimer(bool on) {
    if (!S.hwnd) return;
    S.caretOn = true;
    if (on) SetTimer(S.hwnd, kCaretTimer, 530, nullptr);
    else KillTimer(S.hwnd, kCaretTimer);
}

// ------------------------------------------------------------ text helpers
IDWriteTextFormat* WrapFormat(float size, DWRITE_FONT_WEIGHT w, bool center) {
    auto key = std::make_tuple((int)(size * 10), (int)w, center ? 1 : 0);
    auto it = S.wrapFormats.find(key);
    if (it != S.wrapFormats.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    g_gfx.dw->CreateTextFormat(g_gfx.family.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (!f) return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    f->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    S.wrapFormats[key] = f;
    return f;
}

// Wrapped text; returns its height.
float Paragraph(const std::wstring& s, float x, float y, float width, float size, Color c,
                DWRITE_FONT_WEIGHT w = kRegular, bool center = false) {
    IDWriteTextFormat* f = WrapFormat(size, w, center);
    if (!f || s.empty()) return 0;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_gfx.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(1.f, width), 2000, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    S.rt->DrawTextLayout(D2D1::Point2F(x, y), layout, C().Brush(c));
    layout->Release();
    return m.height;
}

// ------------------------------------------------------------ menus
template <class T>
T ProcAs(FARPROC p) { return reinterpret_cast<T>(reinterpret_cast<void*>(p)); }

// Dark context menus on Windows 10 1903+ (undocumented uxtheme ordinals).
// Returns the previous app mode, or -1 if unsupported.
int SetMenuMode(int mode) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return -1;
    static auto setMode = ProcAs<int(WINAPI*)(int)>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    static auto flush = ProcAs<void(WINAPI*)()>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (!setMode || !flush) return -1;
    int prev = setMode(mode);
    flush();
    return prev;
}

int ShowMenu(const Options& opts, const std::wstring& current, D2D1_RECT_F anchor) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < opts.size(); i++)
        AppendMenuW(m, MF_STRING | (opts[i].second == current ? MF_CHECKED : 0), i + 1, opts[i].first.c_str());
    POINT pt{(LONG)(anchor.left * S.scale), (LONG)(anchor.bottom * S.scale + 2)};
    ClientToScreen(S.hwnd, &pt);
    int prev = SetMenuMode(2);   // ForceDark
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                  S.hwnd, nullptr);
    if (prev >= 0) SetMenuMode(prev);
    DestroyMenu(m);
    return cmd - 1;
}

// ------------------------------------------------------------ controls
// Each trailing control takes the right edge and vertical centre and returns its left edge.

float SwitchCtl(const std::wstring& key, bool on, float right, float cy, std::function<void()> toggle) {
    D2D1_RECT_F r = Rect(right - 32, cy - 9, 32, 18);
    float target = on ? 1.f : 0.f;
    auto it = S.knob.find(key);
    if (it == S.knob.end()) it = S.knob.emplace(key, target).first;
    S.knobTarget[key] = target;
    if (it->second != target) StartAnim();
    float p = it->second;
    Color off(1, 1, 1, Hovered(key) ? 0.2f : 0.15f);
    C().FillRound(r, 9, off.mix(pal::accent(), p));
    float kx = r.left + 9 + p * 14;
    C().FillCircle(kx, cy + 0.5f, 7.5f, Color(0, 0, 0, 0.18f));
    C().FillCircle(kx, cy, 7, Pressed(key) ? Color::Hex(0xE5E5EA) : kWhite);
    AddHot(Inset(r, -4, -6), key, Cur::Hand, std::move(toggle));
    return r.left;
}

void Chevrons(float cx, float cy, Color c) {
    C().Line(cx - 3, cy - 2, cx, cy - 5, c, 1.4f);
    C().Line(cx, cy - 5, cx + 3, cy - 2, c, 1.4f);
    C().Line(cx - 3, cy + 2, cx, cy + 5, c, 1.4f);
    C().Line(cx, cy + 5, cx + 3, cy + 2, c, 1.4f);
}

float PopupCtl(const std::wstring& key, const Options& opts, const std::wstring& current, float right, float cy,
               std::function<void(const std::wstring&)> pick, bool enabled = true) {
    float tw = 0;
    std::wstring label;
    for (auto& o : opts) {
        tw = std::max(tw, C().Measure(o.first, 13));
        if (o.second == current) label = o.first;
    }
    if (label.empty() && !opts.empty()) label = opts[0].first;
    float w = std::ceil(tw) + 10 + 24;
    D2D1_RECT_F r = Rect(right - w, cy - 11, w, 22);
    float a = enabled ? 1.f : 0.4f;
    float fill = Pressed(key) ? 0.18f : Hovered(key) ? 0.14f : 0.10f;
    C().FillRound(r, 5, Color(1, 1, 1, fill * a));
    C().Text(label, Rect(r.left + 9, r.top, w - 30, 22), 13, pal::ink.alpha(a));
    Chevrons(r.right - 11, cy, pal::ink.alpha(0.85f * a));
    if (enabled)
        AddHot(r, key, Cur::Hand, [opts, current, r, pick] {
            int i = ShowMenu(opts, current, r);
            if (i >= 0 && i < (int)opts.size() && opts[(size_t)i].second != current) pick(opts[(size_t)i].second);
        });
    return r.left;
}

float SegCtl(const std::wstring& key, const std::vector<std::wstring>& labels, int sel, float right, float cy,
             std::function<void(int)> pick) {
    float segW = 0;
    for (auto& l : labels) segW = std::max(segW, C().Measure(l, 12));
    segW = std::ceil(segW) + 26;
    float total = segW * labels.size() + 4;
    D2D1_RECT_F r = Rect(right - total, cy - 11, total, 22);
    C().FillRound(r, 6, Color(1, 1, 1, 0.08f));
    for (size_t i = 0; i < labels.size(); i++) {
        D2D1_RECT_F s = Rect(r.left + 2 + segW * i, r.top + 2, segW, 18);
        std::wstring k = key + L":" + std::to_wstring(i);
        if ((int)i == sel) {
            C().FillRound(s, 5, Color(1, 1, 1, 0.22f));
        } else {
            if (Hovered(k)) C().FillRound(s, 5, Color(1, 1, 1, Pressed(k) ? 0.12f : 0.06f));
            if (i > 0 && (int)i - 1 != sel) C().Line(s.left, s.top + 4, s.left, s.bottom - 4, Color(1, 1, 1, 0.12f), 1, false);
        }
        C().Text(labels[i], s, 12, (int)i == sel ? pal::ink : pal::ink.alpha(0.85f), (int)i == sel ? kSemibold : kRegular,
                 Align::Center);
        if ((int)i != sel) AddHot(s, k, Cur::Hand, [pick, i] { pick((int)i); });
    }
    return r.left;
}

// Slider across [x0, x1] centred on cy.
void SliderCtl(const std::wstring& key, int* value, int lo, int hi, int step, float x0, float x1, float cy,
               bool enabled, std::function<void()> changed) {
    float a = enabled ? 1.f : 0.35f;
    float k0 = x0 + 10, k1 = x1 - 10;
    float t = hi > lo ? (float)(*value - lo) / (float)(hi - lo) : 0;
    t = std::clamp(t, 0.f, 1.f);
    float kx = k0 + t * (k1 - k0);
    D2D1_RECT_F track = Rect(x0, cy - 2, x1 - x0, 4);
    C().FillRound(track, 2, Color(1, 1, 1, 0.15f * a));
    C().FillRound(D2D1::RectF(x0, cy - 2, kx, cy + 2), 2, enabled ? pal::accent() : Color(1, 1, 1, 0.3f));
    bool active = enabled && (Hovered(key) || (S.dragging && S.dragKey == key));
    C().FillCircle(kx, cy + 0.75f, 10.5f, Color(0, 0, 0, 0.25f * a));
    C().FillCircle(kx, cy, 10, (active && S.dragging ? Color::Hex(0xE5E5EA) : kWhite).alpha(enabled ? 1.f : 0.45f));
    if (enabled)
        AddHot(Rect(x0, cy - 12, x1 - x0, 24), key, Cur::Hand, nullptr, Kind::Slider,
               SliderInfo{value, lo, hi, step, k0, k1, std::move(changed)});
}

float ButtonWidth(const std::wstring& label) { return std::max(64.f, std::ceil(C().Measure(label, 13)) + 28); }

void ButtonAt(const std::wstring& key, const std::wstring& label, D2D1_RECT_F r, bool primary,
              std::function<void()> click) {
    Color base = primary ? pal::accent() : Color(1, 1, 1, 0.12f);
    if (Pressed(key)) base = primary ? base.mix(kWhite, 0.25f) : Color(1, 1, 1, 0.24f);
    else if (Hovered(key)) base = primary ? base.mix(kWhite, 0.12f) : Color(1, 1, 1, 0.17f);
    C().FillRound(r, 6, base);
    C().StrokeRound(Inset(r, 0.5f, 0.5f), 6, Color(1, 1, 1, 0.06f));
    C().Text(label, r, 13, kWhite, kRegular, Align::Center);
    AddHot(r, key, Cur::Hand, std::move(click));
}

float ButtonCtl(const std::wstring& key, const std::wstring& label, float right, float cy, bool primary,
                std::function<void()> click) {
    float w = ButtonWidth(label);
    ButtonAt(key, label, Rect(right - w, cy - 12, w, 24), primary, std::move(click));
    return right - w;
}

float SwatchesCtl(float right, float cy) {
    auto& all = AllAccents();
    const float d = 16, gap = 9;
    float total = all.size() * d + (all.size() - 1) * gap;
    float x = right - total;
    for (size_t i = 0; i < all.size(); i++) {
        float cx = x + d / 2 + i * (d + gap);
        Color c = Color::Hex(all[i].rgb);
        std::wstring key = std::wstring(L"accent:") + all[i].id;
        bool sel = g_config.accent == all[i].id;
        if (sel) C().StrokeCircle(cx, cy, 11, c, 2);
        else if (Hovered(key)) C().StrokeCircle(cx, cy, 11, c.alpha(0.45f), 2);
        C().FillCircle(cx, cy, d / 2, c);
        if (sel) C().FillCircle(cx, cy, 3, kWhite);
        std::wstring id = all[i].id;
        AddHot(Rect(cx - 11, cy - 11, 22, 22), key, Cur::Hand, [id] {
            g_config.accent = id;
            Changed();
        });
    }
    return x;
}

void Magnifier(float cx, float cy, Color c) {
    C().StrokeCircle(cx - 1, cy - 1, 4.2f, c, 1.4f);
    C().Line(cx + 2.2f, cy + 2.2f, cx + 5, cy + 5, c, 1.6f);
}

// Single-line text field; the caret always sits at the end.
void FieldCtl(const std::wstring& key, D2D1_RECT_F r, const std::wstring& text, const wchar_t* placeholder,
              Focus which, float padL, float radius) {
    bool focused = S.focus == which;
    C().FillRound(r, radius, Color(1, 1, 1, Hovered(key) && !focused ? 0.10f : 0.08f));
    if (focused) C().StrokeRound(Inset(r, -1.5f, -1.5f), radius + 1.5f, pal::accent().alpha(0.65f), 3);
    D2D1_RECT_F inner = D2D1::RectF(r.left + padL, r.top, r.right - 7, r.bottom);
    float avail = inner.right - inner.left;
    S.rt->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (text.empty()) {
        C().Text(placeholder, D2D1::RectF(inner.left, r.top, inner.right + 200, r.bottom), 13, pal::ink3);
        if (focused && S.caretOn)
            C().FillRect(Rect(inner.left, (r.top + r.bottom) / 2 - 7.5f, 1.5f, 15), pal::accent());
    } else {
        float tw = C().Measure(text, 13);
        float shift = std::max(0.f, tw + 3 - avail);
        C().Text(text, D2D1::RectF(inner.left - shift, r.top, inner.left - shift + tw + 40, r.bottom), 13, pal::ink);
        if (focused && S.caretOn)
            C().FillRect(Rect(inner.left - shift + tw + 0.5f, (r.top + r.bottom) / 2 - 7.5f, 1.5f, 15), pal::accent());
    }
    S.rt->PopAxisAlignedClip();
    AddHot(r, key, Cur::IBeam, [which] {
        S.focus = which;
        CaretTimer(true);
        Invalidate();
    });
}

// Small round list buttons: 0 remove, 1 add, 2 up, 3 down.
void ListBtn(const std::wstring& key, float cx, float cy, int kind, bool enabled, std::function<void()> click) {
    bool hov = enabled && Hovered(key);
    if (kind <= 1) {
        Color c = kind == 0 ? pal::danger : pal::ok;
        if (hov) c = c.mix(kWhite, Pressed(key) ? 0.3f : 0.15f);
        C().FillCircle(cx, cy, 8, c);
        C().Line(cx - 4, cy, cx + 4, cy, kWhite, 1.8f);
        if (kind == 1) C().Line(cx, cy - 4, cx, cy + 4, kWhite, 1.8f);
        AddHot(Rect(cx - 11, cy - 11, 22, 22), key, Cur::Hand, std::move(click));
        return;
    }
    D2D1_RECT_F r = Rect(cx - 11, cy - 10, 22, 20);
    if (hov) C().FillRound(r, 5, Color(1, 1, 1, Pressed(key) ? 0.18f : 0.10f));
    Color c = enabled ? pal::ink.alpha(0.85f) : pal::ink3.alpha(0.5f);
    float d = kind == 2 ? -1.f : 1.f;
    C().Line(cx - 4, cy - 2 * d, cx, cy + 2 * d, c, 1.6f);
    C().Line(cx, cy + 2 * d, cx + 4, cy - 2 * d, c, 1.6f);
    if (enabled) AddHot(r, key, Cur::Hand, std::move(click));
}

void Tile(D2D1_RECT_F r, unsigned rgb, const std::wstring& icon, float radius) {
    Color c = Color::Hex(rgb);
    C().FillRound(r, radius, c);
    float lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    Color fg = lum > 0.75f ? Color::Hex(0x1C1C1E) : kWhite;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = r.right - r.left;
    if (icon == L"spark") DrawSpark(C(), cx, cy, s * 0.78f, fg);
    else DrawIcon(C(), icon, cx, cy, s * 0.7f, fg, 1.8f);
}

// ------------------------------------------------------------ layout
float BoxLeft() { return S.cx0 + kMargin; }
float BoxWidth() { return S.cw - 2 * kMargin; }

void Section(const std::wstring& title) {
    S.y += 10;
    C().Text(title, Rect(BoxLeft() + 10, S.y, BoxWidth() - 20, 16), 11, pal::ink3, kSemibold);
    S.y += 22;
}

void Footnote(const std::wstring& text, Color c = pal::ink2) {
    S.y -= 2;
    S.y += Paragraph(text, BoxLeft() + 10, S.y, BoxWidth() - 20, 11, c) + 12;
}

void Group(std::vector<Row> rows) {
    float x = BoxLeft(), w = BoxWidth(), total = 0;
    for (auto& r : rows) {
        if (r.height <= 0) r.height = r.subtitle.empty() ? 44.f : 54.f;
        total += r.height;
    }
    D2D1_RECT_F box = Rect(x, S.y, w, total);
    C().FillRound(box, 10, kBoxFill);
    C().StrokeRound(Inset(box, 0.5f, 0.5f), 9.5f, kBoxLine);
    float y = S.y;
    for (size_t i = 0; i < rows.size(); i++) {
        auto& r = rows[i];
        if (i) C().Line(x + 12, y + 0.5f, x + w - 12, y + 0.5f, kSep, 1, false);
        float tx = x + 12;
        if (r.leading) {
            r.leading(Rect(tx, y + (r.height - 22) / 2, 22, 22));
            tx += 32;
        }
        float tw = std::max(40.f, w * 0.62f - (tx - x));
        if (r.subtitle.empty()) {
            C().Text(r.title, Rect(tx, y, tw, r.height), 13, r.titleColor);
        } else {
            float mid = y + r.height / 2;
            C().Text(r.title, Rect(tx, mid - 17, tw, 18), 13, r.titleColor);
            C().Text(r.subtitle, Rect(tx, mid + 1, tw, 16), 11, pal::ink2);
        }
        if (r.trailing) r.trailing(D2D1::RectF(x, y, x + w - 12, y + r.height));
        y += r.height;
    }
    S.y = y + 12;
}

float Mid(D2D1_RECT_F r) { return (r.top + r.bottom) / 2; }

// Common row builders.
Row ToggleRow(const std::wstring& title, const std::wstring& sub, bool* field, std::function<void()> after = nullptr) {
    Row r;
    r.title = title;
    r.subtitle = sub;
    std::wstring key = L"sw:" + title;
    r.trailing = [key, field, after](D2D1_RECT_F rr) {
        SwitchCtl(key, *field, rr.right, Mid(rr), [field, after] {
            *field = !*field;
            Changed();
            if (after) after();
        });
    };
    return r;
}

Row InfoRow(const std::wstring& title, const std::wstring& value) {
    Row r;
    r.title = title;
    r.trailing = [value](D2D1_RECT_F rr) {
        C().Text(value, D2D1::RectF(rr.right - 240, rr.top, rr.right, rr.bottom), 13, pal::ink2, kRegular, Align::Right);
    };
    return r;
}

Row SliderRow(const std::wstring& title, const std::wstring& sub, int* value, int lo, int hi, int step,
              const std::wstring& suffix, bool enabled, std::function<void()> after = nullptr) {
    Row r;
    r.title = title;
    r.subtitle = sub;
    if (!enabled) r.titleColor = pal::ink.alpha(0.45f);
    std::wstring key = L"sl:" + title;
    r.trailing = [=](D2D1_RECT_F rr) {
        float cy = Mid(rr);
        float lw = 52;
        C().Text(std::to_wstring(*value) + suffix, D2D1::RectF(rr.right - lw, rr.top, rr.right, rr.bottom), 12,
                 enabled ? pal::ink2 : pal::ink3, kRegular, Align::Right);
        float sw = std::clamp((rr.right - rr.left) * 0.42f, 120.f, 260.f);
        SliderCtl(key, value, lo, hi, step, rr.right - lw - 8 - sw, rr.right - lw - 8, cy, enabled, [after] {
            Changed();
            if (after) after();
        });
    };
    return r;
}

Row PopupRow(const std::wstring& title, const std::wstring& sub, const Options& opts, std::wstring* field,
             std::function<void()> after = nullptr) {
    Row r;
    r.title = title;
    r.subtitle = sub;
    std::wstring key = L"pop:" + title;
    r.trailing = [=](D2D1_RECT_F rr) {
        PopupCtl(key, opts, *field, rr.right, Mid(rr), [field, after](const std::wstring& v) {
            *field = v;
            Changed();
            if (after) after();
        });
    };
    return r;
}

// ------------------------------------------------------------ pages
void PageGeneral() {
    Row start;
    start.title = L"Start with Windows";
    start.subtitle = L"Open Lost Island when you sign in";
    start.trailing = [](D2D1_RECT_F rr) {
        SwitchCtl(L"sw:start", S.startWithWindows, rr.right, Mid(rr), [] {
            S.startWithWindows = !S.startWithWindows;
            sys::SetStartWithWindows(S.startWithWindows);
            S.startWithWindows = sys::StartWithWindows();
            Invalidate();
        });
    };
    Group({start,
           ToggleRow(L"Open card on hover", L"Rest the pointer on the island to open the card", &g_config.hoverOpen),
           ToggleRow(L"Hide over fullscreen apps", L"Games and videos keep the whole screen", &g_config.hideFullscreen),
           ToggleRow(L"Reduce motion", L"Use quick fades instead of springy animations", &g_config.reducedMotion)});

    Section(L"Settings");
    Row folder;
    folder.title = L"Config folder";
    folder.subtitle = sys::ConfigDir();
    folder.trailing = [](D2D1_RECT_F rr) {
        ButtonCtl(L"btn:folder", L"Open", rr.right, Mid(rr), false, [] {
            std::wstring dir = sys::ConfigDir();
            CreateDirectoryW(dir.c_str(), nullptr);
            ShellExecuteW(S.hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
    };
    Row reset;
    reset.title = L"Reset all settings";
    reset.subtitle = L"Restore the defaults, including the card layout";
    reset.trailing = [](D2D1_RECT_F rr) {
        ButtonCtl(L"btn:reset", L"Reset…", rr.right, Mid(rr), false, [] {
            if (MessageBoxW(S.hwnd, L"Reset all Lost Island settings to their defaults?", L"Lost Island",
                            MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return;
            g_config.Reset();
            S.city = g_config.weatherCity;
            Changed();
            weather::Refresh();
        });
    };
    Group({folder, reset});
}

// Soft light: stacked translucent ellipses (radial gradient brushes are unreliable under Wine).
void Glow(float cx, float cy, float rx, float ry, Color c) {
    const int steps = 32;
    for (int i = 0; i < steps; i++) {
        float t = 1.f - (float)i / steps;
        S.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx * t, ry * t), C().Brush(c.alpha(1.f / steps)));
    }
}

// Paint the four corners outside a rounded rect with the background colour.
void MaskCorners(D2D1_RECT_F r, float rad, Color bg) {
    const float sx[4] = {r.left, r.right, r.right, r.left}, sy[4] = {r.top, r.top, r.bottom, r.bottom};
    for (int i = 0; i < 4; i++) {
        float dx = (i == 0 || i == 3) ? 1.f : -1.f, dy = i < 2 ? 1.f : -1.f;
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* k = nullptr;
        if (FAILED(g_gfx.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&k))) { SafeRelease(g); continue; }
        k->BeginFigure(D2D1::Point2F(sx[i], sy[i]), D2D1_FIGURE_BEGIN_FILLED);
        k->AddLine(D2D1::Point2F(sx[i] + dx * rad, sy[i]));
        bool cw = (dx * dy) < 0;
        k->AddArc(D2D1::ArcSegment(D2D1::Point2F(sx[i], sy[i] + dy * rad), D2D1::SizeF(rad, rad), 0,
                                   cw ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                   D2D1_ARC_SIZE_SMALL));
        k->EndFigure(D2D1_FIGURE_END_CLOSED);
        k->Close();
        C().FillGeometry(g, bg);
        k->Release();
        g->Release();
    }
}

void IslandPreview() {
    float x = BoxLeft(), w = BoxWidth(), h = 136;
    D2D1_RECT_F box = Rect(x, S.y, w, h);
    S.rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);
    C().LinearV(box, 0, Color::Hex(0x1D2B64), Color::Hex(0x8E4E7E));
    Glow(x + w * 0.2f, box.bottom, w * 0.45f, h * 0.9f, Color::Hex(0xF8B26A, 0.5f));
    Glow(x + w * 0.85f, box.top + 10, w * 0.35f, h * 0.8f, Color::Hex(0x4AC6E8, 0.4f));
    bool notch = g_config.shape == L"notch";
    float k = 0.62f * g_config.scale / 100.f;
    float pw = 190 * k, ph = 34 * k;
    float cx = x + w / 2;
    float top = box.top + (notch ? 0 : g_config.offsetY * 0.62f);
    D2D1_RECT_F pill = Rect(cx - pw / 2, top, pw, ph);
    if (notch) C().FillNotch(pill, ph * 0.42f, 6, Color(0, 0, 0, 1));
    else C().FillRound(pill, ph / 2, Color(0, 0, 0, 1));
    float fs = 13 * k * 1.25f;
    float pad = ph * 0.55f;
    if (g_config.face == L"weather") {
        C().Text(L"9:41", D2D1::RectF(pill.left + pad, pill.top, cx, pill.bottom), fs, kWhite, kSemibold);
        DrawIcon(C(), L"cloud", pill.right - pad - fs * 2.2f, Mid(pill), fs * 1.3f, kWhite, 1.6f);
        C().Text(L"18°", D2D1::RectF(cx, pill.top, pill.right - pad, pill.bottom), fs, kWhite, kSemibold,
                 Align::Right);
    } else {
        DrawWindowsMark(C(), pill.left + pad + fs * 0.45f, Mid(pill), fs * 0.9f, Color::Hex(0x4CC2FF));
        C().Text(L"9:41", D2D1::RectF(cx, pill.top, pill.right - pad, pill.bottom), fs, kWhite, kSemibold, Align::Right);
    }
    MaskCorners(box, 10, kContentBg);
    S.rt->PopAxisAlignedClip();
    C().StrokeRound(Inset(box, 0.5f, 0.5f), 9.5f, kBoxLine);
    S.y += h + 16;
}

void PageIsland() {
    IslandPreview();
    bool notch = g_config.shape == L"notch";
    Row shape;
    shape.title = L"Shape";
    shape.trailing = [](D2D1_RECT_F rr) {
        SegCtl(L"seg:shape", {L"Island", L"Notch"}, g_config.shape == L"notch" ? 1 : 0, rr.right, Mid(rr), [](int i) {
            g_config.shape = i == 1 ? L"notch" : L"island";
            Changed();
        });
    };
    Group({shape,
           PopupRow(L"Pill face", L"What the resting island shows",
                    {{L"Windows logo + clock", L"logo"}, {L"Clock + weather", L"weather"}}, &g_config.face),
           PopupRow(L"Display", L"", {{L"Primary display", L"primary"}, {L"Follows pointer", L"cursor"}},
                    &g_config.monitor)});
    Section(L"Position and Size");
    Group({SliderRow(L"Distance from top", notch ? L"Not used with the notch shape" : L"", &g_config.offsetY, 0, 40, 1,
                     L" px", !notch),
           SliderRow(L"Size", L"", &g_config.scale, 80, 150, 5, L"%", true)});
}

void PageAppearance() {
    Row accent;
    accent.title = L"Accent color";
    for (auto& a : AllAccents())
        if (g_config.accent == a.id) accent.subtitle = a.label;
    accent.trailing = [](D2D1_RECT_F rr) { SwatchesCtl(rr.right - 4, Mid(rr)); };
    Row style;
    style.title = L"Card style";
    style.subtitle = L"Background of the expanded card";
    style.trailing = [](D2D1_RECT_F rr) {
        SegCtl(L"seg:style", {L"Black", L"Graphite"}, g_config.cardStyle == L"graphite" ? 1 : 0, rr.right, Mid(rr),
               [](int i) {
                   g_config.cardStyle = i == 1 ? L"graphite" : L"black";
                   Changed();
               });
    };
    Group({accent, style});
    Section(L"Clock");
    Group({ToggleRow(L"24-hour clock", L"", &g_config.clock24), ToggleRow(L"Show seconds", L"", &g_config.showSeconds)});
    Section(L"Island");
    Group({ToggleRow(L"EQ bars while music plays", L"Animated bars next to the artwork", &g_config.showEq)});
}

unsigned ControlTint(const std::wstring& id) {
    static const std::map<std::wstring, unsigned> tints = {
        {L"wifi", 0x0A84FF},      {L"bluetooth", 0x0A84FF}, {L"sound", 0xFF375F},     {L"mic", 0xFF9F0A},
        {L"focus", 0x5E5CE6},     {L"dark", 0x636366},      {L"cast", 0x30D158},      {L"screenshot", 0x8E8E93},
        {L"snip", 0x8E8E93},      {L"color", 0xBF5AF2},     {L"record", 0xFF453A},    {L"clipboard", 0x8E8E93},
        {L"lock", 0x636366},      {L"nightlight", 0xFF9F0A}, {L"power", 0xFF453A},    {L"timer", 0xFF9F0A},
        {L"settings", 0x8E8E93},  {L"files", 0x0A84FF},
    };
    auto it = tints.find(id);
    return it == tints.end() ? 0x8E8E93 : it->second;
}

Row EmptyRow(const std::wstring& text) {
    Row r;
    r.title = text;
    r.titleColor = pal::ink3;
    return r;
}

// One editable list ("in the card") and its complement ("available").
void ListEditor(std::vector<std::wstring>* list, const std::vector<std::wstring>& all,
                std::function<void(const std::wstring&, D2D1_RECT_F)> tile,
                std::function<std::wstring(const std::wstring&)> label, const std::wstring& tag) {
    std::vector<Row> in;
    for (size_t i = 0; i < list->size(); i++) {
        std::wstring id = (*list)[i];
        Row r;
        r.title = label(id);
        r.leading = [tile, id](D2D1_RECT_F t) { tile(id, t); };
        r.trailing = [list, i, id, tag](D2D1_RECT_F rr) {
            float cy = Mid(rr);
            ListBtn(L"rm:" + tag + id, rr.right - 9, cy, 0, true, [list, id] {
                list->erase(std::remove(list->begin(), list->end(), id), list->end());
                Changed();
            });
            ListBtn(L"dn:" + tag + id, rr.right - 40, cy, 3, i + 1 < list->size(), [list, i] {
                if (i + 1 < list->size()) std::swap((*list)[i], (*list)[i + 1]);
                Changed();
            });
            ListBtn(L"up:" + tag + id, rr.right - 64, cy, 2, i > 0, [list, i] {
                if (i > 0 && i < list->size()) std::swap((*list)[i], (*list)[i - 1]);
                Changed();
            });
        };
        r.height = 40;
        in.push_back(r);
    }
    if (in.empty()) in.push_back(EmptyRow(L"Nothing in the card"));
    Group(in);
    if (tag == L"c") Footnote(L"You can also press and hold a control in the card to edit it there.");

    Section(tag == L"c" ? L"Available Controls" : L"Available Launchers");
    std::vector<Row> avail;
    for (auto& id : all) {
        if (std::find(list->begin(), list->end(), id) != list->end()) continue;
        Row r;
        r.title = label(id);
        r.leading = [tile, id](D2D1_RECT_F t) { tile(id, t); };
        r.trailing = [list, id, tag](D2D1_RECT_F rr) {
            ListBtn(L"add:" + tag + id, rr.right - 9, Mid(rr), 1, true, [list, id] {
                if (std::find(list->begin(), list->end(), id) == list->end()) list->push_back(id);
                Changed();
            });
        };
        r.height = 40;
        avail.push_back(r);
    }
    if (avail.empty()) avail.push_back(EmptyRow(tag == L"c" ? L"All controls are in the card" : L"All launchers are in the card"));
    Group(avail);
}

// Folder picker for the wallpaper library (the shell's own dialog).
void ChooseWallpaperFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Wallpaper folder");
    IShellItem* start = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(wallpaper::Folder().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
        dlg->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(dlg->Show(S.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                g_config.wallpaperFolder = path;
                Changed();
            }
            CoTaskMemFree(path);
            item->Release();
        }
    }
    dlg->Release();
}

void PageControls() {
    std::vector<std::wstring> allC, allL;
    for (auto& c : AllControls()) allC.push_back(c.id);
    for (auto& l : AllLaunchers()) allL.push_back(l.id);
    Section(L"Controls in the Card");
    ListEditor(
        &g_config.controls, allC,
        [](const std::wstring& id, D2D1_RECT_F t) {
            auto* d = FindControl(id);
            Tile(t, ControlTint(id), d ? d->icon : L"", 6);
        },
        [](const std::wstring& id) {
            auto* d = FindControl(id);
            return std::wstring(d ? d->label : id.c_str());
        },
        L"c");
    Section(L"Launchers in the Card");
    ListEditor(
        &g_config.launchers, allL,
        [](const std::wstring& id, D2D1_RECT_F t) {
            auto* d = FindLauncher(id);
            Tile(t, d ? d->color : 0x8E8E93, id == L"claude" ? L"spark" : L"terminal", 6);
        },
        [](const std::wstring& id) {
            auto* d = FindLauncher(id);
            return std::wstring(d ? d->label : id.c_str());
        },
        L"l");
    Section(L"Wallpapers");
    Row folder;
    folder.title = L"Folder";
    folder.subtitle = wallpaper::Folder();
    folder.trailing = [](D2D1_RECT_F rr) {
        float left = ButtonCtl(L"btn:wpchoose", L"Choose\u2026", rr.right, Mid(rr), false, [] { ChooseWallpaperFolder(); });
        ButtonCtl(L"btn:wpopen", L"Open", left - 8, Mid(rr), false,
                  [] { ShellExecuteW(S.hwnd, L"open", wallpaper::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    };
    Row reset;
    reset.title = L"Use the default folder";
    reset.subtitle = L"Pictures\\Wallpapers, or Pictures";
    reset.trailing = [](D2D1_RECT_F rr) {
        ButtonCtl(L"btn:wpdefault", L"Reset", rr.right, Mid(rr), false, [] {
            g_config.wallpaperFolder.clear();
            Changed();
        });
    };
    std::vector<Row> rows = {folder};
    if (!g_config.wallpaperFolder.empty()) rows.push_back(reset);
    rows.push_back(PopupRow(L"Fit", L"How the picture fills each screen",
                            {{L"Fill", L"fill"}, {L"Fit", L"fit"}, {L"Stretch", L"stretch"}, {L"Center", L"center"},
                             {L"Tile", L"tile"}, {L"Span", L"span"}},
                            &g_config.wallpaperFit));
    Group(rows);
}

void CommitCity() {
    std::wstring c = S.city;
    while (!c.empty() && c.back() == L' ') c.pop_back();
    while (!c.empty() && c.front() == L' ') c.erase(c.begin());
    S.city = c;
    if (c != g_config.weatherCity) {
        g_config.weatherCity = c;
        Changed();
        weather::Refresh();
    }
}

void PageActivities() {
    Section(L"Live Activities");
    Group({ToggleRow(L"Music", L"Now playing, with artwork and controls", &g_config.actMusic),
           ToggleRow(L"Focus timer", L"Countdown while a timer runs", &g_config.actTimer),
           ToggleRow(L"Claude Code", L"What each session is doing and permission requests", &g_config.actClaude),
           ToggleRow(L"Downloads", L"Progress of files arriving in Downloads", &g_config.actDownloads)});
    Section(L"Peeks");
    Group({ToggleRow(L"Volume", L"", &g_config.peekVolume), ToggleRow(L"Caps Lock", L"", &g_config.peekCaps),
           ToggleRow(L"Charger", L"Plugged in, unplugged and battery level", &g_config.peekCharger),
           ToggleRow(L"Track changes", L"", &g_config.peekTrack)});
    Footnote(L"Peeks briefly expand the island, then shrink back on their own.");
    Section(L"Focus Timer");
    Group({SliderRow(L"Length", L"", &g_config.timerMinutes, 5, 90, 5, L" min", true),
           ToggleRow(L"Chime when done", L"", &g_config.timerChime)});
    Section(L"Weather");
    Row city;
    city.title = L"City";
    city.subtitle = L"Leave empty to locate automatically";
    city.trailing = [](D2D1_RECT_F rr) {
        float w = std::clamp((rr.right - rr.left) * 0.4f, 140.f, 220.f);
        FieldCtl(L"field:city", Rect(rr.right - w, Mid(rr) - 11, w, 22), S.city, L"Automatic", Focus::City, 7, 5);
    };
    Row units;
    units.title = L"Units";
    units.trailing = [](D2D1_RECT_F rr) {
        SegCtl(L"seg:units", {L"°C", L"°F"}, g_config.fahrenheit ? 1 : 0, rr.right, Mid(rr), [](int i) {
            g_config.fahrenheit = i == 1;
            Changed();
            weather::Refresh();
        });
    };
    Group({city, units});
}

void PageClaude() {
    float x = BoxLeft();
    S.y += 2;
    S.y += Paragraph(L"The island shows what Claude Code is doing in each session and lets you know when it needs "
                     L"permission. Click the island bubble to jump to the terminal running that session.",
                     x + 2, S.y, BoxWidth() - 4, 13, pal::ink2) +
           16;
    Row status;
    status.title = L"Hooks";
    status.subtitle = L"Added to ~/.claude/settings.json";
    status.trailing = [](D2D1_RECT_F rr) {
        std::wstring t = S.claudeInstalled ? L"Installed" : L"Not installed";
        float tw = C().Measure(t, 13);
        C().Text(t, D2D1::RectF(rr.right - tw - 2, rr.top, rr.right, rr.bottom), 13, pal::ink2);
        C().FillCircle(rr.right - tw - 12, Mid(rr), 4, S.claudeInstalled ? pal::ok : Color(1, 1, 1, 0.3f));
    };
    Row actions;
    actions.title = S.claudeInstalled ? L"Reinstall or remove the hooks" : L"Install the hooks";
    actions.trailing = [](D2D1_RECT_F rr) {
        float cy = Mid(rr);
        float l = ButtonCtl(L"btn:cremove", L"Remove", rr.right, cy, false, [] {
            S.claudeOk = claude::Remove(S.claudeMsg);
            S.claudeInstalled = claude::Installed();
            Changed();
        });
        ButtonCtl(L"btn:cinstall", L"Install", l - 8, cy, !S.claudeInstalled, [] {
            S.claudeOk = claude::Setup(S.claudeMsg);
            S.claudeInstalled = claude::Installed();
            Changed();
        });
    };
    Group({ToggleRow(L"Show Claude Code activity", L"", &g_config.actClaude), status, actions});
    if (!S.claudeMsg.empty()) Footnote(S.claudeMsg, S.claudeOk ? pal::ink2 : pal::danger);
}

void PageKeyboard() {
    Group({PopupRow(L"Open card shortcut", L"Works from any app",
                    {{L"Ctrl+Alt+I", L"ctrl+alt+i"},
                     {L"Win+Alt+I", L"win+alt+i"},
                     {L"Ctrl+Shift+Space", L"ctrl+shift+space"},
                     {L"Off", L"off"}},
                    &g_config.hotkey)});
    Section(L"Island Gestures");
    Group({InfoRow(L"Click", L"Open the card"), InfoRow(L"Scroll", L"Change the volume"),
           InfoRow(L"Middle-click", L"Play or pause"), InfoRow(L"Right-click", L"Menu")});
}

void PageAbout() {
    float cx = S.cx0 + S.cw / 2;
    float top = S.y + 12;
    D2D1_RECT_F icon = Rect(cx - 40, top, 80, 80);
    C().FillRound(Rect(cx - 40, top + 2, 80, 80), 18, Color(0, 0, 0, 0.35f));
    C().LinearV(icon, 18, Color::Hex(0x2A2A2E), Color::Hex(0x050506));
    C().StrokeRound(Inset(icon, 0.5f, 0.5f), 17.5f, Color(1, 1, 1, 0.14f));
    D2D1_RECT_F pill = Rect(cx - 22, top + 13, 44, 13);
    C().FillRound(pill, 6.5f, Color(0, 0, 0, 1));
    C().StrokeRound(pill, 6.5f, Color(1, 1, 1, 0.12f), 0.8f);
    C().FillCircle(pill.right - 6.5f, Mid(pill), 2.2f, pal::ok);
    DrawWindowsMark(C(), pill.left + 7, Mid(pill), 5.5f, Color::Hex(0x4CC2FF));
    S.y = top + 96;
    float bw = BoxWidth();
    C().Text(L"Lost Island", Rect(BoxLeft(), S.y, bw, 28), 22, pal::ink, kBold, Align::Center);
    S.y += 30;
    C().Text(L"Version " LI_VERSION, Rect(BoxLeft(), S.y, bw, 18), 12, pal::ink2, kRegular, Align::Center);
    S.y += 26;
    S.y += Paragraph(L"A Dynamic Island for Windows, written in C++ with Direct2D.", BoxLeft(), S.y, bw, 13, pal::ink,
                     kRegular, true) +
           18;
    float w1 = ButtonWidth(L"GitHub"), w2 = ButtonWidth(L"Report an issue");
    float bx = cx - (w1 + w2 + 10) / 2;
    ButtonAt(L"btn:github", L"GitHub", Rect(bx, S.y, w1, 24), false,
             [] { sys::OpenUri(L"https://github.com/byte4day/lost-island-windows"); });
    ButtonAt(L"btn:issue", L"Report an issue", Rect(bx + w1 + 10, S.y, w2, 24), false,
             [] { sys::OpenUri(L"https://github.com/byte4day/lost-island-windows/issues"); });
    S.y += 24 + 28;
    S.y += Paragraph(L"Based on Lost Island for Sway by byte4day, itself based on Marco Zorn's Lost Island (MIT).",
                     BoxLeft(), S.y, bw, 11, pal::ink3, kRegular, true) +
           8;
}

// ------------------------------------------------------------ frame
void DrawContent() {
    S.cx0 = kSidebarW;
    S.cw = S.w - kSidebarW;
    S.view = D2D1::RectF(kSidebarW, kHeaderH, S.w, S.h);
    S.viewH = S.h - kHeaderH;
    S.clip = S.view;
    S.rt->PushAxisAlignedClip(S.view, D2D1_ANTIALIAS_MODE_ALIASED);
    float y0 = kHeaderH + 2 - S.scrollY;
    S.y = y0;
    switch (S.page) {
        case 0: PageGeneral(); break;
        case 1: PageIsland(); break;
        case 2: PageAppearance(); break;
        case 3: PageControls(); break;
        case 4: PageActivities(); break;
        case 5: PageClaude(); break;
        case 6: PageKeyboard(); break;
        default: PageAbout(); break;
    }
    S.contentH = S.y - y0 + 12;
    S.rt->PopAxisAlignedClip();

    // overlay scroller
    if (S.contentH > S.viewH + 0.5f) {
        float th = std::max(30.f, S.viewH * S.viewH / S.contentH);
        float t = S.scrollY / (S.contentH - S.viewH);
        float ty = S.view.top + 3 + t * (S.viewH - 6 - th);
        C().FillRound(Rect(S.w - 8, ty, 5, th), 2.5f, Color(1, 1, 1, 0.22f));
    }

    // header (toolbar) with the page title
    S.clip = D2D1::RectF(kSidebarW, 0, S.w, kHeaderH);
    C().FillRect(S.clip, kContentBg);
    if (S.scrollY > 0.5f) C().Line(kSidebarW, kHeaderH - 0.5f, S.w, kHeaderH - 0.5f, Color(0, 0, 0, 0.6f), 1, false);
    C().Text(kPages[S.page].label, Rect(kSidebarW + kMargin, 0, S.cw - 2 * kMargin, kHeaderH), 15, pal::ink, kSemibold);
}

void SetPage(int i);

void DrawSidebar() {
    S.clip = D2D1::RectF(0, 0, kSidebarW, S.h);
    C().FillRect(S.clip, kSidebarBg);
    C().Line(kSidebarW - 0.5f, 0, kSidebarW - 0.5f, S.h, Color(0, 0, 0, 0.55f), 1, false);

    D2D1_RECT_F field = Rect(10, 12, kSidebarW - 20, 28);
    FieldCtl(L"field:search", field, S.search, L"Search", Focus::Search, 28, 6);
    Magnifier(field.left + 14, Mid(field), pal::ink3.mix(pal::ink, 0.25f));
    if (!S.search.empty()) {
        float cx = field.right - 14, cy = Mid(field);
        C().FillCircle(cx, cy, 7, Color(1, 1, 1, Hovered(L"search:clear") ? 0.45f : 0.3f));
        C().Line(cx - 2.5f, cy - 2.5f, cx + 2.5f, cy + 2.5f, kSidebarBg, 1.4f);
        C().Line(cx + 2.5f, cy - 2.5f, cx - 2.5f, cy + 2.5f, kSidebarBg, 1.4f);
        AddHot(Rect(cx - 9, cy - 9, 18, 18), L"search:clear", Cur::Hand, [] {
            S.search.clear();
            Invalidate();
        });
    }

    float y = 52;
    int lastGroup = -1;
    bool any = false;
    for (int i = 0; i < kPageCount; i++) {
        if (!PageVisible(i)) continue;
        if (any && kPages[i].group != lastGroup) y += 10;
        lastGroup = kPages[i].group;
        any = true;
        D2D1_RECT_F r = Rect(10, y, kSidebarW - 20, 28);
        std::wstring key = std::wstring(L"nav:") + kPages[i].id;
        bool sel = i == S.page;
        if (sel) C().FillRound(r, 6, pal::accent());
        else if (Hovered(key)) C().FillRound(r, 6, Color(1, 1, 1, 0.06f));
        Tile(Rect(r.left + 6, r.top + 4, 20, 20), kPages[i].tile, kPages[i].icon, 5);
        C().Text(kPages[i].label, D2D1::RectF(r.left + 34, r.top, r.right - 6, r.bottom), 13, sel ? kWhite : pal::ink);
        AddHot(r, key, Cur::Arrow, [i] {
            S.focus = Focus::Sidebar;
            SetPage(i);
        });
        y += 30;
    }
    if (!any) C().Text(L"No Results", Rect(10, 60, kSidebarW - 20, 24), 13, pal::ink3, kRegular, Align::Center);
}

// ------------------------------------------------------------ target
void ReleaseTarget() {
    if (S.cv) {
        S.cv->Attach(nullptr);
        delete S.cv;
        S.cv = nullptr;
    }
    SafeRelease(S.rt);
}

bool EnsureTarget() {
    if (S.rt) return true;
    RECT rc;
    GetClientRect(S.hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.f * S.scale, 96.f * S.scale);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
        S.hwnd, D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom)));
    if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, hp, &S.rt))) return false;
    S.rt->SetDpi(96.f * S.scale, 96.f * S.scale);
    S.cv = new Canvas();
    S.cv->Attach(S.rt);
    return true;
}

void UpdateHover();

void Paint() {
    if (!EnsureTarget()) return;
    RECT rc;
    GetClientRect(S.hwnd, &rc);
    S.w = rc.right / S.scale;
    S.h = rc.bottom / S.scale;
    if (S.focus != Focus::City) S.city = g_config.weatherCity;

    S.rt->BeginDraw();
    S.rt->SetTransform(D2D1::IdentityMatrix());
    S.rt->Clear(kContentBg.d2d());
    S.hots.clear();
    DrawContent();
    DrawSidebar();
    HRESULT hr = S.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseTarget();
        Invalidate();
        return;
    }
    float maxScroll = std::max(0.f, S.contentH - S.viewH);
    if (S.scrollY > maxScroll) {
        S.scrollY = maxScroll;
        Invalidate();
    }
    UpdateHover();
}

// ------------------------------------------------------------ input
void UpdateHover() {
    const Hot* h = (S.mx >= 0) ? HitTest(S.mx, S.my) : nullptr;
    std::wstring k = h ? h->key : L"";
    if (k != S.hover) {
        S.hover = k;
        Invalidate();
    }
}

void SetPage(int i) {
    if (S.focus == Focus::City) {
        CommitCity();
        S.focus = Focus::None;
        CaretTimer(false);
    }
    i = std::clamp(i, 0, kPageCount - 1);
    if (i != S.page) S.scrollY = 0;
    S.page = i;
    S.startWithWindows = sys::StartWithWindows();
    S.claudeInstalled = claude::Installed();
    Invalidate();
}

void MovePage(int dir) {
    int i = S.page;
    for (int n = 0; n < kPageCount; n++) {
        i += dir;
        if (i < 0 || i >= kPageCount) return;
        if (PageVisible(i)) { SetPage(i); return; }
    }
}

void SlideTo(float x) {
    auto& d = S.drag;
    if (!d.value) return;
    float t = d.x1 > d.x0 ? std::clamp((x - d.x0) / (d.x1 - d.x0), 0.f, 1.f) : 0;
    float raw = d.lo + t * (d.hi - d.lo);
    int v = d.lo + (int)std::lround((raw - d.lo) / d.step) * d.step;
    v = std::clamp(v, d.lo, d.hi);
    if (v != *d.value) {
        *d.value = v;
        if (d.changed) d.changed();
        Invalidate();
    }
}

void Blur(Focus keep) {
    if (S.focus == keep) return;
    if (S.focus == Focus::City) CommitCity();
    if (S.focus == Focus::City || S.focus == Focus::Search) CaretTimer(false);
    S.focus = Focus::None;
    Invalidate();
}

void Scroll(float dy) {
    float maxScroll = std::max(0.f, S.contentH - S.viewH);
    float ns = std::clamp(S.scrollY + dy, 0.f, maxScroll);
    if (ns != S.scrollY) {
        S.scrollY = ns;
        Invalidate();
    }
}

void OnMouseDown(float x, float y) {
    SetFocus(S.hwnd);
    const Hot* h = HitTest(x, y);
    std::wstring key = h ? h->key : L"";
    if (key == L"field:city") Blur(Focus::City);
    else if (key == L"field:search" || key == L"search:clear") Blur(Focus::Search);
    else Blur(Focus::None);
    if (!h) {
        if (x < kSidebarW) S.focus = Focus::Sidebar;
        return;
    }
    if (h->kind == Kind::Slider) {
        S.dragging = true;
        S.drag = h->slider;
        S.dragKey = h->key;
        SetCapture(S.hwnd);
        SlideTo(x);
        Invalidate();
        return;
    }
    S.press = h->key;
    SetCapture(S.hwnd);
    Invalidate();
}

void OnMouseUp(float x, float y) {
    if (S.dragging) {
        S.dragging = false;
        S.drag = {};
        S.dragKey.clear();
        ReleaseCapture();
        Invalidate();
        return;
    }
    if (S.press.empty()) return;
    std::wstring pressed = S.press;
    S.press.clear();
    ReleaseCapture();
    const Hot* h = HitTest(x, y);
    std::function<void()> fn;
    if (h && h->key == pressed) fn = h->click;   // copy: a menu loop may repaint and rebuild the hots
    Invalidate();
    if (fn) fn();
}

void OnWheel(float x, float y, int delta) {
    const Hot* h = HitTest(x, y);
    if (h && h->kind == Kind::Slider && h->slider.value) {
        SliderInfo sl = h->slider;
        int v = std::clamp(*sl.value + (delta > 0 ? sl.step : -sl.step), sl.lo, sl.hi);
        if (v != *sl.value) {
            *sl.value = v;
            if (sl.changed) sl.changed();
            Invalidate();
        }
        return;
    }
    if (x >= kSidebarW) Scroll(-(float)delta / WHEEL_DELTA * 48.f);
}

bool OnKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && vk == 'W') {
        panel::Close();
        return true;
    }
    std::wstring* buf = S.focus == Focus::Search ? &S.search : S.focus == Focus::City ? &S.city : nullptr;
    switch (vk) {
        case VK_ESCAPE:
            if (S.focus == Focus::Search) {
                if (!S.search.empty()) S.search.clear();
                else Blur(Focus::None);
            } else if (S.focus == Focus::City) {
                S.city = g_config.weatherCity;
                S.focus = Focus::None;
                CaretTimer(false);
            }
            Invalidate();
            return true;
        case VK_UP:
        case VK_DOWN:
            if (S.focus == Focus::Sidebar || S.focus == Focus::Search || S.focus == Focus::None) {
                MovePage(vk == VK_UP ? -1 : 1);
                return true;
            }
            return false;
        case VK_BACK:
            if (buf && !buf->empty()) {
                if (ctrl) buf->clear();
                else buf->pop_back();
                S.caretOn = true;
                Invalidate();
            }
            return buf != nullptr;
        case VK_RETURN:
            if (S.focus == Focus::City) {
                CommitCity();
                S.focus = Focus::None;
                CaretTimer(false);
                Invalidate();
            } else if (S.focus == Focus::Search) {
                for (int i = 0; i < kPageCount; i++)
                    if (PageVisible(i)) { SetPage(i); break; }
            }
            return true;
        case VK_PRIOR: Scroll(-S.viewH * 0.9f); return true;
        case VK_NEXT: Scroll(S.viewH * 0.9f); return true;
        case VK_HOME:
            if (!buf) { Scroll(-1e6f); return true; }
            return false;
        case VK_END:
            if (!buf) { Scroll(1e6f); return true; }
            return false;
    }
    return false;
}

void OnChar(wchar_t ch) {
    std::wstring* buf = S.focus == Focus::Search ? &S.search : S.focus == Focus::City ? &S.city : nullptr;
    if (!buf || ch < 32 || ch == 127) return;
    if (buf->size() >= 80) return;
    buf->push_back(ch);
    S.caretOn = true;
    Invalidate();
}

// ------------------------------------------------------------ window
void ApplyDarkFrame(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corners = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corners, sizeof(corners));
    int backdrop = 2;  // DWMSBT_MAINWINDOW (Mica)
    DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (S.rt) S.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            UINT dpi = GetDpiForWindow(hwnd);
            if (!dpi) dpi = 96;
            RECT rc{0, 0, (LONG)(kMinW * dpi / 96), (LONG)(kMinH * dpi / 96)};
            AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
            mm->ptMinTrackSize = {rc.right - rc.left, rc.bottom - rc.top};
            return 0;
        }
        case WM_DPICHANGED: {
            S.scale = HIWORD(wp) / 96.f;
            if (S.rt) S.rt->SetDpi(96.f * S.scale, 96.f * S.scale);
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            return 0;
        }
        case WM_MOUSEMOVE: {
            S.mx = GET_X_LPARAM(lp) / S.scale;
            S.my = GET_Y_LPARAM(lp) / S.scale;
            if (!S.tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                S.tracking = TrackMouseEvent(&t) != FALSE;
            }
            if (S.dragging) SlideTo(S.mx);
            else UpdateHover();
            return 0;
        }
        case WM_MOUSELEAVE:
            S.tracking = false;
            S.mx = S.my = -1;
            if (!S.dragging) UpdateHover();
            return 0;
        case WM_LBUTTONDOWN:
            OnMouseDown(GET_X_LPARAM(lp) / S.scale, GET_Y_LPARAM(lp) / S.scale);
            return 0;
        case WM_LBUTTONUP:
            OnMouseUp(GET_X_LPARAM(lp) / S.scale, GET_Y_LPARAM(lp) / S.scale);
            return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd) {
                S.dragging = false;
                S.drag = {};
                S.press.clear();
                Invalidate();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            OnWheel(pt.x / S.scale, pt.y / S.scale, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                const Hot* h = HitTest(pt.x / S.scale, pt.y / S.scale);
                Cur c = h ? h->cur : Cur::Arrow;
                SetCursor(LoadCursorW(nullptr, c == Cur::Hand ? IDC_HAND : c == Cur::IBeam ? IDC_IBEAM : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            if (OnKey(wp)) return 0;
            break;
        case WM_CHAR:
            OnChar((wchar_t)wp);
            return 0;
        case WM_TIMER:
            if (wp == kAnimTimer) StepAnim();
            else if (wp == kCaretTimer) {
                S.caretOn = !S.caretOn;
                Invalidate();
            }
            return 0;
        case WM_KILLFOCUS:
            if (S.focus == Focus::City) CommitCity();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (S.focus == Focus::City) CommitCity();
            KillTimer(hwnd, kAnimTimer);
            KillTimer(hwnd, kCaretTimer);
            ReleaseTarget();
            S.hwnd = nullptr;
            S.hots.clear();
            S.knob.clear();
            S.knobTarget.clear();
            S.animating = false;
            S.focus = Focus::None;
            S.hover.clear();
            S.press.clear();
            S.dragging = false;
            S.tracking = false;
            S.search.clear();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int PageIndex(const wchar_t* id) {
    if (!id) return -1;
    for (int i = 0; i < kPageCount; i++)
        if (wcscmp(kPages[i].id, id) == 0) return i;
    return -1;
}

void Create(int page) {
    static bool registered = false;
    HINSTANCE inst = app::Instance();
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
        wc.lpszClassName = L"LostIslandSettings";
        registered = RegisterClassExW(&wc) != 0;
    }
    const DWORD style = WS_OVERLAPPEDWINDOW;
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT work = mi.rcWork;
    HWND hwnd = CreateWindowExW(0, L"LostIslandSettings", L"Lost Island Settings", style, work.left + 40,
                                work.top + 40, 200, 200, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return;
    S.hwnd = hwnd;
    ApplyDarkFrame(hwnd);
    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    S.scale = dpi / 96.f;
    RECT rc{0, 0, (LONG)std::lround(kStartW * S.scale), (LONG)std::lround(kStartH * S.scale)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int x = work.left + std::max(0L, ((work.right - work.left) - ww) / 2);
    int y = work.top + std::max(0L, ((work.bottom - work.top) - wh) / 2);
    SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    S.page = page >= 0 ? page : 0;
    S.scrollY = 0;
    S.city = g_config.weatherCity;
    S.claudeMsg.clear();
    S.startWithWindows = sys::StartWithWindows();
    S.claudeInstalled = claude::Installed();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

}  // namespace

namespace panel {

void Show(const wchar_t* page) {
    int i = PageIndex(page);
    if (S.hwnd) {
        if (i >= 0) SetPage(i);
        if (IsIconic(S.hwnd)) ShowWindow(S.hwnd, SW_RESTORE);
        SetForegroundWindow(S.hwnd);
        return;
    }
    Create(i);
}

void Close() {
    if (S.hwnd) DestroyWindow(S.hwnd);
}

bool IsOpen() { return S.hwnd != nullptr; }

}  // namespace panel
