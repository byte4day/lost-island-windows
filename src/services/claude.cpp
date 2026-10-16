// Live Claude Code status.
//
// Claude Code runs `LostIslandHook.exe --claude-hook` on session events with
// a JSON payload on stdin. The hook writes one small file per session to
// %LOCALAPPDATA%\LostIsland\claude-status\<session>.json and pokes the running
// island, which re-reads the folder. The hook never prints and always exits 0.
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <mutex>
#include <regex>
#include <thread>

#include "json.h"
#include "services.h"

namespace {
const char* kEvents[] = {"SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest",
                         "Notification", "Stop", "PreCompact", "SessionEnd"};
const wchar_t* kMarker = L"LostIslandHook.exe";
constexpr double kDoneSeconds = 8;
constexpr double kStaleSeconds = 30 * 60;

HWND g_notify = nullptr;
std::mutex g_mu;
std::vector<ClaudeSession> g_sessions;
std::thread g_watch;
HANDLE g_stop = nullptr;

std::wstring statusDir() { return sys::CacheDir() + L"\\claude-status"; }
std::wstring hookExe() { return sys::CacheDir() + L"\\LostIslandHook.exe"; }
std::wstring claudeDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) out = std::wstring(p) + L"\\.claude";
    CoTaskMemFree(p);
    return out;
}

double now() { return (double)time(nullptr); }

int rank(const std::wstring& s) {
    if (s == L"needs") return 3;
    if (s == L"tool" || s == L"thinking") return 2;
    if (s == L"done") return 1;
    return 0;
}

std::string shortText(std::string t, size_t limit = 38) {
    std::string out;
    bool space = false;
    for (char c : t) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { space = !out.empty(); continue; }
        if (space) { out += ' '; space = false; }
        out += c;
    }
    std::wstring w = json::FromUtf8(out);
    if (w.size() > limit) w = w.substr(0, limit - 1) + L"…";
    return json::ToUtf8(w);
}

std::string baseName(std::string p) {
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string toolDetail(const std::string& name, const json::Value& args) {
    if (name == "Bash" || name == "PowerShell") {
        std::string d = args["description"].asString();
        if (d.empty()) d = args["command"].asString();
        return shortText(d.empty() ? "Running" : d);
    }
    for (const char* key : {"file_path", "notebook_path", "path"})
        if (!args[key].asString().empty()) return name + " " + baseName(args[key].asString());
    if (!args["pattern"].asString().empty()) return shortText(name + " " + args["pattern"].asString());
    if (!args["url"].asString().empty()) {
        std::string u = args["url"].asString();
        size_t s = u.find("://");
        if (s != std::string::npos) u = u.substr(s + 3);
        return name + " " + u.substr(0, u.find('/'));
    }
    if (!args["query"].asString().empty()) return shortText(name + " " + args["query"].asString());
    if (!args["description"].asString().empty()) return shortText(args["description"].asString());
    if (name.rfind("mcp__", 0) == 0) {
        std::string t = name.substr(name.rfind("__") + 2);
        std::replace(t.begin(), t.end(), '_', ' ');
        return shortText(t);
    }
    return name;
}

// (state, detail); empty state = ignore, "ended" = remove the file
std::pair<std::string, std::string> stateFor(const json::Value& p) {
    std::string ev = p["hook_event_name"].asString();
    if (ev == "SessionStart") return {"idle", ""};
    if (ev == "UserPromptSubmit") return {"thinking", "Thinking"};
    if (ev == "PreToolUse") return {"tool", toolDetail(p["tool_name"].asString(), p["tool_input"])};
    if (ev == "PostToolUse" || ev == "PostToolUseFailure") return {"thinking", "Thinking"};
    if (ev == "PreCompact") return {"thinking", "Compacting"};
    if (ev == "PermissionRequest") {
        std::string t = p["tool_name"].asString();
        return {"needs", shortText("Allow " + (t.empty() ? std::string("tool") : t) + "?")};
    }
    if (ev == "Notification") {
        std::string kind = p["notification_type"].asString(), msg = p["message"].asString(), low = msg;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (kind == "idle_prompt" || kind == "auth_success" || low.find("waiting for your input") != std::string::npos)
            return {"", ""};
        if (kind == "permission_prompt" || low.find("permission") != std::string::npos) {
            std::smatch m;
            std::string tool = "a tool";
            if (std::regex_search(msg, m, std::regex("use (\\S+)"))) tool = m[1];
            return {"needs", "Allow " + tool + "?"};
        }
        return {"needs", shortText(msg.empty() ? "Needs you" : msg)};
    }
    if (ev == "Stop") return {"done", "Done"};
    if (ev == "SessionEnd") return {"ended", ""};
    return {"", ""};
}

DWORD parentPid(DWORD pid) {
    DWORD parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == pid) { parent = pe.th32ParentProcessID; break; }
    CloseHandle(snap);
    return parent;
}

void reload() {
    std::vector<ClaudeSession> all;
    WIN32_FIND_DATAW fd;
    std::wstring dir = statusDir();
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    double t = now();
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string text;
            json::Value v;
            std::wstring path = dir + L"\\" + fd.cFileName;
            if (!json::ReadFile(path, text) || !json::Parse(text, v)) continue;
            ClaudeSession s;
            s.session = v["session"].asWString();
            s.state = v["state"].asWString();
            s.detail = v["detail"].asWString();
            s.project = v["project"].asWString();
            s.pid = (DWORD)v["pid"].asNumber();
            s.updated = v["updated"].asNumber();
            double age = t - s.updated;
            // old files from sessions that never sent SessionEnd
            if (age > 24 * 3600) { DeleteFileW(path.c_str()); continue; }
            all.push_back(s);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_sessions = all;
    }
    if (g_notify) PostMessageW(g_notify, WM_APP_SERVICE, SVC_CLAUDE, 0);
}

struct FindWin { DWORD pid; HWND hwnd; };
BOOL CALLBACK findTop(HWND h, LPARAM l) {
    auto* f = reinterpret_cast<FindWin*>(l);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->hwnd = h; return FALSE; }
    return TRUE;
}

bool hookInGroups(const json::Value& groups) {
    for (auto& g : groups.items())
        for (auto& h : g["hooks"].items())
            if (h["command"].asWString().find(kMarker) != std::wstring::npos) return true;
    return false;
}

bool loadSettings(json::Value& settings, std::wstring& path) {
    path = claudeDir() + L"\\settings.json";
    std::string text;
    if (!json::ReadFile(path, text) || text.empty()) { settings = json::Value::Object(); return true; }
    return json::Parse(text, settings) && settings.isObject();
}

void backup(const std::wstring& path) {
    wchar_t suffix[48];
    swprintf(suffix, 48, L".bak-lostisland-%lld", (long long)time(nullptr));
    CopyFileW(path.c_str(), (path + suffix).c_str(), TRUE);
}
}  // namespace

namespace claude {

void Start(HWND notify) {
    g_notify = notify;
    CreateDirectoryW(statusDir().c_str(), nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    reload();
    g_watch = std::thread([] {
        HANDLE ch = FindFirstChangeNotificationW(statusDir().c_str(), FALSE,
                                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        for (;;) {
            HANDLE w[2] = {g_stop, ch};
            // re-check every 4 s anyway so "Done" expires on time
            DWORD r = WaitForMultipleObjects(ch != INVALID_HANDLE_VALUE ? 2 : 1, w, FALSE, 4000);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) { FindNextChangeNotification(ch); Sleep(30); }
            reload();
        }
        if (ch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ch);
    });
}

void Stop() {
    if (g_stop) SetEvent(g_stop);
    if (g_watch.joinable()) g_watch.join();
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
}

std::vector<ClaudeSession> Visible() {
    std::vector<ClaudeSession> out;
    double t = now();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& s : g_sessions) {
            int r = rank(s.state);
            double age = t - s.updated;
            if (!r) continue;
            if (s.state == L"done" && age > kDoneSeconds) continue;
            if ((s.state == L"thinking" || s.state == L"tool") && age > kStaleSeconds) continue;
            out.push_back(s);
        }
    }
    std::sort(out.begin(), out.end(), [](const ClaudeSession& a, const ClaudeSession& b) {
        int ra = rank(a.state), rb = rank(b.state);
        return ra != rb ? ra > rb : a.updated > b.updated;
    });
    return out;
}

int RunHook() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE) return 0;
    std::string text;
    char buf[8192];
    DWORD got = 0;
    while (text.size() < 4 * 1024 * 1024 && ::ReadFile(in, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
    json::Value p;
    if (!json::Parse(text, p)) return 0;
    std::string session;
    for (char c : p["session_id"].asString())
        if (isalnum((unsigned char)c) || c == '-' || c == '_') session += c;
    if (session.empty()) return 0;
    auto [state, detail] = stateFor(p);
    if (state.empty()) return 0;
    std::wstring dir = statusDir();
    CreateDirectoryW(sys::CacheDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\" + json::FromUtf8(session) + L".json";
    if (state == "ended") {
        DeleteFileW(path.c_str());
    } else {
        json::Value v = json::Value::Object();
        v.set("session", session);
        v.set("state", state);
        v.set("detail", detail);
        v.set("project", baseName(p["cwd"].asString()));
        // our parent is the shell Claude Code started; its parent is Claude Code
        DWORD parent = parentPid(GetCurrentProcessId());
        DWORD grand = parentPid(parent);
        v.set("pid", (double)(grand ? grand : parent));
        v.set("updated", now());
        json::WriteFileAtomic(path, v.dump(0));
    }
    if (HWND island = FindWindowW(L"LostIslandWindow", nullptr)) PostMessageW(island, WM_APP_SERVICE, SVC_CLAUDE, 1);
    return 0;
}

bool Installed() {
    json::Value s;
    std::wstring path;
    if (!loadSettings(s, path)) return false;
    for (auto ev : kEvents)
        if (hookInGroups(s["hooks"][ev])) return true;
    return false;
}

bool Setup(std::wstring& message) {
    std::wstring dir = claudeDir();
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"Claude Code's folder (" + dir + L") was not found. Run Claude Code once, then try again.";
        return false;
    }
    // a stable copy of ourselves, so moving the download folder doesn't break the hook
    std::wstring dest = hookExe();
    if (!CopyFileW(sys::ExePath().c_str(), dest.c_str(), FALSE) && GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message = L"Could not copy the hook to " + dest + L".";
        return false;
    }
    json::Value s;
    std::wstring path;
    if (!loadSettings(s, path)) {
        message = L"~/.claude/settings.json is not valid JSON, so it was left alone.";
        return false;
    }
    std::wstring cmd = dest;
    std::replace(cmd.begin(), cmd.end(), L'\\', L'/');   // Claude Code runs hooks through Git Bash
    cmd = L"\"" + cmd + L"\" --claude-hook";
    json::Value& hooks = s.get("hooks");
    if (!hooks.isObject()) hooks = json::Value::Object();
    for (auto ev : kEvents) {
        json::Value& groups = hooks.get(ev);
        if (!groups.isArray()) groups = json::Value::Array();
        if (hookInGroups(groups)) continue;
        json::Value h = json::Value::Object();
        h.set("type", "command");
        h.set("command", json::Value(cmd));
        h.set("timeout", 5);
        json::Value list = json::Value::Array();
        list.push(h);
        json::Value g = json::Value::Object();
        std::string e = ev;
        if (e == "PreToolUse" || e == "PostToolUse" || e == "PermissionRequest") g.set("matcher", "*");
        g.set("hooks", list);
        groups.push(g);
    }
    backup(path);
    if (!json::WriteFileAtomic(path, s.dump())) {
        message = L"Could not write " + path + L".";
        return false;
    }
    message = L"Hooks added. New Claude Code sessions will show up on the island.";
    return true;
}

bool Remove(std::wstring& message) {
    json::Value s;
    std::wstring path;
    if (!loadSettings(s, path) || !s.has("hooks")) { message = L"Nothing to remove."; return true; }
    json::Value& hooks = s.get("hooks");
    bool changed = false;
    for (auto ev : kEvents) {
        if (!hooks.has(ev)) continue;
        json::Value& groups = hooks.get(ev);
        for (size_t i = groups.size(); i-- > 0;) {
            json::Value& g = groups.at(i);
            json::Value& list = g.get("hooks");
            for (size_t j = list.size(); j-- > 0;)
                if (list[j]["command"].asWString().find(kMarker) != std::wstring::npos) { list.erase(j); changed = true; }
            if (list.size() == 0) groups.erase(i);
        }
        if (groups.size() == 0) hooks.remove(ev);
    }
    if (hooks.size() == 0) s.remove("hooks");
    if (!changed) { message = L"Lost Island's hooks were not installed."; return true; }
    backup(path);
    json::WriteFileAtomic(path, s.dump());
    message = L"Hooks removed. A backup of settings.json was kept next to it.";
    return true;
}

void Focus(const ClaudeSession& s) {
    // walk up from Claude Code to the first process that owns a window
    // (Windows Terminal, conhost, VS Code...)
    DWORD pid = s.pid;
    for (int i = 0; i < 8 && pid; i++) {
        FindWin f{pid, nullptr};
        EnumWindows(findTop, reinterpret_cast<LPARAM>(&f));
        if (f.hwnd) {
            if (IsIconic(f.hwnd)) ShowWindow(f.hwnd, SW_RESTORE);
            SetForegroundWindow(f.hwnd);
            return;
        }
        pid = parentPid(pid);
    }
    // a console in Windows Terminal belongs to WindowsTerminal.exe, not to the parent chain
    if (HWND wt = FindWindowW(L"CASCADIA_HOSTING_WINDOW_CLASS", nullptr)) {
        if (IsIconic(wt)) ShowWindow(wt, SW_RESTORE);
        SetForegroundWindow(wt);
    }
}

}  // namespace claude
