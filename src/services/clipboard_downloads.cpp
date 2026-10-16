// Clipboard text history (memory only) and browser download progress.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "services.h"

// ---------------------------------------------------------------- clipboard
namespace {
std::vector<std::wstring> g_clips;
HWND g_clipHwnd = nullptr;
bool g_selfCopy = false;
}

namespace clipboard {
void Start(HWND listener) {
    g_clipHwnd = listener;
    AddClipboardFormatListener(listener);
}

void OnUpdate() {
    if (g_selfCopy) { g_selfCopy = false; return; }
    // skip content that password managers mark as private
    static UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static UINT noHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(exclude)) return;
    if (!OpenClipboard(g_clipHwnd)) return;
    bool allowed = true;
    if (IsClipboardFormatAvailable(noHistory)) {
        HANDLE h = GetClipboardData(noHistory);
        if (auto* p = static_cast<DWORD*>(GlobalLock(h))) { allowed = *p != 0; GlobalUnlock(h); }
    }
    std::wstring text;
    if (allowed && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text.assign(p, wcsnlen(p, 64 * 1024));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    bool blank = std::all_of(text.begin(), text.end(), [](wchar_t c) { return iswspace(c); });
    if (text.empty() || blank) return;
    g_clips.erase(std::remove(g_clips.begin(), g_clips.end(), text), g_clips.end());
    g_clips.insert(g_clips.begin(), text);
    if (g_clips.size() > 25) g_clips.resize(25);
    PostMessageW(g_clipHwnd, WM_APP_SERVICE, SVC_CLIPBOARD, 0);
}

std::vector<std::wstring> History() { return g_clips; }

void Copy(const std::wstring& text) {
    if (!OpenClipboard(g_clipHwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
    // move it to the top without counting it as a new copy
    g_clips.erase(std::remove(g_clips.begin(), g_clips.end(), text), g_clips.end());
    g_clips.insert(g_clips.begin(), text);
    g_selfCopy = true;
}

void Clear() { g_clips.clear(); }
}  // namespace clipboard

// ---------------------------------------------------------------- downloads
// Chromium writes "<name>.crdownload", Firefox "<name>.part". Watching the
// Downloads folder for those files gives the name and bytes received.
namespace {
HWND g_dlNotify = nullptr;
std::thread g_dlThread;
HANDLE g_dlStop = nullptr;
std::mutex g_dlMu;
std::vector<Transfer> g_transfers;

struct Seen { uint64_t bytes; ULONGLONG tick; double rate; };

std::wstring downloadsDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

bool endsWith(const std::wstring& s, const wchar_t* suf) {
    size_t n = wcslen(suf);
    return s.size() > n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

void scan(const std::wstring& dir, std::map<std::wstring, Seen>& seen) {
    std::vector<Transfer> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    ULONGLONG now = GetTickCount64();
    std::map<std::wstring, Seen> next;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            const wchar_t* suf = endsWith(name, L".crdownload") ? L".crdownload" : endsWith(name, L".part") ? L".part" : nullptr;
            if (!suf) continue;
            uint64_t bytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            Transfer t;
            t.name = name.substr(0, name.size() - wcslen(suf));
            if (t.name.rfind(L"Unconfirmed ", 0) == 0) t.name = L"Download";
            t.bytes = bytes;
            auto it = seen.find(name);
            Seen s{bytes, now, 0};
            if (it != seen.end()) {
                double dt = (now - it->second.tick) / 1000.0;
                double inst = dt > 0 ? (double)(bytes - std::min(bytes, it->second.bytes)) / dt : 0;
                s.rate = it->second.rate * 0.6 + inst * 0.4;
            }
            t.rate = s.rate;
            next[name] = s;
            // a Firefox .part of 0 bytes next to a finished file is not a transfer
            if (bytes > 0) out.push_back(t);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    seen.swap(next);
    bool changed;
    {
        std::lock_guard<std::mutex> lock(g_dlMu);
        changed = out.size() != g_transfers.size() || !out.empty();
        g_transfers = out;
    }
    if (changed && g_dlNotify) PostMessageW(g_dlNotify, WM_APP_SERVICE, SVC_DOWNLOADS, 0);
}
}  // namespace

namespace downloads {
void Start(HWND notify) {
    g_dlNotify = notify;
    g_dlStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_dlThread = std::thread([] {
        std::wstring dir = downloadsDir();
        if (dir.empty()) return;
        HANDLE change = FindFirstChangeNotificationW(
            dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        std::map<std::wstring, Seen> seen;
        scan(dir, seen);
        for (;;) {
            bool active;
            {
                std::lock_guard<std::mutex> lock(g_dlMu);
                active = !g_transfers.empty();
            }
            // while something downloads, refresh every second for the rate
            HANDLE waits[2] = {g_dlStop, change};
            DWORD r = WaitForMultipleObjects(change != INVALID_HANDLE_VALUE ? 2 : 1, waits, FALSE,
                                             active ? 1000 : (change != INVALID_HANDLE_VALUE ? INFINITE : 5000));
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) FindNextChangeNotification(change);
            scan(dir, seen);
        }
        if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);
    });
}
void Stop() {
    if (g_dlStop) SetEvent(g_dlStop);
    if (g_dlThread.joinable()) g_dlThread.join();
    if (g_dlStop) { CloseHandle(g_dlStop); g_dlStop = nullptr; }
}
std::vector<Transfer> Active() {
    std::lock_guard<std::mutex> lock(g_dlMu);
    return g_transfers;
}
}  // namespace downloads
