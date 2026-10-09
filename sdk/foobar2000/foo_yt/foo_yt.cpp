// foo_yt.cpp v0.3 - youtube.com / youtu.be -> playable stream URL.
// Settings: Preferences > Advanced > Tools > YouTube.
// v0.3: yt-dlp timeout + abort, 10 min URL cache, console logging.
#include "stdafx.h"
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <map>
#include <mutex>

DECLARE_COMPONENT_VERSION("YouTube resolver", "0.3",
    "Invidious and/or yt-dlp. Configure under Preferences > Advanced > Tools > YouTube.");
VALIDATE_COMPONENT_FILENAME("foo_yt.dll");

// ---- settings -------------------------------------------------------------
static const GUID g_b  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x01 } };
static const GUID g_o  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x02 } };
static const GUID g_i  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x03 } };
static const GUID g_t  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x04 } };
static const GUID g_e  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x05 } };
static const GUID g_a  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x06 } };
static const GUID g_s  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x07 } };
static const GUID g_x  = { 0x6a1c0b52, 0x3e47, 0x4d0a, { 0x9b, 0x21, 0x5f, 0x77, 0x0c, 0xa1, 0x42, 0x08 } };

static advconfig_branch_factory g_branch("YouTube", g_b, advconfig_branch::guid_branch_tools, 0);
static advconfig_string_factory c_order  ("Backend order (space separated: invidious ytdlp)", g_o, g_b, 1, "invidious ytdlp");
static advconfig_string_factory c_inst   ("Invidious instances (space separated, no trailing slash)", g_i, g_b, 2, "https://yewtu.be");
static advconfig_string_factory c_itag   ("Invidious itag (140 = m4a audio)", g_t, g_b, 3, "140");
static advconfig_string_factory c_exe    ("yt-dlp path", g_e, g_b, 4, "yt-dlp");
static advconfig_string_factory c_args   ("yt-dlp arguments (must print a stream URL, e.g. keep -g)", g_a, g_b, 5,
                                          "-g -f bestaudio[ext=m4a]/bestaudio");
static advconfig_string_factory c_suffix ("URL suffix (decoder hint, appended to the stream URL)", g_s, g_b, 6, "#.m4a");
static advconfig_string_factory c_timeout("yt-dlp timeout (seconds)", g_x, g_b, 7, "60");

static std::string cfg(advconfig_string_factory& f) { pfc::string8 s; f.get(s); return s.c_str(); }

static std::vector<std::string> words(const std::string& s) {
    std::istringstream is(s); std::vector<std::string> v; std::string w;
    while (is >> w) v.push_back(w);
    return v;
}

// ---- cache (stream URLs live for hours; foobar may resolve a link repeatedly) ----
static std::mutex g_mx;
static std::map<std::string, std::pair<std::string, DWORD>> g_cache; // id -> (url, tick)
static const DWORD CACHE_MS = 10 * 60 * 1000;

// ---- helpers --------------------------------------------------------------
static bool extract_id(const char* p, std::string& id) {
    const char* s = strstr(p, "v=");
    if (s) s += 2; else if ((s = strstr(p, "youtu.be/"))) s += 9; else return false;
    id.assign(s, strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"));
    return id.size() == 11; // also keeps the command line below injection-safe
}

// run a command with no console window; return stdout. Kills the process on
// timeout or when foobar aborts the operation. stderr is discarded.
static std::string run_capture(std::string cmd, abort_callback& abort, DWORD timeout_ms, bool& timed_out) {
    timed_out = false;
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
            if (!ok || WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) done = true; // one more drain pass
            if (!done && (GetTickCount() - start > timeout_ms || abort.is_aborting())) {
                timed_out = !abort.is_aborting();
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    if (w) CloseHandle(w);
    CloseHandle(r);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return out;
}

static bool try_invidious(const std::string& id, std::string& url, abort_callback& abort) {
    for (auto& inst : words(cfg(c_inst))) {
        std::string u = inst + "/latest_version?id=" + id + "&itag=" + cfg(c_itag) + "&local=true";
        try {
            file::ptr f = http_client::get()->create_request("GET")->run(u.c_str(), abort);
            char b[16] = {};
            if (f->read(b, sizeof b, abort) == 0) continue;
            if (b[0] == '<' || b[0] == '{') { // captcha / error page, not audio
                FB2K_console_formatter() << "foo_yt: " << inst.c_str() << " returned a web page (captcha?), skipping";
                continue;
            }
            url = u;
            return true;
        } catch (exception_aborted) { throw; } catch (...) {}
    }
    return false;
}

static bool try_ytdlp(const std::string& id, std::string& url, abort_callback& abort) {
    int secs = atoi(cfg(c_timeout).c_str());
    if (secs < 5) secs = 60;
    std::string cmd = "\"" + cfg(c_exe) + "\" " + cfg(c_args) + " https://youtu.be/" + id;
    const DWORD t0 = GetTickCount();
    bool timed_out = false;
    std::string o = run_capture(cmd, abort, (DWORD)secs * 1000, timed_out);
    abort.check();
    const DWORD ms = GetTickCount() - t0;
    size_t n = o.find_first_of("\r\n");
    if (n != std::string::npos) o.resize(n);
    if (o.compare(0, 4, "http") != 0) {
        FB2K_console_formatter() << "foo_yt: yt-dlp gave no URL after " << (unsigned)ms << " ms"
                                 << (timed_out ? " (timed out)" : "");
        return false;
    }
    FB2K_console_formatter() << "foo_yt: yt-dlp resolved " << id.c_str() << " in " << (unsigned)ms << " ms";
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
        {
            std::lock_guard<std::mutex> l(g_mx);
            auto it = g_cache.find(id);
            if (it != g_cache.end() && GetTickCount() - it->second.second < CACHE_MS) {
                out = it->second.first.c_str();
                return;
            }
        }
        std::string url;
        for (auto& b : words(cfg(c_order))) {
            if (b == "invidious" && try_invidious(id, url, abort)) break;
            if (b == "ytdlp" && try_ytdlp(id, url, abort)) break;
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

static link_resolver_factory_t<yt_resolver> g_yt;