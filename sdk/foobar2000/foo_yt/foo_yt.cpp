#include "stdafx.h"
// foo_yt.cpp - youtube.com / youtu.be -> playable stream URL.
// Order: Invidious redirect (no parsing), then yt-dlp -g. NewPipe skipped (JVM).
#include <stdio.h>
#include <string>

DECLARE_COMPONENT_VERSION("YouTube resolver", "0.1", "Invidious, then yt-dlp.");
VALIDATE_COMPONENT_FILENAME("foo_yt.dll");

static const char* INSTANCE = "https://yewtu.be"; // change to taste

static bool extract_id(const char* p, std::string& id) {
    const char* s = strstr(p, "v=");
    if (s) s += 2; else if ((s = strstr(p, "youtu.be/"))) s += 9; else return false;
    id.assign(s, strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"));
    return id.size() == 11; // also makes the popen below injection-safe
}

class yt_resolver : public link_resolver {
public:
    bool is_our_path(const char* p, const char*) override {
        std::string id;
        return (strstr(p, "youtube.com/") || strstr(p, "youtu.be/")) && extract_id(p, id);
    }
    void resolve(file::ptr, const char* p, pfc::string_base& out, abort_callback& abort) override {
        std::string id; extract_id(p, id);
        std::string inv = std::string(INSTANCE) + "/latest_version?id=" + id + "&itag=140&local=true";
        try { // probe the instance
            http_client::get()->create_request("GET")->run(inv.c_str(), abort);
            out = inv.c_str();
            return;
        } catch (exception_aborted) { throw; } catch (...) {}
        std::string cmd = "yt-dlp -g -f bestaudio https://youtu.be/" + id;
        if (FILE* f = _popen(cmd.c_str(), "r")) {
            char buf[4096] = {};
            fgets(buf, sizeof buf, f);
            _pclose(f);
            pfc::string8 u(buf); u.skip_trailing_char("\r\n");
            if (!u.is_empty()) { out = u; return; }
        }
        throw exception_io_not_found();
    }
};

static link_resolver_factory_t<yt_resolver> g_yt;