// Details page: artwork, logo, metadata, Play/Resume, and for series the season + episode rows.

#include <algorithm>
#include <set>

#include "screens.hpp"

namespace {

struct MetaResult {
    Meta meta;
    bool ok = false;
    std::string err;
};

constexpr int kX = 60;

class DetailScreen : public Screen {
    MetaItem item;
    Task<MetaResult> task;
    bool started = false, loaded = false;
    Meta meta;
    std::string err;

    std::vector<int> seasons;
    std::vector<int> eps;  // indices into meta.videos
    int seasonIdx = 0, epSel = 0;
    float epScroll = 0, seasonScroll = 0;

    enum { FocusButtons, FocusSeasons, FocusEpisodes };
    int focus = FocusButtons, btn = 0;

    struct Btn {
        std::string label;
        ButtonIcon icon;
        bool primary;
        int action;  // 0 play/resume, 1 start over, 2 episodes
        int video;   // index into meta.videos, -1 for the movie itself
    };
    std::vector<Btn> buttons;
    double resumeProgress = -1;
    int seenHistory = -1;

    bool isSeries() const { return loaded && !meta.videos.empty() && meta.type != "movie"; }

    void buildSeasons() {
        std::set<int> s;
        for (auto& v : meta.videos) s.insert(v.season);
        seasons.clear();
        for (int x : s)
            if (x > 0) seasons.push_back(x);
        if (s.count(0)) seasons.push_back(0);  // specials last
    }

    void selectSeason(int idx) {
        seasonIdx = idx;
        eps.clear();
        epSel = 0;
        if (seasons.empty()) return;
        int season = seasons[seasonIdx];
        for (int i = 0; i < (int)meta.videos.size(); i++)
            if (meta.videos[i].season == season) eps.push_back(i);
        std::stable_sort(eps.begin(), eps.end(),
                         [&](int a, int b) { return meta.videos[a].episode < meta.videos[b].episode; });
    }

    // Episodes in watch order (seasons ascending, specials excluded).
    std::vector<int> watchOrder() const {
        std::vector<int> v;
        for (int i = 0; i < (int)meta.videos.size(); i++)
            if (meta.videos[i].season > 0) v.push_back(i);
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
            const Video &x = meta.videos[a], &y = meta.videos[b];
            return x.season != y.season ? x.season < y.season : x.episode < y.episode;
        });
        if (v.empty())
            for (int i = 0; i < (int)meta.videos.size(); i++) v.push_back(i);
        return v;
    }

    int videoIndex(const std::string& id) const {
        for (int i = 0; i < (int)meta.videos.size(); i++)
            if (meta.videos[i].id == id) return i;
        return -1;
    }

    static std::string epCode(const Video& v) {
        return v.season > 0 ? "S" + std::to_string(v.season) + ":E" + std::to_string(v.episode) : v.title;
    }

    enum { ActPlay, ActStartOver, ActLibrary };

    void buildButtons(App& app) {
        buttons.clear();
        resumeProgress = -1;
        const HistoryEntry* h = app.history.latestForMeta(meta.id);
        int target = -1;  // episode index (series) or -1 (movie)
        std::string label = "Play";
        bool resume = false;
        if (!isSeries()) {
            if (h && h->position > 30 && !h->finished()) {
                resume = true;
                label = "Resume";
                resumeProgress = h->progress();
            }
        } else {
            auto order = watchOrder();
            target = order.empty() ? -1 : order[0];
            if (h) {
                int idx = videoIndex(h->videoId);
                if (idx >= 0 && !h->finished() && h->position > 30) {
                    target = idx;
                    label = "Resume";
                    resume = true;
                    resumeProgress = h->progress();
                } else if (idx >= 0) {  // finished: next episode
                    auto it = std::find(order.begin(), order.end(), idx);
                    if (it != order.end() && it + 1 != order.end()) target = *(it + 1);
                }
            }
            if (target < 0) return;
            label += " " + epCode(meta.videos[target]);
            // open the season of that episode
            int season = meta.videos[target].season;
            for (int i = 0; i < (int)seasons.size(); i++)
                if (seasons[i] == season) {
                    selectSeason(i);
                    for (int k = 0; k < (int)eps.size(); k++)
                        if (eps[k] == target) epSel = k;
                }
        }
        buttons.push_back({label, IconPlay, true, ActPlay, target});
        if (resume) buttons.push_back({"Start Over", IconRestart, false, ActStartOver, target});
        bool inLib = app.library.contains(meta.id);
        buttons.push_back({inLib ? "In My List" : "My List", inLib ? IconCheck : IconAdd, false, ActLibrary, target});
    }

    // Always through the stream list, so another stream can be picked (Continue Watching on the home
    // screen is the shortcut straight back to the same one).
    void play(App& app, int video, bool resume) {
        PlayContext ctx = video < 0 ? contextFor() : contextForEpisode(meta, meta.videos[video]);
        const HistoryEntry* h = app.history.findVideo(ctx.videoId);
        if (h && h->stream.valid()) ctx.stream = h->stream;  // marked "Last used", on top of the list
        app.push(makeStreamsScreen(ctx, resume));
    }

    void runButton(App& app, const Btn& b) {
        switch (b.action) {
            case ActPlay: play(app, b.video, true); break;
            case ActStartOver: play(app, b.video, false); break;
            case ActLibrary: {
                bool in = !app.library.contains(meta.id);
                MetaItem m = item;
                m.id = meta.id;
                m.type = meta.type;
                m.name = meta.name;
                m.poster = meta.poster;
                m.background = meta.background;
                m.logo = meta.logo;
                app.setInLibrary(m, in);
                app.toast(in ? "Added to My List" : "Removed from My List");
                buildButtons(app);
                break;
            }
        }
    }

    PlayContext contextFor() const {
        MetaItem m = item;
        m.id = meta.id;
        m.name = meta.name;
        m.poster = meta.poster;
        m.background = meta.background;
        m.logo = meta.logo;
        m.genres = meta.genreList;
        return contextForMovie(m);
    }

public:
    explicit DetailScreen(const MetaItem& it) : item(it) {}

    void start(App& app) {
        started = true;
        std::vector<Addon> cands;
        for (auto& a : app.addons)
            if (addonSupports(a, "meta", item.type, item.id)) cands.push_back(a);
        std::string type = item.type, id = item.id;
        task.start([cands, type, id]() {
            MetaResult r;
            for (auto& a : cands)
                if (fetchMeta(a, type, id, r.meta, r.err)) {
                    r.ok = true;
                    break;
                }
            if (!r.ok && cands.empty()) r.err = "No addon provides details for this item";
            return r;
        });
    }

    void update(App& app) override {
        if (!started) start(app);
        if (task.ready()) {
            MetaResult r = task.take();
            if (r.ok) {
                meta = r.meta;
                if (meta.name.empty()) meta.name = item.name;
                if (meta.background.empty()) meta.background = item.background;
                if (meta.logo.empty()) meta.logo = item.logo;
                if (meta.description.empty()) meta.description = item.description;
                loaded = true;
                buildSeasons();
                if (!seasons.empty()) selectSeason(0);
                buildButtons(app);
            } else {
                err = r.err;
            }
        }

        if (loaded && seenHistory != app.historyVersion) {  // came back from the player
            seenHistory = app.historyVersion;
            int keepSeason = seasonIdx, keepEp = epSel;
            buildButtons(app);
            if (focus != FocusButtons) {
                selectSeason(keepSeason);
                epSel = std::min(keepEp, (int)eps.size() - 1);
            }
        }

        const Input& in = app.in;
        int tid = 0, ta = 0, tb = 0;
        bool tap = app.tapped(tid, ta, tb);
        if (in.pressed(K_B) || (tap && tid == kHitBack)) {
            app.pop();
            return;
        }
        if (!loaded) return;
        if (tap) {  // touch: buttons, season chips, episode cards
            if (tid == 60 && ta < (int)buttons.size()) {
                focus = FocusButtons;
                btn = ta;
                runButton(app, buttons[ta]);
                return;
            }
            if (tid == 61 && ta < (int)seasons.size()) {
                focus = FocusSeasons;
                selectSeason(ta);
                return;
            }
            if (tid == 62 && ta < (int)eps.size()) {
                focus = FocusEpisodes;
                if (epSel == ta) play(app, eps[ta], true);
                epSel = ta;
                return;
            }
        }

        int maxFocus = isSeries() ? FocusEpisodes : FocusButtons;
        if (in.pressed(K_DOWN) && focus < maxFocus) focus++;
        if (in.pressed(K_UP) && focus > FocusButtons) focus--;
        if (seasons.size() > 1 && (in.pressed(K_L) || in.pressed(K_R))) {
            selectSeason((seasonIdx + (in.pressed(K_R) ? 1 : -1) + (int)seasons.size()) % (int)seasons.size());
        }

        switch (focus) {
            case FocusButtons:
                if (in.pressed(K_RIGHT) && btn + 1 < (int)buttons.size()) btn++;
                if (in.pressed(K_LEFT) && btn > 0) btn--;
                if (in.pressed(K_A) && btn < (int)buttons.size()) runButton(app, buttons[btn]);
                break;
            case FocusSeasons:
                if (in.pressed(K_RIGHT) && seasonIdx + 1 < (int)seasons.size()) selectSeason(seasonIdx + 1);
                if (in.pressed(K_LEFT) && seasonIdx > 0) selectSeason(seasonIdx - 1);
                if (in.pressed(K_A)) focus = FocusEpisodes;
                break;
            case FocusEpisodes:
                if (in.pressed(K_RIGHT) && epSel + 1 < (int)eps.size()) epSel++;
                if (in.pressed(K_LEFT) && epSel > 0) epSel--;
                if (in.pressed(K_A) && !eps.empty()) play(app, eps[epSel], true);
                break;
        }
    }

    void draw(App& app) override {
        Gfx& g = app.gfx;
        const std::string& bgUrl = loaded ? meta.background : item.background;
        drawBackdrop(g, bgUrl);

        // Title: logo artwork when there is one.
        Uint8 lf = 0;
        SDL_Texture* logo = g.image(loaded ? meta.logo : item.logo, &lf);
        int top = 64;
        if (logo)
            g.drawFit(logo, kX, top, 480, 140, AlignLeft, true, lf);
        else
            g.textWrapped(loaded ? meta.name : item.name, kX, top + 50, 46, col::text, 700, 110);

        if (!loaded) {
            if (!err.empty())
                g.textWrapped("Error: " + err, kX, 240, 22, col::error, 700, 120);
            else
                app.drawLoading(kX, 240, "Loading details", AlignLeft);
            drawBackButton(app);
            app.drawFooter("B Back");
            return;
        }

        int y = 220, x = kX;
        int rw = drawRating(g, x, y, meta.imdbRating);
        if (rw) x += rw + 20;
        for (const std::string& part : {meta.releaseInfo, meta.runtime}) {
            if (part.empty()) continue;
            x += g.text(part, x, y, 20, col::text) + 20;
        }
        if (isSeries()) {
            std::string s = std::to_string(std::count_if(seasons.begin(), seasons.end(), [](int v) { return v > 0; })) +
                            " Seasons";
            int tw = g.textWidth(s, 16) + 14;
            g.roundOutline(x, y + 1, tw, 24, 5, 1, col::dim);
            g.text(s, x + 7, y + 3, 16, col::dim);
            x += tw + 20;
        }
        g.text(joinGenres(meta.genreList, 3), x, y, 20, col::dim, W - x - 60);

        bool series = isSeries();
        int descH = series ? 78 : 130;
        int dh = g.textWrapped(meta.description, kX, 258, 19, col::text, 640, descH);
        int infoY = 258 + dh + 10;
        if (!series) {
            if (!meta.cast.empty()) g.text("Starring: " + meta.cast, kX, infoY, 17, col::dim, 700), infoY += 26;
            if (!meta.director.empty()) g.text("Director: " + meta.director, kX, infoY, 17, col::dim, 700);
        }

        int by = series ? 352 : 470;
        int bx = kX;
        for (int i = 0; i < (int)buttons.size(); i++) {
            int w = buttonWidth(g, buttons[i].label, buttons[i].icon);
            drawButton(g, bx, by, w, 52, buttons[i].label, focus == FocusButtons && btn == i, buttons[i].primary,
                       buttons[i].icon);
            if (i == 0 && resumeProgress >= 0) drawProgress(g, bx + 20, by + 62, w - 40, 4, resumeProgress);
            app.hit(bx, by, w, 52, 60, i);
            bx += w + 18;
        }

        if (series) drawEpisodes(app);
        drawBackButton(app);
        app.drawFooter(series ? (focus == FocusEpisodes ? "A Play   L/R Season   B Back" : "A Select   L/R Season   B Back")
                              : "A Select   B Back");
    }

    void drawEpisodes(App& app) {
        Gfx& g = app.gfx;
        // Season chips
        int y = 428, x = kX - (int)seasonScroll;
        float selX = 0;
        for (int i = 0; i < (int)seasons.size(); i++) {
            std::string label = seasons[i] == 0 ? "Specials" : "Season " + std::to_string(seasons[i]);
            int w = g.textWidth(label, 19) + 32;
            bool s = i == seasonIdx;
            if (s) selX = (float)(x + (int)seasonScroll);
            if (s)
                g.roundRect(x, y, w, 36, 18, focus == FocusSeasons ? col::white : col::white.alpha(50));
            else
                g.roundOutline(x, y, w, 36, 18, 1, col::white.alpha(60));
            g.text(label, x + 16, y + 7, 19, s && focus == FocusSeasons ? col::black : s ? col::text : col::dim);
            app.hit(x, y, w, 36, 61, i);
            x += w + 12;
        }
        seasonScroll = approach(seasonScroll, std::max(0.0f, selX - 600), 0.25f);

        // Episode cards
        const int cw = 256, ch = 144, step = cw + 16, cy = 482;
        float target = (float)std::max(0, epSel - 2) * step;
        epScroll = approach(epScroll, target, 0.25f);
        for (int k = 0; k < (int)eps.size(); k++) {
            int ex = kX + k * step - (int)epScroll;
            if (ex > W || ex + cw < 0) continue;
            const Video& v = meta.videos[eps[k]];
            bool s = focus == FocusEpisodes && k == epSel;
            g.rect(ex, cy, cw, ch, col::card);
            Uint8 fade = 0;
            SDL_Texture* t = g.image(v.thumbnail.empty() ? meta.background : v.thumbnail, &fade);
            if (t) g.drawCover(t, ex, cy, cw, ch, fade);
            const HistoryEntry* h = app.history.findVideo(v.id);
            if (h && h->finished()) {
                g.rect(ex, cy, cw, ch, col::black.alpha(110));
                g.roundRect(ex + cw - 86, cy + 10, 76, 24, 12, col::black.alpha(170));
                g.text("Watched", ex + cw - 48, cy + 12, 15, col::text, 0, AlignCenter);
            } else if (h) {
                drawProgress(g, ex, cy + ch - 5, cw, 5, h->progress());
            }
            if (s) {
                g.outline(ex - 3, cy - 3, cw + 6, ch + 6, 3, col::white);
                g.circle(ex + cw / 2, cy + ch / 2, 24, col::black.alpha(150));
                g.triangle(ex + cw / 2 - 7, cy + ch / 2 - 12, ex + cw / 2 - 7, cy + ch / 2 + 12, ex + cw / 2 + 13,
                           cy + ch / 2, col::white);
            }
            std::string title = (v.episode > 0 ? std::to_string(v.episode) + ".  " : "") +
                                (v.title.empty() ? "Episode " + std::to_string(v.episode) : v.title);
            g.text(title, ex, cy + ch + 8, 18, s ? col::text : col::dim, cw);
            app.hit(ex, cy, cw, ch, 62, k);
        }
        if (focus == FocusEpisodes && epSel < (int)eps.size()) {
            const Video& v = meta.videos[eps[epSel]];
            std::string info = v.released.empty() ? v.overview : v.released + (v.overview.empty() ? "" : "  ·  " + v.overview);
            g.text(info, kX, cy + ch + 36, 17, col::faint, W - 2 * kX);
        }
    }
};

}  // namespace

std::unique_ptr<Screen> makeDetailScreen(const MetaItem& item) {
    return std::make_unique<DetailScreen>(item);
}
