// Battery state from GetSystemPowerStatus, and laptop brightness via WMI
// (WmiMonitorBrightness), with DDC/CI as the fallback for external screens.
#include <windows.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "gfx.h"
#include "services.h"

namespace {
HWND g_notify = nullptr;
BatteryInfo g_batt;
std::mutex g_mu;

IWbemServices* wmi() {
    static IWbemServices* svc = nullptr;
    static bool tried = false;
    if (tried) return svc;
    tried = true;
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void**>(&loc))))
        return nullptr;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    if (SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)))
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    SysFreeString(ns);
    loc->Release();
    return svc;
}

int wmiBrightness() {
    IWbemServices* svc = wmi();
    if (!svc) return -1;
    IEnumWbemClassObject* en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
    HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !en) return -1;
    int result = -1;
    IWbemClassObject* obj = nullptr;
    ULONG got = 0;
    if (SUCCEEDED(en->Next(1000, 1, &obj, &got)) && got) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)))
            result = v.vt == VT_UI1 ? v.bVal : (v.vt == VT_I4 ? v.lVal : -1);
        VariantClear(&v);
        obj->Release();
    }
    en->Release();
    return result;
}

bool wmiSetBrightness(int pct) {
    IWbemServices* svc = wmi();
    if (!svc) return false;
    IEnumWbemClassObject* en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
    HRESULT hr = svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (FAILED(hr) || !en) return false;
    bool ok = false;
    IWbemClassObject* inst = nullptr;
    ULONG got = 0;
    if (SUCCEEDED(en->Next(1000, 1, &inst, &got)) && got) {
        VARIANT path;
        VariantInit(&path);
        inst->Get(L"__PATH", 0, &path, nullptr, nullptr);
        IWbemClassObject *cls = nullptr, *inDef = nullptr, *args = nullptr;
        BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
        BSTR method = SysAllocString(L"WmiSetBrightness");
        if (SUCCEEDED(svc->GetObject(clsName, 0, nullptr, &cls, nullptr)) &&
            SUCCEEDED(cls->GetMethod(method, 0, &inDef, nullptr)) && SUCCEEDED(inDef->SpawnInstance(0, &args))) {
            VARIANT t, b;
            VariantInit(&t);
            VariantInit(&b);
            t.vt = VT_I4; t.lVal = 0;
            b.vt = VT_UI1; b.bVal = (BYTE)pct;
            args->Put(L"Timeout", 0, &t, 0);
            args->Put(L"Brightness", 0, &b, 0);
            ok = SUCCEEDED(svc->ExecMethod(path.bstrVal, method, 0, nullptr, args, nullptr, nullptr));
        }
        SysFreeString(clsName);
        SysFreeString(method);
        SafeRelease(args);
        SafeRelease(inDef);
        SafeRelease(cls);
        VariantClear(&path);
        inst->Release();
    }
    en->Release();
    return ok;
}

// DDC/CI for an external monitor on the primary display
bool ddc(int* get, int set) {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || !n) return false;
    std::vector<PHYSICAL_MONITOR> mons(n);
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, mons.data())) return false;
    bool ok = false;
    DWORD mn = 0, cur = 0, mx = 0;
    if (GetMonitorBrightness(mons[0].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn) {
        if (get) *get = (int)((cur - mn) * 100 / (mx - mn));
        if (set >= 0) SetMonitorBrightness(mons[0].hPhysicalMonitor, mn + (DWORD)set * (mx - mn) / 100);
        ok = true;
    }
    DestroyPhysicalMonitors(n, mons.data());
    return ok;
}

int g_mode [[maybe_unused]] = -1;   // -1 unknown, 0 none, 1 wmi, 2 ddc
}  // namespace

namespace power {
void Start(HWND notify) {
    g_notify = notify;
    Refresh();
    brightness::Start();
}
void Refresh() {
    SYSTEM_POWER_STATUS s{};
    BatteryInfo b;
    if (GetSystemPowerStatus(&s)) {
        b.present = !(s.BatteryFlag & 128) && s.BatteryFlag != 255;
        b.pluggedIn = s.ACLineStatus == 1;
        b.charging = (s.BatteryFlag & 8) != 0 || (b.pluggedIn && s.BatteryLifePercent < 100);
        b.percent = s.BatteryLifePercent <= 100 ? s.BatteryLifePercent : -1;
    }
    bool changed;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        changed = b.present != g_batt.present || b.pluggedIn != g_batt.pluggedIn || b.percent != g_batt.percent ||
                  b.charging != g_batt.charging;
        g_batt = b;
    }
    if (changed && g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_POWER, 0);
}
BatteryInfo Get() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_batt;
}
}  // namespace power

// WMI and DDC/CI calls can take hundreds of milliseconds (or hang on
// drivers that don't answer), so they run on a worker and the UI only reads
// cached values.
namespace {
std::thread g_brThread;
HANDLE g_brWake = nullptr;
std::atomic<bool> g_brStop{false}, g_brRead{true};
std::atomic<int> g_brValue{-1}, g_brWant{-1};
std::atomic<int> g_brMode{-1};

void brightnessWorker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int mode = wmiBrightness() >= 0 ? 1 : (ddc(nullptr, -1) ? 2 : 0);
    g_mode = mode;
    g_brMode = mode;
    while (!g_brStop) {
        int want = g_brWant.exchange(-1);
        if (mode && want >= 0) {
            if (mode == 1) wmiSetBrightness(want);
            else ddc(nullptr, want);
            g_brValue = want;
        }
        if (mode && g_brRead.exchange(false)) {
            int v = -1;
            if (mode == 1) v = wmiBrightness();
            else ddc(&v, -1);
            if (v != g_brValue.load()) {
                g_brValue = v;
                if (g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_POWER, 0);
            }
        }
        WaitForSingleObject(g_brWake, INFINITE);
    }
    CoUninitialize();
}
}  // namespace

namespace brightness {
void Start() {
    if (g_brThread.joinable()) return;
    g_brWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_brThread = std::thread(brightnessWorker);
}
void Stop() {
    if (!g_brThread.joinable()) return;
    g_brStop = true;
    SetEvent(g_brWake);
    // a hung WMI call must not block exit
    if (WaitForSingleObject(g_brThread.native_handle(), 1500) == WAIT_OBJECT_0) g_brThread.join();
    else g_brThread.detach();
}
bool Available() { return g_brMode.load() > 0; }
int Get() { return g_brValue.load(); }
void Refresh() {
    g_brRead = true;
    if (g_brWake) SetEvent(g_brWake);
}
void Set(int pct) {
    g_brWant = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    g_brValue = g_brWant.load();
    if (g_brWake) SetEvent(g_brWake);
}
}  // namespace brightness
