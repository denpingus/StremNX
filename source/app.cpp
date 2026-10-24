#include "app.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>

#include "http.hpp"
#include "downloads.hpp"
#include "log.hpp"
#include "screens.hpp"
#include "thread.hpp"

// ---------------------------------------------------------------- Config

static const char* kConfigPath = APP_DIR "/config.json";

void Config::load() {
    std::string text;
    if (!readFile(kConfigPath, text)) return;
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok) {
        logMsg("invalid config.json, using defaults");
        return;
    }
    authKey = j["authKey"].str();
    uploadMode = (int)j["uploadMode"].num(1);
    if (j.has("subLang")) subLang = j["subLang"].str();
    subAddons = j["subAddons"].boolean(true);
    subSize = (int)j["subSize"].num(100);
    subPos = (int)j["subPos"].num(100);
    subStyle = (int)j["subStyle"].num(0);
    subColor = (int)j["subColor"].num(0);
    subDelayMs = (int)j["subDelayMs"].num(0);
    subStepMs = (int)j["subStepMs"].num(100);
    volume = (int)j["volume"].num(100);
    dither = j["dither"].boolean(false);
    prefQuality = j["prefQuality"].str("auto");
    lastQuality = j["lastQuality"].str();
    preferSource = (int)j["preferSource"].num(0);
    autoplayNext = j["autoplayNext"].boolean(true);
    resumeSavedStream = j["resumeSavedStream"].boolean(true);
    autoDownload = j["autoDownload"].boolean(false);
    email = j["email"].str();
    server = j["server"].str();
    addons.clear();
    for (auto& a : j["addons"].items())
        if (!a.str().empty()) addons.push_back(normalizeAddonUrl(a.str()));  // fixes ones saved before
    disabledAddons.clear();
    for (auto& a : j["disabledAddons"].items())
        if (!a.str().empty()) disabledAddons.push_back(a.str());
    homeOrder.clear();
    for (auto& a : j["homeOrder"].items()) homeOrder.push_back(a.str());
    homeHidden.clear();
    for (auto& a : j["homeHidden"].items()) homeHidden.push_back(a.str());
}

void Config::save() const {
    Json j = Json::object();
    j.set("authKey", authKey).set("email", email).set("server", server).set("autoDownload", autoDownload);
    j.set("uploadMode", uploadMode);
    j.set("subLang", subLang).set("subAddons", subAddons).set("subSize", subSize).set("subPos", subPos);
    j.set("subStyle", subStyle).set("subColor", subColor).set("subDelayMs", subDelayMs).set("subStepMs", subStepMs);
    j.set("prefQuality", prefQuality).set("lastQuality", lastQuality).set("preferSource", preferSource);
    j.set("autoplayNext", autoplayNext).set("resumeSavedStream", resumeSavedStream).set("volume", volume);
    j.set("dither", dither);
    Json arr = Json::array();
    for (auto& a : addons) arr.push(a);
    j.set("addons", arr);
    Json disabled = Json::array();
    for (auto& a : disabledAddons) disabled.push(a);
    j.set("disabledAddons", disabled);
    Json order = Json::array(), off = Json::array();
    for (auto& k : homeOrder) order.push(k);
    for (auto& k : homeHidden) off.push(k);
    j.set("homeOrder", order).set("homeHidden", off);
    std::string text = j.dump();
    if (!writeFile(kConfigPath, text.data(), text.size())) logMsg("failed to save config");
}

// ---------------------------------------------------------------- App

bool App::init() {
    ensureDir("sdmc:/switch");
    ensureDir(APP_DIR);
    logOpen(APP_DIR "/log.txt");
    watchdogStart();
    logMsg("StremNX starting");

    // The app used to be called "Stremio NX": bring over the login and watch history once.
    for (const char* file : {"config.json", "history.json"}) {
        std::string from = std::string("sdmc:/switch/stremio-nx/") + file, to = std::string(APP_DIR "/") + file;
        std::string data;
        if (!fileExists(to) && readFile(from, data) && writeFile(to, data.data(), data.size()))
            logMsg("init: migrated %s from the old Stremio NX folder", file);
    }

    u64 totalMem = 0, usedMem = 0;
    svcGetInfo(&totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&usedMem, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    AppletType at = appletGetAppletType();
    logMsg("init: applet type %d (%s), memory %llu/%llu MB", (int)at,
           (at == AppletType_Application || at == AppletType_SystemApplication) ? "title mode" : "APPLET MODE",
           (unsigned long long)(usedMem >> 20), (unsigned long long)(totalMem >> 20));

    cfg.load();
    logMsg("init: config ok");
    httpInit();
    logMsg("init: curl ok");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        logMsg("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    logMsg("init: SDL ok");
    requestColorDepth();
    win = SDL_CreateWindow("StremNX", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_W, SCREEN_H,
                           SDL_WINDOW_OPENGL);
    if (!win) {
        logMsg("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    logMsg("init: window ok");
    TTF_Init();
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    logMsg("init: TTF/IMG ok");
    if (!gfx.init(win)) return false;
    in.init();
    dumpSystemFont();
    logMsg("init: input/font file ok");

    history.load();
    library.load();
    logMsg("init: history ok");

    push(makeHomeScreen());
    applyPending();
    reloadAddons();
    logMsg("init: done");
    return true;
}

// mpv/libass needs a font file for the OSD and subtitles.
void App::dumpSystemFont() {
    ensureDir(APP_DIR "/fonts");
    std::string path = APP_DIR "/fonts/system.ttf";
    if (!fileExists(path) && gfx.fontData.address)
        writeFile(path, gfx.fontData.address, gfx.fontData.size);
}

void App::shutdown() {
    // Torrents first: their engines finish writing and close the SD card files, so a download resumes
    // where it stopped.
    std::vector<std::shared_ptr<Torrent>> torrents = downloads;
    if (activeTorrent) torrents.push_back(activeTorrent);
    if (warm) torrents.push_back(warm);
    for (auto& t : torrents) t->stop();
    for (auto& t : torrents) t->waitStopped(3000);

    stack.clear();
    toPush.clear();
    gfx.destroy();
    if (win) SDL_DestroyWindow(win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    // curl and the sockets are left alone: threads loading artwork or catalogs, or asking trackers, can
    // still be in the middle of a request, and tearing those down under them crashed the app on exit
    // ("An error has occurred"). The process exit releases them.
    logClose();
}

void App::push(std::unique_ptr<Screen> s) {
    toPush.push_back(std::move(s));
}

void App::pop() {
    toPop++;
}

void App::applyPending() {
    for (; toPop > 0 && !stack.empty(); toPop--) stack.pop_back();
    toPop = 0;
    for (auto& s : toPush) stack.push_back(std::move(s));
    toPush.clear();
}

void App::toast(const std::string& msg) {
    logMsg("toast: %s", msg.c_str());
    toastMsg = msg;
    toastUntil = SDL_GetTicks() + 3500;
}

void App::requestPlay(const std::string& url, const PlayContext& ctx, bool resume) {
    playUrl = url;
    playCtx = ctx;
    playResume = resume;
}

void App::keepDownloading(const std::shared_ptr<Torrent>& t) {
    if (std::find(downloads.begin(), downloads.end(), t) == downloads.end()) downloads.push_back(t);
    logMsg("downloads: %s keeps downloading in the background", t->infoHash().c_str());
}

void App::keepWarm(const std::shared_ptr<Torrent>& t) {
    if (warm && warm != t) warm->stop();
    warm = t && !t->stopped() ? t : nullptr;
    if (!warm) return;
    warm->yieldNetwork(true);
    warmUntil = SDL_GetTicks() + 5 * 60 * 1000;
}

std::shared_ptr<Torrent> App::takeWarm(const std::string& infoHash, int fileIdx) {
    if (!warm || warm->infoHash() != infoHash || warm->fileIndex() != fileIdx) return nullptr;
    std::shared_ptr<Torrent> t = std::move(warm);
    warm.reset();
    t->yieldNetwork(false);
    return t;
}

std::shared_ptr<Torrent> App::findDownload(const std::string& infoHash, int fileIdx) {
    for (auto& t : downloads)
        if (t->infoHash() == infoHash && t->fileIndex() == fileIdx) return t;
    return nullptr;
}

void App::stopDownload(const std::string& infoHash, int fileIdx) {
    for (auto it = downloads.begin(); it != downloads.end(); ++it) {
        if ((*it)->infoHash() != infoHash || (*it)->fileIndex() != fileIdx) continue;
        (*it)->stop();

        downloads.erase(it);
        return;
    }
}

void App::setInLibrary(const MetaItem& m, bool in) {
    if (in)
        library.add(m);
    else
        library.remove(m.id);
    libraryVersion++;
    if (cfg.authKey.empty()) return;
    auto it = accountItems.find(m.id);
    Json item = libraryItem(it != accountItems.end() ? &it->second : nullptr, m.id, m.type, m.name, m.poster);
    item.set("removed", !in).set("temp", false);
    accountItems[m.id] = item;
    std::string key = cfg.authKey;
    startThread([key, item]() {
        std::string err;
        if (!apiDatastorePut(key, item, err)) logMsg("library: update failed: %s", err.c_str());
    });
}

void App::syncProgress(const PlayContext& ctx, double position, double duration, bool ended) {
    if (cfg.authKey.empty() || ctx.metaId.empty() || duration <= 0) return;
    auto it = accountItems.find(ctx.metaId);
    Json item = libraryItem(it != accountItems.end() ? &it->second : nullptr, ctx.metaId, ctx.type, ctx.name, ctx.poster);
    Json st = item["state"];
    if (!st.isObject()) st = Json::object();
    int64_t posMs = (int64_t)(position * 1000), durMs = (int64_t)(duration * 1000);
    bool finished = ended || position >= duration * 0.94;
    st.set("lastWatched", isoTime((int64_t)time(nullptr)));
    st.set("video_id", ctx.videoId);
    st.set("duration", (double)durMs);
    st.set("timeWatched", (double)std::max<int64_t>((int64_t)st["timeWatched"].num(), posMs));
    st.set("overallTimeWatched", (double)std::max<int64_t>((int64_t)st["overallTimeWatched"].num(), posMs));
    if (finished) {  // like Stremio: a finished video resets the offset and counts as watched
        st.set("timeOffset", 0);
        st.set("timesWatched", st["timesWatched"].num() + 1);
    } else {
        st.set("timeOffset", (double)posMs);
    }
    item.set("state", st);
    accountItems[ctx.metaId] = item;
    std::string key = cfg.authKey;
    startThread([key, item]() {
        std::string err;
        if (!apiDatastorePut(key, item, err)) logMsg("progress sync failed: %s", err.c_str());
        else logMsg("progress synced to the account");
    });
}

void App::onResume() {
    logMsg("resumed after sleep/suspend");
    if (activeTorrent) activeTorrent->wakeUp();
    for (auto& t : downloads) t->wakeUp();
}

void App::hit(int x, int y, int w, int h, int id, int a, int b) {
    hits.push_back(Hit{SDL_Rect{x, y, w, h}, id, a, b});
}

bool App::tapped(int& id, int& a, int& b) {
    int tx, ty;
    if (!in.tap(tx, ty)) return false;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {  // topmost (drawn last) first
        const SDL_Rect& r = it->r;
        if (tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h) {
            id = it->id;
            a = it->a;
            b = it->b;
            return true;
        }
    }
    return false;
}

// The network can be briefly unavailable or busy (Wi-Fi reconnecting after sleep, the console just
// started): one failed request must not leave the app without the account's addons.
template <class F>
static bool withRetries(F attempt) {
    for (int i = 0;; i++) {
        if (attempt()) return true;
        if (i == 2) return false;
        svcSleepThread((i + 1) * 2000000000ULL);  // 2 s, then 4 s
    }
}

void App::reloadAddons() {
    Config c = cfg;
    addonsTask.start([c]() {
        AddonsResult res;
        std::vector<AddonEntry> entries;
        if (!c.authKey.empty()) {
            std::string err;
            if (!withRetries([&] { return apiAddonCollection(c.authKey, entries, err); }))
                res.errors.push_back("Account: " + err);
            std::string libErr;
            if (withRetries([&] { return apiDatastoreGet(c.authKey, res.accountItems, libErr); })) {
                res.accountOk = true;
                res.library = progressFromLibrary(res.accountItems);
                res.libraryItems = libraryFromItems(res.accountItems);
            } else {
                logMsg("library sync failed: %s", libErr.c_str());
            }
        }
        if (entries.empty()) entries.push_back(AddonEntry{CINEMETA_URL, Json()});
        // Stremio ships OpenSubtitles by default; make sure it is there even if the account removed it
        // or nobody is logged in (duplicates are skipped below).
        entries.push_back(AddonEntry{OPENSUBTITLES_URL, Json()});
        for (auto& u : c.addons) entries.push_back(AddonEntry{u, Json()});

        std::set<std::string> seen;
        for (auto& e : entries) {
            std::string url = normalizeAddonUrl(e.url);
            if (!seen.insert(url).second) continue;
            if (std::find(c.disabledAddons.begin(), c.disabledAddons.end(), url) != c.disabledAddons.end()) continue;
            Addon a;
            std::string err;
            if (addonFromManifest(url, e.manifest, a) || withRetries([&] { return fetchManifest(url, a, err); })) {
                logMsg("addon: %s (%s)", a.name.c_str(), url.c_str());
                res.addons.push_back(a);
            } else {
                res.errors.push_back(url + ": " + err);
                res.failed[url] = err;
            }
        }
        return res;
    });
}

// Hints are "KEY Label" pairs separated by three spaces, e.g. "A Open   B Back".
void App::drawFooter(const std::string& hints) {
    gfx.gradientV(0, SCREEN_H - 90, SCREEN_W, 90, col::bg, 0, 235);
    std::vector<std::pair<std::string, std::string>> items;
    size_t start = 0;
    while (start < hints.size()) {
        size_t end = hints.find("   ", start);
        std::string part = hints.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t sp = part.find(' ');
        if (!part.empty()) items.emplace_back(part.substr(0, sp), sp == std::string::npos ? "" : part.substr(sp + 1));
        if (end == std::string::npos) break;
        start = end + 3;
        while (start < hints.size() && hints[start] == ' ') start++;
    }
    int x = SCREEN_W - 40;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        int lw = gfx.textWidth(it->second, 19);
        x -= lw;
        gfx.text(it->second, x, SCREEN_H - 34, 19, col::dim);
        int kw = std::max(26, gfx.textWidth(it->first, 16) + 14);
        x -= kw + 8;
        gfx.roundRect(x, SCREEN_H - 35, kw, 26, 13, col::white.alpha(36));
        gfx.text(it->first, x + kw / 2, SCREEN_H - 31, 16, col::text, 0, AlignCenter);
        x -= 28;
    }
}

void App::drawPointer() {
    int x = 0, y = 0;
    if (!in.pointer(x, y)) return;
    gfx.circle((float)x, (float)y, 12, col::black.alpha(150));
    gfx.circle((float)x, (float)y, 8, col::white);
}

void App::drawSpinner(int cx, int cy, int radius) {
    const int dots = 10;
    float base = (float)(SDL_GetTicks() % 1000) / 1000.0f * dots;
    for (int i = 0; i < dots; i++) {
        float a = (float)i / dots * 2 * (float)M_PI;
        float age = std::fmod(base - i + dots, (float)dots) / dots;  // 0 = newest
        Uint8 alpha = (Uint8)(255 * (1.0f - age * 0.85f));
        gfx.circle(cx + std::cos(a) * radius, cy + std::sin(a) * radius, radius * 0.16f, col::accentLight.alpha(alpha));
    }
}

void App::drawLoading(int x, int y, const std::string& label, Align align) {
    int w = gfx.textWidth(label, 22);
    int sx = align == AlignCenter ? x - (w + 44) / 2 : align == AlignRight ? x - w - 44 : x;
    drawSpinner(sx + 14, y + 13, 12);
    gfx.text(label, sx + 40, y, 22, col::dim);
}

void App::run() {
    uint32_t lastDownloadCheck = 0;
    int64_t lastWall = (int64_t)time(nullptr);
    while (running && appletMainLoop()) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) running = false;

        // The wall clock keeps going while the console sleeps or sits in the HOME menu.
        int64_t wall = (int64_t)time(nullptr);
        if (wall - lastWall > 5) onResume();
        lastWall = wall;

        in.update();
        gfx.beginFrame();
        gfx.pumpImages();

        watchBeat(WatchMain, "main: addons");
        if (addonsTask.ready()) {
            AddonsResult res = addonsTask.take();
            addons = std::move(res.addons);
            addonErrors = std::move(res.failed);
            addonsVersion++;
            if (!res.library.empty()) {
                history.mergeAccount(res.library);
                historyVersion++;
            }
            if (res.accountOk) {
                accountItems.clear();
                for (auto& it : res.accountItems.items()) accountItems[it["_id"].str()] = it;
                library.replace(res.libraryItems);
                libraryVersion++;
            }
            if (!res.errors.empty()) toast(std::to_string(res.errors.size()) + " addon(s) failed: " + res.errors[0]);
        }

        // Background downloads step aside while the app fetches anything for the viewer (catalogs,
        // details, episodes, artwork): browsing comes first. The torrent being watched never does.
        bool browsing = httpForegroundBusy() || gfx.loadingImages();
        for (auto& t : downloads) {
            t->yieldNetwork(t != activeTorrent && browsing);
            t->setWatched(t == activeTorrent);
        }

        if (warm && SDL_GetTicks() > warmUntil) keepWarm(nullptr);  // not reopened: free it

        // Finished or failed background downloads release their peers and memory.
        watchBeat(WatchMain, "main: downloads check");
        if (SDL_GetTicks() - lastDownloadCheck > 1000) {
            lastDownloadCheck = SDL_GetTicks();
            for (auto it = downloads.begin(); it != downloads.end();) {
                if (*it != activeTorrent && !(*it)->keepsDownloading()) {
                    TorrentStatus st = (*it)->status();
                    if (st.complete) toast("Download finished: " + st.fileName.substr(st.fileName.rfind('/') + 1));
                    else if (!st.saveError.empty()) toast("Download stopped: " + st.saveError);
                    (*it)->stop();
                    it = downloads.erase(it);
                } else {
                    ++it;
                }
            }
        }

        watchBeat(WatchMain, "main: screen update");
        if (!stack.empty()) stack.back()->update(*this);
        applyPending();
        if (stack.empty()) running = false;
        if (!running) break;

        if (!playUrl.empty()) {
            std::string url = playUrl;
            PlayContext ctx = playCtx;
            playUrl.clear();
            double startAt = 0;
            const HistoryEntry* h = history.resumePoint(ctx);
            if (playResume && h) startAt = h->position;
            logMsg("playing: %s (start %.0fs)", url.c_str(), startAt);

            watchBeat(WatchMain, "main: starting player");
            gfx.releaseRenderer();
            PlayResult res = runPlayer(*this, url, ctx, startAt);
            watchBeat(WatchMain, "main: back from player");
            if (!gfx.restoreRenderer()) break;

            if (res.duration > 0) {
                double at = res.ended || res.playNext ? res.duration : res.position;
                history.record(ctx, at, res.duration);
                syncProgress(ctx, at, res.duration, res.ended || res.playNext);
                historyVersion++;
            }
            lastWall = (int64_t)time(nullptr);
            if (!res.error.empty()) {
                if (ctx.fromSaved) {  // the remembered link expired or broke: let the viewer pick again
                    toast("The saved stream did not work - pick another one");
                    PlayContext again = ctx;
                    again.fromSaved = false;
                    push(makeStreamsScreen(again, true));
                } else {
                    toast("Playback error: " + res.error);
                }
            } else if (res.playNext) {
                // The finished episode's torrent is done (unless it is still being saved to the SD card).
                if (activeTorrent && !activeTorrent->keepsDownloading()) activeTorrent->stop();
                push(makeNextEpisodeScreen(ctx));
            }
            applyPending();
            continue;
        }

        watchBeat(WatchMain, "main: draw");
        hits.clear();  // rebuilt while drawing; taps next frame are matched against them
        gfx.clear(col::bg);
        stack.back()->draw(*this);
        if (!toastMsg.empty() && SDL_GetTicks() < toastUntil) {
            int w = std::min(gfx.textWidth(toastMsg, 21), SCREEN_W - 160) + 48;
            int x = (SCREEN_W - w) / 2;
            gfx.roundRect(x, SCREEN_H - 120, w, 50, 25, col::cardSel.alpha(245));
            gfx.text(toastMsg, SCREEN_W / 2, SCREEN_H - 107, 21, col::text, SCREEN_W - 160, AlignCenter);
        }
        drawPointer();
        watchBeat(WatchMain, "main: present");
        gfx.present();
        if (gfx.frameCount() == 1) logMsg("run: first frame drawn");
    }
}
