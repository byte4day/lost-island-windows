// Wi-Fi name (WLAN API) and weather (wttr.in over WinHTTP), each on its own
// low-frequency worker thread.
#include <windows.h>
#include <objbase.h>
#include <wlanapi.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "config.h"
#include "json.h"
#include "services.h"

namespace {

struct Worker {
    std::thread t;
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    bool kick = false;

    template <class F>
    void Run(DWORD periodMs, F body) {
        t = std::thread([this, periodMs, body] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;) {
                body();
                std::unique_lock<std::mutex> lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(periodMs), [this] { return stop || kick; });
                if (stop) break;
                kick = false;
            }
            CoUninitialize();
        });
    }
    void Kick() {
        std::lock_guard<std::mutex> lock(mu);
        kick = true;
        cv.notify_all();
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu);
            stop = true;
            cv.notify_all();
        }
        if (t.joinable()) t.join();
    }
};

// ---------------------------------------------------------------- network
Worker g_netWorker;
std::mutex g_netMu;
std::wstring g_ssid;
HWND g_netNotify = nullptr;

std::wstring querySsid() {
    HANDLE h = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return {};
    std::wstring ssid;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < list->dwNumberOfItems && ssid.empty(); i++) {
            auto& itf = list->InterfaceInfo[i];
            if (itf.isState != wlan_interface_state_connected) continue;
            PWLAN_CONNECTION_ATTRIBUTES attr = nullptr;
            DWORD size = 0;
            if (WlanQueryInterface(h, &itf.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size,
                                   reinterpret_cast<PVOID*>(&attr), nullptr) == ERROR_SUCCESS) {
                auto& s = attr->wlanAssociationAttributes.dot11Ssid;
                std::string raw(reinterpret_cast<const char*>(s.ucSSID), s.uSSIDLength);
                ssid = json::FromUtf8(raw);
                WlanFreeMemory(attr);
            }
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(h, nullptr);
    return ssid;
}

// ---------------------------------------------------------------- weather
Worker g_wxWorker;
std::mutex g_wxMu;
WeatherInfo g_wx;
HWND g_wxNotify = nullptr;
std::wstring g_city;   // copied from g_config on the UI thread

bool httpGet(const std::wstring& host, const std::wstring& path, std::string& body) {
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"LostIsland/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"LostIsland/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 5000, 5000, 8000, 8000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < 512 * 1024) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got)) break;
            body.append(chunk.data(), got);
        }
        ok = status == 200;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

std::wstring urlEncode(const std::wstring& w) {
    std::string s = json::ToUtf8(w), out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ',') out += (char)ch;
        else if (ch == ' ') out += '+';
        else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
    }
    return json::FromUtf8(out);
}

// wttr.in uses WWO weather codes
std::wstring glyphFor(int code, bool night) {
    if (code == 113) return night ? L"night" : L"sun";
    if (code == 116 || code == 119 || code == 122) return L"cloud";
    if (code == 143 || code == 248 || code == 260) return L"fog";
    if (code == 200 || code == 386 || code == 389 || code == 392 || code == 395) return L"storm";
    if ((code >= 179 && code <= 182) || code == 227 || code == 230 || (code >= 323 && code <= 338) ||
        code == 368 || code == 371 || code == 374 || code == 377)
        return L"snow";
    return L"rain";
}

void fetchWeather() {
    std::wstring city;
    {
        std::lock_guard<std::mutex> lock(g_wxMu);
        city = g_city;
    }
    std::string body;
    if (!httpGet(L"wttr.in", L"/" + urlEncode(city) + L"?format=j1", body)) return;
    json::Value v;
    if (!json::Parse(body, v)) return;
    const json::Value& cur = v["current_condition"][0];
    if (!cur.isObject()) return;
    WeatherInfo w;
    w.valid = true;
    w.tempC = atoi(cur["temp_C"].asString().c_str());
    w.condition = cur["weatherDesc"][0]["value"].asWString();
    while (!w.condition.empty() && w.condition.back() == L' ') w.condition.pop_back();
    SYSTEMTIME st;
    GetLocalTime(&st);
    w.glyph = glyphFor(atoi(cur["weatherCode"].asString().c_str()), st.wHour < 6 || st.wHour >= 20);
    w.place = v["nearest_area"][0]["areaName"][0]["value"].asWString();
    {
        std::lock_guard<std::mutex> lock(g_wxMu);
        g_wx = w;
    }
    if (g_wxNotify) PostMessageW(g_wxNotify, WM_APP_SERVICE, SVC_WEATHER, 0);
}
}  // namespace

namespace network {
void Start(HWND notify) {
    g_netNotify = notify;
    g_netWorker.Run(6000, [] {
        std::wstring s = querySsid();
        bool changed;
        {
            std::lock_guard<std::mutex> lock(g_netMu);
            changed = s != g_ssid;
            g_ssid = s;
        }
        if (changed && g_netNotify) PostMessageW(g_netNotify, WM_APP_SERVICE, SVC_NETWORK, 0);
    });
}
void Stop() { g_netWorker.Stop(); }
std::wstring WifiName() {
    std::lock_guard<std::mutex> lock(g_netMu);
    return g_ssid;
}
bool Online() { return !WifiName().empty(); }
}  // namespace network

namespace weather {
void Start(HWND notify) {
    g_wxNotify = notify;
    g_city = g_config.weatherCity;
    g_wxWorker.Run(20 * 60 * 1000, [] { fetchWeather(); });
}
void Stop() { g_wxWorker.Stop(); }
WeatherInfo Get() {
    std::lock_guard<std::mutex> lock(g_wxMu);
    return g_wx;
}
void Refresh() {
    {
        std::lock_guard<std::mutex> lock(g_wxMu);
        g_city = g_config.weatherCity;
    }
    g_wxWorker.Kick();
}
}  // namespace weather
