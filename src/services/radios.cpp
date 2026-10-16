// Lost Island for Windows: Wi-Fi and Bluetooth on/off through
// Windows.Devices.Radios, using the raw WinRT ABI from winrt_abi.h
// (llvm-mingw's windows.devices.radios.h pulls in a windows.foundation.h
// that does not compile as C++).
//
// One MTA worker thread owns every WinRT object: it asks for radio access
// once, enumerates the radios, re-reads their state every 3 s and applies
// Set() requests. It posts WM_APP_SERVICE/SVC_RADIOS when a state changes
// and after every Set().
#include "services.h"
#include "winrt_abi.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace radios {
namespace {

using namespace lirt;

constexpr DWORD kPollMs = 3000;
constexpr DWORD kAsyncTimeoutMs = 10000;
constexpr ULONGLONG kEnumerateRetryMs = 30000;

struct Request {
    RadioKind kind;
    bool on;
};

struct Slot {
    std::atomic<bool> available{false};
    std::atomic<bool> on{false};
};
Slot g_slots[2];   // indexed by Index(RadioKind)

std::mutex g_cmdMu;
std::deque<Request> g_cmds;

HANDLE g_stop = nullptr;   // manual reset
HANDLE g_wake = nullptr;   // auto reset, signalled when a request is queued
HWND g_notify = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};

int Index(RadioKind kind) { return kind == RadioKind::WiFi ? 0 : 1; }

void Notify() {
    if (g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_RADIOS, 0);
}

class Worker {
public:
    void Run();

private:
    void Enumerate();
    bool ReadStates();   // true when anything visible changed
    void Apply(const Request& req);

    Ptr<IRadioStatics> statics_;
    Ptr<IRadio> radios_[2];
    bool accessRequested_ = false;
    ULONGLONG nextEnumerate_ = 0;
};

void Worker::Enumerate() {
    nextEnumerate_ = GetTickCount64() + kEnumerateRetryMs;
    if (!statics_ && FAILED(GetStatics(RuntimeClass_Radio, IID_IRadioStatics, statics_))) {
        statics_.Reset();
        return;
    }
    if (!accessRequested_) {
        accessRequested_ = true;
        Ptr<IAsyncOperation<INT32>> op;
        INT32 status = RadioAccess_Unspecified;
        if (SUCCEEDED(statics_->RequestAccessAsync(op.Put())) && op)
            AwaitResult(op.Get(), kAsyncTimeoutMs, g_stop, &status);
        // Denied access still lets us read states; SetStateAsync reports it.
    }
    Ptr<IAsyncOperation<IVectorView<IRadio*>*>> op;
    Ptr<IVectorView<IRadio*>> list;
    if (FAILED(statics_->GetRadiosAsync(op.Put())) || !op) return;
    if (!AwaitResult(op.Get(), kAsyncTimeoutMs, g_stop, list.Put()) || !list) return;

    Ptr<IRadio> found[2];
    UINT32 count = 0;
    if (FAILED(list->get_Size(&count))) return;
    for (UINT32 i = 0; i < count; ++i) {
        Ptr<IRadio> radio;
        if (FAILED(list->GetAt(i, radio.Put())) || !radio) continue;
        INT32 kind = RadioKind_Other;
        if (FAILED(radio->get_Kind(&kind))) continue;
        int slot = kind == RadioKind_WiFi ? 0 : kind == RadioKind_Bluetooth ? 1 : -1;
        if (slot >= 0 && !found[slot]) found[slot] = std::move(radio);
    }
    for (int k = 0; k < 2; ++k) radios_[k] = std::move(found[k]);
}

bool Worker::ReadStates() {
    if ((!radios_[0] || !radios_[1]) && GetTickCount64() >= nextEnumerate_) Enumerate();
    bool changed = false;
    for (int k = 0; k < 2; ++k) {
        bool available = false, on = false;
        if (radios_[k]) {
            INT32 state = RadioState_Unknown;
            if (SUCCEEDED(radios_[k]->get_State(&state))) {
                available = true;
                on = state == RadioState_On;
            } else {
                radios_[k].Reset();   // device went away; re-enumerate soon
                nextEnumerate_ = 0;
            }
        }
        if (g_slots[k].available.exchange(available) != available) changed = true;
        if (g_slots[k].on.exchange(on) != on) changed = true;
    }
    return changed;
}

void Worker::Apply(const Request& req) {
    int k = Index(req.kind);
    if (!radios_[k]) Enumerate();
    if (radios_[k]) {
        Ptr<IAsyncOperation<INT32>> op;
        INT32 status = RadioAccess_Unspecified;
        if (SUCCEEDED(radios_[k]->SetStateAsync(req.on ? RadioState_On : RadioState_Off, op.Put())) && op)
            AwaitResult(op.Get(), kAsyncTimeoutMs, g_stop, &status);
    }
    ReadStates();
    Notify();   // always: Set() promises a post when done
}

void Worker::Run() {
    Enumerate();
    if (ReadStates()) Notify();
    HANDLE handles[2] = {g_stop, g_wake};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, kPollMs);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            for (;;) {
                Request req;
                {
                    std::lock_guard<std::mutex> lock(g_cmdMu);
                    if (g_cmds.empty()) break;
                    req = g_cmds.front();
                    g_cmds.pop_front();
                }
                Apply(req);
                if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) return;
            }
            continue;
        }
        if (ReadStates()) Notify();
    }
}

void ThreadMain() {
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    {
        Worker worker;   // releases every WinRT object before RoUninitialize
        worker.Run();
    }
    if (SUCCEEDED(hr)) RoUninitialize();
}

} // namespace

void Start(HWND notify) {
    if (g_running) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stop || !g_wake) {
        if (g_stop) CloseHandle(g_stop);
        if (g_wake) CloseHandle(g_wake);
        g_stop = g_wake = nullptr;
        return;
    }
    g_notify = notify;
    g_running = true;
    g_thread = std::thread(ThreadMain);
}

void Stop() {
    if (!g_running) return;
    SetEvent(g_stop);
    if (g_thread.joinable()) g_thread.join();
    CloseHandle(g_stop);
    CloseHandle(g_wake);
    g_stop = g_wake = nullptr;
    g_running = false;
    g_notify = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cmdMu);
        g_cmds.clear();
    }
    for (Slot& s : g_slots) {
        s.available = false;
        s.on = false;
    }
}

bool Available(RadioKind kind) { return g_slots[Index(kind)].available; }

bool IsOn(RadioKind kind) { return g_slots[Index(kind)].on; }

void Set(RadioKind kind, bool on) {
    if (!g_running) return;
    {
        std::lock_guard<std::mutex> lock(g_cmdMu);
        g_cmds.push_back(Request{kind, on});
    }
    SetEvent(g_wake);
}

} // namespace radios
