// Video player: libmpv rendering through OpenGL into the same SDL_Window.
// The UI's SDL_Renderer is destroyed before (App::run) and recreated afterwards, so the player's
// controls are drawn by mpv itself as an ASS overlay (osd-overlay), streaming-service style.

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "app.hpp"
#include "http.hpp"
#include "log.hpp"

namespace {

void* getProcAddress(void*, const char* name) {
    return SDL_GL_GetProcAddress(name);
}

SDL_GLContext createContext(SDL_Window* win) {
    SDL_GL_ResetAttributes();
    requestColorDepth();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (ctx) return ctx;
    logMsg("GL 3.3 core failed (%s), trying GLES 3", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    return SDL_GL_CreateContext(win);
}

void setOpt(mpv_handle* mpv, const char* name, const char* value) {
    int rc = mpv_set_option_string(mpv, name, value);
    if (rc < 0) logMsg("mpv: option %s=%s ignored (%s)", name, value, mpv_error_string(rc));
}

void command(mpv_handle* mpv, const char* a, const char* b = nullptr, const char* c = nullptr) {
    const char* args[] = {a, b, c, nullptr};
    mpv_command_async(mpv, 0, args);
}

// Property changes are never read synchronously: mpv_get_property waits for the player core, which
// can be busy for a long time when a stream stalls (a torrent without peers, a slow link), and the
// whole app would freeze with it. Values arrive through MPV_EVENT_PROPERTY_CHANGE instead, and
// writes use the async API.

void setAsync(mpv_handle* mpv, const char* name, double v) {
    mpv_set_property_async(mpv, 0, name, MPV_FORMAT_DOUBLE, &v);
}

void setAsync(mpv_handle* mpv, const char* name, const char* v) {
    mpv_set_property_async(mpv, 0, name, MPV_FORMAT_STRING, &v);
}

struct TrackInfo {
    int64_t id = 0;
    std::string type, lang, title, codec;
    bool selected = false;
};

struct Observed {
    double pos = 0, dur = 0, cacheTime = 0;
    bool paused = false, pausedForCache = false, seeking = false;
    int64_t bufferPct = 0;
    std::string sid, subLang;
    std::vector<TrackInfo> tracks;
};

enum ObsId { ObsPos = 1, ObsDur, ObsPause, ObsPausedForCache, ObsSeeking, ObsBuffer, ObsCacheTime, ObsTracks, ObsSid, ObsSubLang,
             ObsHwdec, ObsCodec,      // only logged
             ObsWidth, ObsGamma };    // pick light rendering for heavy videos

void observeAll(mpv_handle* mpv) {
    mpv_observe_property(mpv, ObsPos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, ObsDur, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, ObsPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, ObsPausedForCache, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, ObsSeeking, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, ObsBuffer, "cache-buffering-state", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, ObsCacheTime, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, ObsTracks, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(mpv, ObsSid, "sid", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, ObsSubLang, "current-tracks/sub/lang", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, ObsHwdec, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, ObsCodec, "video-format", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, ObsWidth, "video-params/w", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, ObsGamma, "video-params/gamma", MPV_FORMAT_STRING);
}

std::vector<TrackInfo> parseTracks(const mpv_node& list) {
    std::vector<TrackInfo> out;
    if (list.format != MPV_FORMAT_NODE_ARRAY) return out;
    for (int i = 0; i < list.u.list->num; i++) {
        const mpv_node& m = list.u.list->values[i];
        if (m.format != MPV_FORMAT_NODE_MAP) continue;
        TrackInfo t;
        for (int k = 0; k < m.u.list->num; k++) {
            const char* key = m.u.list->keys[k];
            const mpv_node& v = m.u.list->values[k];
            std::string s = v.format == MPV_FORMAT_STRING ? v.u.string : "";
            if (!strcmp(key, "id") && v.format == MPV_FORMAT_INT64) t.id = v.u.int64;
            else if (!strcmp(key, "type")) t.type = s;
            else if (!strcmp(key, "lang")) t.lang = s;
            else if (!strcmp(key, "title")) t.title = s;
            else if (!strcmp(key, "codec")) t.codec = s;
            else if (!strcmp(key, "selected") && v.format == MPV_FORMAT_FLAG) t.selected = v.u.flag != 0;
        }
        out.push_back(t);
    }
    return out;
}

// Applies one MPV_EVENT_PROPERTY_CHANGE to the observed state.
void applyProperty(Observed& o, const mpv_event* ev) {
    auto* p = static_cast<mpv_event_property*>(ev->data);
    bool has = p->format != MPV_FORMAT_NONE && p->data;
    switch (ev->reply_userdata) {
        case ObsPos: if (has) o.pos = *(double*)p->data; break;
        case ObsDur: o.dur = has ? *(double*)p->data : 0; break;
        case ObsPause: o.paused = has && *(int*)p->data; break;
        case ObsPausedForCache: o.pausedForCache = has && *(int*)p->data; break;
        case ObsSeeking: o.seeking = has && *(int*)p->data; break;
        case ObsBuffer: o.bufferPct = has ? *(int64_t*)p->data : 0; break;
        case ObsCacheTime: o.cacheTime = has ? *(double*)p->data : 0; break;
        case ObsTracks: o.tracks = has ? parseTracks(*(mpv_node*)p->data) : std::vector<TrackInfo>(); break;
        case ObsSid: o.sid = has ? *(char**)p->data : ""; break;
        case ObsSubLang: o.subLang = has ? *(char**)p->data : ""; break;
    }
}

// "sdmc:/x" is how libnx names SD card files, but mpv may not take that for a local path; a plain
// "/x" reaches the same file through the default (SD card) device.
std::string mpvPath(const std::string& p) {
    return p.compare(0, 6, "sdmc:/") == 0 ? p.substr(5) : p;
}

std::string clock(double s) {
    if (s < 0 || std::isnan(s)) s = 0;
    int t = (int)s;
    char buf[32];
    if (t >= 3600)
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else
        snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

// The gyro pointer is drawn straight into each frame after mpv, so it moves at the display rate (the
// OSD is re-rendered by mpv at its own pace). A disc of scissored clears, one per row: no shaders needed.
void drawPointerGl(float x, float y, int w, int h) {
    using Scissor = void (*)(int, int, int, int);
    using Cap = void (*)(unsigned);
    using ClearColor = void (*)(float, float, float, float);
    static auto scissor = (Scissor)SDL_GL_GetProcAddress("glScissor");
    static auto enable = (Cap)SDL_GL_GetProcAddress("glEnable");
    static auto disable = (Cap)SDL_GL_GetProcAddress("glDisable");
    static auto clearColor = (ClearColor)SDL_GL_GetProcAddress("glClearColor");
    static auto clear = (Cap)SDL_GL_GetProcAddress("glClear");
    if (!scissor || !enable || !disable || !clearColor || !clear) return;
    const unsigned kScissorTest = 0x0C11, kColorBit = 0x4000;
    float s = w / 1280.0f;  // 1280x720 coordinates to the drawable (1920x1080 docked)
    int cx = (int)(x * s), cy = h - (int)(y * s);  // GL counts rows from the bottom
    enable(kScissorTest);
    for (auto [radius, shade] : {std::pair<float, float>{12, 0.08f}, {8, 1.0f}}) {
        clearColor(shade, shade, shade, 1);
        int r = (int)(radius * s);
        for (int dy = -r; dy < r; dy++) {
            int half = (int)std::sqrt((float)r * r - (dy + 0.5f) * (dy + 0.5f));
            scissor(cx - half, cy + dy, 2 * half, 1);
            clear(kColorBit);
        }
    }
    disable(kScissorTest);
}

// ---------------------------------------------------------------- torrent:// protocol

struct TorrentCookie {
    std::shared_ptr<Torrent> t;
    int64_t pos = 0;
    std::atomic<bool> cancel{false};
};

int64_t torrentRead(void* c, char* buf, uint64_t n) {
    auto* s = static_cast<TorrentCookie*>(c);
    int64_t r = s->t->read(s->pos, buf, (int64_t)n, s->cancel);
    if (r > 0) s->pos += r;
    return r;
}

int64_t torrentSeek(void* c, int64_t off) {
    auto* s = static_cast<TorrentCookie*>(c);
    if (off < 0 || off > s->t->fileSize()) return MPV_ERROR_GENERIC;
    s->pos = off;
    return off;
}

int64_t torrentSize(void* c) {
    return static_cast<TorrentCookie*>(c)->t->fileSize();
}

void torrentClose(void* c) {
    delete static_cast<TorrentCookie*>(c);
}

void torrentCancel(void* c) {
    static_cast<TorrentCookie*>(c)->cancel = true;
}

int torrentOpen(void* userData, char*, mpv_stream_cb_info* info) {
    auto* torrent = static_cast<std::shared_ptr<Torrent>*>(userData);
    if (!*torrent || !(*torrent)->hasMetadata()) return MPV_ERROR_LOADING_FAILED;
    auto* c = new TorrentCookie;
    c->t = *torrent;
    info->cookie = c;
    info->read_fn = torrentRead;
    info->seek_fn = torrentSeek;
    info->size_fn = torrentSize;
    info->close_fn = torrentClose;
    info->cancel_fn = torrentCancel;
    return 0;
}

std::string torrentLine(Torrent& t) {
    TorrentStatus st = t.status();
    char buf[128];
    snprintf(buf, sizeof(buf), "%d peers  ·  %.1f MB/s  ·  %.0f MB downloaded", st.peersConnected,
             st.speed / (1024 * 1024), (double)st.downloaded / (1024 * 1024));
    return buf;
}

// ---------------------------------------------------------------- ASS drawing

// Builds "ass-events" for osd-overlay on a 1280x720 canvas.
class Ass {
public:
    explicit Ass(const std::string& font) : fn(font) {}
    std::string out;

    static std::string bgr(Color c) {
        char b[16];
        snprintf(b, sizeof(b), "&H%02X%02X%02X&", c.b, c.g, c.r);
        return b;
    }
    static std::string alpha(Uint8 a) {  // ASS alpha is inverted: 00 opaque, FF transparent
        char b[8];
        snprintf(b, sizeof(b), "&H%02X&", 255 - a);
        return b;
    }

    void shape(const std::string& path, Color c) {
        out += "{\\an7\\pos(0,0)\\bord0\\shad0\\1c" + bgr(c) + "\\1a" + alpha(c.a) + "\\p1}" + path + "{\\p0}\n";
    }

    void rect(float x, float y, float w, float h, Color c) {
        char b[160];
        snprintf(b, sizeof(b), "m %.1f %.1f l %.1f %.1f %.1f %.1f %.1f %.1f", x, y, x + w, y, x + w, y + h, x, y + h);
        shape(b, c);
    }

    void roundRect(float x, float y, float w, float h, float r, Color c) {
        r = std::min(r, std::min(w, h) / 2);
        const float k = 0.4477f * r;  // control-point inset for a circular corner
        char b[512];
        snprintf(b, sizeof(b),
                 "m %.1f %.1f l %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f l %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f "
                 "l %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f l %.1f %.1f b %.1f %.1f %.1f %.1f %.1f %.1f",
                 x + r, y, x + w - r, y, x + w - k, y, x + w, y + k, x + w, y + r, x + w, y + h - r, x + w, y + h - k,
                 x + w - k, y + h, x + w - r, y + h, x + r, y + h, x + k, y + h, x, y + h - k, x, y + h - r, x, y + r, x,
                 y + k, x + k, y, x + r, y);
        shape(b, c);
    }

    void circle(float cx, float cy, float r, Color c) { roundRect(cx - r, cy - r, 2 * r, 2 * r, r, c); }

    void poly(std::initializer_list<float> pts, Color c) {
        std::string p;
        char b[32];
        size_t i = 0;
        for (auto it = pts.begin(); it != pts.end(); it += 2, i++) {
            snprintf(b, sizeof(b), "%s %.1f %.1f ", i == 0 ? "m" : (i == 1 ? "l" : ""), *it, *(it + 1));
            p += b;
        }
        shape(p, c);
    }

    // Layers stacked from the strong edge, each a little shorter: strips laid side by side leave a
    // visible line wherever their antialiased edges meet (or overlap).
    void gradientV(float y, float h, Color c, Uint8 a0, Uint8 a1, int steps = 18) {
        float sh = h / steps, covered = 0;
        for (int i = 0; i < steps; i++) {  // weakest band first
            int band = a1 > a0 ? i : steps - 1 - i;
            float want = (a0 + (a1 - a0) * (band + 0.5f) / steps) / 255.0f;
            float layer = 1 - (1 - want) / (1 - covered);  // opacity that brings `covered` up to `want`
            covered = want;
            float top = a1 > a0 ? y + band * sh : y, bottom = a1 > a0 ? y + h : y + (band + 1) * sh;
            rect(0, top, 1280, bottom - top, c.alpha((Uint8)std::lround(layer * 255)));
        }
    }

    // an: ASS numpad alignment of (x, y): 7 top-left, 4 middle-left, 5 center, 6 middle-right, 9 top-right...
    void text(float x, float y, int size, Color c, int an, const std::string& s, bool shadow = true) {
        char b[256];
        snprintf(b, sizeof(b), "{\\an%d\\pos(%.1f,%.1f)\\fs%d\\bord0\\shad%d\\4c&H000000&\\4a&H90&\\1c%s\\1a%s", an, x,
                 y, size, shadow ? 1 : 0, bgr(c).c_str(), alpha(c.a).c_str());
        out += b;
        if (!fn.empty()) out += "\\fn" + fn;
        out += "}" + escape(s) + "\n";
    }

    void pill(float x, float cy, const std::string& key) {  // controller button hint
        float w = std::max(28.0f, key.size() * 12.0f + 14);
        roundRect(x, cy - 14, w, 28, 14, col::white.alpha(50));
        text(x + w / 2, cy, 17, col::white, 5, key, false);
    }

    void spinner(float cx, float cy, float r) {
        const int dots = 10;
        float base = (float)(SDL_GetTicks() % 1000) / 1000.0f * dots;
        for (int i = 0; i < dots; i++) {
            float a = (float)i / dots * 2 * (float)M_PI;
            float age = std::fmod(base - i + dots, (float)dots) / dots;
            circle(cx + std::cos(a) * r, cy + std::sin(a) * r, r * 0.16f,
                   col::accentLight.alpha((Uint8)(255 * (1.0f - age * 0.85f))));
        }
    }

    void lockIcon(float cx, float cy, float s, Color c) {
        rect(cx - s * 0.55f, cy - s * 1.1f, s * 0.22f, s * 0.9f, c);  // shackle
        rect(cx + s * 0.33f, cy - s * 1.1f, s * 0.22f, s * 0.9f, c);
        rect(cx - s * 0.55f, cy - s * 1.1f, s * 1.1f, s * 0.22f, c);
        roundRect(cx - s * 0.8f, cy - s * 0.3f, s * 1.6f, s * 1.2f, s * 0.2f, c);  // body
    }

    void playIcon(float x, float cy, float s, Color c) { poly({x, cy - s, x, cy + s, x + s * 1.7f, cy}, c); }
    void pauseIcon(float x, float cy, float s, Color c) {
        rect(x, cy - s, s * 0.6f, s * 2, c);
        rect(x + s, cy - s, s * 0.6f, s * 2, c);
    }

private:
    std::string fn;
    static std::string escape(const std::string& s) {
        std::string o;
        for (char ch : s) {
            if (ch == '{') o += '(';
            else if (ch == '}') o += ')';
            else if (ch == '\\') o += '/';
            else if (ch == '\n') o += "\\N";
            else o += ch;
        }
        return o;
    }
};

struct Track {
    int64_t id = 0;
    std::string label;
    bool selected = false;
};

std::vector<Track> loadTracks(const Observed& obs, const std::string& type) {
    std::vector<Track> out;
    for (auto& i : obs.tracks) {
        if (i.type != type) continue;
        Track t;
        t.id = i.id;
        t.label = !i.title.empty() ? i.title : !i.lang.empty() ? i.lang : "Track " + std::to_string(t.id);
        if (!i.lang.empty() && !i.title.empty() && i.title.find(i.lang) == std::string::npos) t.label += "  (" + i.lang + ")";
        if (type == "audio" && !i.codec.empty()) t.label += "  ·  " + i.codec;
        t.selected = i.selected;
        out.push_back(t);
    }
    return out;
}

struct SubFile {
    std::string path, title, lang;
};

// Asks every subtitles addon, keeps the preferred language and downloads up to 6 files to the SD
// card (mpv then loads them like local files, so no TLS is needed inside mpv).
std::vector<SubFile> fetchAddonSubs(const std::vector<Addon>& addons, const std::string& type,
                                    const std::string& videoId, const std::string& langCode) {
    const SubLanguage& lang = subLanguage(langCode);
    std::vector<SubtitleTrack> found;
    for (auto& a : addons) {
        std::string err;
        for (auto& t : fetchSubtitles(a, type, videoId, err))
            if (subLangMatches(lang, t.lang)) found.push_back(t);
    }
    ensureDir(APP_DIR "/subs");
    std::vector<SubFile> out;
    std::vector<std::string> seen;
    for (auto& t : found) {
        if (out.size() >= 6) break;
        if (std::find(seen.begin(), seen.end(), t.url) != seen.end()) continue;
        seen.push_back(t.url);
        std::string body, err;
        if (!httpGet(t.url, body, err, 20) || body.size() < 16) continue;
        std::string low = t.url;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        const char* ext = low.find(".vtt") != std::string::npos   ? ".vtt"
                          : low.find(".ass") != std::string::npos ? ".ass"
                          : low.find(".ssa") != std::string::npos ? ".ssa"
                                                                  : ".srt";
        SubFile f;
        f.path = std::string(APP_DIR "/subs/sub_") + std::to_string(out.size()) + ext;
        if (!writeFile(f.path, body.data(), body.size())) continue;
        f.title = std::string(lang.label) + "  ·  " + t.addonName + " " + std::to_string(out.size() + 1);
        f.lang = t.lang;
        out.push_back(f);
    }
    logMsg("subtitles: %zu found in %s, %zu downloaded", found.size(), lang.label, out.size());
    return out;
}

}  // namespace

// ---------------------------------------------------------------- player

PlayResult runPlayer(App& app, const std::string& url, const PlayContext& ctx, double startAt) {
    PlayResult res;
    SDL_Window* win = app.win;
    std::string font = app.gfx.fontFamily();
    SDL_GLContext gl = createContext(win);
    if (!gl) {
        res.error = std::string("SDL_GL_CreateContext: ") + SDL_GetError();
        return res;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);
    // mpv dithers for the framebuffer's color depth: tell it what the system really gave us.
    int rb = 0, gb = 0, bb = 0;
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &rb);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &gb);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &bb);
    int fbDepth = std::min({rb, gb, bb});
    logMsg("player: framebuffer %d/%d/%d bits", rb, gb, bb);
    if (fbDepth <= 0) fbDepth = 8;

    mpv_handle* mpv = mpv_create();
    if (!mpv) {
        SDL_GL_DeleteContext(gl);
        res.error = "mpv_create failed";
        return res;
    }
    setOpt(mpv, "vo", "libmpv");
    setOpt(mpv, "hwdec", "auto");
    setOpt(mpv, "dither", app.cfg.dither ? "fruit" : "no");  // see Config::dither
    setOpt(mpv, "cache", "yes");
    setOpt(mpv, "demuxer-max-bytes", "48MiB");
    setOpt(mpv, "demuxer-max-back-bytes", "8MiB");
    setOpt(mpv, "network-timeout", "30");
    setOpt(mpv, "user-agent", "StremNX/0.3.1");
    setOpt(mpv, "keep-open", "no");
    setOpt(mpv, "volume-max", "200");
    // The console plays stereo. Left to "auto", 5.1 tracks reach the audio output as 6 channels and the
    // center one (where the dialogue is) can be lost: voices vanish and the movie sounds quiet.
    // Downmixing in mpv folds the center into both sides.
    setOpt(mpv, "audio-channels", "stereo");
    setOpt(mpv, "volume", std::to_string(app.cfg.volume).c_str());
    setOpt(mpv, "osd-level", "1");
    setOpt(mpv, "osd-bar", "no");
    setOpt(mpv, "osd-on-seek", "no");
    // Subtitle preferences from Settings
    const Config& cfg = app.cfg;
    const SubLanguage& subLang = subLanguage(cfg.subLang);
    std::string slang;
    for (auto& m : subLang.match) slang += (slang.empty() ? "" : ",") + m;
    if (slang.empty())
        setOpt(mpv, "sid", "no");
    else
        setOpt(mpv, "slang", slang.c_str());
    setOpt(mpv, "alang", "en,eng,ja,jpn");
    setOpt(mpv, "sub-fonts-dir", APP_DIR "/fonts");
    setOpt(mpv, "osd-fonts-dir", APP_DIR "/fonts");
    setOpt(mpv, "sub-font-size", "46");
    setOpt(mpv, "sub-border-size", "2.5");
    char num[32];
    snprintf(num, sizeof(num), "%.2f", cfg.subSize / 100.0);
    setOpt(mpv, "sub-scale", num);
    setOpt(mpv, "sub-pos", std::to_string(cfg.subPos).c_str());
    setOpt(mpv, "sub-color", cfg.subColor ? "#FFE600" : "#FFFFFF");
    // Styled subtitles (ASS/SSA, common in series) bring their own color, box and size: the Settings
    // style applies to every subtitle instead, so all videos look the same.
    setOpt(mpv, "sub-ass-override", "force");
    if (cfg.subStyle == 1) {
        if (mpv_set_option_string(mpv, "sub-border-style", "background-box") < 0)
            setOpt(mpv, "sub-border-style", "opaque-box");
        setOpt(mpv, "sub-back-color", "#C0000000");
    }
    snprintf(num, sizeof(num), "%.3f", cfg.subDelayMs / 1000.0);
    setOpt(mpv, "sub-delay", num);
    if (!font.empty()) {
        setOpt(mpv, "osd-font", font.c_str());
        setOpt(mpv, "sub-font", font.c_str());
    }
    setOpt(mpv, "force-media-title", ctx.name.c_str());
    if (startAt > 0) setOpt(mpv, "start", std::to_string((int)startAt).c_str());
    mpv_request_log_messages(mpv, "warn");

    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        SDL_GL_DeleteContext(gl);
        res.error = "mpv_initialize failed";
        return res;
    }
    const bool isTorrent = url.compare(0, 10, "torrent://") == 0 && app.activeTorrent;
    if (isTorrent) mpv_stream_cb_add_ro(mpv, "torrent", &app.activeTorrent, torrentOpen);

    mpv_opengl_init_params glInit{getProcAddress, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context* rctx = nullptr;
    if (mpv_render_context_create(&rctx, mpv, params) < 0) {
        mpv_terminate_destroy(mpv);
        SDL_GL_DeleteContext(gl);
        res.error = "mpv_render_context_create failed";
        return res;
    }

    appletSetMediaPlaybackState(true);  // keep the console from sleeping during playback
    command(mpv, "loadfile", mpvPath(url).c_str());

    // Subtitles from addons (OpenSubtitles...): fetched and downloaded in the background, then added
    // to mpv once the file is loaded.
    Task<std::vector<SubFile>> subsTask;
    if (cfg.subAddons && !slang.empty() && !ctx.videoId.empty()) {
        std::string type = ctx.type.empty() ? "movie" : ctx.type, vid = ctx.videoId, code = cfg.subLang;
        std::vector<Addon> cands;
        for (auto& a : app.addons)
            if (addonSupports(a, "subtitles", type, vid)) cands.push_back(a);
        if (!cands.empty()) subsTask.start([cands, type, vid, code]() { return fetchAddonSubs(cands, type, vid, code); });
    }
    bool fileLoaded = false;
    int64_t videoW = 0;  // video width and transfer (pq / hlg = HDR), to spot heavy videos
    std::string videoGamma;
    bool lightRendering = false;
    Observed obs;
    observeAll(mpv);
    double subDelay = cfg.subDelayMs / 1000.0, subScale = cfg.subSize / 100.0;
    std::vector<SubFile> addedSubs;  // re-added after a reconnect

    // UI state
    bool quit = false, started = false;
    uint32_t lastInput = SDL_GetTicks(), lastDraw = 0;
    double pos = startAt, dur = 0, cacheAhead = 0;
    bool paused = false, buffering = false;
    int64_t bufferPct = 0;
    std::string flash;  // "+10s", "Subtitles off"...
    bool flashRight = true;
    uint32_t flashAt = 0;
    int seekAcc = 0;
    std::string lastSubId = "auto";
    std::string lastAss;
    bool scrubbing = false;  // finger on the progress bar
    double scrubFrac = 0;
    int sideTap = 0;  // -1 left, 1 right: a second quick tap there skips 10 s
    uint32_t sideTapAt = 0;
    // Locked: touch and buttons do nothing (no accidental skips). Any input shows a badge; tapping it
    // twice, or pressing - twice, unlocks. Locks by itself after 30 s without input while playing.
    bool locked = false, unlockArmed = false;
    bool hintTouch = false;  // the badge explains the touch way or the controller way, whichever was used
    uint32_t lockHintAt = 0, lockPressAt = 0;  // controller: any two quick presses unlock

    // Menu: 0 audio, 1 subtitles, 2 playback options
    bool menuOpen = false;
    int menuCol = 0, menuSel[3] = {0, 0, 0};
    std::vector<Track> audio, subs;
    static const double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    static const char* aspects[] = {"Auto", "16:9", "4:3", "2.35:1", "Fill screen"};
    static const double zooms[] = {0, 0.1, 0.2, 0.3};
    static const char* zoomNames[] = {"Off", "+7%", "+15%", "+23%"};
    static const int volumes[] = {100, 125, 150, 200};
    int speedIdx = 2, aspectIdx = 0, zoomIdx = 0, volumeIdx = 0;
    for (int i = 0; i < 4; i++)
        if (volumes[i] == app.cfg.volume) volumeIdx = i;

    // Up next (series)
    std::string nextId = !ctx.nextVideoId.empty() ? ctx.nextVideoId : guessNextEpisode(ctx.videoId, false);
    std::string nextLabel = !ctx.nextEpisodeLabel.empty() ? ctx.nextEpisodeLabel : episodeLabelFromId(nextId);
    bool upNextOn = ctx.type == "series" && cfg.autoplayNext && !nextId.empty();
    bool upShown = false, upCancelled = false;
    uint32_t upShownAt = 0;

    // Reconnecting after sleep / network drops
    int retries = 0;
    bool reconnecting = false;
    uint32_t reconnectAt = 0;
    int64_t lastWall = (int64_t)time(nullptr);

    auto openMenu = [&]() {
        audio = loadTracks(obs, "audio");
        subs = loadTracks(obs, "sub");
        Track off;
        off.id = -1;
        off.label = "Off";
        off.selected = std::none_of(subs.begin(), subs.end(), [](const Track& t) { return t.selected; });
        subs.insert(subs.begin(), off);
        menuSel[0] = menuSel[1] = menuSel[2] = 0;
        for (int i = 0; i < (int)audio.size(); i++)
            if (audio[i].selected) menuSel[0] = i;
        for (int i = 0; i < (int)subs.size(); i++)
            if (subs[i].selected) menuSel[1] = i;
        menuCol = audio.empty() ? 1 : 0;
        menuOpen = true;
        scrubbing = false;
    };

    auto optionLabel = [&](int i) -> std::string {
        char b[48];
        switch (i) {
            case 0: snprintf(b, sizeof(b), "Speed: %gx", speeds[speedIdx]); return b;
            case 1: return std::string("Aspect: ") + aspects[aspectIdx];
            case 2: return std::string("Zoom: ") + zoomNames[zoomIdx];
            case 3: snprintf(b, sizeof(b), "Volume: %d%%", volumes[volumeIdx]); return b;
            default: return std::string("Dithering: ") + (app.cfg.dither ? "On" : "Off");
        }
    };

    auto changeOption = [&](int i, int dir) {
        switch (i) {
            case 0: {
                speedIdx = (speedIdx + dir + 6) % 6;
                double v = speeds[speedIdx];
                setAsync(mpv, "speed", v);
                break;
            }
            case 1: {
                aspectIdx = (aspectIdx + dir + 5) % 5;
                double pan = aspectIdx == 4 ? 1.0 : 0.0;  // "fill" crops the sides instead of letterboxing
                setAsync(mpv, "panscan", pan);
                setAsync(mpv, "video-aspect-override", aspectIdx == 0 || aspectIdx == 4 ? "no" : aspects[aspectIdx]);
                break;
            }
            case 2: {
                zoomIdx = (zoomIdx + dir + 4) % 4;
                double z = zooms[zoomIdx];
                setAsync(mpv, "video-zoom", z);
                break;
            }
            case 3: {
                volumeIdx = (volumeIdx + dir + 4) % 4;
                double v = volumes[volumeIdx];
                setAsync(mpv, "volume", v);
                break;
            }
            case 4:  // live, to compare on the same scene: dithering hides banding but can speckle dark areas
                app.cfg.dither = !app.cfg.dither;
                app.cfg.save();
                setAsync(mpv, "dither", app.cfg.dither ? "fruit" : "no");
                break;
        }
    };

    auto applyTrack = [&](int col, int idx) {
        std::vector<Track>& list = col == 0 ? audio : subs;
        if (idx < 0 || idx >= (int)list.size()) return;
        Track& t = list[idx];
        std::string id = t.id < 0 ? "no" : std::to_string(t.id);
        setAsync(mpv, col == 0 ? "aid" : "sid", id.c_str());
        for (auto& x : list) x.selected = &x == &t;
        if (col == 1 && t.id >= 0) lastSubId = id;
    };

    auto seekBy = [&](int secs) {
        uint32_t now = SDL_GetTicks();
        bool chain = now - flashAt < 900 && ((secs > 0) == flashRight) && !flash.empty() && flash[0] != 'S';
        seekAcc = chain ? seekAcc + secs : secs;
        std::string s = std::to_string(secs);
        command(mpv, "seek", s.c_str());
        flash = (seekAcc > 0 ? "+" : "") + std::to_string(seekAcc) + "s";
        flashRight = secs > 0;
        flashAt = now;
    };

    // Menu geometry (shared by drawing and touch)
    const float colX[3] = {130, 520, 910};
    auto menuItemY = [](int i, int first) { return 210.0f + (i - first) * 44; };
    auto menuFirst = [&](int c) { return std::max(0, menuSel[c] - 8); };
    auto menuCount = [&](int c) { return c == 0 ? (int)audio.size() : c == 1 ? (int)subs.size() : 5; };

    // Watching over the network comes first: background downloads wait (not when playing from the SD card).
    bool fromSd = url.compare(0, 5, "sdmc:") == 0;
    for (auto& t : app.downloads) t->yieldNetwork(t != app.activeTorrent && !fromSd);
    while (!quit && appletMainLoop()) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) quit = true;

        watchBeat(WatchMain, "player: input");
        Input& in = app.in;
        in.dragKeys = menuOpen;  // a swipe on the video would otherwise seek 10 s at a time
        in.pointerEnabled = !locked;  // locked means locked: no pointer either
        in.update();
        uint32_t now = SDL_GetTicks();

        // Back from sleep / HOME menu: pause, and let network users reconnect.
        int64_t wall = (int64_t)time(nullptr);
        if (wall - lastWall > 5) {
            app.onResume();
            command(mpv, "set", "pause", "yes");
            flash = "Paused while the console was asleep";
            flashAt = now;
            lastInput = now;
        }
        lastWall = wall;

        bool anyInput = in.pressed(K_A | K_B | K_X | K_Y | K_UP | K_DOWN | K_LEFT | K_RIGHT | K_ZL | K_ZR | K_L |
                                   K_R | K_MINUS | K_PLUS | K_LSTICK);
        int tx = 0, ty = 0;
        bool tap = in.tap(tx, ty);
        int ptrX = 0, ptrY = 0;
        bool pointing = in.pointer(ptrX, ptrY);
        if (pointing) lastInput = now;  // like a mouse: pointing shows the controls
        bool controlsVisible = now - lastInput < 4000 || paused;
        if (anyInput || tap) lastInput = now;

        if (locked) {
            bool badge = now - lockHintAt < 3000;
            bool onBadge = tap && badge && tx >= 420 && tx < 900 && ty < 110;
            // Any physical button unlocks (twice, quickly), the D-pad included; tilting a stick only shows
            // how, so brushing one never unlocks.
            bool button = in.pressed(K_A | K_B | K_X | K_Y | K_L | K_R | K_ZL | K_ZR | K_PLUS | K_MINUS | K_LSTICK) ||
                          in.dpadPressed();
            if (!badge) unlockArmed = false;
            if (button) {  // controller: any button twice, quickly (either Joy-Con)
                if (now - lockPressAt < 400) locked = false;
                lockPressAt = now;
                hintTouch = false;
            } else if (onBadge) {  // touch: the badge, twice
                if (unlockArmed) locked = false;
                unlockArmed = !unlockArmed;
                hintTouch = true;
            } else if (tap) {
                hintTouch = true;
            } else if (anyInput) {
                hintTouch = false;
            }
            lockHintAt = anyInput || tap ? now : lockHintAt;
            if (!locked) {
                unlockArmed = false;
                lastInput = now;  // show the controls again
            }
        } else if (menuOpen) {
            if (tap) {  // touch: pick an item, or tap outside the columns to close
                bool hitItem = false;
                for (int c = 0; c < 3; c++) {
                    if (tx < colX[c] - 20 || tx > colX[c] + 340) continue;
                    int first = menuFirst(c);
                    for (int i = first; i < menuCount(c) && i < first + 10; i++) {
                        float y = menuItemY(i, first);
                        if (ty >= y - 6 && ty < y + 38) {
                            menuCol = c;
                            menuSel[c] = i;
                            if (c == 2) changeOption(i, 1);
                            else applyTrack(c, i);
                            hitItem = true;
                        }
                    }
                }
                if (!hitItem && ty > 140 && ty < 620) menuOpen = false;
            }
            if (in.pressed(K_B) || in.pressed(K_X)) menuOpen = false;
            if (in.pressed(K_LEFT) && menuCol > 0) menuCol = menuCol == 1 && audio.empty() ? 1 : menuCol - 1;
            if (in.pressed(K_RIGHT) && menuCol < 2) menuCol++;
            if (in.pressed(K_UP) && menuSel[menuCol] > 0) menuSel[menuCol]--;
            if (in.pressed(K_DOWN) && menuSel[menuCol] + 1 < menuCount(menuCol)) menuSel[menuCol]++;
            if (in.pressed(K_A)) {
                if (menuCol == 2) changeOption(menuSel[2], 1);
                else applyTrack(menuCol, menuSel[menuCol]);
            }
            if (in.pressed(K_Y) && menuCol == 2) changeOption(menuSel[2], -1);
        } else if (upShown) {  // "Up next" card takes A / B
            bool playNow = in.pressed(K_A) || (tap && tx >= 880 && tx < 1050 && ty >= 560 && ty < 610);
            bool cancel = in.pressed(K_B) || (tap && tx >= 1060 && tx < 1210 && ty >= 560 && ty < 610);
            if (playNow) {
                res.playNext = true;
                quit = true;
            } else if (cancel) {
                upShown = false;
                upCancelled = true;
            }
        } else {
            if (in.pressed(K_B) || in.pressed(K_PLUS)) quit = true;
            if (in.pressed(K_A)) command(mpv, "cycle", "pause");
            if (in.pressed(K_LEFT)) seekBy(-10);
            if (in.pressed(K_RIGHT)) seekBy(10);
            if (in.pressed(K_ZL)) seekBy(-60);
            if (in.pressed(K_ZR)) seekBy(60);
            if (in.pressed(K_X)) openMenu();
            if (in.pressed(K_Y)) {
                std::string sid = obs.sid;
                bool off = sid == "no" || sid.empty();
                setAsync(mpv, "sid", off ? lastSubId.c_str() : "no");
                if (!off) lastSubId = sid;
                flash = off ? "Subtitles on" : "Subtitles off";
                flashAt = now;
            }
            if (in.pressed(K_LSTICK)) command(mpv, "script-binding", "stats/display-stats-toggle");
            if (in.pressed(K_MINUS) && started) {
                locked = true;
                unlockArmed = hintTouch = false;
                lockHintAt = now;
            }

            // Touch: first tap shows the controls; then the bar seeks (tap or drag, applied on release),
            // the buttons row works like the buttons, the sides skip and the middle pauses.
            int fx = 0, fy = 0, fsx = 0, fsy = 0;
            bool finger = in.touch(fx, fy, fsx, fsy), scrubEnded = false;
            if (finger && !scrubbing && controlsVisible && started && dur > 0 && fsy >= 585 && fsy < 648 &&
                fsx >= 40 && fsx < 1140)
                scrubbing = true;
            if (scrubbing) {
                lastInput = now;
                if (finger) {
                    scrubFrac = std::max(0.0, std::min(1.0, (fx - 60) / 1060.0));
                } else {
                    std::string to = std::to_string((int)(dur * scrubFrac));
                    const char* args[] = {"seek", to.c_str(), "absolute", nullptr};
                    mpv_command_async(mpv, 0, args);
                    scrubbing = false;
                    scrubEnded = true;
                }
            }
            // Sides: a double tap skips 10 s (each further quick tap adds 10 more); one tap does nothing.
            int side = tap && started && ty >= 140 && ty < 585 ? (tx < 400 ? -1 : tx > 880 ? 1 : 0) : 0;
            if (side) {
                if (side == sideTap && now - sideTapAt < 400) seekBy(10 * side);
                sideTap = side;
                sideTapAt = now;
            }
            if (tap && controlsVisible && started && !scrubEnded) {
                if (ty < 120 && tx < 420) {
                    quit = true;  // title / back area
                } else if (ty < 120 && tx >= 1110) {
                    locked = true;
                    unlockArmed = false;
                    hintTouch = true;
                    lockHintAt = now;
                } else if (ty >= 585 && ty < 648 && tx >= 40 && tx < 1140 && dur > 0) {  // bar (pointer click)
                    std::string to = std::to_string((int)(dur * std::max(0.0, std::min(1.0, (tx - 60) / 1060.0))));
                    const char* args[] = {"seek", to.c_str(), "absolute", nullptr};
                    mpv_command_async(mpv, 0, args);
                } else if (ty >= 648) {  // buttons row
                    if (tx < 150) command(mpv, "cycle", "pause");
                    else if (tx < 213) seekBy(-10);
                    else if (tx < 270) seekBy(10);
                    else if (tx >= 840 && tx < 1080) openMenu();
                    else if (tx >= 1140) quit = true;
                } else if (ty >= 140 && ty < 585 && !side) {
                    command(mpv, "cycle", "pause");
                }
            }
        }

        // Subtitle timing (L/R, also inside the menu) and size (ZL/ZR inside the menu)
        if (!locked && (in.pressed(K_L) || in.pressed(K_R))) {
            subDelay += (in.pressed(K_R) ? 1 : -1) * cfg.subStepMs / 1000.0;
            if (std::fabs(subDelay) < 0.001) subDelay = 0;
            setAsync(mpv, "sub-delay", subDelay);
            char b[48];
            snprintf(b, sizeof(b), "Subtitle delay %+.2f s", subDelay);
            flash = b;
            flashAt = now;
        }
        if (menuOpen && (in.pressed(K_ZL) || in.pressed(K_ZR))) {
            subScale = std::max(0.5, std::min(2.5, subScale + (in.pressed(K_ZR) ? 0.1 : -0.1)));
            setAsync(mpv, "sub-scale", subScale);
        }

        // Addon subtitles arrived: add them, and pick one if nothing in the preferred language is on.
        if (fileLoaded && subsTask.ready()) {
            std::vector<SubFile> files = subsTask.take();
            std::string cur = obs.subLang;
            bool select = cur.empty() || !subLangMatches(subLang, cur);
            for (size_t i = 0; i < files.size(); i++) {
                std::string mp = mpvPath(files[i].path);
                const char* args[] = {"sub-add", mp.c_str(), i == 0 && select ? "select" : "auto",
                                      files[i].title.c_str(), files[i].lang.c_str(), nullptr};
                mpv_command_async(mpv, 0, args);
            }
            addedSubs = files;
            if (!files.empty() && select) {
                flash = "Subtitles: " + files[0].title;
                flashAt = now;
            } else if (files.empty() && select) {
                flash = std::string("No ") + subLang.label + " subtitles found for this video";
                flashAt = now;
            }
        }

        for (;;) {
            mpv_event* ev = mpv_wait_event(mpv, 0);
            if (ev->event_id == MPV_EVENT_NONE) break;
            if (ev->event_id == MPV_EVENT_SHUTDOWN) quit = true;
            if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) started = true;
            if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                applyProperty(obs, ev);
                auto* p = static_cast<mpv_event_property*>(ev->data);
                if ((ev->reply_userdata == ObsHwdec || ev->reply_userdata == ObsCodec) && p->format == MPV_FORMAT_STRING)
                    logMsg("player: %s = %s", p->name, *(char**)p->data);  // which decoder a video really got
                if (ev->reply_userdata == ObsWidth && p->format == MPV_FORMAT_INT64) videoW = *(int64_t*)p->data;
                if (ev->reply_userdata == ObsGamma && p->format == MPV_FORMAT_STRING) videoGamma = *(char**)p->data;
                // 4K or HDR only: light scaling and HDR handling. mpv's high quality downscaling and HDR peak
                // detection made those stutter on the Tegra GPU, but on everything else (1080p is downscaled
                // too in handheld) the light filters make film grain and fine texture look noisy.
                if (!lightRendering && (videoW > 2560 || videoGamma == "pq" || videoGamma == "hlg")) {
                    lightRendering = true;
                    command(mpv, "apply-profile", "fast");
                    logMsg("player: heavy video (%lld px wide, %s): light rendering", (long long)videoW, videoGamma.c_str());
                }
            }
            if (ev->event_id == MPV_EVENT_FILE_LOADED) {
                fileLoaded = true;
                if (retries > 0)  // a reconnect reloaded the file: put the addon subtitles back
                    for (auto& f : addedSubs) {
                        std::string mp = mpvPath(f.path);
                        const char* args[] = {"sub-add", mp.c_str(), "auto", f.title.c_str(), f.lang.c_str(), nullptr};
                        mpv_command_async(mpv, 0, args);
                    }
            }
            if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
                auto* m = static_cast<mpv_event_log_message*>(ev->data);
                logMsg("mpv [%s] %s: %s", m->level, m->prefix, m->text);
            }
            if (ev->event_id == MPV_EVENT_END_FILE) {
                auto* ef = static_cast<mpv_event_end_file*>(ev->data);
                if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                    if (started && retries < 3) {  // dropped connection (sleep, Wi-Fi): reload where we were
                        retries++;
                        reconnecting = true;
                        reconnectAt = now + 1500 * retries;
                        started = false;
                        logMsg("player: playback error (%s), reconnecting (%d)", mpv_error_string(ef->error), retries);
                        continue;
                    }
                    res.error = mpv_error_string(ef->error);
                }
                if (ef->reason == MPV_END_FILE_REASON_EOF) {
                    res.ended = true;
                    if (upNextOn && !upCancelled) res.playNext = true;
                }
                if (ef->reason != MPV_END_FILE_REASON_STOP || !reconnecting) quit = true;
            }
        }
        if (reconnecting && now >= reconnectAt) {
            reconnecting = false;
            std::string at = std::to_string((int)pos);
            setAsync(mpv, "start", at.c_str());
            command(mpv, "loadfile", mpvPath(url).c_str());
        }

        watchBeat(WatchMain, "player: state");
        pos = obs.pos > 0 || started ? obs.pos : pos;
        dur = obs.dur;
        paused = obs.paused;
        buffering = obs.pausedForCache || obs.seeking;
        bufferPct = obs.bufferPct;
        cacheAhead = obs.cacheTime - pos;

        // Up next: the card appears in the last 20 s and counts down from 10.
        if (upNextOn && !upCancelled && !upShown && started && dur > 60 && dur - pos <= 20 && !menuOpen) {
            upShown = true;
            upShownAt = now;
            locked = false;  // the card needs A / B
        }
        if (!started) locked = false;  // the loading screen (reconnecting) must take B
        if (!locked && started && !paused && !buffering && !menuOpen && !upShown && !scrubbing &&
            now - lastInput > 30000) {
            locked = true;
            unlockArmed = false;
        }
        int countdown = upShown ? 10 - (int)((now - upShownAt) / 1000) : 10;
        if (upShown && countdown <= 0) {
            res.playNext = true;
            quit = true;
        }

        // ------------------------------------------------ overlay
        watchBeat(WatchMain, "player: overlay");
        if (now - lastDraw > 66) {
            lastDraw = now;
            bool controls = (now - lastInput < 4000 || paused) && !locked;
            bool showFlash = !flash.empty() && now - flashAt < 900;
            // Informational messages stay up longer than the quick seek / delay feedback.
            uint32_t flashLife = (flash.rfind("Paused", 0) == 0 || flash.rfind("Subtitles:", 0) == 0 || flash.rfind("No ", 0) == 0) ? 3500 : 900;
            showFlash = !flash.empty() && now - flashAt < flashLife;
            Ass a(font);
            if (!started) {
                // Loading screen
                a.rect(0, 0, 1280, 720, col::black.alpha(255));
                a.text(640, 300, 40, col::white, 5, ctx.name);
                if (!ctx.episodeLabel.empty()) a.text(640, 350, 24, col::dim, 5, ctx.episodeLabel);
                a.spinner(640, 430, 26);
                std::string sub = retries > 0 ? "Reconnecting..." : startAt > 0 ? "Resuming at " + clock(startAt) : "Loading";
                a.text(640, 492, 22, col::dim, 5, sub);
                if (isTorrent) a.text(640, 530, 19, col::faint, 5, torrentLine(*app.activeTorrent));
                a.text(640, 670, 18, col::faint, 5, "B  Cancel");
            } else if (menuOpen) {
                a.rect(0, 0, 1280, 720, col::black.alpha(205));
                const char* heads[3] = {"Audio", "Subtitles", "Playback"};
                for (int c = 0; c < 3; c++) {
                    a.text(colX[c], 140, 32, c == menuCol ? col::white : col::dim, 7, heads[c]);
                    int first = menuFirst(c), n = menuCount(c);
                    for (int i = first; i < n && i < first + 10; i++) {
                        float y = menuItemY(i, first);
                        bool focused = c == menuCol && i == menuSel[c];
                        bool selected = c == 0 ? audio[i].selected : c == 1 ? subs[i].selected : false;
                        std::string label = c == 0 ? audio[i].label : c == 1 ? subs[i].label : optionLabel(i);
                        if (focused) a.roundRect(colX[c] - 20, y - 6, 360, 40, 10, col::white.alpha(36));
                        if (selected)
                            a.poly({colX[c] - 4, y + 12, colX[c] + 2, y + 20, colX[c] + 14, y + 4, colX[c] + 14, y + 9,
                                    colX[c] + 2, y + 25, colX[c] - 4, y + 17},
                                   col::accentLight);
                        if (label.size() > 30) label = label.substr(0, 28) + "...";
                        a.text(colX[c] + 26, y, 22, focused ? col::white : selected ? col::text : col::dim, 7, label);
                    }
                    if (n == 0) a.text(colX[c], 210, 22, col::faint, 7, "None");
                }
                if (subsTask.active()) a.text(520, 600, 18, col::faint, 7, "Looking for more subtitles...");
                char line[64];
                a.pill(60, 660, "A");
                a.text(100, 660, 18, col::dim, 4, menuCol == 2 ? "Change" : "Select");
                a.pill(200, 660, "B");
                a.text(240, 660, 18, col::dim, 4, "Close");
                a.pill(540, 660, "L/R");
                snprintf(line, sizeof(line), "Subtitle delay  %+.2f s", subDelay);
                a.text(600, 660, 18, col::white, 4, line);
                a.pill(880, 660, "ZL/ZR");
                snprintf(line, sizeof(line), "Subtitle size  %d%%", (int)std::lround(subScale * 100));
                a.text(958, 660, 18, col::white, 4, line);
            } else {
                if (controls) {
                    a.gradientV(0, 170, col::black, 190, 0);
                    a.gradientV(440, 280, col::black, 0, 215);
                    a.poly({60, 50, 72, 40, 72, 60}, col::white);  // back arrow
                    a.pill(1130, 50, "-");
                    a.lockIcon(1196, 52, 12, col::white);
                    a.text(86, 50, 30, col::white, 4, ctx.name);
                    if (!ctx.episodeLabel.empty()) a.text(86, 90, 21, col::dim, 4, ctx.episodeLabel);

                    // Progress bar: played (accent), buffered (light), rest (dim)
                    float bx = 60, bw = 1060, by = 616;
                    double at = scrubbing ? dur * scrubFrac : pos;  // while dragging: where the finger is
                    float p = dur > 0 ? (float)std::min(1.0, at / dur) : 0;
                    float b = dur > 0 ? (float)std::min(1.0, (pos + std::max(0.0, cacheAhead)) / dur) : 0;
                    a.roundRect(bx, by, bw, 6, 3, col::white.alpha(70));
                    a.roundRect(bx, by, bw * b, 6, 3, col::white.alpha(120));
                    a.roundRect(bx, by, std::max(6.0f, bw * p), 6, 3, col::accent);
                    a.circle(bx + bw * p, by + 3, scrubbing ? 13 : 9, col::white);
                    a.text(1220, by + 3, 22, col::white, 6, dur > 0 ? "-" + clock(dur - at) : clock(at));
                    a.text(bx, by - 30, scrubbing ? 24 : 18, scrubbing ? col::white : col::dim, 1,
                           clock(at) + (dur > 0 ? "  /  " + clock(dur) : ""));
                    if (speedIdx != 2) a.text(bx + 1060, by - 30, 18, col::accentLight, 3, optionLabel(0));

                    // Buttons row
                    float ry = 668, x = 60;
                    if (paused)
                        a.playIcon(x, ry, 11, col::white);
                    else
                        a.pauseIcon(x, ry, 11, col::white);
                    a.pill(x + 34, ry, "A");
                    x += 110;
                    a.poly({x + 14, ry - 10, x + 14, ry + 10, x, ry}, col::white);  // rewind
                    a.text(x + 22, ry, 18, col::white, 4, "10");
                    a.poly({x + 70, ry - 10, x + 70, ry + 10, x + 84, ry}, col::white);  // forward
                    a.text(x + 48, ry, 18, col::white, 4, "10");
                    a.text(x + 96, ry, 17, col::dim, 4, "Left / Right");
                    a.pill(860, ry, "X");
                    a.text(898, ry, 19, col::white, 4, "Audio, subtitles & more");
                    a.pill(1150, ry, "B");
                    a.text(1188, ry, 19, col::white, 4, "Back");
                }
                if (locked && now - lockHintAt < 3000) {
                    // At the top: a second tap there can't hit the progress bar.
                    a.roundRect(420, 30, 520, 64, 32, col::black.alpha(200));
                    a.lockIcon(466, 64, 12, unlockArmed ? col::accentLight : col::white);
                    a.text(498, 62, 21, col::white, 4,
                           hintTouch ? (unlockArmed ? "Tap again to unlock" : "Locked  -  tap here twice to unlock")
                                     : "Locked  -  press any button twice, quickly");
                }
                if (paused && !buffering) {
                    a.circle(640, 360, 46, col::black.alpha(140));
                    a.playIcon(626, 360, 20, col::white);
                }
                if (buffering) {
                    a.spinner(640, 350, 28);
                    a.text(640, 410, 22, col::white, 5,
                           bufferPct > 0 && bufferPct < 100 ? "Buffering " + std::to_string(bufferPct) + "%" : "Buffering");
                    if (isTorrent) a.text(640, 444, 18, col::dim, 5, torrentLine(*app.activeTorrent));
                }
                if (showFlash) {
                    bool side = flash[0] == '+' || flash[0] == '-';
                    float fx = !side ? 640.0f : flashRight ? 980.0f : 300.0f;
                    uint32_t life = flashLife;
                    Uint8 fa = (Uint8)(255 * std::min(1.0f, (float)(life - (now - flashAt)) / 300.0f));
                    float fw = side ? 120.0f : std::max(260.0f, flash.size() * 13.0f);
                    a.roundRect(fx - fw / 2, 330, fw, 60, 30, col::black.alpha((Uint8)(fa * 0.6f)));
                    a.text(fx, 360, 26, col::white.alpha(fa), 5, flash, false);
                }
                if (upShown) {  // Up next card
                    float cx = 860, cy = 440, cw = 360, ch = 190;
                    a.roundRect(cx, cy, cw, ch, 16, col::black.alpha(215));
                    a.text(cx + 24, cy + 22, 18, col::accentLight, 7, "Up next");
                    std::string lbl = nextLabel.size() > 34 ? nextLabel.substr(0, 32) + "..." : nextLabel;
                    a.text(cx + 24, cy + 50, 24, col::white, 7, lbl);
                    a.text(cx + 24, cy + 88, 18, col::dim, 7, "Playing in " + std::to_string(std::max(1, countdown)) + " s");
                    a.roundRect(cx + 24, cy + 116, (cw - 48) * std::max(0, countdown) / 10.0f, 4, 2, col::accent);
                    a.roundRect(cx + 20, cy + 130, 170, 44, 22, col::white);
                    a.playIcon(cx + 40, cy + 152, 8, col::black);
                    a.text(cx + 62, cy + 152, 19, col::black, 4, "Play now", false);
                    a.roundRect(cx + 200, cy + 130, 140, 44, 22, col::white.alpha(50));
                    a.text(cx + 270, cy + 152, 19, col::white, 5, "Cancel", false);
                }
            }
            if (a.out != lastAss) {
                lastAss = a.out;
                if (a.out.empty()) {
                    const char* args[] = {"osd-overlay", "1", "none", "", nullptr};
                    mpv_command_async(mpv, 0, args);
                } else {
                    const char* args[] = {"osd-overlay", "1", "ass-events", a.out.c_str(), "1280", "720", nullptr};
                    mpv_command_async(mpv, 0, args);
                }
            }
        }

        watchBeat(WatchMain, "player: render");
        mpv_render_context_update(rctx);
        int w = SCREEN_W, h = SCREEN_H;
        SDL_GL_GetDrawableSize(win, &w, &h);
        mpv_opengl_fbo fbo{0, w, h, 0};
        int flipY = 1;
        mpv_render_param rp[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_DEPTH, &fbDepth},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        disableGlDither();  // every frame: mpv owns the GL state while it renders
        mpv_render_context_render(rctx, rp);
        if (pointing) drawPointerGl((float)ptrX, (float)ptrY, w, h);
        watchBeat(WatchMain, "player: swap");
        SDL_GL_SwapWindow(win);
        mpv_render_context_report_swap(rctx);
    }

    app.in.dragKeys = true;
    app.in.pointerEnabled = true;

    // A size and a volume picked in the player become the new defaults.
    int newSize = (int)std::lround(subScale * 100);
    if (newSize != app.cfg.subSize || volumes[volumeIdx] != app.cfg.volume) {
        app.cfg.subSize = newSize;
        app.cfg.volume = volumes[volumeIdx];
        app.cfg.save();
    }

    res.position = pos;  // last values mpv reported (asking now could block on a stalled stream)
    res.duration = dur;
    logMsg("player: stopped at %.0f / %.0f s%s", res.position, res.duration, res.ended ? " (ended)" : "");

    appletSetMediaPlaybackState(false);
    mpv_render_context_free(rctx);
    watchBeat(WatchMain, "player: shutting down mpv");
    mpv_terminate_destroy(mpv);
    SDL_GL_DeleteContext(gl);
    return res;
}
