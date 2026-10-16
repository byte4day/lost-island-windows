// Lost Island for Windows: hand-written WinRT ABI declarations.
//
// llvm-mingw ships no windows.media.control.h, and its C++ windows.foundation.h
// does not compile (IReference<boolean> collides with IReference<BYTE>), so the
// few interfaces the media and radios services need are declared here as raw
// COM-style vtables.
//
// Provenance (do not edit from memory):
//   * Interface GUIDs and method order were read from the ECMA-335 metadata in
//     Windows.Foundation.UniversalApiContract.winmd and
//     Windows.Foundation.FoundationContract.winmd (NuGet package
//     Microsoft.Windows.SDK.Contracts 10.0.28000.2705).
//   * Parametrized IIDs were computed with the WinRT pinterface algorithm
//     (SHA-1 over namespace 11f47ad5-7b73-42c0-abae-878b1e16adee + UTF-8
//     signature, version 5). The algorithm was checked against the known
//     IVectorView<String> IID 2f13c006-a03a-5f69-b090-75a43e33423e, and every
//     value that also appears in llvm-mingw's generated headers matches.
//
// Every interface derives from IInspectable, so each vtable starts with
// QueryInterface, AddRef, Release, GetIids, GetRuntimeClassName, GetTrustLevel.
// WinRT ABI types used below: boolean = BYTE, enum = INT32, TimeSpan/DateTime/
// EventRegistrationToken = one INT64, String = HSTRING.
#pragma once
#include <windows.h>
#include <inspectable.h>
#include <objidl.h>
#include <roapi.h>
#include <winstring.h>
#include <string>

// shcore.dll (Windows 8+). Declared here so this does not depend on the
// NTDDI_VERSION that <shcore.h> requires.
extern "C" HRESULT STDAPICALLTYPE CreateStreamOverRandomAccessStream(IUnknown* randomAccessStream,
                                                                      REFIID riid, void** ppv);

namespace lirt {

// ------------------------------------------------------------ Windows.Foundation
struct TimeSpan { INT64 Duration; };      // 100 ns units
struct DateTime { INT64 UniversalTime; }; // 100 ns units since 1601-01-01 UTC
struct EventRegistrationToken { INT64 value; };

enum AsyncStatus : INT32 { AsyncStatus_Canceled = 2, AsyncStatus_Completed = 1,
                           AsyncStatus_Error = 3, AsyncStatus_Started = 0 };

// Windows.Foundation.IAsyncInfo  {00000036-0000-0000-c000-000000000046}
inline constexpr GUID IID_IAsyncInfo =
    {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
struct IAsyncInfo : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(INT32* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* errorCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// Windows.Foundation.IAsyncOperation`1  generic {9fc2b0bb-e446-44e2-aa61-9cab8f636af2}
// Same vtable for every T; T is the ABI result type.
template <class T>
struct IAsyncOperation : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;   // AsyncOperationCompletedHandler<T>
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(T* results) = 0;
};

// Windows.Foundation.Collections.IVectorView`1  generic {bbe1fa4c-b0e3-4583-baef-1f1b2e483e56}
template <class T>
struct IVectorView : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, T* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(T value, UINT32* index, BYTE* found) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32 startIndex, UINT32 capacity, T* items,
                                              UINT32* actual) = 0;
};

// ------------------------------------------------------------ Windows.Storage.Streams
// IRandomAccessStreamWithContentType {cc254827-4b3d-438f-9232-10c76bc7e038}
// Only passed through as IUnknown to CreateStreamOverRandomAccessStream.
inline constexpr GUID IID_IRandomAccessStreamWithContentType =
    {0xcc254827, 0x4b3d, 0x438f, {0x92, 0x32, 0x10, 0xc7, 0x6b, 0xc7, 0xe0, 0x38}};

// IRandomAccessStreamReference {33ee3134-1dd6-4e3a-8067-d1c162e8642b}
inline constexpr GUID IID_IRandomAccessStreamReference =
    {0x33ee3134, 0x1dd6, 0x4e3a, {0x80, 0x67, 0xd1, 0xc1, 0x62, 0xe8, 0x64, 0x2b}};
struct IRandomAccessStreamReference : public IInspectable {
    // [0] IAsyncOperation<IRandomAccessStreamWithContentType> OpenReadAsync()
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOperation<IInspectable*>** operation) = 0;
};

// ------------------------------------------------------------ Windows.Media.Control
inline constexpr wchar_t RuntimeClass_GSMTCSessionManager[] =
    L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";

// GlobalSystemMediaTransportControlsSessionPlaybackStatus
enum GsmtcPlaybackStatus : INT32 { Gsmtc_Closed = 0, Gsmtc_Opened = 1, Gsmtc_Changing = 2,
                                   Gsmtc_Stopped = 3, Gsmtc_Playing = 4, Gsmtc_Paused = 5 };

struct IGSMTCSession;
struct IGSMTCSessionManager;
struct IGSMTCSessionMediaProperties;
struct IGSMTCSessionPlaybackInfo;
struct IGSMTCSessionTimelineProperties;

// IGlobalSystemMediaTransportControlsSessionManagerStatics {2050c4ee-11a0-57de-aed7-c97c70338245}
inline constexpr GUID IID_IGSMTCSessionManagerStatics =
    {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
struct IGSMTCSessionManagerStatics : public IInspectable {
    // [0] IAsyncOperation<GlobalSystemMediaTransportControlsSessionManager> RequestAsync()
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOperation<IGSMTCSessionManager*>** operation) = 0;
};

// IGlobalSystemMediaTransportControlsSessionManager {cace8eac-e86e-504a-ab31-5ff8ff1bce49}
inline constexpr GUID IID_IGSMTCSessionManager =
    {0xcace8eac, 0xe86e, 0x504a, {0xab, 0x31, 0x5f, 0xf8, 0xff, 0x1b, 0xce, 0x49}};
struct IGSMTCSessionManager : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(IGSMTCSession** result) = 0;                       // [0]
    virtual HRESULT STDMETHODCALLTYPE GetSessions(IVectorView<IGSMTCSession*>** result) = 0;               // [1]
    virtual HRESULT STDMETHODCALLTYPE add_CurrentSessionChanged(IUnknown* handler, EventRegistrationToken* token) = 0; // [2]
    virtual HRESULT STDMETHODCALLTYPE remove_CurrentSessionChanged(EventRegistrationToken token) = 0;      // [3]
    virtual HRESULT STDMETHODCALLTYPE add_SessionsChanged(IUnknown* handler, EventRegistrationToken* token) = 0;       // [4]
    virtual HRESULT STDMETHODCALLTYPE remove_SessionsChanged(EventRegistrationToken token) = 0;            // [5]
};

// IGlobalSystemMediaTransportControlsSession {7148c835-9b14-5ae2-ab85-dc9b1c14e1a8}
inline constexpr GUID IID_IGSMTCSession =
    {0x7148c835, 0x9b14, 0x5ae2, {0xab, 0x85, 0xdc, 0x9b, 0x1c, 0x14, 0xe1, 0xa8}};
struct IGSMTCSession : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING* value) = 0;                                    // [0]
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOperation<IGSMTCSessionMediaProperties*>** op) = 0; // [1]
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(IGSMTCSessionTimelineProperties** result) = 0;             // [2]
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IGSMTCSessionPlaybackInfo** result) = 0;                         // [3]
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IAsyncOperation<BYTE>** op) = 0;                                    // [4]
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IAsyncOperation<BYTE>** op) = 0;                                   // [5]
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IAsyncOperation<BYTE>** op) = 0;                                    // [6]
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IAsyncOperation<BYTE>** op) = 0;                                  // [7]
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IAsyncOperation<BYTE>** op) = 0;                             // [8]
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IAsyncOperation<BYTE>** op) = 0;                                  // [9]
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IAsyncOperation<BYTE>** op) = 0;                                // [10]
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IAsyncOperation<BYTE>** op) = 0;                            // [11]
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IAsyncOperation<BYTE>** op) = 0;                         // [12]
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IAsyncOperation<BYTE>** op) = 0;                       // [13]
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IAsyncOperation<BYTE>** op) = 0;                         // [14]
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(INT32 requestedAutoRepeatMode, IAsyncOperation<BYTE>** op) = 0; // [15]
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(DOUBLE requestedPlaybackRate, IAsyncOperation<BYTE>** op) = 0;    // [16]
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(BYTE requestedShuffleState, IAsyncOperation<BYTE>** op) = 0;     // [17]
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64 requestedPlaybackPosition, IAsyncOperation<BYTE>** op) = 0; // [18]
    virtual HRESULT STDMETHODCALLTYPE add_TimelinePropertiesChanged(IUnknown* handler, EventRegistrationToken* token) = 0; // [19]
    virtual HRESULT STDMETHODCALLTYPE remove_TimelinePropertiesChanged(EventRegistrationToken token) = 0;                  // [20]
    virtual HRESULT STDMETHODCALLTYPE add_PlaybackInfoChanged(IUnknown* handler, EventRegistrationToken* token) = 0;       // [21]
    virtual HRESULT STDMETHODCALLTYPE remove_PlaybackInfoChanged(EventRegistrationToken token) = 0;                        // [22]
    virtual HRESULT STDMETHODCALLTYPE add_MediaPropertiesChanged(IUnknown* handler, EventRegistrationToken* token) = 0;    // [23]
    virtual HRESULT STDMETHODCALLTYPE remove_MediaPropertiesChanged(EventRegistrationToken token) = 0;                     // [24]
};

// IGlobalSystemMediaTransportControlsSessionMediaProperties {68856cf6-adb4-54b2-ac16-05837907acb6}
inline constexpr GUID IID_IGSMTCSessionMediaProperties =
    {0x68856cf6, 0xadb4, 0x54b2, {0xac, 0x16, 0x05, 0x83, 0x79, 0x07, 0xac, 0xb6}};
struct IGSMTCSessionMediaProperties : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING* value) = 0;                          // [0]
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING* value) = 0;                       // [1]
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING* value) = 0;                    // [2]
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING* value) = 0;                         // [3]
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING* value) = 0;                     // [4]
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32* value) = 0;                      // [5]
    virtual HRESULT STDMETHODCALLTYPE get_Genres(IVectorView<HSTRING>** value) = 0;           // [6]
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32* value) = 0;                  // [7]
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IInspectable** value) = 0;             // [8] IReference<MediaPlaybackType>
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IRandomAccessStreamReference** value) = 0; // [9]
};

// IGlobalSystemMediaTransportControlsSessionPlaybackInfo {94b4b6cf-e8ba-51ad-87a7-c10ade106127}
inline constexpr GUID IID_IGSMTCSessionPlaybackInfo =
    {0x94b4b6cf, 0xe8ba, 0x51ad, {0x87, 0xa7, 0xc1, 0x0a, 0xde, 0x10, 0x61, 0x27}};
struct IGSMTCSessionPlaybackInfo : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IInspectable** value) = 0;        // [0] ...PlaybackControls
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(INT32* value) = 0;          // [1] GsmtcPlaybackStatus
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IInspectable** value) = 0;    // [2] IReference<MediaPlaybackType>
    virtual HRESULT STDMETHODCALLTYPE get_AutoRepeatMode(IInspectable** value) = 0;  // [3] IReference<MediaPlaybackAutoRepeatMode>
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackRate(IInspectable** value) = 0;    // [4] IReference<Double>
    virtual HRESULT STDMETHODCALLTYPE get_IsShuffleActive(IInspectable** value) = 0; // [5] IReference<Boolean>
};

// IGlobalSystemMediaTransportControlsSessionTimelineProperties {ede34136-6f25-588d-8ecf-ea5b6735aaa5}
inline constexpr GUID IID_IGSMTCSessionTimelineProperties =
    {0xede34136, 0x6f25, 0x588d, {0x8e, 0xcf, 0xea, 0x5b, 0x67, 0x35, 0xaa, 0xa5}};
struct IGSMTCSessionTimelineProperties : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(TimeSpan* value) = 0;       // [0]
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(TimeSpan* value) = 0;         // [1]
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(TimeSpan* value) = 0;     // [2]
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(TimeSpan* value) = 0;     // [3]
    virtual HRESULT STDMETHODCALLTYPE get_Position(TimeSpan* value) = 0;        // [4]
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(DateTime* value) = 0; // [5]
};

// ------------------------------------------------------------ Windows.Devices.Radios
inline constexpr wchar_t RuntimeClass_Radio[] = L"Windows.Devices.Radios.Radio";

enum RadioKindAbi : INT32 { RadioKind_Other = 0, RadioKind_WiFi = 1, RadioKind_MobileBroadband = 2,
                            RadioKind_Bluetooth = 3, RadioKind_FM = 4 };
enum RadioStateAbi : INT32 { RadioState_Unknown = 0, RadioState_On = 1, RadioState_Off = 2,
                             RadioState_Disabled = 3 };
enum RadioAccessStatusAbi : INT32 { RadioAccess_Unspecified = 0, RadioAccess_Allowed = 1,
                                    RadioAccess_DeniedByUser = 2, RadioAccess_DeniedBySystem = 3 };

// Windows.Devices.Radios.IRadio {252118df-b33e-416a-875f-1cf38ae2d83e}
inline constexpr GUID IID_IRadio =
    {0x252118df, 0xb33e, 0x416a, {0x87, 0x5f, 0x1c, 0xf3, 0x8a, 0xe2, 0xd8, 0x3e}};
struct IRadio : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetStateAsync(INT32 value, IAsyncOperation<INT32>** op) = 0;      // [0] -> RadioAccessStatus
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(IUnknown* handler, EventRegistrationToken* token) = 0; // [1]
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(EventRegistrationToken token) = 0;           // [2]
    virtual HRESULT STDMETHODCALLTYPE get_State(INT32* value) = 0;                                     // [3] RadioState
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING* value) = 0;                                    // [4]
    virtual HRESULT STDMETHODCALLTYPE get_Kind(INT32* value) = 0;                                      // [5] RadioKind
};

// Windows.Devices.Radios.IRadioStatics {5fb6a12e-67cb-46ae-aae9-65919f86eff4}
inline constexpr GUID IID_IRadioStatics =
    {0x5fb6a12e, 0x67cb, 0x46ae, {0xaa, 0xe9, 0x65, 0x91, 0x9f, 0x86, 0xef, 0xf4}};
struct IRadioStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetRadiosAsync(IAsyncOperation<IVectorView<IRadio*>*>** op) = 0; // [0]
    virtual HRESULT STDMETHODCALLTYPE GetDeviceSelector(HSTRING* selector) = 0;                        // [1]
    virtual HRESULT STDMETHODCALLTYPE FromIdAsync(HSTRING deviceId, IAsyncOperation<IRadio*>** op) = 0; // [2]
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(IAsyncOperation<INT32>** op) = 0;             // [3] -> RadioAccessStatus
};

// ------------------------------------------------------------ parametrized IIDs
// Not needed for the calls above (results come back already typed), kept for
// QueryInterface checks and as the reference values.

// pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};b1)
inline constexpr GUID IID_IAsyncOperation_Boolean =
    {0xcdb5efb3, 0x5788, 0x509d, {0x9b, 0xe1, 0x71, 0xcc, 0xb8, 0xa3, 0x36, 0x2a}};
// pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};rc(Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager;{cace8eac-e86e-504a-ab31-5ff8ff1bce49}))
inline constexpr GUID IID_IAsyncOperation_GSMTCSessionManager =
    {0x3eec115e, 0x7346, 0x5c27, {0x8c, 0x5f, 0xda, 0x78, 0x51, 0x4a, 0x27, 0x7b}};
// pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};rc(Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties;{68856cf6-adb4-54b2-ac16-05837907acb6}))
inline constexpr GUID IID_IAsyncOperation_GSMTCSessionMediaProperties =
    {0xb185e6f3, 0xe0d8, 0x51cb, {0x91, 0x3f, 0xc9, 0x8d, 0x48, 0xc9, 0x3c, 0x46}};
// pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};{cc254827-4b3d-438f-9232-10c76bc7e038})
inline constexpr GUID IID_IAsyncOperation_IRandomAccessStreamWithContentType =
    {0xc4a57c5e, 0x32b0, 0x55b3, {0xad, 0x13, 0xce, 0x1c, 0x23, 0x04, 0x1e, 0xd6}};
// pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};rc(Windows.Devices.Radios.Radio;{252118df-b33e-416a-875f-1cf38ae2d83e}))
inline constexpr GUID IID_IVectorView_Radio =
    {0x65066c36, 0x090b, 0x5466, {0xb8, 0xe5, 0xe7, 0x56, 0x5d, 0xc3, 0x41, 0x75}};
// pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};rc(Windows.Devices.Radios.Radio;{252118df-b33e-416a-875f-1cf38ae2d83e})))
inline constexpr GUID IID_IAsyncOperation_IVectorView_Radio =
    {0x040b54a1, 0x203e, 0x58f5, {0x94, 0x3f, 0xc1, 0xcc, 0xa8, 0x6b, 0xd5, 0x32}};
// pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};enum(Windows.Devices.Radios.RadioAccessStatus;i4))
inline constexpr GUID IID_IAsyncOperation_RadioAccessStatus =
    {0x21fb30ef, 0x072f, 0x502c, {0x98, 0x98, 0xd0, 0xc3, 0xb2, 0xcd, 0x9a, 0xc5}};

// ------------------------------------------------------------ small helpers

// Owning COM pointer.
template <class T>
class Ptr {
public:
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    Ptr(Ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Ptr& operator=(Ptr&& o) noexcept {
        if (this != &o) { Reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~Ptr() { Reset(); }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T** Put() { Reset(); return &p_; }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

// Owning HSTRING.
class HStr {
public:
    HStr() = default;
    explicit HStr(const wchar_t* s) { WindowsCreateString(s, static_cast<UINT32>(wcslen(s)), &h_); }
    HStr(const HStr&) = delete;
    HStr& operator=(const HStr&) = delete;
    ~HStr() { Reset(); }
    void Reset() { if (h_) { WindowsDeleteString(h_); h_ = nullptr; } }
    HSTRING* Put() { Reset(); return &h_; }
    HSTRING Get() const { return h_; }
    std::wstring Str() const {
        if (!h_) return {};
        UINT32 n = 0;
        const wchar_t* b = WindowsGetStringRawBuffer(h_, &n);
        return b ? std::wstring(b, n) : std::wstring();
    }
private:
    HSTRING h_ = nullptr;
};

// Activation factory / statics interface for a runtime class.
template <class T>
inline HRESULT GetStatics(const wchar_t* runtimeClass, const GUID& iid, Ptr<T>& out) {
    HStr cls(runtimeClass);
    if (!cls.Get()) return E_OUTOFMEMORY;
    return RoGetActivationFactory(cls.Get(), iid, reinterpret_cast<void**>(out.Put()));
}

// Waits for an IAsyncOperation by polling IAsyncInfo::Status every 10 ms.
// Returns true when it completed; on timeout (or `stop` signalled) it is
// cancelled and false is returned. The caller still owns and releases `op`.
template <class T>
inline bool Await(IAsyncOperation<T>* op, DWORD timeoutMs, HANDLE stop = nullptr) {
    if (!op) return false;
    Ptr<IAsyncInfo> info;
    if (FAILED(op->QueryInterface(IID_IAsyncInfo, reinterpret_cast<void**>(info.Put())))) return false;
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        INT32 status = AsyncStatus_Started;
        if (FAILED(info->get_Status(&status))) return false;
        if (status == AsyncStatus_Completed) return true;
        if (status != AsyncStatus_Started) return false;   // Error or Canceled
        if (GetTickCount64() >= deadline) break;
        if (stop) {
            if (WaitForSingleObject(stop, 10) == WAIT_OBJECT_0) break;
        } else {
            Sleep(10);
        }
    }
    info->Cancel();
    return false;
}

// Await + GetResults in one step.
template <class T>
inline bool AwaitResult(IAsyncOperation<T>* op, DWORD timeoutMs, HANDLE stop, T* result) {
    return Await(op, timeoutMs, stop) && SUCCEEDED(op->GetResults(result));
}

} // namespace lirt
