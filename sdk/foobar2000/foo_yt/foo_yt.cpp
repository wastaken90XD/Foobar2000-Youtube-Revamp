// foo_yt.cpp v0.4 - youtube.com / youtu.be -> playable source.
// Settings: Preferences > Advanced > Tools > YouTube.
// v0.4: "download" mode (yt-dlp saves audio to a local file, foobar plays the
//       file) as the default; "stream" mode (v0.3 behaviour) as the alternative.
#include "stdafx.h"
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <map>
#include <mutex>

DECLARE_COMPONENT_VERSION("YouTube resolver", "0.4",
    "Invidious and/or yt-dlp. Configure under Preferences > Advanced > Tools > YouTube.");
VALIDATE_COMPONENT_FILENAME("foo_yt.dll");

// ---- settings -------------------------------------------------------------
#define YT_GUID(n) { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, n } }
static const GUID g_b  = YT_GUID(0x01);
static const GUID g_o  = YT_GUID(0x02);
static const GUID g_i  = YT_GUID(0x03);
static const GUID g_t  = YT_GUID(0x04);
static const GUID g_e  = YT_GUID(0x05);
static const GUID g_a  = YT_GUID(0x06);
static const GUID g_s  = YT_GUID(0x07);
static const GUID g_x  = YT_GUID(0x08);
static const GUID g_m  = YT_GUID(0x09);
static const GUID g_da = YT_GUID(0x0a);
static const GUID g_d  = YT_GUID(0x0b);

static advconfig_branch_factory g_branch("YouTube", g_b, advconfig_branch::guid_branch_tools, 0);
static advconfig_string_factory c_mode   ("Playback mode (download | stream)", g_m, g_b, 1, "download");
static advconfig_string_factory c_dlargs ("yt-dlp DOWNLOAD arguments (put cookies/js flags here; no -g, no -o)", g_da, g_b, 2,
                                          "-f bestaudio[ext=m4a]/bestaudio --no-playlist");
static advconfig_string_factory c_dir    ("Download folder (blank = %TEMP%\\foo_yt; ASCII path)", g_d, g_b, 3, "");
static advconfig_string_factory c_exe    ("yt-dlp path", g_e, g_b, 4, "yt-dlp");
static advconfig_string_factory c_args   ("yt-dlp STREAM arguments (stream mode; must print a URL, keep -g)", g_a, g_b, 5,
                                          "-g -f bestaudio[ext=m4a]/bestaudio");
static advconfig_string_factory c_timeout("yt-dlp timeout (seconds)", g_x, g_b, 6, "120");
static advconfig_string_factory c_order  ("Stream mode: backend order (invidious ytdlp)", g_o, g_b, 7, "ytdlp");
static advconfig_string_factory c_inst   ("Stream mode: Invidious instances (space separated)", g_i, g_b, 8, "https://yewtu.be");
static advconfig_string_factory c_itag   ("Stream mode: Invidious itag (140 = m4a audio)", g_t, g_b, 9, "140");
static advconfig_string_factory c_suffix ("Stream mode: URL suffix (decoder hint)", g_s, g_b, 10, "#.m4a");

static std::string cfg(advconfig_string_factory& f) { pfc::string8 s; f.get(s); return s.c_str(); }

static std::vector<std::string> words(const std::string& s) {
    std::istringstream is(s); std::vector<std::string> v; std::string w;
    while (is >> w) v.push_back(w);
    return v;
}

// ---- state ----------------------------------------------------------------
static std::mutex g_mx;   // stream-URL cache
static std::map<std::string, std::pair<std::string, DWORD>> g_cache; // id -> (url, tick)
static const DWORD CACHE_MS = 5 * 60 * 60 * 1000; // stream URLs expire after ~6 h
static std::mutex g_dl;   // one download at a time (also keeps us under rate limits)

// ---- helpers --------------------------------------------------------------
static bool extract_id(const char* p, std::string& id) {
    const char* s = strstr(p, "v=");
    if (s) s += 2; else if ((s = strstr(p, "youtu.be/"))) s += 9; else return false;
    id.assign(s, strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"));
    return id.size() == 11; // also keeps the command lines below injection-safe
}

// Run a command with no console window; return stdout. Kills the process on
// timeout or when foobar aborts. stderr is discarded.
static std::string run_capture(std::string cmd, abort_callback& abort, DWORD timeout_ms,
                               bool& timed_out, DWORD& exit_code) {
    timed_out = false; exit_code = 1;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) return "";
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w; si.hStdError = nul; si.hStdInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {};
    std::string out;
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(w); w = nullptr;
        const DWORD start = GetTickCount();
        bool done = false;
        for (;;) {
            DWORD avail = 0, n = 0;
            char buf[4096];
            BOOL ok = PeekNamedPipe(r, nullptr, 0, nullptr, &avail, nullptr);
            if (ok && avail) {
                if (ReadFile(r, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &n, nullptr) && n) out.append(buf, n);
                continue;
            }
            if (done) break; // exited and pipe drained
            if (!ok || WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) { done = true; continue; }
            if (GetTickCount() - start > timeout_ms || abort.is_aborting()) {
                timed_out = !abort.is_aborting();
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
                break;
            }
        }
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    if (w) CloseHandle(w);
    CloseHandle(r);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return out;
}

// ---- download mode --------------------------------------------------------
static std::string dl_dir() {
    std::string d = cfg(c_dir);
    if (d.empty()) {
        char t[MAX_PATH + 1];
        DWORD n = GetTempPathA(MAX_PATH, t);
        d.assign(t, n);
        d += "foo_yt";
    }
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

// finished file for this video id (skips yt-dlp's partial files), or ""
static std::string find_file(const std::string& dir, const std::string& id) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\" + id + ".*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string found;
    do {
        std::string n = fd.cFileName;
        auto ends = [&](const char* s) { size_t l = strlen(s); return n.size() >= l && n.compare(n.size() - l, l, s) == 0; };
        if (ends(".part") || ends(".ytdl") || ends(".temp")) continue;
        found = dir + "\\" + n;
        break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static std::string download_audio(const std::string& id, abort_callback& abort) {
    const std::string dir = dl_dir();
    std::string f = find_file(dir, id);
    if (!f.empty()) { FB2K_console_formatter() << "foo_yt: using downloaded file for " << id.c_str(); return f; }

    std::unique_lock<std::mutex> lk(g_dl, std::defer_lock);
    while (!lk.try_lock()) { Sleep(50); abort.check(); }
    f = find_file(dir, id); // another caller may have finished it while we waited
    if (!f.empty()) return f;

    int secs = atoi(cfg(c_timeout).c_str());
    if (secs < 5) secs = 120;
    std::string cmd = "\"" + cfg(c_exe) + "\" " + cfg(c_dlargs) +
                      " -o \"" + dir + "\\%(id)s.%(ext)s\" https://youtu.be/" + id;
    const DWORD t0 = GetTickCount();
    bool timed_out = false; DWORD code = 1;
    run_capture(cmd, abort, (DWORD)secs * 1000, timed_out, code);
    abort.check();
    const unsigned ms = GetTickCount() - t0;
    f = find_file(dir, id);
    if (code != 0 || f.empty()) {
        FB2K_console_formatter() << "foo_yt: download failed after " << ms << " ms"
                                 << (timed_out ? " (timed out)" : "");
        return "";
    }
    FB2K_console_formatter() << "foo_yt: downloaded " << id.c_str() << " in " << ms << " ms";
    return f;
}

// ---- stream mode (v0.3) ---------------------------------------------------
static bool try_invidious(const std::string& id, std::string& url, abort_callback& abort) {
    for (auto& inst : words(cfg(c_inst))) {
        std::string u = inst + "/latest_version?id=" + id + "&itag=" + cfg(c_itag) + "&local=true";
        try {
            file::ptr f = http_client::get()->create_request("GET")->run(u.c_str(), abort);
            char b[16] = {};
            if (f->read(b, sizeof b, abort) == 0) continue;
            if (b[0] == '<' || b[0] == '{') {
                FB2K_console_formatter() << "foo_yt: " << inst.c_str() << " returned a web page (captcha?), skipping";
                continue;
            }
            url = u;
            return true;
        } catch (exception_aborted) { throw; } catch (...) {}
    }
    return false;
}

static bool try_ytdlp_url(const std::string& id, std::string& url, abort_callback& abort) {
    int secs = atoi(cfg(c_timeout).c_str());
    if (secs < 5) secs = 120;
    std::string cmd = "\"" + cfg(c_exe) + "\" " + cfg(c_args) + " https://youtu.be/" + id;
    const DWORD t0 = GetTickCount();
    bool timed_out = false; DWORD code = 1;
    std::string o = run_capture(cmd, abort, (DWORD)secs * 1000, timed_out, code);
    abort.check();
    const unsigned ms = GetTickCount() - t0;
    size_t n = o.find_first_of("\r\n");
    if (n != std::string::npos) o.resize(n);
    if (o.compare(0, 4, "http") != 0) {
        FB2K_console_formatter() << "foo_yt: yt-dlp gave no URL after " << ms << " ms" << (timed_out ? " (timed out)" : "");
        return false;
    }
    FB2K_console_formatter() << "foo_yt: yt-dlp resolved " << id.c_str() << " in " << ms << " ms";
    url = o;
    return true;
}

// ---- resolver -------------------------------------------------------------
class yt_resolver : public link_resolver {
public:
    bool is_our_path(const char* p, const char*) override {
        std::string id;
        return (strstr(p, "youtube.com/") || strstr(p, "youtu.be/")) && extract_id(p, id);
    }
    void resolve(file::ptr, const char* p, pfc::string_base& out, abort_callback& abort) override {
        std::string id; extract_id(p, id);

        if (cfg(c_mode) == "download") {
            std::string f = download_audio(id, abort);
            if (!f.empty()) { out = ("file://" + f).c_str(); return; }
            // fall through to stream mode on failure
        }

        {
            std::lock_guard<std::mutex> l(g_mx);
            auto it = g_cache.find(id);
            if (it != g_cache.end() && GetTickCount() - it->second.second < CACHE_MS) {
                FB2K_console_formatter() << "foo_yt: using cached stream URL for " << id.c_str();
                out = it->second.first.c_str();
                return;
            }
        }
        std::string url;
        for (auto& b : words(cfg(c_order))) {
            if (b == "invidious" && try_invidious(id, url, abort)) break;
            if (b == "ytdlp" && try_ytdlp_url(id, url, abort)) break;
            abort.check();
        }
        if (url.empty()) throw exception_io_not_found();
        std::string full = url + cfg(c_suffix);
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_cache[id] = { full, GetTickCount() };
        }
        out = full.c_str();
    }
};

static service_factory_single_t<yt_resolver> g_yt;