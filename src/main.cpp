// Lost Island for Windows: entry point, single instance, command line.
//
//   LostIsland.exe                     start (or open Settings if already running)
//   LostIsland.exe --toggle            open or close the card
//   LostIsland.exe --wallpapers        open the wallpaper picker
//   LostIsland.exe --timer             start or pause the focus timer
//   LostIsland.exe --settings [page]   open the control panel
//   LostIsland.exe --quit              quit the running island
//   LostIsland.exe --setup-claude-status / --remove-claude-status
//   LostIsland.exe --claude-hook       (run by Claude Code, reads JSON on stdin)
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <cstdio>
#include <string>

#include "app.h"
#include "config.h"
#include "gfx.h"
#include "island.h"
#include "services.h"

namespace {
HINSTANCE g_inst = nullptr;

void printConsole(const std::wstring& text) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        MessageBoxW(nullptr, text.c_str(), L"Lost Island", MB_OK | MB_ICONINFORMATION);
        return;
    }
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    std::wstring line = L"\r\n" + text + L"\r\n";
    DWORD written;
    if (!WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &written, nullptr)) {
        // redirected output: write UTF-8
        int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), s.data(), n, nullptr, nullptr);
        WriteFile(out, s.data(), (DWORD)s.size(), &written, nullptr);
    }
    FreeConsole();
}

bool sendToRunning(WPARAM cmd) {
    HWND h = FindWindowW(L"LostIslandWindow", nullptr);
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    AllowSetForegroundWindow(pid);
    PostMessageW(h, WM_APP_CMD, cmd, 0);
    return true;
}
}  // namespace

namespace app {
HINSTANCE Instance() { return g_inst; }

float DpiScaleFor(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    return (dpi ? dpi : 96) / 96.f;
}

void ConfigChanged() {
    g_config.Save();
    weather::Refresh();
    island::ApplyConfig();
}

void OpenSettings(const wchar_t* page) { panel::Show(page); }
void ToggleCard() { island::ToggleCard(); }
void ToggleTimer() { island::ToggleTimer(); }
void Quit() {
    panel::Close();
    PostQuitMessage(0);
}
}  // namespace app

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    std::wstring arg2 = argc > 2 ? argv[2] : L"";
    LocalFree(argv);

    if (cmd == L"--claude-hook") return claude::RunHook();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (cmd == L"--setup-claude-status" || cmd == L"--remove-claude-status") {
        std::wstring msg;
        bool ok = cmd == L"--setup-claude-status" ? claude::Setup(msg) : claude::Remove(msg);
        printConsole(msg);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (cmd == L"--version") {
        printConsole(L"Lost Island " LI_VERSION);
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\byte4day.LostIsland");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        WPARAM c = CMD_SETTINGS;
        if (cmd == L"--toggle") c = CMD_TOGGLE;
        else if (cmd == L"--timer") c = CMD_TIMER;
        else if (cmd == L"--quit") c = CMD_QUIT;
        else if (cmd == L"--open") c = CMD_OPEN;
        else if (cmd == L"--close") c = CMD_CLOSE;
        else if (cmd == L"--wallpapers") c = CMD_WALLPAPERS;
        sendToRunning(c);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (cmd == L"--quit") return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!g_gfx.Init()) {
        MessageBoxW(nullptr, L"Direct2D is not available on this system.", L"Lost Island", MB_ICONERROR);
        return 1;
    }
    bool firstRun = GetFileAttributesW(Config::Path().c_str()) == INVALID_FILE_ATTRIBUTES;
    g_config.Load();
    if (firstRun) g_config.Save();

    if (!island::Create(inst)) {
        MessageBoxW(nullptr, L"Could not create the island window.", L"Lost Island", MB_ICONERROR);
        return 1;
    }
    if (firstRun || cmd == L"--settings") panel::Show(arg2.empty() ? L"general" : arg2.c_str());
    if (GetEnvironmentVariableW(L"LOSTISLAND_DEMO", nullptr, 0) > 0) island::SetDemo(true);
    if (cmd == L"--debug-state" && !arg2.empty()) {
        static std::wstring state = arg2;
        SetTimer(nullptr, 0, 1200, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            island::DebugState(state);
        });
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    island::Destroy();
    g_gfx.Shutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
