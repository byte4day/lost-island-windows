// Wallpaper library for the card's picker: list a folder, make thumbnails on
// a worker thread, set the desktop wallpaper. The idea comes from Tide
// Island's wallpaper switcher; this is a fresh implementation for Windows.
//
// Thumbnails are decoded with their own WIC factory on one background thread
// (the main thread's factory lives in its STA) into 32bpp premultiplied BGRA
// at kThumbW x kThumbH, centre-cropped. They live in memory only while the
// picker is open; Release() drops them.
#include "services.h"
#include "config.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>

namespace wallpaper {
namespace {

HWND g_notify = nullptr;
std::mutex g_mu;
std::vector<std::wstring> g_queue;
std::map<std::wstring, Thumb> g_done;       // path -> pixels (w == 0: failed)
bool g_running = false;
std::atomic<unsigned> g_gen{0};             // bumps on Release(), drops stale results

bool hasImageExt(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& ch : ext) ch = (wchar_t)towlower(ch);
    static const wchar_t* exts[] = {L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff",
                                    L".webp", L".heic", L".avif", L".jxr", L".jfif"};
    for (auto e : exts) if (ext == e) return true;
    return false;
}

std::wstring lower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

bool isDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

Thumb decode(IWICImagingFactory* wic, const std::wstring& path) {
    Thumb t;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT fw = 0, fh = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&fw, &fh)) && fw && fh) {
        // scale to cover the thumbnail box, then crop the middle
        double s = std::max((double)kThumbW / fw, (double)kThumbH / fh);
        UINT sw = std::max(kThumbW, (UINT)(fw * s + 0.5)), sh = std::max(kThumbH, (UINT)(fh * s + 0.5));
        WICRect r{(INT)(sw - kThumbW) / 2, (INT)(sh - kThumbH) / 2, (INT)kThumbW, (INT)kThumbH};
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, sw, sh, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateBitmapClipper(&clip)) && SUCCEEDED(clip->Initialize(scaler, &r)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(clip, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            t.pixels.resize((size_t)kThumbW * kThumbH * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, kThumbW * 4, (UINT)t.pixels.size(), t.pixels.data()))) {
                t.w = kThumbW;
                t.h = kThumbH;
            } else t.pixels.clear();
        }
    }
    if (conv) conv->Release();
    if (clip) clip->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return t;
}

void worker(unsigned gen) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_queue.empty() || !wic || gen != g_gen) { g_running = false; break; }
            path = g_queue.front();
            g_queue.erase(g_queue.begin());
        }
        Thumb t = decode(wic, path);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (gen != g_gen) { g_running = false; break; }
            g_done[path] = std::move(t);
        }
        if (g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_WALLPAPER, 0);
    }
    if (wic) wic->Release();
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace

void Start(HWND notify) { g_notify = notify; }

std::wstring Folder() {
    if (!g_config.wallpaperFolder.empty()) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = ExpandEnvironmentStringsW(g_config.wallpaperFolder.c_str(), buf, MAX_PATH * 2);
        return n && n < MAX_PATH * 2 ? std::wstring(buf) : g_config.wallpaperFolder;
    }
    std::wstring pics = knownFolder(FOLDERID_Pictures);
    for (const wchar_t* sub : {L"\\Wallpapers", L"\\Wallpaper"})
        if (isDir(pics + sub)) return pics + sub;
    // Windows' own wallpapers as a last resort, so the picker is never empty
    std::wstring win = knownFolder(FOLDERID_Windows) + L"\\Web\\Wallpaper\\Windows";
    if (!isDir(pics + L"\\Wallpapers") && isDir(win)) return win;
    return pics;
}

std::vector<std::wstring> List() {
    std::vector<std::wstring> out;
    std::wstring dir = Folder();
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (hasImageExt(fd.cFileName)) out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return lower(Name(a)) < lower(Name(b));
    });
    return out;
}

std::wstring Name(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

bool Matches(const std::wstring& path, const std::wstring& query) {
    if (query.empty()) return true;
    return lower(Name(path)).find(lower(query)) != std::wstring::npos;
}

std::wstring Current() {
    std::wstring out;
    IDesktopWallpaper* dw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) {
        LPWSTR p = nullptr;
        if (SUCCEEDED(dw->GetWallpaper(nullptr, &p)) && p) out = p;   // fails with mixed monitors
        if (out.empty()) {
            UINT n = 0;
            dw->GetMonitorDevicePathCount(&n);
            for (UINT i = 0; i < n && out.empty(); i++) {
                LPWSTR mon = nullptr;
                if (SUCCEEDED(dw->GetMonitorDevicePathAt(i, &mon)) && mon) {
                    LPWSTR q = nullptr;
                    if (SUCCEEDED(dw->GetWallpaper(mon, &q)) && q) out = q;
                    CoTaskMemFree(q);
                }
                CoTaskMemFree(mon);
            }
        }
        CoTaskMemFree(p);
        dw->Release();
    }
    if (out.empty()) {
        wchar_t buf[MAX_PATH] = {};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buf, 0)) out = buf;
    }
    return out;
}

const Thumb* Get(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_done.find(path);
    if (it != g_done.end()) return &it->second;
    if (std::find(g_queue.begin(), g_queue.end(), path) == g_queue.end()) g_queue.push_back(path);
    if (!g_running) {
        g_running = true;
        std::thread(worker, (unsigned)g_gen).detach();
    }
    return nullptr;
}

void Prioritise(const std::vector<std::wstring>& first) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<std::wstring> ahead, rest;
    for (auto& p : g_queue)
        (std::find(first.begin(), first.end(), p) != first.end() ? ahead : rest).push_back(p);
    ahead.insert(ahead.end(), rest.begin(), rest.end());
    g_queue.swap(ahead);
}

void Release() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_gen++;
    g_queue.clear();
    g_done.clear();
    g_running = false;   // a running worker sees the new generation and exits
}

bool Set(const std::wstring& path, std::wstring& error) {
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    const std::wstring& fit = g_config.wallpaperFit;
    if (fit == L"fit") pos = DWPOS_FIT;
    else if (fit == L"stretch") pos = DWPOS_STRETCH;
    else if (fit == L"center") pos = DWPOS_CENTER;
    else if (fit == L"tile") pos = DWPOS_TILE;
    else if (fit == L"span") pos = DWPOS_SPAN;
    IDesktopWallpaper* dw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw));
    if (SUCCEEDED(hr)) {
        dw->Enable(TRUE);              // leave slideshow / solid color mode
        dw->SetPosition(pos);
        hr = dw->SetWallpaper(nullptr, path.c_str());   // all monitors, with Windows' own fade
        dw->Release();
        if (SUCCEEDED(hr)) return true;
    }
    // older shells and Wine: the classic call
    static const wchar_t* styles[][2] = {{L"10", L"0"}, {L"6", L"0"}, {L"2", L"0"}, {L"0", L"0"}, {L"0", L"1"}, {L"22", L"0"}};
    int si = pos == DWPOS_FIT ? 1 : pos == DWPOS_STRETCH ? 2 : pos == DWPOS_CENTER ? 3 : pos == DWPOS_TILE ? 4
           : pos == DWPOS_SPAN ? 5 : 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"WallpaperStyle", 0, REG_SZ, (const BYTE*)styles[si][0], (DWORD)(wcslen(styles[si][0]) + 1) * 2);
        RegSetValueExW(k, L"TileWallpaper", 0, REG_SZ, (const BYTE*)styles[si][1], 4);
        RegCloseKey(k);
    }
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)path.c_str(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        return true;
    wchar_t buf[160];
    swprintf(buf, 160, L"Windows refused it (0x%08lX)", (unsigned long)(FAILED(hr) ? hr : HRESULT_FROM_WIN32(GetLastError())));
    error = buf;
    return false;
}

}  // namespace wallpaper
