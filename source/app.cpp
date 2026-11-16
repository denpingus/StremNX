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
    saveDownloads = j["saveDownloads"].boolean();
    uploadMode = (int)j["uploadMode"].num(1);
    if (j.has("subLang")) subLang = j["subLang"].str();
    subAddons = j["subAddons"].boolean(true);
    subSize = (int)j["subSize"].num(100);
    subPos = (int)j["subPos"].num(100);
    subStyle = (int)j["subStyle"].num(0);
    subColor = (int)j["subColor"].num(0);
    subDelayMs = (int)j["subDelayMs"].num(0);
    subStepMs = (int)j["subStepMs"].num(100);
    prefQuality = j["prefQuality"].str("auto");
    lastQuality = j["lastQuality"].str();
    preferSource = (int)j["preferSource"].num(0);
    autoplayNext = j["autoplayNext"].boolean(true);
    resumeSavedStream = j["resumeSavedStream"].boolean(true);
    checkUpdates = j["checkUpdates"].boolean(true);
    email = j["email"].str();
    server = j["server"].str();
    addons.clear();
    for (auto& a : j["addons"].items())
        if (!a.str().empty()) addons.push_back(a.str());
}

void Config::save() const {
    Json j = Json::object();
    j.set("authKey", authKey).set("email", email).set("server", server).set("saveDownloads", saveDownloads);
    j.set("uploadMode", uploadMode);
    j.set("subLang", subLang).set("subAddons", subAddons).set("subSize", subSize).set("subPos", subPos);
    j.set("subStyle", subStyle).set("subColor", subColor).set("subDelayMs", subDelayMs).set("subStepMs", subStepMs);
    j.set("prefQuality", prefQuality).set("lastQuality", lastQuality).set("preferSource", preferSource);
    j.set("autoplayNext", autoplayNext).set("resumeSavedStream", resumeSavedStream).set("checkUpdates", checkUpdates);
    Json arr = Json::array();
    for (auto& a : addons) arr.push(a);
    j.set("addons", arr);
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
    if (cfg.checkUpdates) checkForUpdates(false);
    logMsg("init: done (version " APP_VERSION ")");
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
    stack.clear();
    toPush.clear();
    gfx.destroy();
    if (win) SDL_DestroyWindow(win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
    httpCleanup();
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

void App::reloadAddons() {
    Config c = cfg;
    addonsTask.start([c]() {
        AddonsResult res;
        std::vector<AddonEntry> entries;
        if (!c.authKey.empty()) {
            std::string err;
            if (!apiAddonCollection(c.authKey, entries, err)) res.errors.push_back("Account: " + err);
            std::string libErr;
            if (apiDatastoreGet(c.authKey, res.accountItems, libErr)) {
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
            Addon a;
            std::string err;
            if (addonFromManifest(url, e.manifest, a) || fetchManifest(url, a, err)) {
                logMsg("addon: %s (%s)", a.name.c_str(), url.c_str());
                res.addons.push_back(a);
            } else {
                res.errors.push_back(url + ": " + err);
            }
        }
        return res;
    });
}

// ---------------------------------------------------------------- updates

void App::checkForUpdates(bool manual) {
    if (updateTask.active()) return;
    updateManual = manual;
    updateTask.start([]() { return checkForUpdate(); });
}

void App::installUpdate() {
    if (!update.available || (updateDl && !updateDl->finished)) return;
    updateDl = startUpdateDownload(update);
    updateDlReported = false;
}

void App::restartAfterUpdate() {
    std::string nro = updateDl ? updateDl->path : updaterSelfPath();
    if (!updaterRelaunch(nro)) logMsg("update: cannot relaunch, the viewer has to reopen the app");
    quit();
}

// Release notes are Markdown: drop the heading marks and keep the first lines.
static std::string plainNotes(const std::string& md) {
    std::string out;
    size_t start = 0;
    int lines = 0;
    while (start < md.size() && lines < 7) {
        size_t end = md.find('\n', start);
        std::string line = md.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? md.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t i = 0;
        while (i < line.size() && (line[i] == '#' || line[i] == ' ')) i++;
        line = line.substr(i);
        std::string clean;
        for (char c : line)
            if (c != '*' && c != '`') clean += c;
        if (clean.empty()) continue;
        if (clean[0] == '-') clean = "\xE2\x80\xA2" + clean.substr(1);
        out += (out.empty() ? "" : "\n") + clean;
        lines++;
    }
    return out;
}

void App::updateUpdatePopup() {
    bool installing = updateDl && !updateDl->finished;
    bool installed = updateDl && updateDl->finished && updateDl->ok;
    int buttons = installing ? 0 : 2;
    if (in.pressed(K_LEFT) && popupBtn > 0) popupBtn--;
    if (in.pressed(K_RIGHT) && popupBtn + 1 < buttons) popupBtn++;
    int id = 0, a = 0, b = 0;
    bool press = in.pressed(K_A);
    if (tapped(id, a, b) && id == 900) {
        popupBtn = a;
        press = true;
    }
    if (in.pressed(K_B) || in.pressed(K_PLUS)) {
        updatePopup = false;  // a running download keeps going; Settings shows its progress
        return;
    }
    if (!press || buttons == 0) return;
    if (popupBtn == 1) {
        updatePopup = false;
    } else if (installed) {
        restartAfterUpdate();
    } else {
        installUpdate();  // first try or "Try again"
    }
}

void App::drawUpdatePopup() {
    const int w = 680, h = 380, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    gfx.rect(0, 0, SCREEN_W, SCREEN_H, col::shade);
    gfx.roundRect(x, y, w, h, 20, col::card);
    gfx.roundOutline(x, y, w, h, 20, 1, col::accent.alpha(140));
    if (SDL_Texture* logo = gfx.image("romfs:/logo.png")) gfx.drawTex(logo, x + 32, y + 30, 56, 56);

    std::string title, sub, body, b0, b1 = "Later";
    Color subColor = col::dim;
    bool installing = updateDl && !updateDl->finished;
    if (!updateDl) {
        title = "New version available";
        sub = "StremNX " + update.version + "  \xC2\xB7  you have " APP_VERSION;
        body = plainNotes(update.notes);
        b0 = "Update now";
    } else if (installing) {
        title = "Updating...";
        sub = "Downloading StremNX " + update.version;
        body = "You can keep using the app. B hides this window.";
    } else if (updateDl->ok) {
        title = "Update installed";
        sub = "StremNX " + update.version + " is ready";
        subColor = col::success;
        bool loader = envHasNextLoad();
        body = loader ? "Restart to start using the new version."
                      : "Close StremNX and open it again to start using the new version.";
        b0 = loader ? "Restart now" : "Close StremNX";
    } else {
        title = "Update failed";
        sub = updateDl->error;
        subColor = col::error;
        body = "Nothing was changed. Check the connection and try again.";
        b0 = "Try again";
        b1 = "Close";
    }
    gfx.text(title, x + 108, y + 30, 30, col::text, w - 140);
    gfx.text(sub, x + 108, y + 68, 19, subColor, w - 140);
    gfx.textWrapped(body, x + 36, y + 114, 18, col::dim, w - 72, installing ? 60 : 160);

    if (installing) {
        int64_t done = updateDl->done, total = updateDl->total;
        float p = total > 0 ? std::min(1.0f, (float)done / (float)total) : 0;
        int bx = x + 36, bw = w - 72, by = y + 220;
        gfx.roundRect(bx, by, bw, 10, 5, col::white.alpha(40));
        gfx.roundRect(bx, by, std::max(10, (int)(bw * p)), 10, 5, col::accent);
        char line[64];
        snprintf(line, sizeof(line), "%.1f / %.1f MB", done / 1048576.0, total / 1048576.0);
        gfx.text(line, bx, by + 24, 18, col::faint);
        gfx.text(std::to_string((int)(p * 100)) + "%", bx + bw, by + 24, 18, col::text, 0, AlignRight);
        return;
    }
    int bw0 = buttonWidth(gfx, b0, IconNone), bw1 = buttonWidth(gfx, b1, IconNone);
    int by = y + h - 80;
    drawButton(gfx, x + 36, by, bw0, 50, b0, popupBtn == 0, popupBtn == 0, IconNone);
    drawButton(gfx, x + 54 + bw0, by, bw1, 50, b1, popupBtn == 1, false, IconNone);
    hit(x + 36, by, bw0, 50, 900, 0);
    hit(x + 54 + bw0, by, bw1, 50, 900, 1);
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

        watchBeat(WatchMain, "main: updates");
        if (updateTask.ready()) {
            update = updateTask.take();
            if (update.available) {
                showUpdatePopup();
            } else if (updateManual) {
                toast(update.ok ? "StremNX is up to date (" APP_VERSION ")" : "Could not check for updates: " + update.error);
            }
        }
        if (updateDl && updateDl->finished && !updateDlReported) {
            updateDlReported = true;
            showUpdatePopup();  // installed (restart?) or failed (try again?)
        }

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
        if (updatePopup) updateUpdatePopup();  // the window takes the input while it is up
        else if (!stack.empty()) stack.back()->update(*this);
        applyPending();
        if (stack.empty()) running = false;
        if (!running) break;

        if (!playUrl.empty()) {
            std::string url = playUrl;
            PlayContext ctx = playCtx;
            playUrl.clear();
            double startAt = 0;
            const HistoryEntry* h = history.findVideo(ctx.videoId);
            if (playResume && h && h->position > 30 && !h->finished()) startAt = h->position;
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
        if (updatePopup) drawUpdatePopup();
        if (!toastMsg.empty() && SDL_GetTicks() < toastUntil) {
            int w = std::min(gfx.textWidth(toastMsg, 21), SCREEN_W - 160) + 48;
            int x = (SCREEN_W - w) / 2;
            gfx.roundRect(x, SCREEN_H - 120, w, 50, 25, col::cardSel.alpha(245));
            gfx.text(toastMsg, SCREEN_W / 2, SCREEN_H - 107, 21, col::text, SCREEN_W - 160, AlignCenter);
        }
        watchBeat(WatchMain, "main: present");
        gfx.present();
        if (gfx.frameCount() == 1) logMsg("run: first frame drawn");
    }
}
