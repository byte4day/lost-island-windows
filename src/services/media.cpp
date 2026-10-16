// Lost Island for Windows: now playing, from the Global System Media
// Transport Controls (Windows.Media.Control) through the raw WinRT ABI.
//
// One MTA worker thread owns every WinRT object. It polls about once a
// second, runs transport commands queued by the UI thread, and posts
// WM_APP_SERVICE/SVC_MEDIA only when something visible changed.
// When GSMTC cannot be activated (Wine, very old Windows) the transport
// commands fall back to synthesized media keys and `active` stays false.
#include "services.h"
#include "winrt_abi.h"

#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace media {
namespace {

using namespace lirt;

constexpr DWORD kPollMs = 1000;
constexpr DWORD kAsyncTimeoutMs = 3000;
constexpr DWORD kCommandTimeoutMs = 2000;
constexpr DWORD kActivationRetryMs = 10000;
constexpr int kArtRetries = 3;                     // re-read an empty thumbnail this many polls
constexpr size_t kMaxArtBytes = 16u * 1024 * 1024;
constexpr double kTicksPerSecond = 10000000.0;     // TimeSpan is 100 ns

enum class CmdKind { PlayPause, Next, Previous, Seek };
struct Cmd {
    CmdKind kind;
    INT64 ticks;   // Seek target, 100 ns, relative to the timeline start
};

// Shared state (UI thread <-> worker).
std::mutex g_mu;
MediaInfo g_info;
ULONGLONG g_sampleTick = 0;   // GetTickCount64() when g_info.position was sampled

std::mutex g_cmdMu;
std::deque<Cmd> g_cmds;

HANDLE g_stop = nullptr;      // manual reset
HANDLE g_wake = nullptr;      // auto reset, signalled when a command is queued
HWND g_notify = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_gsmtc{false};   // the session manager is available
uint64_t g_artCounter = 0;          // worker only; survives Stop/Start so versions never repeat

void Notify() {
    if (g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_MEDIA, 0);
}

void SendMediaKey(BYTE vk) {
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
}

void KeyFallback(CmdKind kind) {
    switch (kind) {
    case CmdKind::PlayPause: SendMediaKey(VK_MEDIA_PLAY_PAUSE); break;
    case CmdKind::Next:      SendMediaKey(VK_MEDIA_NEXT_TRACK); break;
    case CmdKind::Previous:  SendMediaKey(VK_MEDIA_PREV_TRACK); break;
    case CmdKind::Seek:      break;   // no key for this
    }
}

INT64 NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return static_cast<INT64>((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// Position as Get() reports it: extrapolated while playing, clamped.
double Extrapolate(const MediaInfo& m, ULONGLONG sampleTick, ULONGLONG now) {
    double pos = m.position;
    if (m.playing && m.hasTimeline && now > sampleTick) pos += (now - sampleTick) / 1000.0;
    if (m.hasTimeline && pos > m.duration) pos = m.duration;
    if (pos < 0) pos = 0;
    return pos;
}

class Worker {
public:
    void Run();

private:
    bool EnsureManager();
    void Poll(MediaInfo& out, bool& artChanged);
    bool ReadThumbnail(IGSMTCSessionMediaProperties* props, std::vector<uint8_t>& bytes);
    void ClearArt(bool& artChanged);
    void Publish(MediaInfo&& next, bool artChanged);
    void Exec(const Cmd& cmd);

    Ptr<IGSMTCSessionManagerStatics> statics_;
    Ptr<IGSMTCSessionManager> manager_;
    ULONGLONG nextActivation_ = 0;

    INT64 timelineStart_ = 0;             // ticks, for Seek
    bool artKeyValid_ = false;
    std::wstring artTitle_, artArtist_;   // track the current art belongs to
    int artRetriesLeft_ = 0;
    std::vector<uint8_t> art_;
    uint64_t artVersion_ = g_artCounter;
};

bool Worker::EnsureManager() {
    if (manager_) return true;
    ULONGLONG now = GetTickCount64();
    if (now < nextActivation_) return false;
    nextActivation_ = now + kActivationRetryMs;

    if (!statics_ &&
        FAILED(GetStatics(RuntimeClass_GSMTCSessionManager, IID_IGSMTCSessionManagerStatics, statics_))) {
        statics_.Reset();
        g_gsmtc = false;
        return false;
    }
    Ptr<IAsyncOperation<IGSMTCSessionManager*>> op;
    if (FAILED(statics_->RequestAsync(op.Put())) || !op) return false;
    if (!AwaitResult(op.Get(), kAsyncTimeoutMs, g_stop, manager_.Put()) || !manager_) {
        manager_.Reset();
        return false;
    }
    g_gsmtc = true;
    return true;
}

bool Worker::ReadThumbnail(IGSMTCSessionMediaProperties* props, std::vector<uint8_t>& bytes) {
    bytes.clear();
    Ptr<IRandomAccessStreamReference> ref;
    if (FAILED(props->get_Thumbnail(ref.Put())) || !ref) return false;
    Ptr<IAsyncOperation<IInspectable*>> op;
    if (FAILED(ref->OpenReadAsync(op.Put())) || !op) return false;
    Ptr<IInspectable> ras;   // IRandomAccessStreamWithContentType
    if (!AwaitResult(op.Get(), kAsyncTimeoutMs, g_stop, ras.Put()) || !ras) return false;
    Ptr<IStream> stream;
    if (FAILED(CreateStreamOverRandomAccessStream(ras.Get(), __uuidof(IStream),
                                                  reinterpret_cast<void**>(stream.Put()))) ||
        !stream)
        return false;
    uint8_t buf[64 * 1024];
    for (;;) {
        ULONG got = 0;
        HRESULT hr = stream->Read(buf, sizeof(buf), &got);
        if (FAILED(hr)) { bytes.clear(); return false; }
        bytes.insert(bytes.end(), buf, buf + got);
        if (bytes.size() > kMaxArtBytes) { bytes.clear(); return false; }
        if (hr != S_OK || got == 0) break;   // S_FALSE: end of stream
    }
    return !bytes.empty();
}

void Worker::ClearArt(bool& artChanged) {
    artKeyValid_ = false;
    artTitle_.clear();
    artArtist_.clear();
    artRetriesLeft_ = 0;
    if (!art_.empty()) {
        art_.clear();
        artVersion_ = ++g_artCounter;
        artChanged = true;
    }
}

void Worker::Poll(MediaInfo& out, bool& artChanged) {
    out = MediaInfo{};
    artChanged = false;

    Ptr<IGSMTCSession> session;
    if (EnsureManager()) {
        if (FAILED(manager_->GetCurrentSession(session.Put()))) {
            // e.g. RPC_E_DISCONNECTED after the shell restarted: request a new manager.
            session.Reset();
            manager_.Reset();
            nextActivation_ = 0;
        }
    }
    if (!session) {
        timelineStart_ = 0;
        ClearArt(artChanged);
        out.artVersion = artVersion_;
        return;
    }
    out.active = true;

    HStr app;
    if (SUCCEEDED(session->get_SourceAppUserModelId(app.Put()))) out.appId = app.Str();

    Ptr<IGSMTCSessionPlaybackInfo> playback;
    if (SUCCEEDED(session->GetPlaybackInfo(playback.Put())) && playback) {
        INT32 status = Gsmtc_Closed;
        if (SUCCEEDED(playback->get_PlaybackStatus(&status))) out.playing = status == Gsmtc_Playing;
    }

    Ptr<IGSMTCSessionTimelineProperties> timeline;
    if (SUCCEEDED(session->GetTimelineProperties(timeline.Put())) && timeline) {
        TimeSpan start{}, end{}, pos{};
        DateTime updated{};
        if (SUCCEEDED(timeline->get_StartTime(&start)) && SUCCEEDED(timeline->get_EndTime(&end)) &&
            SUCCEEDED(timeline->get_Position(&pos))) {
            INT64 duration = end.Duration - start.Duration;
            timelineStart_ = start.Duration;
            if (duration > 0) {
                INT64 p = pos.Duration - start.Duration;
                // Position is as of LastUpdatedTime; players refresh it rarely.
                if (out.playing && SUCCEEDED(timeline->get_LastUpdatedTime(&updated)) &&
                    updated.UniversalTime > 0) {
                    INT64 delta = NowFileTime() - updated.UniversalTime;
                    if (delta > 0 && delta < duration) p += delta;
                }
                out.hasTimeline = true;
                out.duration = duration / kTicksPerSecond;
                out.position = p / kTicksPerSecond;
                if (out.position > out.duration) out.position = out.duration;
                if (out.position < 0) out.position = 0;
            }
        }
    }

    Ptr<IAsyncOperation<IGSMTCSessionMediaProperties*>> op;
    Ptr<IGSMTCSessionMediaProperties> props;
    if (SUCCEEDED(session->TryGetMediaPropertiesAsync(op.Put())) && op)
        AwaitResult(op.Get(), kAsyncTimeoutMs, g_stop, props.Put());
    if (props) {
        HStr title, artist, album;
        if (SUCCEEDED(props->get_Title(title.Put()))) out.title = title.Str();
        if (SUCCEEDED(props->get_Artist(artist.Put()))) out.artist = artist.Str();
        if (SUCCEEDED(props->get_AlbumTitle(album.Put()))) out.album = album.Str();

        bool trackChanged = !artKeyValid_ || out.title != artTitle_ || out.artist != artArtist_;
        if (trackChanged) {
            artKeyValid_ = true;
            artTitle_ = out.title;
            artArtist_ = out.artist;
            artRetriesLeft_ = kArtRetries;
        }
        // Players often publish the title before the artwork, so an empty
        // thumbnail is retried for a few polls; otherwise read once per track.
        if (trackChanged || (art_.empty() && artRetriesLeft_ > 0)) {
            if (artRetriesLeft_ > 0) --artRetriesLeft_;
            std::vector<uint8_t> bytes;
            ReadThumbnail(props.Get(), bytes);
            if (bytes != art_) {
                art_ = std::move(bytes);
                artVersion_ = ++g_artCounter;
                artChanged = true;
            }
            if (!art_.empty()) artRetriesLeft_ = 0;
        }
    }
    out.artVersion = artVersion_;
}

void Worker::Publish(MediaInfo&& next, bool artChanged) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ULONGLONG now = GetTickCount64();
        MediaInfo& cur = g_info;
        double expected = Extrapolate(cur, g_sampleTick, now);
        changed = cur.active != next.active || cur.playing != next.playing ||
                  cur.title != next.title || cur.artist != next.artist || cur.album != next.album ||
                  cur.appId != next.appId || cur.artVersion != next.artVersion ||
                  cur.hasTimeline != next.hasTimeline ||
                  std::fabs(cur.duration - next.duration) > 0.5 ||
                  (next.hasTimeline && std::fabs(expected - next.position) > 2.0);   // a seek
        cur.active = next.active;
        cur.playing = next.playing;
        cur.hasTimeline = next.hasTimeline;
        cur.title = std::move(next.title);
        cur.artist = std::move(next.artist);
        cur.album = std::move(next.album);
        cur.appId = std::move(next.appId);
        cur.position = next.position;
        cur.duration = next.duration;
        cur.artVersion = next.artVersion;
        if (artChanged) cur.art = art_;
        g_sampleTick = now;
    }
    if (changed) Notify();
}

void Worker::Exec(const Cmd& cmd) {
    Ptr<IGSMTCSession> session;
    if (manager_ && FAILED(manager_->GetCurrentSession(session.Put()))) session.Reset();
    if (!session) {
        KeyFallback(cmd.kind);
        return;
    }
    Ptr<IAsyncOperation<BYTE>> op;
    HRESULT hr = E_FAIL;
    switch (cmd.kind) {
    case CmdKind::PlayPause: hr = session->TryTogglePlayPauseAsync(op.Put()); break;
    case CmdKind::Next:      hr = session->TrySkipNextAsync(op.Put()); break;
    case CmdKind::Previous:  hr = session->TrySkipPreviousAsync(op.Put()); break;
    case CmdKind::Seek:
        hr = session->TryChangePlaybackPositionAsync(timelineStart_ + cmd.ticks, op.Put());
        break;
    }
    if (SUCCEEDED(hr) && op) {
        BYTE ok = 0;
        AwaitResult(op.Get(), kCommandTimeoutMs, g_stop, &ok);
    }
}

void Worker::Run() {
    auto pollAndPublish = [this] {
        MediaInfo next;
        bool artChanged = false;
        Poll(next, artChanged);
        Publish(std::move(next), artChanged);
    };
    pollAndPublish();
    HANDLE handles[2] = {g_stop, g_wake};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, kPollMs);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            for (;;) {
                Cmd cmd;
                {
                    std::lock_guard<std::mutex> lock(g_cmdMu);
                    if (g_cmds.empty()) break;
                    cmd = g_cmds.front();
                    g_cmds.pop_front();
                }
                Exec(cmd);
            }
            // Give the player a moment to apply it before reading back.
            if (WaitForSingleObject(g_stop, 250) == WAIT_OBJECT_0) break;
        }
        pollAndPublish();
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

void Enqueue(CmdKind kind, INT64 ticks = 0) {
    if (!g_running || !g_gsmtc) {
        KeyFallback(kind);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_cmdMu);
        g_cmds.push_back(Cmd{kind, ticks});
    }
    SetEvent(g_wake);
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
    g_gsmtc = false;
    g_notify = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cmdMu);
        g_cmds.clear();
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_info = MediaInfo{};
    g_sampleTick = 0;
}

MediaInfo Get() {
    std::lock_guard<std::mutex> lock(g_mu);
    MediaInfo copy = g_info;
    copy.position = Extrapolate(g_info, g_sampleTick, GetTickCount64());
    return copy;
}

void PlayPause() { Enqueue(CmdKind::PlayPause); }
void Next() { Enqueue(CmdKind::Next); }
void Previous() { Enqueue(CmdKind::Previous); }

void Seek(double seconds) {
    if (seconds < 0) seconds = 0;
    bool show = false;
    {
        // Move the visible position now; the worker's next poll confirms it.
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_info.active && g_info.hasTimeline) {
            g_info.position = seconds > g_info.duration ? g_info.duration : seconds;
            g_sampleTick = GetTickCount64();
            show = true;
        }
    }
    if (show) Notify();
    Enqueue(CmdKind::Seek, static_cast<INT64>(std::llround(seconds * kTicksPerSecond)));
}

} // namespace media
