// foo_yt.cpp v0.6 - youtube.com / youtu.be -> playable source.
// Settings: Preferences > Advanced > Tools > YouTube.
// v0.6: "proxy" mode. foobar2000 opens http://127.0.0.1:<port>/<id>.m4a; this
//       component fetches the real googlevideo stream in 4 MiB Range chunks
//       (unranged reads are throttled to ~30 KB/s) and serves only the bytes
//       foobar asks for. Nothing is written to disk. Requires stdafx.h to
//       include <winsock2.h> and <ws2tcpip.h> BEFORE the foobar2000 SDK.
#include "stdafx.h"
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <map>
#include <list>
#include <memory>
#include <atomic>
#include <mutex>
#include <deque>
#include <thread>
#include <functional>
#include <condition_variable>

#pragma comment(lib, "ws2_32.lib")

DECLARE_COMPONENT_VERSION("YouTube resolver", "0.6",
    "Invidious and/or yt-dlp. Configure under Preferences > Advanced > Tools > YouTube.\n"
    "File > Add YouTube links from clipboard: one URL, playlist URL or \"yt: search\" per line.");
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
static const GUID g_c  = YT_GUID(0x0c);
static const GUID g_cl = YT_GUID(0x0d);
static const GUID g_pf = YT_GUID(0x0e);
static const GUID g_sr = YT_GUID(0x0f);
static const GUID g_cmd_add = YT_GUID(0x20);

static advconfig_branch_factory g_branch("YouTube", g_b, advconfig_branch::guid_branch_tools, 0);
static advconfig_string_factory c_mode   ("Playback mode (proxy | download | stream)", g_m, g_b, 1, "proxy");
static advconfig_string_factory c_args   ("yt-dlp STREAM arguments (proxy/stream modes; must print a URL, keep -g and [ext=m4a])", g_a, g_b, 2,
                                          "-g -f bestaudio[ext=m4a]/bestaudio");
static advconfig_string_factory c_dlargs ("yt-dlp DOWNLOAD arguments (download mode; no -g, no -o)", g_da, g_b, 3,
                                          "-f bestaudio[ext=m4a]/bestaudio --no-playlist");
static advconfig_string_factory c_common ("yt-dlp COMMON arguments (appended to EVERY call: cookies, --js-runtimes)", g_c, g_b, 4, "");
static advconfig_string_factory c_exe    ("yt-dlp path", g_e, g_b, 5, "yt-dlp");
static advconfig_string_factory c_timeout("yt-dlp timeout (seconds)", g_x, g_b, 6, "120");
static advconfig_string_factory c_prefetch("Batch add: prefetch the first N videos (0 = off)", g_pf, g_b, 7, "3");
static advconfig_string_factory c_search ("Search results per \"yt: query\" line", g_sr, g_b, 8, "10");
static advconfig_string_factory c_dir    ("Download mode: folder (blank = %TEMP%\\foo_yt; ASCII path)", g_d, g_b, 9, "");
static advconfig_string_factory c_cleanup("Download mode: delete files older than N days (0 = never)", g_cl, g_b, 10, "30");
static advconfig_string_factory c_order  ("Backend order (invidious ytdlp)", g_o, g_b, 11, "ytdlp");
static advconfig_string_factory c_inst   ("Invidious instances (space separated)", g_i, g_b, 12, "https://yewtu.be");
static advconfig_string_factory c_itag   ("Invidious itag (140 = m4a audio)", g_t, g_b, 13, "140");
static advconfig_string_factory c_suffix ("Stream mode: URL suffix (decoder hint)", g_s, g_b, 14, "#.m4a");

static std::string cfg(advconfig_string_factory& f) { pfc::string8 s; f.get(s); return s.c_str(); }

static std::vector<std::string> words(const std::string& s) {
    std::istringstream is(s); std::vector<std::string> v; std::string w;
    while (is >> w) v.push_back(w);
    return v;
}
static std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

// ---- state ----------------------------------------------------------------
static std::mutex g_mx;   // stream-URL cache
static std::map<std::string, std::pair<std::string, DWORD>> g_cache; // id -> (raw url, tick)
static const DWORD CACHE_MS = 5 * 60 * 60 * 1000; // stream URLs expire after ~6 h
static std::mutex g_dl;   // one download at a time

// ---- helpers --------------------------------------------------------------
static const char* ID_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";

static bool extract_id(const char* p, std::string& id) {
    const char* s = strstr(p, "v=");
    if (s) s += 2; else if ((s = strstr(p, "youtu.be/"))) s += 9; else return false;
    id.assign(s, strspn(s, ID_CHARS));
    return id.size() == 11; // also keeps the command lines below injection-safe
}
static bool is_yt(const std::string& s) {
    return s.find("youtube.com/") != std::string::npos || s.find("youtu.be/") != std::string::npos;
}

// value of ?key= / &key= in a URL ("" if absent)
static std::string url_param(const std::string& url, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t p = url.find("?" + k);
    if (p == std::string::npos) p = url.find("&" + k);
    if (p == std::string::npos) return "";
    p += 1 + k.size();
    size_t e = url.find('&', p);
    return url.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
static const char* ext_for(const std::string& url) {
    return url_param(url, "mime").find("webm") != std::string::npos ? "webm" : "m4a";
}

// Run a command with no console window; return stdout. Kills the process on
// timeout or when foobar aborts. stderr is discarded unless with_stderr.
static std::string run_capture(std::string cmd, abort_callback& abort, DWORD timeout_ms,
                               bool& timed_out, DWORD& exit_code, bool with_stderr = false) {
    timed_out = false; exit_code = 1;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) return "";
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w; si.hStdError = with_stderr ? w : nul; si.hStdInput = INVALID_HANDLE_VALUE;
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

static void cleanup_old(const std::string& dir) {
    static bool done = false;
    if (done) return;
    done = true;
    int days = atoi(cfg(c_cleanup).c_str());
    if (days <= 0) return;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now; now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime;
    const ULONGLONG age = (ULONGLONG)days * 24 * 3600 * 10000000ULL;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string n = fd.cFileName;
        if (n.find('.') != 11) continue;
        ULARGE_INTEGER t; t.LowPart = fd.ftLastWriteTime.dwLowDateTime; t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (now.QuadPart > t.QuadPart && now.QuadPart - t.QuadPart > age) DeleteFileA((dir + "\\" + n).c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static std::string download_audio(const std::string& id, abort_callback& abort) {
    const std::string dir = dl_dir();
    std::string f = find_file(dir, id);
    if (!f.empty()) { FB2K_console_formatter() << "foo_yt: using downloaded file for " << id.c_str(); return f; }

    std::unique_lock<std::mutex> lk(g_dl, std::defer_lock);
    while (!lk.try_lock()) { Sleep(50); abort.check(); }
    f = find_file(dir, id);
    if (!f.empty()) return f;
    cleanup_old(dir);

    int secs = atoi(cfg(c_timeout).c_str());
    if (secs < 5) secs = 120;
    std::string cmd = "\"" + cfg(c_exe) + "\" " + cfg(c_dlargs) + " " + cfg(c_common) +
                      " -o \"" + dir + "\\%(id)s.%(ext)s\" https://youtu.be/" + id;
    const DWORD t0 = GetTickCount();
    bool timed_out = false; DWORD code = 1;
    std::string o = run_capture(cmd, abort, (DWORD)secs * 1000, timed_out, code, true);
    abort.check();
    const unsigned ms = GetTickCount() - t0;
    f = find_file(dir, id);
    if (code == 0 && f.empty()) {
        FB2K_console_formatter() << "foo_yt: no file produced after " << ms
                                 << " ms (skipped by a size/duration filter?), falling back";
        return "";
    }
    if (code != 0 || f.empty()) {
        FB2K_console_formatter() << "foo_yt: download failed after " << ms << " ms, exit code " << (unsigned)code
                                 << (timed_out ? " (timed out)" : "");
        while (!o.empty() && (o.back() == '\n' || o.back() == '\r' || o.back() == ' ')) o.pop_back();
        size_t p = o.find_last_of('\n');
        std::string last = (p == std::string::npos) ? o : o.substr(p + 1);
        if (last.size() > 300) last.resize(300);
        if (!last.empty()) FB2K_console_formatter() << "foo_yt: yt-dlp said: " << last.c_str();
        return "";
    }
    FB2K_console_formatter() << "foo_yt: downloaded " << id.c_str() << " in " << ms << " ms";
    return f;
}

// ---- stream URL (used by proxy and stream modes) --------------------------
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
    std::string cmd = "\"" + cfg(c_exe) + "\" " + cfg(c_args) + " " + cfg(c_common) + " https://youtu.be/" + id;
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

// raw stream URL for a video id, cached; "" on failure
static std::string stream_url(const std::string& id, abort_callback& abort) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_cache.find(id);
        if (it != g_cache.end() && GetTickCount() - it->second.second < CACHE_MS) {
            FB2K_console_formatter() << "foo_yt: using cached stream URL for " << id.c_str();
            return it->second.first;
        }
    }
    std::string url;
    for (auto& b : words(cfg(c_order))) {
        if (b == "invidious" && try_invidious(id, url, abort)) break;
        if (b == "ytdlp" && try_ytdlp_url(id, url, abort)) break;
        abort.check();
    }
    if (!url.empty()) {
        std::lock_guard<std::mutex> l(g_mx);
        g_cache[id] = { url, GetTickCount() };
    }
    return url;
}

// ---- local range proxy ----------------------------------------------------
static bool send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = send(s, p, (int)(n > 65536 ? 65536 : n), 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
static void reply_status(SOCKET s, const char* status) {
    char h[160];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    send_all(s, h, (size_t)n);
}

class yt_proxy {
public:
    bool start() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
        m_ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_ls == INVALID_SOCKET) return false;
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = 0; // any free port
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr); // loopback only
        if (bind(m_ls, (sockaddr*)&a, sizeof a) || listen(m_ls, 8)) {
            closesocket(m_ls); m_ls = INVALID_SOCKET; return false;
        }
        int len = sizeof a;
        getsockname(m_ls, (sockaddr*)&a, &len);
        m_port = ntohs(a.sin_port);
        m_acc = std::thread([this] { accept_loop(); });
        FB2K_console_formatter() << "foo_yt: proxy listening on 127.0.0.1:" << (unsigned)m_port.load();
        return true;
    }
    unsigned short port() const { return m_port; }

    void stop() {
        m_ab.set();
        if (m_acc.joinable()) m_acc.join();
        if (m_ls != INVALID_SOCKET) { closesocket(m_ls); m_ls = INVALID_SOCKET; }
        {
            std::lock_guard<std::mutex> l(m_m);
            for (auto& c : m_conns) shutdown(c->s, SD_BOTH); // unblock send/recv
        }
        for (int i = 0; i < 30; ++i) { // wait up to 3 s for connection threads
            reap();
            std::lock_guard<std::mutex> l(m_m);
            if (m_conns.empty()) break;
            Sleep(100);
        }
        {
            std::lock_guard<std::mutex> l(m_m);
            for (auto& c : m_conns) c->t.detach(); // last resort; process is exiting
            m_conns.clear();
        }
        m_port = 0;
        WSACleanup();
    }

private:
    struct conn {
        std::thread t;
        SOCKET s = INVALID_SOCKET;
        std::atomic<bool> done{ false };
    };

    void reap() {
        std::lock_guard<std::mutex> l(m_m);
        for (auto it = m_conns.begin(); it != m_conns.end();) {
            if ((*it)->done) { (*it)->t.join(); it = m_conns.erase(it); }
            else ++it;
        }
    }

    void accept_loop() {
        while (!m_ab.is_aborting()) {
            fd_set fs; FD_ZERO(&fs); FD_SET(m_ls, &fs);
            timeval tv = { 0, 200000 };
            if (select(0, &fs, nullptr, nullptr, &tv) <= 0) { reap(); continue; }
            SOCKET c = accept(m_ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            auto cn = std::make_unique<conn>();
            cn->s = c;
            conn* raw = cn.get();
            cn->t = std::thread([this, raw] {
                try { serve(raw->s); } catch (...) {}
                closesocket(raw->s);
                raw->done = true;
            });
            std::lock_guard<std::mutex> l(m_m);
            m_conns.push_back(std::move(cn));
        }
    }

    void serve(SOCKET s) {
        std::string req;
        char b[1024];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            int n = recv(s, b, sizeof b, 0);
            if (n <= 0) return;
            req.append(b, (size_t)n);
        }
        size_t sp1 = req.find(' ');
        size_t sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return;
        const std::string method = req.substr(0, sp1);
        const std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        if (path.size() < 12 || path[0] != '/') { reply_status(s, "404 Not Found"); return; }
        const std::string id = path.substr(1, 11);
        if (id.find_first_not_of(ID_CHARS) != std::string::npos) { reply_status(s, "404 Not Found"); return; }

        // Range: bytes=a-b | a- | -n
        std::string low = req;
        for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        unsigned long long a = 0, last = ~0ULL, sufn = 0;
        bool ranged = false, suffix = false;
        size_t rp = low.find("\r\nrange:");
        if (rp != std::string::npos) {
            size_t bp = low.find("bytes=", rp);
            if (bp != std::string::npos) {
                bp += 6; ranged = true;
                if (low[bp] == '-') { suffix = true; sufn = strtoull(low.c_str() + bp + 1, nullptr, 10); }
                else {
                    char* e = nullptr;
                    a = strtoull(low.c_str() + bp, &e, 10);
                    if (e && *e == '-' && e[1] >= '0' && e[1] <= '9') last = strtoull(e + 1, nullptr, 10);
                }
            }
        }

        const std::string url = stream_url(id, m_ab);
        if (url.empty()) { reply_status(s, "502 Bad Gateway"); return; }
        const unsigned long long total = strtoull(url_param(url, "clen").c_str(), nullptr, 10);
        if (!total) { FB2K_console_formatter() << "foo_yt: proxy: no clen in stream URL"; reply_status(s, "502 Bad Gateway"); return; }
        if (suffix) { a = sufn >= total ? 0 : total - sufn; last = total - 1; }
        if (last >= total) last = total - 1;
        if (a >= total || a > last) { reply_status(s, "416 Range Not Satisfiable"); return; }

        char cr[96] = "";
        if (ranged) snprintf(cr, sizeof cr, "Content-Range: bytes %llu-%llu/%llu\r\n", a, last, total);
        char h[512];
        int hl = snprintf(h, sizeof h,
            "HTTP/1.1 %s\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\nContent-Length: %llu\r\n%sConnection: close\r\n\r\n",
            ranged ? "206 Partial Content" : "200 OK",
            strcmp(ext_for(url), "webm") == 0 ? "audio/webm" : "audio/mp4",
            last - a + 1, cr);
        if (!send_all(s, h, (size_t)hl)) return;
        FB2K_console_formatter() << "foo_yt: proxy " << id.c_str() << " bytes " << (unsigned)(a / 1024) << "K-" << (unsigned)(last / 1024) << "K";
        if (method == "HEAD") return;

        const unsigned long long CHUNK = 4ULL * 1024 * 1024;
        unsigned long long pos = a;
        char buf[65536];
        while (pos <= last && !m_ab.is_aborting()) {
            const unsigned long long e = (last - pos + 1 > CHUNK) ? pos + CHUNK - 1 : last;
            auto rq = http_client::get()->create_request("GET");
            char rh[80];
            snprintf(rh, sizeof rh, "Range: bytes=%llu-%llu", pos, e);
            rq->add_header(rh);
            file::ptr f = rq->run(url.c_str(), m_ab);
            const unsigned long long want = e - pos + 1;
            unsigned long long got = 0;
            while (got < want) {
                size_t n = f->read(buf, (size_t)((want - got) < sizeof buf ? (want - got) : sizeof buf), m_ab);
                if (!n) break;
                if (!send_all(s, buf, n)) return; // foobar closed the connection
                got += n;
            }
            if (!got) return;
            pos += got;
        }
    }

    SOCKET m_ls = INVALID_SOCKET;
    std::atomic<unsigned short> m_port{ 0 };
    std::thread m_acc;
    std::mutex m_m;
    std::list<std::unique_ptr<conn>> m_conns;
    abort_callback_impl m_ab;
};
static yt_proxy g_proxy;

// ---- resolver -------------------------------------------------------------
class yt_resolver : public link_resolver {
public:
    bool is_our_path(const char* p, const char*) override {
        std::string id;
        return is_yt(p) && extract_id(p, id);
    }
    void resolve(file::ptr, const char* p, pfc::string_base& out, abort_callback& abort) override {
        std::string id; extract_id(p, id);
        const std::string mode = cfg(c_mode);

        if (mode == "download") {
            std::string f = download_audio(id, abort);
            if (!f.empty()) { out = ("file://" + f).c_str(); return; }
            // fall through to stream on failure
        }
        const std::string url = stream_url(id, abort);
        if (url.empty()) throw exception_io_not_found();

        if (mode == "proxy" && g_proxy.port()) {
            out = ("http://127.0.0.1:" + std::to_string(g_proxy.port()) + "/" + id + "." + ext_for(url)).c_str();
            return;
        }
        out = (url + cfg(c_suffix)).c_str();
    }
};
static service_factory_single_t<yt_resolver> g_yt;

// ---- background worker (one thread; stopped cleanly at foobar2000 quit) ----
class yt_worker {
public:
    void start() { m_th = std::thread([this] { loop(); }); }
    void stop()  { m_ab.set(); m_cv.notify_all(); if (m_th.joinable()) m_th.join(); }
    void post(std::function<void()> f) {
        { std::lock_guard<std::mutex> l(m_m); if (m_q.size() >= 50) return; m_q.push_back(std::move(f)); }
        m_cv.notify_one();
    }
    abort_callback& abort() { return m_ab; }
private:
    void loop() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> l(m_m);
                m_cv.wait(l, [&] { return !m_q.empty() || m_ab.is_aborting(); });
                if (m_ab.is_aborting()) return;
                f = std::move(m_q.front()); m_q.pop_front();
            }
            try { f(); } catch (...) {}
        }
    }
    std::thread m_th;
    std::mutex m_m;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_q;
    abort_callback_impl m_ab;
};
static yt_worker g_worker;

class yt_initquit : public initquit {
public:
    void on_init() override {
        if (!g_proxy.start()) FB2K_console_formatter() << "foo_yt: proxy failed to start; proxy mode will use direct streams";
        g_worker.start();
    }
    void on_quit() override { g_worker.stop(); g_proxy.stop(); }
};
static service_factory_single_t<yt_initquit> g_iq;

// ---- batch add from clipboard ---------------------------------------------
static std::string clipboard_text() {
    std::string s;
    if (!OpenClipboard(nullptr)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* w = (const wchar_t*)GlobalLock(h)) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) { s.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr); }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return s;
}

// One clipboard line -> watch URLs.
//   "yt: some words"        -> yt-dlp search (ytsearchN:)
//   youtube playlist URL     -> expanded to its videos
//   single youtube URL       -> passed through
static std::vector<std::string> expand_line(const std::string& line, abort_callback& abort) {
    std::string target;
    if (line.compare(0, 3, "yt:") == 0) {
        std::string q = trim(line.substr(3));
        for (auto& c : q) if (c == '"') c = ' ';
        if (q.empty()) return {};
        int n = atoi(cfg(c_search).c_str());
        if (n < 1) n = 10;
        target = "ytsearch" + std::to_string(n) + ":" + q;
    } else if (is_yt(line) && (line.find("list=") != std::string::npos || line.find("/playlist") != std::string::npos)) {
        if (line.find('"') != std::string::npos) return {};
        target = line;
    } else if (is_yt(line)) {
        return { line };
    } else {
        return {};
    }
    std::string cmd = "\"" + cfg(c_exe) + "\" " + cfg(c_common) + " --flat-playlist --print id \"" + target + "\"";
    bool timed_out = false; DWORD code = 1;
    std::string o = run_capture(cmd, abort, 120 * 1000, timed_out, code);
    std::vector<std::string> urls;
    std::istringstream is(o);
    std::string l;
    while (std::getline(is, l)) {
        l = trim(l);
        if (l.size() == 11 && l.find_first_not_of(ID_CHARS) == std::string::npos)
            urls.push_back("https://www.youtube.com/watch?v=" + l);
    }
    return urls;
}

static void add_from_clipboard() {
    std::vector<std::string> lines;
    {
        std::istringstream is(clipboard_text());
        std::string l;
        while (std::getline(is, l)) { l = trim(l); if (!l.empty()) lines.push_back(l); }
    }
    if (lines.empty()) { FB2K_console_formatter() << "foo_yt: clipboard has no text"; return; }

    g_worker.post([lines] { // expansion runs yt-dlp, so keep it off the UI thread
        abort_callback& ab = g_worker.abort();
        std::vector<std::string> urls;
        for (auto& l : lines) {
            if (ab.is_aborting()) return;
            for (auto& u : expand_line(l, ab)) urls.push_back(u);
        }
        if (urls.empty()) { FB2K_console_formatter() << "foo_yt: nothing to add (no YouTube links or searches found)"; return; }
        FB2K_console_formatter() << "foo_yt: adding " << (unsigned)urls.size() << " item(s)";

        fb2k::inMainThread([urls] {
            pfc::list_t<const char*> loc;
            for (auto& u : urls) loc.add_item(u.c_str());
            playlist_manager::get()->activeplaylist_add_locations(loc, true, core_api::get_main_window());
        });

        // prefetch the first few: download mode fetches the file, other modes
        // just resolve the stream URL so playback skips the ~9 s yt-dlp wait
        int n = atoi(cfg(c_prefetch).c_str());
        const bool dl = cfg(c_mode) == "download";
        for (auto& u : urls) {
            if (n-- <= 0 || ab.is_aborting()) break;
            std::string id;
            if (!extract_id(u.c_str(), id)) continue;
            try { if (dl) download_audio(id, ab); else stream_url(id, ab); } catch (...) { return; }
            for (int i = 0; i < 50 && !ab.is_aborting(); ++i) Sleep(100); // 5 s gap between yt-dlp runs
        }
    });
}

class yt_menu : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 1; }
    GUID get_command(t_uint32) override { return g_cmd_add; }
    void get_name(t_uint32, pfc::string_base& out) override { out = "Add YouTube links from clipboard"; }
    bool get_description(t_uint32, pfc::string_base& out) override {
        out = "One YouTube URL, playlist URL or \"yt: search words\" per clipboard line.";
        return true;
    }
    GUID get_parent() override { return mainmenu_groups::file; }
    void execute(t_uint32, service_ptr_t<service_base>) override { add_from_clipboard(); }
};
static service_factory_single_t<yt_menu> g_menu;