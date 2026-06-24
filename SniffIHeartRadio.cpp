// ─────────────────────────────────────────────────────────────────────────────
// SniffIHeartRadio.cpp  —  standalone iHeart now-playing API prober for RE-MOCT
//
// NOT part of the RE-MOCT build. Compile on demand, point it at an iHeart HLS
// URL, and it walks iHeart's (undocumented) API to discover how to resolve a
// station's now-playing track. It is an ACTIVE PROBER, not a packet sniffer:
// it makes the same HTTP requests the future IHeartRadio module will make and
// dumps every stage's raw + pretty JSON so we can see exactly what iHeart returns.
//
// Two outputs:
//   1. %TEMP%\re-moct-iheartbeat.log        — human-readable trace (for diffing
//                                              when iHeart churns their API)
//   2. %TEMP%\re-moct-iheart-stations.json  — structured identity cache the app
//                                              can later read (zc#### -> id/meta_url)
//
// Build (MSYS2 ucrt64):
//   g++ -std=c++20 SniffIHeartRadio.cpp -o SniffIHeartRadio.exe -lwininet
//   (Uses the nlohmann single-header json.hpp sitting next to this .cpp. If it
//    lives elsewhere, add -I<dir-containing-json.hpp>.)
//
// Usage:
//   SniffIHeartRadio.exe -S https://stream.revma.ihrhls.com/zc4366/hls.m3u8
// ─────────────────────────────────────────────────────────────────────────────

#include <windows.h>
#include <wininet.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <fstream>

#include "json.hpp"        // nlohmann single-header (sits next to this .cpp in E:\code)
using json = nlohmann::json;

// ── Dual logging: stdout + %TEMP%\re-moct-iheartbeat.log ─────────────────────
static std::string g_logpath;

static std::string tempDir() {
    char buf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, buf);   // includes trailing backslash
    if (n == 0 || n > MAX_PATH) return ".\\";
    return std::string(buf, n);
}

static void logf(const char* fmt, ...) {
    char buf[8192];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
    if (!g_logpath.empty()) {
        FILE* f = std::fopen(g_logpath.c_str(), "a");
        if (f) { std::fputs(buf, f); std::fputs("\n", f); std::fclose(f); }
    }
}

static std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    tmv = *std::localtime(&t);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tmv);
    return b;
}

// ── HTTP GET via WinINet ─────────────────────────────────────────────────────
struct HttpResult {
    bool        ok      = false;   // request completed (any status)
    DWORD       status  = 0;       // HTTP status code
    std::string body;              // response body
    std::string final_url;         // post-redirect URL
    DWORD       win_err = 0;       // GetLastError if the request failed to open
};

static HttpResult httpGet(HINTERNET hInet, const std::string& url) {
    HttpResult r;
    // A browser-ish UA + JSON Accept; iHeart's amp API is picky about some of this.
    const char* headers = "Accept: application/json\r\n";
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION |
                  INTERNET_FLAG_SECURE;
    HINTERNET h = InternetOpenUrlA(hInet, url.c_str(), headers, (DWORD)-1L, flags, 0);
    if (!h) { r.win_err = GetLastError(); return r; }

    // Post-redirect URL
    char fbuf[2048]; DWORD flen = sizeof(fbuf);
    if (InternetQueryOptionA(h, INTERNET_OPTION_URL, fbuf, &flen)) r.final_url.assign(fbuf, flen);
    else r.final_url = url;

    // Status code
    DWORD code = 0, clen = sizeof(code), idx = 0;
    if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &clen, &idx))
        r.status = code;

    // Body
    std::string body;
    char chunk[8192]; DWORD got = 0;
    while (InternetReadFile(h, chunk, sizeof(chunk), &got) && got > 0) {
        body.append(chunk, got);
        if (body.size() > 4u * 1024u * 1024u) break;   // sanity cap
    }
    InternetCloseHandle(h);
    r.body = std::move(body);
    r.ok   = true;
    return r;
}

// ── JSON helpers ─────────────────────────────────────────────────────────────
// Recursively find the first string value under any key whose name matches one
// of `keys` (case-insensitive). Returns "" if none found. Lets us spot artist/
// title/name fields regardless of how iHeart nests them.
static std::string lower(std::string s){ for(char&c:s)c=(char)std::tolower((unsigned char)c); return s; }

static std::string findFirstString(const json& j, const std::vector<std::string>& keys) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string k = lower(it.key());
            for (const auto& want : keys)
                if (k == want && it.value().is_string())
                    return it.value().get<std::string>();
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string sub = findFirstString(it.value(), keys);
            if (!sub.empty()) return sub;
        }
    } else if (j.is_array()) {
        for (const auto& e : j) {
            std::string sub = findFirstString(e, keys);
            if (!sub.empty()) return sub;
        }
    }
    return {};
}

// Dump a response: raw (truncated) + pretty JSON if parseable, + heuristic fields.
static void dumpResponse(const HttpResult& r, bool look_for_track) {
    if (!r.ok) { logf("    (request failed to open, WinINet err=%lu)", r.win_err); return; }
    logf("    HTTP %lu   final_url=%s", r.status, r.final_url.c_str());
    logf("    body bytes: %zu", r.body.size());
    // Raw, truncated to keep the log readable
    std::string raw = r.body.substr(0, 1200);
    logf("    raw: %s%s", raw.c_str(), r.body.size() > 1200 ? " ...[truncated]" : "");
    // Try to parse + pretty-print
    json j;
    bool parsed = false;
    try { j = json::parse(r.body); parsed = true; } catch (...) { parsed = false; }
    if (!parsed) { logf("    (body is not valid JSON)"); return; }
    std::string pretty = j.dump(2);
    if (pretty.size() > 3000) pretty = pretty.substr(0, 3000) + "\n    ...[pretty truncated]";
    logf("    pretty:\n%s", pretty.c_str());
    // Heuristic field spotting
    std::string name   = findFirstString(j, {"name","stationname","description","callletters","calllettersleft"});
    if (!name.empty()) logf("    >> spotted station name-ish: \"%s\"", name.c_str());
    if (look_for_track) {
        std::string artist = findFirstString(j, {"artist","artistname","artist_name"});
        std::string title  = findFirstString(j, {"title","tracktitle","songtitle","track_title","song"});
        if (!artist.empty() || !title.empty())
            logf("    >> spotted now-playing: artist=\"%s\" title=\"%s\"",
                 artist.c_str(), title.c_str());
        else
            logf("    >> no artist/title fields spotted");
    }
}

// ── Sidecar identity cache (merge-write) ─────────────────────────────────────
static void writeSidecar(const std::string& zc, long station_id,
                         const std::string& meta_url, const std::string& station_name) {
    std::string path = tempDir() + "re-moct-iheart-stations.json";
    json root = json::object();
    { std::ifstream in(path); if (in) { try { in >> root; } catch (...) { root = json::object(); } } }
    if (!root.is_object()) root = json::object();
    json entry = json::object();
    entry["station_id"]   = station_id;
    entry["meta_url"]     = meta_url;
    if (!station_name.empty()) entry["station_name"] = station_name;
    entry["resolved_at"]  = nowStamp();
    root[zc] = entry;
    std::ofstream out(path, std::ios::trunc);
    if (out) { out << root.dump(2) << "\n"; logf("sidecar written: %s", path.c_str()); }
    else      logf("sidecar WRITE FAILED: %s", path.c_str());
}

// ── Extract the zc#### token and its numeric part from a revma URL ───────────
static bool extractZc(const std::string& url, std::string& zc_out, long& num_out) {
    auto p = url.find("/zc");
    if (p == std::string::npos) return false;
    p += 1;                                  // point at 'z'
    size_t e = p;
    while (e < url.size() && url[e] != '/') ++e;
    zc_out = url.substr(p, e - p);           // e.g. "zc4366"
    std::string digits;
    for (char c : zc_out) if (c >= '0' && c <= '9') digits += c;
    if (digits.empty()) return false;
    num_out = std::strtol(digits.c_str(), nullptr, 10);   // e.g. 4366
    return true;
}

int main(int argc, char** argv) {
    g_logpath = tempDir() + "re-moct-iheartbeat.log";

    if (argc < 3 || std::string(argv[1]) != "-S") {
        std::fprintf(stderr,
            "SniffIHeartRadio - iHeart now-playing API prober\n"
            "Usage: %s -S <iheart-stream-url>\n"
            "Example: %s -S https://stream.revma.ihrhls.com/zc4366/hls.m3u8\n",
            argv[0], argv[0]);
        return 2;
    }
    std::string url = argv[2];

    logf("==================================================================");
    logf("SniffIHeartRadio  run=%s", nowStamp().c_str());
    logf("input url: %s", url.c_str());

    std::string zc; long sid = 0;
    if (!extractZc(url, zc, sid)) {
        logf("ERROR: could not extract a zc#### token from the URL.");
        return 1;
    }
    logf("extracted token: %s   candidate station_id (numeric part): %ld", zc.c_str(), sid);
    logf("NOTE: whether the zc number equals iHeart's station id is exactly what");
    logf("      this probe is here to confirm — watch the [1] liveStations result.");

    HINTERNET hInet = InternetOpenA(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RE-MOCT-Probe/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { logf("FATAL: InternetOpenA failed err=%lu", GetLastError()); return 1; }
    DWORD to = 8000;
    InternetSetOptionA(hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

    // Candidate endpoints, best-guess first. Each is dumped regardless of outcome.
    std::string N = std::to_string(sid);
    struct Probe { std::string label; std::string url; bool track; };
    std::vector<Probe> probes = {
        { "[1] liveStations (validates id + station name)",
          "https://api.iheart.com/api/v2/content/liveStations/" + N, false },
        { "[2] currentTrackMeta (the now-playing)",
          "https://api.iheart.com/api/v3/live-meta/stream/" + N + "/currentTrackMeta", true },
        { "[3] trackHistory (recent songs)",
          "https://api.iheart.com/api/v3/live-meta/stream/" + N + "/trackHistory", true },
        { "[4] currentTrackMeta on us.api host",
          "https://us.api.iheart.com/api/v3/live-meta/stream/" + N + "/currentTrackMeta", true },
        { "[5] track-history (alt path spelling)",
          "https://api.iheart.com/api/v3/live-meta/stream/" + N + "/track-history", true },
    };

    std::string good_meta_url, station_name, np_artist, np_title;
    bool id_confirmed = false;

    for (const auto& pr : probes) {
        logf("------------------------------------------------------------------");
        logf("%s", pr.label.c_str());
        logf("    GET %s", pr.url.c_str());
        HttpResult r = httpGet(hInet, pr.url);
        dumpResponse(r, pr.track);

        if (r.ok && r.status == 200) {
            json j; bool parsed = false;
            try { j = json::parse(r.body); parsed = true; } catch (...) {}
            if (parsed) {
                if (!pr.track) {   // station-details probe
                    std::string nm = findFirstString(j, {"name","description"});
                    if (!nm.empty()) { station_name = nm; id_confirmed = true; }
                } else {           // track probe
                    std::string a = findFirstString(j, {"artist","artistname","artist_name"});
                    std::string t = findFirstString(j, {"title","tracktitle","songtitle","track_title","song"});
                    if ((!a.empty() || !t.empty()) && good_meta_url.empty()) {
                        good_meta_url = pr.url; np_artist = a; np_title = t;
                    }
                }
            }
        }
    }

    logf("==================================================================");
    logf("RESULT SUMMARY");
    logf("  token: %s   numeric id tried: %ld", zc.c_str(), sid);
    logf("  station id confirmed via liveStations: %s", id_confirmed ? "YES" : "no");
    if (!station_name.empty()) logf("  station name: %s", station_name.c_str());
    if (!good_meta_url.empty()) {
        logf("  WORKING now-playing endpoint: %s", good_meta_url.c_str());
        logf("  current now-playing: artist=\"%s\" title=\"%s\"",
             np_artist.c_str(), np_title.c_str());
        writeSidecar(zc, sid, good_meta_url, station_name);
    } else {
        logf("  no now-playing endpoint produced artist/title.");
        logf("  -> none of the guessed endpoints worked as-is. The dumps above");
        logf("     show what each returned; we adjust the candidate list from those.");
    }
    logf("log file: %s", g_logpath.c_str());
    logf("==================================================================");

    InternetCloseHandle(hInet);
    return good_meta_url.empty() ? 1 : 0;
}
