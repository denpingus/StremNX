// Stream picker, stream scoring, the built-in torrent buffering screen and "Up next".

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "downloads.hpp"
#include "log.hpp"
#include "screens.hpp"

// ---------------------------------------------------------------- stream labels and scoring

static std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// Finds "<number> GB|MB" in the text.
static std::string findSize(const std::string& s, double* bytes) {
    for (size_t i = 0; i + 1 < s.size(); i++) {
        bool gb = s.compare(i, 2, "GB") == 0, mb = s.compare(i, 2, "MB") == 0;
        if (!gb && !mb) continue;
        size_t j = i;
        while (j > 0 && s[j - 1] == ' ') j--;
        size_t end = j;
        while (j > 0 && (isdigit((unsigned char)s[j - 1]) || s[j - 1] == '.' || s[j - 1] == ',')) j--;
        if (j < end) {
            std::string num = s.substr(j, end - j);
            std::replace(num.begin(), num.end(), ',', '.');
            *bytes = atof(num.c_str()) * (gb ? 1073741824.0 : 1048576.0);
            return s.substr(j, end - j) + (gb ? " GB" : " MB");
        }
    }
    return "";
}

// Torrentio writes seeders as "👤 123".
static std::string findSeeds(const std::string& s) {
    size_t p = s.find("\xF0\x9F\x91\xA4");
    if (p == std::string::npos) return "";
    p += 4;
    while (p < s.size() && s[p] == ' ') p++;
    size_t e = p;
    while (e < s.size() && isdigit((unsigned char)s[e])) e++;
    return e > p ? s.substr(p, e - p) : "";
}

static int rankOf(const std::string& q) {
    return q == "4K" ? 4 : q == "1080p" ? 3 : q == "720p" ? 2 : q == "SD" ? 1 : 0;
}

StreamInfo describeStream(const Stream& s) {
    StreamInfo i;
    std::string all = upper(s.name + " " + s.title);
    if (all.find("2160P") != std::string::npos || all.find("4K") != std::string::npos || all.find("UHD") != std::string::npos)
        i.quality = "4K";
    else if (all.find("1080P") != std::string::npos)
        i.quality = "1080p";
    else if (all.find("720P") != std::string::npos)
        i.quality = "720p";
    else if (all.find("480P") != std::string::npos || all.find("SD") != std::string::npos)
        i.quality = "SD";
    i.rank = rankOf(i.quality);
    if (all.find("DOLBY VISION") != std::string::npos || all.find(" DV") != std::string::npos)
        i.hdr = "Dolby Vision";
    else if (all.find("HDR") != std::string::npos)
        i.hdr = "HDR";
    i.size = findSize(s.title, &i.bytes);
    i.seeds = findSeeds(s.title);
    i.seedCount = atoi(i.seeds.c_str());
    bool debrid = all.find("RD+") != std::string::npos || all.find("AD+") != std::string::npos ||
                  all.find("PM+") != std::string::npos || all.find("TB+") != std::string::npos ||
                  all.find("DL+") != std::string::npos || all.find("[RD") != std::string::npos;
    i.source = s.url.compare(0, 5, "sdmc:") == 0 ? "On SD card"
               : debrid                           ? "Debrid"
               : !s.url.empty()                   ? "Direct"
                                                  : "Torrent";

    auto lines = splitLines(cleanText(s.title));
    i.title = lines.empty() ? cleanText(s.name) : lines[0];
    auto nameLines = splitLines(cleanText(s.name));
    i.subtitle = joinFrom(nameLines, 0, " ");
    return i;
}

int scoreStream(const Stream& s, const StreamInfo& i, const Config& cfg, const SavedStream* prev) {
    std::string pref = cfg.prefQuality != "auto" ? cfg.prefQuality : cfg.lastQuality.empty() ? "1080p" : cfg.lastQuality;
    double want = std::max(1, rankOf(pref)), q = i.rank ? i.rank : 2.5;
    double score = 100 - 35 * std::fabs(q - want);
    if (q > want) score -= 10;                   // heavier than needed
    if (!i.hdr.empty() && want < 4) score -= 15;  // HDR looks washed out on the Switch
    if (i.source == "On SD card") {
        score += 80;
    } else if (i.source != "Torrent") {
        score += cfg.preferSource == 2 ? 0 : 40;
    } else {
        score += (cfg.preferSource == 1 ? -30 : 0) + std::min(30.0, std::log2(i.seedCount + 1.0) * 4);
        if (!i.seeds.empty() && i.seedCount == 0) score -= 50;
    }
    if (prev && !prev->addonName.empty() && prev->addonName == s.addonName) score += 8;
    if (prev && !prev->source.empty() && prev->source == i.source) score += 5;
    return (int)score;
}

int pickBestStream(const std::vector<Stream>& streams, const Config& cfg, const SavedStream* prev) {
    int best = -1, bestScore = -100000;
    for (int k = 0; k < (int)streams.size(); k++) {
        int sc = scoreStream(streams[k], describeStream(streams[k]), cfg, prev);
        if (sc > bestScore) {
            bestScore = sc;
            best = k;
        }
    }
    return best;
}

SavedStream savedFrom(const Stream& s, const StreamInfo& i) {
    SavedStream v;
    v.url = s.url;
    v.infoHash = s.infoHash;
    v.fileIdx = s.fileIdx;
    v.trackers = s.trackers;
    v.title = i.title;
    v.addonName = s.addonName;
    v.quality = i.quality;
    v.source = i.source;
    return v;
}

Stream streamFromSaved(const SavedStream& v) {
    Stream s;
    s.addonName = v.addonName;
    s.name = v.source;
    s.title = v.title;
    s.url = v.url;
    s.infoHash = v.infoHash;
    s.fileIdx = v.fileIdx;
    s.trackers = v.trackers;
    return s;
}

bool sameStream(const Stream& s, const SavedStream& v) {
    if (!v.valid()) return false;
    if (!v.url.empty()) return s.url == v.url;
    return s.infoHash == v.infoHash && s.fileIdx == v.fileIdx;
}

void playStream(App& app, const Stream& s, PlayContext ctx, bool resume, bool byHand) {
    StreamInfo i = describeStream(s);
    ctx.stream = savedFrom(s, i);
    if (byHand && app.cfg.prefQuality == "auto" && !i.quality.empty() && i.quality != app.cfg.lastQuality) {
        app.cfg.lastQuality = i.quality;  // learn the viewer's preferred quality
        app.cfg.save();
    }
    DownloadInfo saved;
    bool onSd = !s.infoHash.empty() && findSavedDownload(s.infoHash, s.fileIdx, saved);
    if (onSd && saved.complete && fileExists(saved.file)) {
        app.requestPlay(saved.file, ctx, resume);  // already on the SD card: no peers needed
        return;
    }
    // A partial download plays in the built-in engine, which reads what is already on the SD card.
    std::string url = onSd ? "" : streamPlayUrl(s, app.cfg.server);
    if (!url.empty())
        app.requestPlay(url, ctx, resume);  // link, SD card file, or torrent through a stremio-server
    else if (!s.infoHash.empty())
        app.push(makeTorrentScreen(app, s, ctx, resume));  // built-in torrent engine
    else
        app.toast("This stream has no playable URL");
}

void drawBackButton(App& app) {
    Gfx& g = app.gfx;
    int w = g.textWidth("Back", 18) + 46;
    g.roundRect(W - 60 - w, 22, w, 34, 17, col::black.alpha(110));
    g.triangle(W - 60 - w + 24, 31, W - 60 - w + 24, 47, W - 60 - w + 14, 39, col::text);
    g.text("Back", W - 60 - w + 32, 28, 18, col::text);
    app.hit(W - 60 - w, 22, w, 34, kHitBack);
}

namespace {

int chip(Gfx& g, int x, int y, const std::string& text, Color bg, Color fg) {
    if (text.empty()) return 0;
    int w = g.textWidth(text, 16) + 18;
    g.roundRect(x, y, w, 26, 13, bg);
    g.text(text, x + 9, y + 3, 16, fg);
    return w + 8;
}

void drawHeader(Gfx& g, const PlayContext& ctx, int y) {
    Uint8 lf = 0;
    SDL_Texture* logo = g.image(ctx.logo, &lf);
    int x = 60;
    if (logo) {
        SDL_Rect r = g.drawFit(logo, x, y, 300, 80, AlignLeft, true, lf);
        x = r.x + r.w + 24;
    } else {
        x += g.text(ctx.name, x, y + 30, 34, col::text, 700) + 24;
    }
    if (!ctx.episodeLabel.empty()) g.text(ctx.episodeLabel, x, y + 50, 22, col::dim, W - x - 260);
}

// ---------------------------------------------------------------- Torrent buffering

class TorrentScreen : public Screen {
    std::shared_ptr<Torrent> torrent;
    PlayContext ctx;
    bool resume;
    bool played = false, handedOff = false;
    int pct = 0;
    uint32_t startedAt = 0, startReadyAt = 0;
    float shownPct = 0;

    // Leaving: a torrent being saved to the SD card keeps downloading in the background.
    void leave(App& app) {
        if (app.activeTorrent == torrent) app.activeTorrent.reset();
        if (torrent->keepsDownloading()) {
            app.keepDownloading(torrent);
            handedOff = true;
            app.toast("Still downloading to the SD card - see the Downloads tab");
        } else if (!handedOff) {
            app.keepWarm(torrent);  // reopening it soon starts from what is in RAM
            handedOff = true;
        }
        app.pop();
    }

public:
    TorrentScreen(App& app, const Stream& s, const PlayContext& c, bool r) : ctx(c), resume(r) {
        torrent = app.findDownload(s.infoHash, s.fileIdx);  // already downloading in the background?
        handedOff = (bool)torrent;
        if (!torrent) torrent = app.takeWarm(s.infoHash, s.fileIdx);  // left a few minutes ago
        app.keepWarm(nullptr);  // any other stream kept from before is not needed now
        if (!torrent) {
            torrent = std::make_shared<Torrent>(s.infoHash, s.trackers, s.fileIdx);
            torrent->setUploadMode(app.cfg.uploadMode);
            // Streaming only uses RAM, unless "Download what I watch" is on. A download started by hand
            // (- in the list) plays from what is on the SD card and keeps filling its file.
            DownloadInfo saved;
            if (app.cfg.autoDownload || findSavedDownload(s.infoHash, s.fileIdx, saved))
                torrent->saveTo(DOWNLOADS_DIR, downloadInfoJson(ctx, s.trackers));
            torrent->start();
        }
        app.activeTorrent = torrent;  // the one the viewer waits for: background downloads never slow it
        const HistoryEntry* h = app.history.resumePoint(ctx);
        if (resume && h) torrent->prefetch(h->progress());  // the part where playback will resume
        startedAt = SDL_GetTicks();
    }

    ~TorrentScreen() override {
        if (!handedOff) torrent->stop();
    }

    void update(App& app) override {
        if (played) {  // back from the player
            app.activeTorrent.reset();
            leave(app);
            return;
        }
        int id = 0, ta = 0, tb = 0;
        if (app.in.pressed(K_B) || (app.tapped(id, ta, tb) && id == kHitBack)) {
            leave(app);
            return;
        }
        bool startReady = false;
        bool ready = torrent->initialBufferReady(&pct, &startReady);
        if (startReady && !startReadyAt) startReadyAt = SDL_GetTicks();
        // The file tail (index) helps seeking but is not essential: do not wait for it forever.
        if (ready || (startReady && SDL_GetTicks() - startReadyAt > 6000)) {
            played = true;
            app.activeTorrent = torrent;
            std::string name = torrent->fileName();
            app.requestPlay("torrent://" + name.substr(name.rfind('/') + 1), ctx, resume);
        }
    }

    void draw(App& app) override {
        Gfx& g = app.gfx;
        drawBackdrop(g, ctx.background, 0, 120);
        drawHeader(g, ctx, 50);
        drawBackButton(app);
        TorrentStatus st = torrent->status();

        int px = 240, py = 250, pw = 800, ph = 300;
        g.roundRect(px, py, pw, ph, 20, col::card.alpha(210));
        g.roundOutline(px, py, pw, ph, 20, 1, col::white.alpha(30));

        std::string phase;
        if (!st.error.empty())
            phase = "Error: " + st.error;
        else if (st.peersConnected == 0)
            phase = "Finding peers";
        else if (!st.hasMetadata)
            phase = "Downloading torrent info";
        else
            phase = "Preparing playback";
        if (st.error.empty()) {
            app.drawSpinner(px + 50, py + 54, 16);
            g.text(phase, px + 84, py + 40, 26, col::text);
        } else {
            g.text(phase, px + 40, py + 40, 24, col::error, pw - 80);
        }

        if (st.hasMetadata) {
            g.text(st.fileName.substr(st.fileName.rfind('/') + 1), px + 40, py + 96, 18, col::dim, pw - 80);
            g.text(formatBytes((double)st.fileSize), px + pw - 40, py + 124, 18, col::faint, 0, AlignRight);
        }
        shownPct = approach(shownPct, (float)pct, 0.15f);
        g.roundRect(px + 40, py + 160, pw - 80, 10, 5, col::white.alpha(40));
        g.roundRect(px + 40, py + 160, (int)((pw - 80) * shownPct / 100), 10, 5, col::accent);
        g.text(std::to_string((int)shownPct) + "%", px + 40, py + 124, 18, col::text);

        int cx = px + 40, cy = py + 200;
        char buf[64];
        snprintf(buf, sizeof(buf), "%d peers  ·  %d sending", st.peersConnected, st.peersSending);
        cx += chip(g, cx, cy, buf, col::white.alpha(30), col::text);
        snprintf(buf, sizeof(buf), "%.1f MB/s", st.speed / (1024 * 1024));
        cx += chip(g, cx, cy, buf, col::white.alpha(30), col::text);
        cx += chip(g, cx, cy, formatBytes((double)st.downloaded) + " downloaded", col::white.alpha(30), col::text);
        if (st.saving)
            chip(g, cx, cy, "Saving to SD card", col::success.alpha(60), col::success);
        else if (!st.saveError.empty())
            chip(g, cx, cy, st.saveError, col::error.alpha(50), col::error);
        else
            chip(g, cx, cy, std::to_string(st.peersKnown) + " known", col::white.alpha(20), col::dim);

        if (st.peersConnected == 0 && SDL_GetTicks() - startedAt > 30000)
            g.text("No peers yet. This torrent may have few seeders; try another stream.", px + pw / 2, py + 252, 18,
                   col::dim, pw - 60, AlignCenter);
        app.drawFooter("B Cancel");
    }
};

// ---------------------------------------------------------------- Streams

struct StreamResult {
    std::vector<Stream> streams;
    std::string err;
};

// Fetches streams for one video from every addon, plus a finished SD card download of it.
class StreamFetcher {
public:
    std::vector<Stream> streams;
    int total = 0, finished = 0;

    void start(App& app, const std::string& type, const std::string& videoId) {
        for (auto& d : listDownloads()) {
            if (!d.complete || d.ctx.videoId != videoId || !fileExists(d.file)) continue;
            Stream s;
            s.addonName = "Downloads";
            s.name = "Downloaded";
            s.title = d.fileName.substr(d.fileName.rfind('/') + 1) + "\n" + formatBytes((double)d.size);
            s.url = d.file;
            streams.push_back(s);
        }
        for (auto& a : app.addons) {
            if (!addonSupports(a, "stream", type, videoId)) continue;
            Task<StreamResult> task;
            Addon ac = a;
            task.start([ac, type, videoId]() {
                StreamResult r;
                r.streams = fetchStreams(ac, type, videoId, r.err);
                return r;
            });
            tasks.push_back(task);
        }
        total = (int)tasks.size();
    }

    // Returns true when new streams arrived.
    bool poll() {
        bool changed = false;
        for (auto& t : tasks) {
            if (!t.ready()) continue;
            StreamResult r = t.take();
            finished++;
            streams.insert(streams.end(), r.streams.begin(), r.streams.end());
            changed = true;
        }
        return changed;
    }

    bool done() const { return finished >= total; }

private:
    std::vector<Task<StreamResult>> tasks;
};

class StreamsScreen : public Screen {
    PlayContext ctx;
    bool resume;
    bool started = false;
    StreamFetcher fetch;
    std::vector<StreamInfo> infos;
    std::vector<int> view;  // filtered + sorted indices into fetch.streams
    int sel = 0;
    float scroll = 0;
    int qFilter = 0, sFilter = 0, sortMode = 0;
    static const int top = 172, rowH = 84, gap = 10, visible = 5;
    static constexpr const char* qNames[5] = {"All", "4K", "1080p", "720p", "SD"};
    static constexpr const char* sNames[3] = {"All", "Debrid & links", "Torrents"};
    static constexpr const char* sortNames[4] = {"Best match", "Quality", "Size", "Seeders"};

    void rebuildView(App& app) {
        int keep = view.empty() ? -1 : view[std::min(sel, (int)view.size() - 1)];
        while (infos.size() < fetch.streams.size()) infos.push_back(describeStream(fetch.streams[infos.size()]));
        view.clear();
        for (int i = 0; i < (int)fetch.streams.size(); i++) {
            const StreamInfo& in = infos[i];
            if (qFilter == 1 && in.quality != "4K") continue;
            if (qFilter == 2 && in.quality != "1080p") continue;
            if (qFilter == 3 && in.quality != "720p") continue;
            if (qFilter == 4 && in.quality != "SD") continue;
            if (sFilter == 1 && in.source == "Torrent") continue;
            if (sFilter == 2 && in.source != "Torrent") continue;
            view.push_back(i);
        }
        const SavedStream* prev = ctx.stream.valid() ? &ctx.stream : nullptr;
        std::vector<int> score(fetch.streams.size());
        for (int i : view) score[i] = scoreStream(fetch.streams[i], infos[i], app.cfg, prev);
        std::stable_sort(view.begin(), view.end(), [&](int a, int b) {
            const StreamInfo &x = infos[a], &y = infos[b];
            bool la = sameStream(fetch.streams[a], ctx.stream), lb = sameStream(fetch.streams[b], ctx.stream);
            if (la != lb) return la;  // the stream used last time stays on top
            switch (sortMode) {
                case 1: if (x.rank != y.rank) return x.rank > y.rank; break;
                case 2: if (x.bytes != y.bytes) return x.bytes > y.bytes; break;
                case 3: if (x.seedCount != y.seedCount) return x.seedCount > y.seedCount; break;
            }
            return score[a] > score[b];
        });
        sel = 0;
        for (int k = 0; k < (int)view.size(); k++)
            if (view[k] == keep) sel = k;
    }

public:
    StreamsScreen(const PlayContext& c, bool r) : ctx(c), resume(r) {}

    void update(App& app) override {
        if (!started) {
            started = true;
            fetch.start(app, ctx.type.empty() ? "movie" : ctx.type, ctx.videoId);
            rebuildView(app);
        }
        if (fetch.poll()) rebuildView(app);

        const Input& in = app.in;
        int id = 0, a = 0, b = 0;
        bool tap = app.tapped(id, a, b);
        if (in.pressed(K_B) || (tap && id == kHitBack)) {
            app.pop();
            return;
        }
        bool changed = false;
        if (in.pressed(K_Y) || (tap && id == 71 && a == 0)) qFilter = (qFilter + 1) % 5, changed = true;
        if (in.pressed(K_X) || (tap && id == 71 && a == 1)) sFilter = (sFilter + 1) % 3, changed = true;
        if (in.pressed(K_ZR) || (tap && id == 71 && a == 2)) sortMode = (sortMode + 1) % 4, changed = true;
        if (in.pressed(K_ZL)) sortMode = (sortMode + 3) % 4, changed = true;
        if (changed) rebuildView(app);

        int n = (int)view.size();
        if (n == 0) return;
        if (in.pressed(K_DOWN) && sel + 1 < n) sel++;
        if (in.pressed(K_UP) && sel > 0) sel--;
        if (in.pressed(K_R)) sel = std::min(n - 1, sel + visible);
        if (in.pressed(K_L)) sel = std::max(0, sel - visible);
        if (in.pressed(K_MINUS)) download(app, fetch.streams[view[sel]]);
        bool open = in.pressed(K_A);
        if (tap && id == 70 && a < n) sel = a, open = true;
        if (open) {
            PlayContext c = ctx;
            c.fromSaved = false;
            playStream(app, fetch.streams[view[sel]], c, resume, true);
        }
    }

    // Saves a torrent to the SD card in the background, without playing it.
    void download(App& app, const Stream& s) {
        DownloadInfo saved;
        if (s.infoHash.empty()) {
            app.toast("Only torrent streams can be downloaded");
        } else if (findSavedDownload(s.infoHash, s.fileIdx, saved) && saved.complete) {
            app.toast("Already on the SD card - see the Downloads tab");
        } else if (app.findDownload(s.infoHash, s.fileIdx)) {
            app.toast("Already downloading - see the Downloads tab");
        } else {
            auto t = std::make_shared<Torrent>(s.infoHash, s.trackers, s.fileIdx);
            t->setUploadMode(app.cfg.uploadMode);
            t->saveTo(DOWNLOADS_DIR, downloadInfoJson(ctx, s.trackers));
            t->start();
            app.keepDownloading(t);
            app.toast("Downloading to the SD card - see the Downloads tab");
        }
    }

    void draw(App& app) override {
        Gfx& g = app.gfx;
        drawBackdrop(g, ctx.background, 0, 110);
        drawHeader(g, ctx, 40);
        drawBackButton(app);

        std::string status;
        if (fetch.total == 0 && fetch.streams.empty())
            status = "No stream addons installed. Log in in Settings or add an addon by URL.";
        else if (!fetch.done())
            status = "Searching " + std::to_string(fetch.total) + " addons  (" + std::to_string(fetch.finished) + "/" +
                     std::to_string(fetch.total) + ")";
        else
            status = std::to_string(view.size()) + " of " + std::to_string(fetch.streams.size()) + " streams";
        if (!fetch.done())
            app.drawLoading(60, 130, status, AlignLeft);
        else
            g.text(status, 60, 132, 20, col::dim, 480);

        // Filter / sort chips (right side)
        std::string labels[3] = {std::string("Quality: ") + qNames[qFilter], std::string("Source: ") + sNames[sFilter],
                                 std::string("Sort: ") + sortNames[sortMode]};
        const char* keys[3] = {"Y", "X", "ZR"};
        bool active[3] = {qFilter != 0, sFilter != 0, sortMode != 0};
        int cx = W - 60;
        for (int k = 2; k >= 0; k--) {
            int w = g.textWidth(labels[k], 17) + g.textWidth(keys[k], 15) + 40;
            cx -= w;
            g.roundRect(cx, 126, w, 32, 16, active[k] ? col::accent.alpha(120) : col::white.alpha(28));
            g.roundRect(cx + 6, 131, g.textWidth(keys[k], 15) + 14, 22, 11, col::white.alpha(40));
            g.text(keys[k], cx + 13, 133, 15, col::text);
            g.text(labels[k], cx + g.textWidth(keys[k], 15) + 28, 132, 17, col::text);
            app.hit(cx, 126, w, 32, 71, k);
            cx -= 10;
        }

        float target = (float)std::max(0, sel - (visible - 2)) * (rowH + gap);
        float maxS = (float)std::max(0, (int)view.size() - visible) * (rowH + gap);
        scroll = approach(scroll, std::min(target, maxS), 0.25f);

        g.clip(0, top - 6, W, H - top - 40);
        for (int k = 0; k < (int)view.size(); k++) {
            int y = top + k * (rowH + gap) - (int)scroll;
            if (y + rowH < top - 6 || y > H) continue;
            int i = view[k];
            drawStream(g, fetch.streams[i], infos[i], y, k == sel, sameStream(fetch.streams[i], ctx.stream));
            app.hit(60, std::max(y, top - 6), W - 120, rowH, 70, k);
        }
        g.unclip();
        if (fetch.done() && view.empty() && !fetch.streams.empty())
            g.text("No streams match these filters", W / 2, 330, 22, col::dim, 0, AlignCenter);
        app.drawFooter("A Play   - Download   Y Quality   X Source   ZL/ZR Sort   B Back");
    }

    void drawStream(Gfx& g, const Stream& s, const StreamInfo& info, int y, bool sel, bool last) {
        int x = 60, w = W - 120;
        g.roundRect(x, y, w, rowH, 14, sel ? col::cardSel.alpha(240) : col::card.alpha(200));
        if (sel) g.roundOutline(x, y, w, rowH, 14, 2, col::accentLight);

        // Quality badge
        std::string q = info.quality.empty() ? "HD?" : info.quality;
        Color qc = info.quality == "4K" ? col::gold : info.quality == "1080p" ? col::accentLight : col::white.alpha(160);
        g.roundRect(x + 18, y + 20, 92, 44, 10, col::black.alpha(90));
        g.roundOutline(x + 18, y + 20, 92, 44, 10, 2, qc);
        g.text(q, x + 64, y + 29, 22, qc, 84, AlignCenter);

        int tx = x + 132;
        g.text(info.title, tx, y + 12, 20, col::text, w - 160);
        int cx = tx, cy = y + 46;
        if (last) cx += chip(g, cx, cy, "Last used", col::white, col::black);
        cx += chip(g, cx, cy, s.addonName, col::accent.alpha(70), col::accentLight);
        if (!info.subtitle.empty() && info.subtitle != s.addonName)
            cx += chip(g, cx, cy, info.subtitle, col::white.alpha(24), col::dim);
        cx += chip(g, cx, cy, info.source, info.source == "Torrent" ? col::white.alpha(24) : col::success.alpha(60),
                   info.source == "Torrent" ? col::dim : col::success);
        cx += chip(g, cx, cy, info.hdr, col::gold.alpha(50), col::gold);
        if (info.quality == "4K") cx += chip(g, cx, cy, "Heavy for Switch", col::error.alpha(40), col::error);
        cx += chip(g, cx, cy, info.size, col::white.alpha(24), col::dim);
        if (!info.seeds.empty()) chip(g, cx, cy, info.seeds + " seeds", col::white.alpha(24), col::dim);
    }
};

// ---------------------------------------------------------------- Up next

struct NextMeta {
    Meta meta;
    bool ok = false;
};

// After an episode: finds the next one, picks the best stream for it and plays it.
class NextEpisodeScreen : public Screen {
    PlayContext prev, next;
    Task<NextMeta> metaTask;
    std::vector<std::pair<std::string, std::string>> candidates;  // video id, label
    size_t cand = 0;
    StreamFetcher fetch;
    bool started = false, fetching = false, played = false;
    std::string status = "Finding the next episode";
    std::string chainNext, chainLabel;

    void startCandidate(App& app) {
        while (cand < candidates.size()) {
            next = prev;
            next.videoId = candidates[cand].first;
            next.episodeLabel = candidates[cand].second;
            next.nextVideoId.clear();
            next.nextEpisodeLabel.clear();
            next.stream = SavedStream();
            next.fromSaved = false;
            fetch = StreamFetcher();
            fetch.start(app, prev.type, next.videoId);
            fetching = true;
            status = "Finding the best stream for " + next.episodeLabel;
            return;
        }
        app.toast("No next episode found");
        app.pop();
    }

public:
    explicit NextEpisodeScreen(const PlayContext& finished) : prev(finished), next(finished) {}

    void update(App& app) override {
        int id = 0, a = 0, b = 0;
        bool tap = app.tapped(id, a, b);
        if (played || app.in.pressed(K_B) || (tap && id == kHitBack)) {  // back from the player, or cancelled
            app.pop();
            return;
        }
        if (!started) {
            started = true;
            std::vector<Addon> cands;
            for (auto& ad : app.addons)
                if (addonSupports(ad, "meta", prev.type, prev.metaId)) cands.push_back(ad);
            std::string type = prev.type, mid = prev.metaId;
            metaTask.start([cands, type, mid]() {
                NextMeta r;
                std::string err;
                for (auto& ad : cands)
                    if (fetchMeta(ad, type, mid, r.meta, err)) {
                        r.ok = true;
                        break;
                    }
                return r;
            });
        }
        if (metaTask.ready()) {
            NextMeta r = metaTask.take();
            if (r.ok) {  // the real episode list knows season breaks and specials
                for (auto& v : r.meta.videos) {
                    if (v.id != prev.videoId) continue;
                    PlayContext c = contextForEpisode(r.meta, v);
                    if (c.nextVideoId.empty()) break;
                    candidates.emplace_back(c.nextVideoId, c.nextEpisodeLabel);
                    for (auto& v2 : r.meta.videos)  // the next one's own "next", for chaining
                        if (v2.id == c.nextVideoId) {
                            PlayContext c2 = contextForEpisode(r.meta, v2);
                            chainNext = c2.nextVideoId;
                            chainLabel = c2.nextEpisodeLabel;
                        }
                }
                if (candidates.empty()) {
                    app.toast("That was the last episode");
                    app.pop();
                    return;
                }
            }
            if (candidates.empty() && !prev.nextVideoId.empty())
                candidates.emplace_back(prev.nextVideoId, prev.nextEpisodeLabel.empty()
                                                              ? episodeLabelFromId(prev.nextVideoId)
                                                              : prev.nextEpisodeLabel);
            if (candidates.empty()) {  // guess from the numbering: next episode, then next season
                for (bool season : {false, true}) {
                    std::string g = guessNextEpisode(prev.videoId, season);
                    if (!g.empty()) candidates.emplace_back(g, episodeLabelFromId(g));
                }
            }
            startCandidate(app);
        }
        if (fetching) {
            fetch.poll();
            if (fetch.done()) {
                fetching = false;
                int best = pickBestStream(fetch.streams, app.cfg, prev.stream.valid() ? &prev.stream : nullptr);
                if (best < 0) {
                    cand++;
                    startCandidate(app);
                    return;
                }
                next.nextVideoId = cand == 0 ? chainNext : "";
                next.nextEpisodeLabel = cand == 0 ? chainLabel : "";
                played = true;
                playStream(app, fetch.streams[best], next, false, false);
            }
        }
    }

    void draw(App& app) override {
        Gfx& g = app.gfx;
        drawBackdrop(g, prev.background, 0, 160);
        drawHeader(g, prev, 50);
        drawBackButton(app);
        int px = 290, py = 270, pw = 700, ph = 190;
        g.roundRect(px, py, pw, ph, 20, col::card.alpha(220));
        g.text("Up next", px + 36, py + 28, 20, col::accentLight);
        g.text(next.episodeLabel.empty() ? "Next episode" : next.episodeLabel, px + 36, py + 58, 30, col::text,
               pw - 72);
        app.drawSpinner(px + 50, py + 136, 14);
        g.text(status, px + 80, py + 122, 20, col::dim, pw - 120);
        app.drawFooter("B Cancel");
    }
};

// ---------------------------------------------------------------- Addons

// Every addon, one by one, those that failed to load included. Each one switches on or off right away
// (on this Switch only: the Stremio account is not changed). Addons added by URL can also be removed.
class AddonsScreen : public Screen {
    struct Entry {
        std::string url, name;
        enum Kind { ByUrl, Account, BuiltIn } kind = Account;
        bool loaded = false, off = false;
    };
    std::vector<Entry> list;
    int sel = 0, confirm = -1, confirmBtn = 1;  // confirm: entry waiting for "Remove?"
    float scroll = 0;
    int seenVersion = -1;
    static const int top = 140, rowH = 78, gap = 10, visible = 6;
    static const int toggleX = W - 84 - 56, removeX = 700;  // row controls (touch / pointer)

    static std::string hostOf(const std::string& url) {
        size_t a = url.find("://");
        a = a == std::string::npos ? 0 : a + 3;
        return url.substr(a, url.find('/', a) - a);
    }

    void build(App& app) {
        const Config& c = app.cfg;
        auto builtIn = [](const std::string& u) { return u == CINEMETA_URL || u == OPENSUBTITLES_URL; };
        auto off = [&](const std::string& u) {
            return std::find(c.disabledAddons.begin(), c.disabledAddons.end(), u) != c.disabledAddons.end();
        };
        auto add = [&](const std::string& url, const std::string& name, bool loaded) {
            for (auto& e : list)
                if (e.url == url) return;
            Entry e;
            e.url = url;
            e.name = name.empty() ? hostOf(url) : name;
            bool byUrl = std::find(c.addons.begin(), c.addons.end(), url) != c.addons.end();
            e.kind = byUrl ? Entry::ByUrl : builtIn(url) ? Entry::BuiltIn : Entry::Account;
            e.loaded = loaded;
            e.off = off(url);
            list.push_back(e);
        };
        list.clear();
        for (auto& a : app.addons) add(a.transportUrl, a.name, true);
        for (auto& u : c.addons) add(u, "", false);  // added by URL but not loaded: off, or it failed
        for (auto& u : c.disabledAddons) add(u, "", false);
        sel = std::max(0, std::min(sel, (int)list.size() - 1));
    }

    void toggle(App& app, int i) {
        Config& c = app.cfg;
        Entry& e = list[i];
        e.off = !e.off;
        auto& d = c.disabledAddons;
        if (e.off)
            d.push_back(e.url);
        else
            d.erase(std::remove(d.begin(), d.end(), e.url), d.end());
        app.toast(e.name + (e.off ? " is off" : " is on"));
        c.save();
        app.reloadAddons();
    }

    void remove(App& app, int i) {
        Config& c = app.cfg;
        Entry& e = list[i];
        c.addons.erase(std::remove(c.addons.begin(), c.addons.end(), e.url), c.addons.end());
        c.disabledAddons.erase(std::remove(c.disabledAddons.begin(), c.disabledAddons.end(), e.url),
                               c.disabledAddons.end());
        app.toast("Removed " + e.name);
        list.erase(list.begin() + i);
        sel = std::max(0, std::min(sel, (int)list.size() - 1));
        c.save();
        app.reloadAddons();
    }

public:
    void update(App& app) override {
        if (seenVersion != app.addonsVersion) {  // first open, or a reload finished
            seenVersion = app.addonsVersion;
            build(app);
        }
        const Input& in = app.in;
        int id = 0, a = 0, b = 0;
        bool tap = app.tapped(id, a, b);
        if (confirm >= 0) {
            if (in.pressed(K_LEFT)) confirmBtn = 0;
            if (in.pressed(K_RIGHT)) confirmBtn = 1;
            if (tap && id == 81) confirmBtn = a;
            if (in.pressed(K_A) || (tap && id == 81)) {
                if (confirmBtn == 0) remove(app, confirm);
                confirm = -1;
            }
            if (in.pressed(K_B)) confirm = -1;
            return;
        }
        if (in.pressed(K_B) || (tap && id == kHitBack)) {
            app.pop();
            return;
        }
        int n = (int)list.size();
        if (n == 0) return;
        if (in.pressed(K_DOWN) && sel + 1 < n) sel++;
        if (in.pressed(K_UP) && sel > 0) sel--;
        bool flip = in.pressed(K_A), drop = in.pressed(K_Y);
        if (tap && a < n) {
            if (id == 82) flip = true;  // the switch itself
            if (id == 83) drop = true;  // the Remove button
            if (id == 80) flip = sel == a;  // a row: first tap selects, the second switches
            sel = a;
        }
        if (flip) toggle(app, sel);
        if (drop && list[sel].kind == Entry::ByUrl) {
            confirm = sel;
            confirmBtn = 1;
        }
    }

    void draw(App& app) override {
        Gfx& g = app.gfx;
        g.text("Addons", 60, 40, 34, col::text);
        int on = 0;
        for (auto& e : list) on += !e.off;
        g.text(app.addonsLoading() ? "Updating..."
                                   : std::to_string(on) + " on, " + std::to_string(list.size() - on) + " off",
               60, 90, 19, col::dim);
        drawBackButton(app);

        float target = (float)std::max(0, sel - (visible - 2)) * (rowH + gap);
        scroll = approach(scroll, target, 0.25f);
        g.clip(0, top - 6, W, H - top - 40);
        for (int i = 0; i < (int)list.size(); i++) {
            int y = top + i * (rowH + gap) - (int)scroll;
            if (y + rowH < top - 6 || y > H) continue;
            const Entry& e = list[i];
            bool s = i == sel;
            g.roundRect(60, y, W - 120, rowH, 14, s ? col::cardSel.alpha(240) : col::card.alpha(200));
            if (s) g.roundOutline(60, y, W - 120, rowH, 14, 2, col::accentLight);
            app.hit(60, std::max(y, top - 6), W - 120, rowH, 80, i);
            g.text(e.name, 84, y + 12, 22, e.off ? col::dim : col::text, 440);
            auto why = app.addonErrors.find(e.url);  // a failed one shows the reason instead of its URL
            bool showWhy = !e.loaded && !e.off && why != app.addonErrors.end();
            g.text(showWhy ? why->second : e.url, 84, y + 44, 15, showWhy ? col::error : col::faint, 440);
            const char* kind = e.kind == Entry::ByUrl ? "Added by URL" : e.kind == Entry::BuiltIn ? "Built-in" : "Account";
            chip(g, 548, y + 26, kind, col::white.alpha(24), col::dim);
            if (e.kind == Entry::ByUrl) {  // Remove (Y)
                g.roundRect(removeX, y + 22, 96, 34, 17, col::white.alpha(30));
                g.text("Remove", removeX + 48, y + 28, 17, col::text, 0, AlignCenter);
                app.hit(removeX, y + 22, 96, 34, 83, i);
            }
            bool failed = !e.loaded && !e.off && !app.addonsLoading();
            if (failed) g.text("Failed to load", toggleX - 20, y + 28, 18, col::error, 0, AlignRight);
            // On / off switch
            g.roundRect(toggleX, y + 25, 56, 28, 14, e.off ? col::white.alpha(50) : col::accent);
            g.circle(toggleX + (e.off ? 14.0f : 42.0f), y + 39.0f, 11, col::white);
            app.hit(toggleX - 8, y + 15, 72, 48, 82, i);
        }
        g.unclip();
        if (list.empty()) g.text("No addons", W / 2, 300, 24, col::dim, 0, AlignCenter);

        if (confirm >= 0 && confirm < (int)list.size()) {
            const Entry& e = list[confirm];
            g.rect(0, 0, W, H, col::black.alpha(170));
            int pw = 640, ph = 240, px = (W - pw) / 2, py = (H - ph) / 2;
            g.roundRect(px, py, pw, ph, 20, col::bar);
            g.roundOutline(px, py, pw, ph, 20, 1, col::white.alpha(40));
            g.text("Remove " + e.name + "?", px + 36, py + 30, 28, col::text, pw - 72);
            g.textWrapped("It is removed from this Switch. You can add it again by URL. To keep it but not use it, "
                          "switch it off instead.",
                          px + 36, py + 80, 18, col::dim, pw - 72, 70);
            int bw0 = buttonWidth(g, "Remove", IconNone), bw1 = buttonWidth(g, "Cancel", IconNone);
            drawButton(g, px + 36, py + 160, bw0, 50, "Remove", confirmBtn == 0, confirmBtn == 0, IconNone);
            drawButton(g, px + 54 + bw0, py + 160, bw1, 50, "Cancel", confirmBtn == 1, false, IconNone);
            app.hit(px + 36, py + 160, bw0, 50, 81, 0);
            app.hit(px + 54 + bw0, py + 160, bw1, 50, 81, 1);
            app.drawFooter("A Confirm   B Cancel");
        } else if (!list.empty()) {
            const Entry& e = list[sel];
            std::string hints = std::string(e.off ? "A Turn on" : "A Turn off") +
                                (e.kind == Entry::ByUrl ? "   Y Remove" : "") + "   B Back";
            app.drawFooter(hints);
        } else {
            app.drawFooter("B Back");
        }
    }
};

}  // namespace

std::unique_ptr<Screen> makeAddonsScreen() {
    return std::make_unique<AddonsScreen>();
}

std::unique_ptr<Screen> makeStreamsScreen(const PlayContext& ctx, bool resume) {
    return std::make_unique<StreamsScreen>(ctx, resume);
}

std::unique_ptr<Screen> makeTorrentScreen(App& app, const Stream& s, const PlayContext& ctx, bool resume) {
    return std::make_unique<TorrentScreen>(app, s, ctx, resume);
}

std::unique_ptr<Screen> makeNextEpisodeScreen(const PlayContext& finished) {
    return std::make_unique<NextEpisodeScreen>(finished);
}
