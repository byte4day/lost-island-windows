// System actions: theme, launching, power, screenshots, color picker, autostart.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <powrprof.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>

#include "gfx.h"
#include "services.h"

namespace {
const wchar_t* kPersonalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
const wchar_t* kRun = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

bool enableShutdownPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(tok);
    return ok && GetLastError() == ERROR_SUCCESS;
}

std::wstring findOnPath(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr);
    return n && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

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

HBITMAP captureScreen(RECT& area) {
    area.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    area.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    area.right = area.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    area.bottom = area.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int w = area.right - area.left, h = area.bottom - area.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, area.left, area.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return bmp;
}

bool savePng(HBITMAP bmp, const std::wstring& path) {
    IWICImagingFactory* wic = g_gfx.wic;
    if (!wic) return false;
    IWICBitmap* wb = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &wb)) &&
        SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr))) {
        UINT w, h;
        wb->GetSize(&w, &h);
        frame->SetSize(w, h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        frame->SetPixelFormat(&fmt);
        ok = SUCCEEDED(frame->WriteSource(wb, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    SafeRelease(frame);
    SafeRelease(enc);
    SafeRelease(st);
    SafeRelease(wb);
    return ok;
}

void bitmapToClipboard(HBITMAP bmp) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    // the clipboard takes ownership of a copy
    HBITMAP copy = (HBITMAP)CopyImage(bmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    if (copy) SetClipboardData(CF_BITMAP, copy);
    CloseClipboard();
}

// ---- color picker overlay
struct Picker {
    HBITMAP shot = nullptr;
    HDC shotDC = nullptr;
    RECT area{};
    POINT cur{};
    bool done = false;
    COLORREF picked = CLR_INVALID;
};
Picker* g_pick = nullptr;

LRESULT CALLBACK PickerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Picker* p = g_pick;
    switch (m) {
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, IDC_CROSS)); return TRUE;
    case WM_MOUSEMOVE:
        p->cur = POINT{(short)LOWORD(l), (short)HIWORD(l)};
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        p->picked = GetPixel(p->shotDC, (short)LOWORD(l), (short)HIWORD(l));
        p->done = true;
        return 0;
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
        if (m == WM_RBUTTONDOWN || w == VK_ESCAPE) p->done = true;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int w2 = p->area.right - p->area.left, h2 = p->area.bottom - p->area.top;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, w2, h2);
        HGDIOBJ old = SelectObject(mem, buf);
        BitBlt(mem, 0, 0, w2, h2, p->shotDC, 0, 0, SRCCOPY);
        // magnifier: 11x11 pixels at 10x, in a ring next to the pointer
        int z = 10, n = 11, size = z * n;
        int mx = p->cur.x + 24, my = p->cur.y + 24;
        if (mx + size > w2) mx = p->cur.x - 24 - size;
        if (my + size + 28 > h2) my = p->cur.y - 24 - size - 28;
        HRGN rgn = CreateEllipticRgn(mx, my, mx + size, my + size);
        SelectClipRgn(mem, rgn);
        StretchBlt(mem, mx, my, size, size, p->shotDC, p->cur.x - n / 2, p->cur.y - n / 2, n, n, SRCCOPY);
        SelectClipRgn(mem, nullptr);
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
        FrameRgn(mem, rgn, white, 2, 2);
        DeleteObject(white);
        DeleteObject(rgn);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(mem, pen);
        HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, mx + (n / 2) * z, my + (n / 2) * z, mx + (n / 2 + 1) * z, my + (n / 2 + 1) * z);
        SelectObject(mem, ob);
        SelectObject(mem, op);
        DeleteObject(pen);
        COLORREF c = GetPixel(p->shotDC, p->cur.x, p->cur.y);
        wchar_t hex[16];
        swprintf(hex, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        RECT label{mx, my + size + 6, mx + size, my + size + 28};
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 30));
        FillRect(mem, &label, bg);
        DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(245, 245, 247));
        DrawTextW(mem, hex, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        BitBlt(dc, 0, 0, w2, h2, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(buf);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

namespace sys {

bool DarkMode() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kPersonalize, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

void SetDarkMode(bool dark) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPersonalize, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = dark ? 0 : 1;
    RegSetValueExW(k, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<BYTE*>(&v), sizeof(v));
    RegSetValueExW(k, L"SystemUsesLightTheme", 0, REG_DWORD, reinterpret_cast<BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
    DWORD_PTR res;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
                        SMTO_ABORTIFHUNG, 200, &res);
}

bool NightLightSupported() { return true; }

void OpenUri(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

void Launch(const std::wstring& exe, const std::wstring& args) {
    ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
}

void LaunchInTerminal(const std::wstring& command) {
    std::wstring home = knownFolder(FOLDERID_Profile);
    std::wstring wt = findOnPath(L"wt.exe");
    if (!wt.empty()) {
        std::wstring args = L"-w new -d \"" + home + L"\"";
        if (!command.empty()) args += L" cmd /k " + command;
        ShellExecuteW(nullptr, L"open", wt.c_str(), args.c_str(), home.c_str(), SW_SHOWNORMAL);
        return;
    }
    std::wstring cmdline = L"cmd.exe";
    if (!command.empty()) cmdline += L" /k " + command;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, home.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void Lock() { LockWorkStation(); }
void SignOut() { ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED); }
void Sleep() { SetSuspendState(FALSE, FALSE, FALSE); }
void Restart() {
    enableShutdownPrivilege();
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_FLAG_PLANNED);
}
void Shutdown() {
    enableShutdownPrivilege();
    ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_FLAG_PLANNED);
}

bool Screenshot(bool toClipboard, std::wstring& savedPath) {
    RECT area;
    HBITMAP bmp = captureScreen(area);
    if (!bmp) return false;
    std::wstring dir = knownFolder(FOLDERID_Pictures) + L"\\Screenshots";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96];
    swprintf(name, 96, L"\\Screenshot %04d-%02d-%02d %02d%02d%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    savedPath = dir + name;
    bool ok = savePng(bmp, savedPath);
    if (toClipboard) bitmapToClipboard(bmp);
    DeleteObject(bmp);
    return ok;
}

void SnipArea() { OpenUri(L"ms-screenclip:"); }

// Snipping Tool's recorder: Win+Shift+R on current Windows 11 builds.
void ToggleRecording() { sendKeys({VK_LWIN, VK_SHIFT, 'R'}); }

std::wstring PickColor() {
    Picker p;
    p.shot = captureScreen(p.area);
    HDC screen = GetDC(nullptr);
    p.shotDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(p.shotDC, p.shot);
    g_pick = &p;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LostIslandPicker";
        RegisterClassW(&wc);
        registered = true;
    }
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"LostIslandPicker", L"", WS_POPUP, p.area.left,
                             p.area.top, p.area.right - p.area.left, p.area.bottom - p.area.top, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    GetCursorPos(&p.cur);
    p.cur.x -= p.area.left;
    p.cur.y -= p.area.top;
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetCapture(h);
    MSG msg;
    while (!p.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { p.done = true; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ReleaseCapture();
    DestroyWindow(h);
    g_pick = nullptr;
    SelectObject(p.shotDC, old);
    DeleteDC(p.shotDC);
    DeleteObject(p.shot);
    if (p.picked == CLR_INVALID) return {};
    wchar_t hex[16];
    swprintf(hex, 16, L"#%02X%02X%02X", GetRValue(p.picked), GetGValue(p.picked), GetBValue(p.picked));
    return hex;
}

bool StartWithWindows() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRun, L"LostIsland", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetStartWithWindows(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRun, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, L"LostIsland", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"LostIsland");
    }
    RegCloseKey(k);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

std::wstring ConfigDir() {
    std::wstring d = knownFolder(FOLDERID_RoamingAppData) + L"\\LostIsland";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring CacheDir() {
    std::wstring d = knownFolder(FOLDERID_LocalAppData) + L"\\LostIsland";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

}  // namespace sys
