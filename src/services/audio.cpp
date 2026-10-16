// Master volume and mic mute through Core Audio (IAudioEndpointVolume).
// Volume changes from anywhere (keys, flyout, apps) arrive through the
// endpoint callback and post SVC_AUDIO, which drives the volume peek.
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <mutex>

#include "gfx.h"

// mingw only forward-declares IAudioMeterInformation; this matches the SDK's endpointvolume.h.
#ifdef _MSC_VER
struct __declspec(uuid("C02216F6-8C67-4B5B-9D00-D008E73E0064")) IMeter : public IUnknown {
#else
struct IMeter : public IUnknown {
#endif
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};
#ifndef _MSC_VER
__CRT_UUID_DECL(IMeter, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64)
#endif
#include "services.h"

namespace {
HWND g_notify = nullptr;
IMMDeviceEnumerator* g_enum = nullptr;
IAudioEndpointVolume* g_out = nullptr;
IAudioEndpointVolume* g_in = nullptr;
IMeter* g_meter = nullptr;
std::wstring g_outName;
std::mutex g_mu;

class VolumeCallback final : public IAudioEndpointVolumeCallback {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        if (g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_AUDIO, 0);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
};

class DeviceClient final : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) override {
        // rebinding must happen on the UI thread; lParam=1 asks for it
        if (role == eConsole && g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_AUDIO, 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> ref_{1};
};

VolumeCallback* g_cb = nullptr;
DeviceClient* g_dev = nullptr;

IAudioEndpointVolume* bind(EDataFlow flow, std::wstring* name) {
    IMMDevice* dev = nullptr;
    IAudioEndpointVolume* vol = nullptr;
    if (!g_enum || FAILED(g_enum->GetDefaultAudioEndpoint(flow, eConsole, &dev))) return nullptr;
    dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&vol));
    if (name) {
        name->clear();
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) *name = pv.pwszVal;
            PropVariantClear(&pv);
            props->Release();
        }
    }
    dev->Release();
    return vol;
}

void rebind() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_out) {
        g_out->UnregisterControlChangeNotify(g_cb);
        SafeRelease(g_out);
    }
    if (g_in) {
        g_in->UnregisterControlChangeNotify(g_cb);
        SafeRelease(g_in);
    }
    SafeRelease(g_meter);
    g_out = bind(eRender, &g_outName);
    {
        IMMDevice* dev = nullptr;
        if (g_enum && SUCCEEDED(g_enum->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
            dev->Activate(__uuidof(IMeter), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(&g_meter));
            dev->Release();
        }
    }
    g_in = bind(eCapture, nullptr);
    if (g_out) g_out->RegisterControlChangeNotify(g_cb);
    if (g_in) g_in->RegisterControlChangeNotify(g_cb);
}
}  // namespace

namespace audio {

void Start(HWND notify) {
    g_notify = notify;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&g_enum))))
        return;
    g_cb = new VolumeCallback();
    g_dev = new DeviceClient();
    g_enum->RegisterEndpointNotificationCallback(g_dev);
    rebind();
}

void Rebind() { rebind(); }

void Stop() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_out) { g_out->UnregisterControlChangeNotify(g_cb); SafeRelease(g_out); }
        if (g_in) { g_in->UnregisterControlChangeNotify(g_cb); SafeRelease(g_in); }
    }
    SafeRelease(g_meter);
    if (g_enum && g_dev) g_enum->UnregisterEndpointNotificationCallback(g_dev);
    SafeRelease(g_enum);
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    g_notify = nullptr;
}

float Volume() {
    std::lock_guard<std::mutex> lock(g_mu);
    float v = 0;
    if (g_out) g_out->GetMasterVolumeLevelScalar(&v);
    return v;
}
bool Muted() {
    std::lock_guard<std::mutex> lock(g_mu);
    BOOL m = FALSE;
    if (g_out) g_out->GetMute(&m);
    return m != FALSE;
}
void SetVolume(float v) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_out) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    g_out->SetMasterVolumeLevelScalar(v, nullptr);
    if (v > 0) g_out->SetMute(FALSE, nullptr);
}
void SetMuted(bool m) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_out) g_out->SetMute(m, nullptr);
}
bool HasMic() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_in != nullptr;
}
bool MicMuted() {
    std::lock_guard<std::mutex> lock(g_mu);
    BOOL m = FALSE;
    if (g_in) g_in->GetMute(&m);
    return m != FALSE;
}
void SetMicMuted(bool m) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_in) g_in->SetMute(m, nullptr);
}
float Peak() {
    std::lock_guard<std::mutex> lock(g_mu);
    float p = 0;
    if (g_meter) g_meter->GetPeakValue(&p);
    return p;
}

std::wstring OutputName() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_outName;
}

}  // namespace audio
