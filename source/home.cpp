// Home, Movies, Series and Search: a hero banner plus horizontal rows, streaming-service style.

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#include "downloads.hpp"
#include "http.hpp"
#include "keyboard.hpp"
#include "log.hpp"
#include "screens.hpp"
#include "thread.hpp"

namespace {

constexpr int kMarginX = 60;
constexpr int kGap = 16;
constexpr int kHeroRowsTop = 500;  // where rows start below the hero (content coordinates)
constexpr uint32_t kHeroInterval = 9000;
constexpr uint32_t kHeroFadeMs = 700;
constexpr size_t kRowLimit = 40;

struct Card {
    MetaItem item;
    bool isContinue = false;
    bool fromHistory = false;  // Continue Watching / Recently Watched: can be removed
    HistoryEntry entry;
};

struct Row {
    std::string key;  // its Settings > Home sections key (home screen rows)
    std::string title;
    bool landscape = false;  // Continue Watching: backdrop cards with the title on them
    // The catalog's own artwork shape: the addon's posterShape or, when it doesn't say, the first image.
    enum Shape { Tall, Wide, Square } shape = Tall;
    bool shapeKnown = false;
    bool feedsHero = false;
    std::vector<Card> cards;
    std::function<CatalogResult()> loader;
    Task<CatalogResult> task;
    bool started = false, loaded = false;
    int retries = 0;  // a failed fetch (network busy, addon down) is tried again a few times
    uint32_t retryAt = 0;
    int sel = 0;
    float scrollX = 0;

    int cardW() const { return landscape || shape == Wide ? 300 : shape == Square ? 210 : 158; }
    int cardH() const { return landscape || shape == Wide ? 169 : shape == Square ? 210 : 237; }
    bool hidden() const { return loaded && cards.empty(); }
    int height() const { return hidden() ? 0 : 44 + cardH() + (landscape ? 66 : 60); }
};

struct PageAction {
    enum Kind { None, Details, Play, Resume, OtherStreams, Remove } kind = None;
    MetaItem item;
    HistoryEntry entry;
};

std::function<CatalogResult()> catalogLoader(const Addon& a, const CatalogInfo& c, const std::string& extra,
                                              const std::set<std::string>& exclude = {}) {
    return [a, c, extra, exclude]() {
        CatalogResult r;
        for (auto& it : fetchCatalog(a, c, extra, r.err)) {
            if (exclude.count(it.id)) continue;
            r.items.push_back(it);
            if (r.items.size() >= kRowLimit) break;
        }
        return r;
    };
}

// Several genre catalogs merged by alternating their items.
std::function<CatalogResult()> mixedLoader(std::vector<std::pair<Addon, CatalogInfo>> sources,
                                            std::vector<std::string> extras, std::set<std::string> exclude) {
    return [sources, extras, exclude]() {
        std::vector<std::vector<MetaItem>> lists;
        CatalogResult r;
        for (size_t i = 0; i < sources.size(); i++) {
            std::string err;
            lists.push_back(fetchCatalog(sources[i].first, sources[i].second, extras[i], err));
            if (!err.empty()) r.err = err;
        }
        std::set<std::string> seen(exclude);
        for (size_t k = 0; r.items.size() < kRowLimit; k++) {
            bool any = false;
            for (auto& l : lists) {
                if (k >= l.size()) continue;
                any = true;
                if (seen.insert(l[k].id).second) r.items.push_back(l[k]);
            }
            if (!any) break;
        }
        if (!r.items.empty()) r.err.clear();
        return r;
    };
}

// ---------------------------------------------------------------- BrowsePage

class BrowsePage {
public:
    bool hasHero = true;
    int rowsTop = kHeroRowsTop;
    std::vector<Row> rows;
    std::vector<MetaItem> hero;
    std::string emptyText;

    void reset() {
        rows.clear();
        hero.clear();
        heroBuilt = false;
        heroIdx = 0;
        heroPrev = -1;
        toTop();
    }

    void toTop() {
        focus = hasHero ? 0 : 1;
        heroBtn = 0;
        scrollY = 0;
    }

    bool loading() const {
        for (auto& r : rows)
            if (!r.loaded) return true;
        return false;
    }

    PageAction update(App& app) {
        PageAction act;
        int focusRow = std::max(0, focus - 1), fixedRows = 0;  // rows with nothing to fetch
        for (auto& r : rows) fixedRows += !r.loader;
        for (size_t i = 0; i < rows.size(); i++) {
            Row& r = rows[i];
            // Rows load as they come near (rows with nothing to fetch do not count); the ones the hero
            // is built from always load.
            if (!r.started && r.loader && SDL_GetTicks() >= r.retryAt &&
                ((int)i <= focusRow + 3 + fixedRows || r.feedsHero)) {
                r.started = true;
                auto ld = r.loader;
                r.task.start([ld]() { return ld(); });
            }
            if (r.task.ready()) {
                CatalogResult res = r.task.take();
                if (res.items.empty() && !res.err.empty() && r.retries < 4) {  // again in 3, 6, 12, 24 s
                    logMsg("home: %s failed (%s), retrying", r.title.c_str(), res.err.c_str());
                    r.started = false;
                    r.retryAt = SDL_GetTicks() + (3000u << r.retries++);
                    continue;
                }
                for (auto& it : res.items) r.cards.push_back(Card{it});
                for (auto& it : res.items) {
                    if (it.posterShape.empty()) continue;
                    r.shape = it.posterShape == "landscape" ? Row::Wide : it.posterShape == "square" ? Row::Square : Row::Tall;
                    r.shapeKnown = true;
                    break;
                }
                r.loaded = true;
            }
        }
        buildHero();
        if (hasHero && hero.size() > 1 && focus != 0 && SDL_GetTicks() - heroChanged > kHeroInterval) {
            heroPrev = heroIdx;
            heroIdx = (heroIdx + 1) % (int)hero.size();
            heroChanged = SDL_GetTicks();
        }
        if (focus >= 1 && focus <= (int)rows.size() && rows[focus - 1].hidden()) moveFocus(1);

        const Input& in = app.in;
        if (in.pressed(K_DOWN)) moveFocus(1);
        if (in.pressed(K_UP)) moveFocus(-1);

        // Touch: tapping a card selects it; tapping the selected card (or a hero button) opens it.
        int tid = 0, ta = 0, tb = 0;
        bool tapOpen = false, tapRemove = false;
        if (app.tapped(tid, ta, tb)) {
            if (tid == 10 && hasHero) {
                focus = 0;
                heroBtn = ta;
                tapOpen = true;
            } else if (tid == 11 && ta < (int)rows.size() && tb < (int)rows[ta].cards.size()) {
                tapOpen = focus == ta + 1 && rows[ta].sel == tb;
                focus = ta + 1;
                rows[ta].sel = tb;
            } else if (tid == 12 && ta < (int)rows.size() && tb < (int)rows[ta].cards.size()) {
                tapRemove = true;  // the X on a history card
                focus = ta + 1;
                rows[ta].sel = tb;
            }
        }

        if (focus == 0 && hasHero) {
            if (hero.empty()) return act;
            if (in.pressed(K_RIGHT)) heroBtn = 1;
            if (in.pressed(K_LEFT)) heroBtn = 0;
            if (in.pressed(K_A) || tapOpen) {
                act.item = hero[heroIdx];
                act.kind = heroBtn == 0 && act.item.type == "movie" ? PageAction::Play : PageAction::Details;
            }
            return act;
        }
        if (focus < 1 || focus > (int)rows.size()) return act;
        Row& r = rows[focus - 1];
        int n = (int)r.cards.size();
        if (n == 0) return act;
        if (in.pressed(K_RIGHT) && r.sel + 1 < n) r.sel++;
        if (in.pressed(K_LEFT) && r.sel > 0) r.sel--;
        const Card& c = r.cards[std::min(r.sel, n - 1)];
        act.item = c.item;
        act.entry = c.entry;
        if (in.pressed(K_A) || tapOpen)
            act.kind = c.isContinue ? PageAction::Resume : PageAction::Details;
        else if (in.pressed(K_Y))
            act.kind = PageAction::Details;
        else if (in.pressed(K_X) && c.isContinue)
            act.kind = PageAction::OtherStreams;
        else if ((in.pressed(K_MINUS) || tapRemove) && c.fromHistory)
            act.kind = PageAction::Remove;
        return act;
    }

    // The focused card is in Continue Watching / Recently Watched (for the footer hint).
    bool historyFocused() const {
        if (focus < 1 || focus > (int)rows.size()) return false;
        const Row& r = rows[focus - 1];
        return !r.cards.empty() && r.cards[std::min(r.sel, (int)r.cards.size() - 1)].fromHistory;
    }

    void draw(App& app) {
        float target = 0;
        if (focus >= 1 && focus <= (int)rows.size())
            target = std::max(0.0f, rowY(focus - 1) - (hasHero ? 300.0f : (float)rowsTop));
        scrollY = approach(scrollY, target, 0.2f);

        if (hasHero) drawHero(app, (int)-scrollY);
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i].hidden()) continue;
            int y = (int)(rowY((int)i) - scrollY);
            if (y > H || y + rows[i].height() < 0) continue;
            drawRow(app, rows[i], (int)i, y, focus == (int)i + 1);
        }
        if (rows.empty() || (!loading() && std::all_of(rows.begin(), rows.end(), [](const Row& r) { return r.hidden(); }))) {
            if (!emptyText.empty()) app.gfx.text(emptyText, W / 2, hasHero ? 520 : 330, 22, col::dim, W - 120, AlignCenter);
        }
    }

private:
    bool heroBuilt = false;
    int heroIdx = 0, heroPrev = -1, focus = 0, heroBtn = 0;
    uint32_t heroChanged = 0;
    float scrollY = 0;

    void moveFocus(int d) {
        int f = focus + d;
        while (f >= 1 && f <= (int)rows.size() && rows[f - 1].hidden()) f += d;
        int minF = hasHero ? 0 : 1;
        if (f >= minF && f <= (int)rows.size()) focus = f;
    }

    float rowY(int idx) const {
        float y = (float)rowsTop;
        for (int i = 0; i < idx; i++) y += rows[i].height();
        return y;
    }

    void buildHero() {
        if (!hasHero || heroBuilt) return;
        std::vector<const Row*> feeds;
        for (auto& r : rows)
            if (r.feedsHero) {
                if (!r.loaded) return;  // wait until every source row is in
                feeds.push_back(&r);
            }
        heroBuilt = true;
        for (size_t k = 0; hero.size() < 6 && k < 12; k++)
            for (auto* r : feeds)
                if (k < r->cards.size() && !r->cards[k].item.background.empty() && hero.size() < 6)
                    hero.push_back(r->cards[k].item);
        heroChanged = SDL_GetTicks();
    }

    void drawHero(App& app, int offY) {
        Gfx& g = app.gfx;
        if (hero.empty()) {
            if (!heroBuilt) app.drawLoading(W / 2, 260, "Loading");
            return;
        }
        uint32_t since = SDL_GetTicks() - heroChanged;
        float t = std::min(1.0f, (float)since / kHeroFadeMs);
        const MetaItem& m = hero[heroIdx];

        // Artwork: previous one underneath while the new one fades in, with a little parallax.
        int bgY = (int)(offY * 0.5f);
        if (heroPrev >= 0 && t < 1)
            if (SDL_Texture* p = g.image(hero[heroPrev].background)) g.drawCover(p, 0, bgY, W, H);
        Uint8 fade = 0;
        if (SDL_Texture* bg = g.image(m.background, &fade)) g.drawCover(bg, 0, bgY, W, H, (Uint8)(fade * t));
        g.gradientH(0, 0, 900, H, col::bg, 240, 0);
        g.gradientV(0, offY + 280, W, 220, col::bg, 0, 255);
        g.rect(0, offY + 500, W, std::max(0, H - (offY + 500)), col::bg);

        Uint8 a = (Uint8)(std::min(1.0f, t * 1.5f) * 255) & 0xE0;  // quantized to keep the text cache small
        if (t >= 1) a = 255;
        int y = offY + 100;
        Uint8 lf = 0;
        SDL_Texture* logo = g.image(m.logo, &lf);
        if (logo) {
            g.drawFit(logo, kMarginX, y, 460, 150, AlignLeft, true, std::min(lf, a));
        } else {
            g.textWrapped(m.name, kMarginX, y + 60, 50, col::text.alpha(a), 640, 120);
        }
        int my = offY + 272;
        int x = kMarginX;
        int rw = drawRating(g, x, my, m.imdbRating);
        if (rw) x += rw + 18;
        if (!m.releaseInfo.empty()) x += g.text(m.releaseInfo, x, my, 20, col::text.alpha(a)) + 18;
        std::string typeLabel = m.type == "series" ? "Series" : m.type == "movie" ? "Movie" : "";
        if (!typeLabel.empty()) {
            int tw = g.textWidth(typeLabel, 16) + 14;
            g.roundOutline(x, my + 1, tw, 24, 5, 1, col::dim.alpha(a));
            g.text(typeLabel, x + 7, my + 3, 16, col::dim.alpha(a));
            x += tw + 18;
        }
        g.text(joinGenres(m.genres, 3), x, my, 20, col::dim.alpha(a), W - x - 60);
        g.textWrapped(m.description, kMarginX, offY + 310, 20, col::text.alpha(a), 560, 84);

        bool heroFocus = focus == 0;
        int by = offY + 410;
        std::string playLabel = m.type == "movie" ? "Play" : "Episodes";
        int w0 = buttonWidth(g, playLabel, IconPlay);
        drawButton(g, kMarginX, by, w0, 52, playLabel, heroFocus && heroBtn == 0, true, IconPlay);
        app.hit(kMarginX, by, w0, 52, 10, 0);
        app.hit(kMarginX + w0 + 18, by, buttonWidth(g, "More Info", IconInfo), 52, 10, 1);
        drawButton(g, kMarginX + w0 + 18, by, buttonWidth(g, "More Info", IconInfo), 52, "More Info",
                   heroFocus && heroBtn == 1, false, IconInfo);

        // Page dots
        int dx = W - kMarginX - (int)hero.size() * 18;
        for (int i = 0; i < (int)hero.size(); i++)
            g.roundRect(dx + i * 18, by + 22, i == heroIdx ? 14 : 8, 8, 4, i == heroIdx ? col::white : col::white.alpha(90));
    }

    void drawRow(App& app, Row& r, int rowIdx, int y, bool focused) {
        Gfx& g = app.gfx;
        g.text(r.title, kMarginX, y, 24, focused ? col::text : col::text.alpha(200), W - 2 * kMarginX);
        int cw = r.cardW(), ch = r.cardH(), step = cw + kGap;
        int cy = y + 44;
        int n = (int)r.cards.size();

        int slots = (W - 2 * kMarginX) / step;
        float target = (float)std::max(0, r.sel - (slots - 2)) * step;
        float maxX = (float)std::max(0, n * step - kGap - (W - 2 * kMarginX));
        r.scrollX = approach(r.scrollX, std::min(target, maxX), 0.25f);

        if (!r.shapeKnown && !r.landscape && n > 0) {  // the addon didn't say: go by the first image
            if (SDL_Texture* t = g.image(r.cards[0].item.poster)) {
                int tw = 0, th = 0;
                SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
                if (tw > th * 1.2f) r.shape = Row::Wide;
                else if (std::abs(tw - th) < tw / 10) r.shape = Row::Square;
                r.shapeKnown = true;
            }
        }
        if (n == 0 && !r.loaded) {  // skeleton cards while loading
            Uint8 pulse = (Uint8)(40 + 25 * std::sin(SDL_GetTicks() / 250.0));
            for (int i = 0; i * step < W; i++) g.roundRect(kMarginX + i * step, cy, cw, ch, 6, col::card.alpha(pulse + 120));
            return;
        }
        for (int i = 0; i < n; i++) {
            int x = kMarginX + i * step - (int)r.scrollX;
            if (x > W || x + cw < 0) continue;
            bool sel = focused && i == r.sel;
            if (r.landscape)
                drawLandscape(g, r.cards[i], x, cy, cw, ch, sel);
            else
                drawPoster(g, r.cards[i], x, cy, cw, ch, sel);
            app.hit(x, cy, cw, ch, 11, rowIdx, i);
            if (sel && r.cards[i].fromHistory) {  // × to remove it (touch / pointer; - on the controller)
                int bx = x + cw - 22, by = cy + 4;
                g.text("\xC3\x97", bx + 1, by + 1, 30, col::black.alpha(150), 0, AlignCenter);  // soft shadow
                g.text("\xC3\x97", bx, by, 30, col::white, 0, AlignCenter);
                app.hit(bx - 22, by - 6, 44, 44, 12, rowIdx, i);
            }
        }
    }

public:
    static void drawPoster(Gfx& g, const Card& c, int x, int y, int w, int h, bool sel) {
        if (sel) {  // grow from the center
            int gw = w / 14, gh = h / 14;
            x -= gw / 2;
            y -= gh / 2;
            w += gw;
            h += gh;
            g.rect(x + 6, y + 10, w, h, col::black.alpha(110));
        }
        Uint8 fade = 0;
        SDL_Texture* t = g.image(c.item.poster, &fade);
        g.rect(x, y, w, h, col::card);
        int tw = 0, th = 0;
        if (t) SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
        if (t && tw > 0 && th > 0) {
            float s = std::min((float)w / tw, (float)h / th);
            int dw = (int)(tw * s), dh = (int)(th * s);
            if (std::abs(dw - w) < w / 8 && std::abs(dh - h) < h / 8)
                g.drawTex(t, x, y, w, h, fade);  // the card's shape: fill it
            else
                g.drawTex(t, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, fade);  // another shape: whole, never squashed
        } else
            g.textWrapped(c.item.name, x + 12, y + 14, 19, col::dim, w - 24, h - 28);
        if (c.entry.duration > 0) {  // from the watch history
            if (c.entry.finished()) {
                g.roundRect(x + w - 82, y + 8, 74, 24, 12, col::black.alpha(180));
                g.text("Watched", x + w - 45, y + 10, 15, col::text, 0, AlignCenter);
            } else {
                drawProgress(g, x, y + h - 5, w, 5, c.entry.progress());
            }
        }
        if (sel) {
            g.outline(x - 3, y - 3, w + 6, h + 6, 3, col::white);
            g.text(c.item.name, x, y + h + 10, 19, col::text, w + 60);
            if (!c.entry.episodeLabel.empty()) g.text(c.entry.episodeLabel, x, y + h + 34, 16, col::dim, w + 60);
        }
    }

    static void drawLandscape(Gfx& g, const Card& c, int x, int y, int w, int h, bool sel) {
        if (sel) {
            int gw = w / 16, gh = h / 16;
            x -= gw / 2;
            y -= gh / 2;
            w += gw;
            h += gh;
            g.rect(x + 6, y + 10, w, h, col::black.alpha(110));
        }
        g.rect(x, y, w, h, col::card);
        Uint8 fade = 0;
        if (SDL_Texture* t = g.image(c.item.background, &fade)) g.drawCover(t, x, y, w, h, fade);
        g.gradientV(x, y + h / 3, w, h - h / 3, col::black, 0, 200);
        Uint8 lf = 0;
        if (SDL_Texture* logo = g.image(c.item.logo, &lf))
            g.drawFit(logo, x + 14, y + h - 74, w / 2, 52, AlignLeft, true, lf);
        else
            g.text(c.item.name, x + 14, y + h - 48, 22, col::text, w - 28);
        if (c.isContinue) {
            // play badge
            g.circle(x + w - 30, y + h - 34, 18, col::black.alpha(150));
            g.triangle(x + w - 36, y + h - 44, x + w - 36, y + h - 24, x + w - 20, y + h - 34, col::white);
            drawProgress(g, x, y + h - 5, w, 5, c.entry.progress());
        }
        if (sel) g.outline(x - 3, y - 3, w + 6, h + 6, 3, col::white);
        std::string sub = c.entry.episodeLabel.empty() ? c.item.name : c.entry.episodeLabel;
        g.text(sub, x, y + h + 10, 18, sel ? col::text : col::dim, w);
        if (c.isContinue)
            g.text(formatLeft(c.entry.duration - c.entry.position), x, y + h + 34, 16, col::faint, w);
    }
};

// ---------------------------------------------------------------- catalogs helpers

struct CatalogPick {
    const Addon* addon = nullptr;
    CatalogInfo info;
    explicit operator bool() const { return addon != nullptr; }
};

std::vector<CatalogPick> browsable(const std::vector<Addon>& addons, const std::string& type) {
    std::vector<CatalogPick> out;
    for (auto& c : listCatalogs(addons))
        if (c.type == type && !c.required) out.push_back(CatalogPick{&addons[c.addon], c});
    return out;
}

CatalogPick genreCatalog(const std::vector<Addon>& addons, const std::string& type, const std::string& genre) {
    for (auto& c : listCatalogs(addons)) {
        if (c.type != type || c.genres.empty()) continue;
        if (std::find(c.genres.begin(), c.genres.end(), genre) != c.genres.end()) {
            // A catalog that only requires "genre" is fine since we pass one.
            return CatalogPick{&addons[c.addon], c};
        }
    }
    return {};
}

std::string typeLabel(const std::string& type) {
    if (type == "movie") return "Movies";
    if (type == "series") return "Series";
    if (type == "tv") return "TV";
    std::string t = type;  // anime, channel, other...: the addon's own word
    if (!t.empty()) t[0] = (char)toupper((unsigned char)t[0]);
    return t;
}

// Every type the addons publish catalogs for: movies and series first, then the rest in addon order.
std::vector<std::string> catalogTypes(const std::vector<Addon>& addons) {
    std::vector<std::string> types = {"movie", "series"};
    for (auto& c : listCatalogs(addons))
        if (std::find(types.begin(), types.end(), c.type) == types.end()) types.push_back(c.type);
    return types;
}

struct HomeSection {
    std::string key, title, type, addonName;
    CatalogPick pick;  // points into App::addons: use it right away, not after a reload
    bool on = true;
    bool fixed = false;  // Continue Watching, My List, Recently Watched: built from local data
};

// The home screen's rows: Continue Watching, My List and Recently Watched, then every catalog the
// addons offer, in the order picked in Settings > Home sections. Ones not placed yet keep the default
// order (those three, the first movie and series catalogs, usually "Popular", then the rest by type).
// Switched-off ones come back with on = false.
std::vector<HomeSection> homeSections(App& app) {
    std::vector<HomeSection> out, rest;
    const auto& hidden = app.cfg.homeHidden;
    for (auto& f : {std::pair<const char*, const char*>{"#continue", "Continue Watching"},
                    {"#mylist", "My List"}, {"#recent", "Recently Watched"}}) {
        HomeSection s;
        s.key = f.first;
        s.title = f.second;
        s.addonName = s.key == "#mylist" ? "Your Stremio library" : "Your watch history";
        s.fixed = true;
        s.on = std::find(hidden.begin(), hidden.end(), s.key) == hidden.end();
        out.push_back(s);
    }
    std::set<std::string> used;
    for (auto& type : catalogTypes(app.addons)) {
        bool first = true;
        for (auto& c : browsable(app.addons, type)) {
            std::string key = c.addon->id + "/" + type + "/" + c.info.id;
            if (!used.insert(key).second) continue;
            HomeSection s;
            s.key = key;
            s.title = c.info.name + " " + typeLabel(type);
            if (c.addon->id != "com.linvo.cinemeta") s.title += "  ·  " + c.addon->name;
            s.type = type;
            s.addonName = c.addon->name;
            s.pick = c;
            s.on = std::find(hidden.begin(), hidden.end(), key) == hidden.end();
            bool lead = first && (type == "movie" || type == "series");
            (lead ? out : rest).push_back(s);
            first = false;
        }
    }
    out.insert(out.end(), rest.begin(), rest.end());
    const auto& order = app.cfg.homeOrder;
    auto rank = [&](const HomeSection& s) {  // not placed yet: history rows on top, new catalogs at the end
        auto it = std::find(order.begin(), order.end(), s.key);
        return it != order.end() ? (size_t)(it - order.begin()) + 1 : s.fixed ? 0 : order.size() + 1;
    };
    std::stable_sort(out.begin(), out.end(), [&](const HomeSection& a, const HomeSection& b) { return rank(a) < rank(b); });
    return out;
}

const char* const kPreferredGenres[] = {"Action",   "Comedy",  "Drama",     "Thriller",    "Sci-Fi",
                                        "Animation", "Horror", "Adventure", "Crime",       "Romance",
                                        "Fantasy",  "Family",  "Mystery",   "Documentary", "Reality-TV"};

// ---------------------------------------------------------------- Discover

// Stremio-style Discover: pick a type, a catalog and a genre, then browse a poster grid.
class DiscoverPage {
public:
    void reset(App& app) {
        types = catalogTypes(app.addons);
        cats.assign(types.size(), {});
        for (auto& c : listCatalogs(app.addons)) {
            if (c.required && c.genres.empty()) continue;  // needs something we can't offer
            size_t t = std::find(types.begin(), types.end(), c.type) - types.begin();
            cats[t].push_back(Cat{app.addons[c.addon], c});
        }
        typeIdx = catIdx = genreIdx = 0;
        focus = 0;
        chip = 0;
        load(false);
    }

    PageAction update(App& app) {
        PageAction act;
        if (task.ready()) {
            CatalogResult r = task.take();
            size_t before = items.size();
            for (auto& it : r.items)
                if (std::none_of(items.begin(), items.end(), [&](const MetaItem& m) { return m.id == it.id; }))
                    items.push_back(it);
            if (items.size() == before) more = false;
            if (items.empty() && !r.err.empty()) err = r.err;
        }
        const Input& in = app.in;
        int tid = 0, ta = 0, tb = 0;
        bool tap = app.tapped(tid, ta, tb);
        if (tap && tid == 20) {  // filter chip
            focus = 0;
            chip = ta;
            cycle(1);
            return act;
        }
        if (tap && tid == 21 && ta < (int)items.size()) {
            bool open = focus == 1 && sel == ta;
            focus = 1;
            sel = ta;
            if (open) {
                act.kind = PageAction::Details;
                act.item = items[sel];
            }
            return act;
        }
        if (focus == 0) {
            if (in.pressed(K_LEFT) && chip > 0) chip--;
            if (in.pressed(K_RIGHT) && chip < 2) chip++;
            if (in.pressed(K_A)) cycle(1);
            if (in.pressed(K_Y)) cycle(-1);
            if (in.pressed(K_DOWN) && !items.empty()) focus = 1;
            return act;
        }
        int n = (int)items.size();
        if (n == 0) {
            focus = 0;
            return act;
        }
        if (in.pressed(K_RIGHT) && sel + 1 < n && sel % kCols != kCols - 1) sel++;
        if (in.pressed(K_LEFT) && sel % kCols != 0) sel--;
        if (in.pressed(K_DOWN)) sel = std::min(n - 1, sel + kCols);
        if (in.pressed(K_UP)) {
            if (sel < kCols) focus = 0;
            else sel -= kCols;
        }
        if (in.pressed(K_A) || in.pressed(K_Y)) {
            act.kind = PageAction::Details;
            act.item = items[sel];
        }
        if (more && !task.active() && sel / kCols >= (n - 1) / kCols - 1) load(true);
        return act;
    }

    void draw(App& app) {
        Gfx& g = app.gfx;
        // Filter chips
        const auto& list = cats[typeIdx];
        std::string labels[3] = {"Type: " + typeLabel(types.empty() ? "movie" : types[typeIdx]),
                                 "Catalog: " + (list.empty() ? std::string("none") : catLabel(list[catIdx])),
                                 "Genre: " + (list.empty() ? std::string("-") : genreLabel())};
        int x = 60;
        for (int k = 0; k < 3; k++) {
            int w = g.textWidth(labels[k], 20) + 56;
            bool f = focus == 0 && chip == k;
            g.roundRect(x, 90, w, 42, 21, f ? col::white : col::card.alpha(220));
            g.text(labels[k], x + 20, 99, 20, f ? col::black : col::text, w - 40);
            g.triangle(x + w - 26, 107, x + w - 16, 107, x + w - 21, 113, f ? col::black : col::dim);
            app.hit(x, 90, w, 42, 20, k);
            x += w + 14;
        }

        // Poster grid
        const int top = 156, cw = 144, ch = 216, gapX = 20, cellH = 270, left = 76;
        float target = (float)std::max(0, (sel / kCols - 1) * cellH);
        if (focus == 0) target = 0;
        scroll = approach(scroll, target, 0.22f);
        g.clip(0, top - 8, W, H - top - 32);
        for (int i = 0; i < (int)items.size(); i++) {
            int cx = left + (i % kCols) * (cw + gapX);
            int cy = top + 8 + (i / kCols) * cellH - (int)scroll;
            if (cy + cellH < top || cy > H) continue;
            BrowsePage::drawPoster(g, Card{items[i]}, cx, cy, cw, ch, focus == 1 && i == sel);
            app.hit(cx, std::max(cy, top), cw, ch, 21, i);
        }
        g.unclip();
        if (items.empty()) {
            if (task.active())
                app.drawLoading(W / 2, 330, "Loading");
            else
                g.text(err.empty() ? "Nothing here" : "Error: " + err, W / 2, 330, 22, col::dim, W - 120, AlignCenter);
        }
    }

    bool chipsFocused() const { return focus == 0; }

private:
    static constexpr int kCols = 7;
    struct Cat {
        Addon addon;
        CatalogInfo info;
    };
    std::vector<std::string> types;  // movie, series, then whatever else the addons have
    std::vector<std::vector<Cat>> cats;  // per type
    int typeIdx = 0, catIdx = 0, genreIdx = 0;
    int focus = 0, chip = 0, sel = 0;
    float scroll = 0;
    std::vector<MetaItem> items;
    Task<CatalogResult> task;
    bool more = true;
    std::string err;

    static std::string catLabel(const Cat& c) {
        return c.addon.id == "com.linvo.cinemeta" ? c.info.name : c.info.name + " (" + c.addon.name + ")";
    }

    std::vector<std::string> genreOptions() const {
        std::vector<std::string> o;
        if (cats[typeIdx].empty()) return o;
        const Cat& c = cats[typeIdx][catIdx];
        if (!c.info.required) o.push_back("All");
        o.insert(o.end(), c.info.genres.begin(), c.info.genres.end());
        return o;
    }

    std::string genreLabel() const {
        auto o = genreOptions();
        return o.empty() ? "All" : o[std::min(genreIdx, (int)o.size() - 1)];
    }

    void cycle(int dir) {
        if (chip == 0) {
            int n = std::max(1, (int)types.size());
            typeIdx = (typeIdx + dir + n) % n;
            catIdx = genreIdx = 0;
        } else if (chip == 1 && !cats[typeIdx].empty()) {
            int n = (int)cats[typeIdx].size();
            catIdx = (catIdx + dir + n) % n;
            genreIdx = 0;
        } else if (chip == 2) {
            int n = (int)genreOptions().size();
            if (n == 0) return;
            genreIdx = (genreIdx + dir + n) % n;
        }
        load(false);
    }

    void load(bool append) {
        if (!append) {
            items.clear();
            sel = 0;
            scroll = 0;
            more = true;
            err.clear();
            task.cancel();
        }
        if (cats[typeIdx].empty()) return;
        const Cat& c = cats[typeIdx][catIdx];
        std::string extra;
        std::string genre = genreLabel();
        if (!genreOptions().empty() && genre != "All") extra = "genre=" + urlEncode(genre);
        if (append) {
            if (!c.info.skip) {
                more = false;
                return;
            }
            extra += (extra.empty() ? "" : "&") + std::string("skip=") + std::to_string(items.size());
        }
        Addon a = c.addon;
        CatalogInfo info = c.info;
        task.start([a, info, extra]() {
            CatalogResult r;
            r.items = fetchCatalog(a, info, extra, r.err);
            return r;
        });
    }
};

// ---------------------------------------------------------------- HomeScreen

class HomeScreen : public Screen {
    enum { TAB_HOME, TAB_DISCOVER, TAB_MOVIES, TAB_SERIES, TAB_SEARCH, TAB_DOWNLOADS, TAB_SETTINGS, TAB_COUNT };

    BrowsePage* pageFor(int t) {
        return t == TAB_HOME ? &pages[0] : t == TAB_MOVIES ? &pages[1] : t == TAB_SERIES ? &pages[2] : nullptr;
    }
    DiscoverPage discover;

    BrowsePage pages[3];  // home, movies, series
    BrowsePage search;
    std::string query;
    int tab = TAB_HOME;
    int seenAddons = -1, seenHistory = -1, seenLibrary = -1, seenHome = 0;
    std::string removeId, removeName;  // title waiting for "Remove from your history?"
    int removeBtn = 1;                 // 0 remove, 1 cancel
    float tabX = 0, tabW = 0;  // animated underline

    int accSel = 0;
    Task<std::pair<std::string, std::string>> loginTask;  // authKey, error
    std::string pendingEmail;

public:
    HomeScreen() {
        search.hasHero = false;
        search.rowsTop = 170;
        search.emptyText = "Press Y to search movies and series";
    }

    // Continue Watching, My List or Recently Watched: built from local data, nothing to fetch.
    Row fixedRow(App& app, const std::string& key) {
        Row r;
        r.key = key;
        r.loaded = true;
        if (key == "#continue") {
            r.title = "Continue Watching";
            r.landscape = true;
            for (auto& e : app.history.continueWatching(15)) {
                Card c;
                c.item = itemFromEntry(e);
                c.isContinue = true;
                c.fromHistory = true;
                c.entry = e;
                r.cards.push_back(c);
            }
        } else if (key == "#mylist") {
            r.title = "My List";
            for (auto& m : app.library.items()) r.cards.push_back(Card{m});
        } else {
            r.title = "Recently Watched";
            for (auto& e : app.history.recentTitles(20)) {
                Card c;
                c.item = itemFromEntry(e);
                c.fromHistory = true;
                c.entry = e;
                r.cards.push_back(c);
            }
        }
        return r;
    }

    // The home rows in the Settings > Home sections order. Catalog rows found in `keep` are reused as
    // they are (cards, loading state), so nothing is fetched again.
    void composeHome(App& app, std::vector<Row> keep) {
        std::vector<Row> rows;
        bool heroMovie = false, heroSeries = false;  // the hero comes from the first movie and series rows
        for (auto& s : homeSections(app)) {
            if (!s.on) continue;
            if (s.fixed) {
                Row r = fixedRow(app, s.key);
                if (!r.cards.empty()) rows.push_back(std::move(r));
                continue;
            }
            auto it = std::find_if(keep.begin(), keep.end(), [&](const Row& k) { return k.key == s.key; });
            Row r;
            if (it != keep.end()) {
                r = std::move(*it);
            } else {
                r.key = s.key;
                r.title = s.title;
                r.loader = catalogLoader(*s.pick.addon, s.pick.info, "");
            }
            r.feedsHero = (s.type == "movie" && !heroMovie) || (s.type == "series" && !heroSeries);
            if (r.feedsHero) (s.type == "movie" ? heroMovie : heroSeries) = true;
            rows.push_back(std::move(r));
        }
        pages[0].rows = std::move(rows);
    }

    // Progress or My List changed (back from the player): only the rows built from them change.
    // Catalog rows and the hero keep what they loaded; fetching it all again right after playback,
    // while a torrent may still be using the network, left rows and artwork missing.
    void refreshFixedRows(App& app) {
        BrowsePage& p = pages[0];
        composeHome(app, std::move(p.rows));
        p.toTop();
    }

    void rebuildHome(App& app) {
        BrowsePage& p = pages[0];
        p.reset();
        p.emptyText = "Log in on the Account tab or add an addon to start browsing.";
        composeHome(app, {});
    }

    void rebuildType(App& app, BrowsePage& p, const std::string& type) {
        p.reset();
        p.emptyText = "No " + typeLabel(type) + " catalogs available.";
        auto cats = browsable(app.addons, type);
        for (size_t i = 0; i < cats.size(); i++) {
            Row r;
            r.title = cats[i].info.name;
            if (cats[i].addon->id != "com.linvo.cinemeta") r.title += "  ·  " + cats[i].addon->name;
            r.feedsHero = i == 0;
            r.loader = catalogLoader(*cats[i].addon, cats[i].info, "");
            p.rows.push_back(std::move(r));
        }
        int genres = 0;
        for (const char* gname : kPreferredGenres) {
            if (genres >= 9) break;
            CatalogPick pick = genreCatalog(app.addons, type, gname);
            if (!pick) continue;
            Row r;
            r.title = std::string(gname) + " " + typeLabel(type);
            r.loader = catalogLoader(*pick.addon, pick.info, "genre=" + urlEncode(gname));
            p.rows.push_back(std::move(r));
            genres++;
        }
    }

    void startSearch(App& app, const std::string& q) {
        query = q;
        search.reset();
        search.emptyText = "Nothing found for \"" + q + "\"";
        std::string extra = "search=" + urlEncode(q);
        for (const char* type : {"movie", "series"}) {
            std::vector<std::pair<Addon, CatalogInfo>> sources;
            std::vector<std::string> extras;
            for (auto& c : listCatalogs(app.addons))
                if (c.search && c.type == type) {
                    sources.emplace_back(app.addons[c.addon], c);
                    extras.push_back(extra);
                }
            if (sources.empty()) continue;
            Row r;
            r.title = typeLabel(type);
            r.loader = mixedLoader(sources, extras, {});
            search.rows.push_back(std::move(r));
        }
        if (search.rows.empty()) search.emptyText = "No addon with search installed";
    }

    void handle(App& app, const PageAction& a) {
        switch (a.kind) {
            case PageAction::Details: app.push(makeDetailScreen(a.item)); break;
            case PageAction::Play: app.push(makeStreamsScreen(contextForMovie(a.item))); break;
            case PageAction::Resume: {
                PlayContext ctx = a.entry;
                if (app.cfg.resumeSavedStream && ctx.stream.valid()) {  // straight back to the same stream
                    ctx.fromSaved = true;
                    playStream(app, streamFromSaved(ctx.stream), ctx, true, false);
                } else {
                    app.push(makeStreamsScreen(ctx, true));
                }
                break;
            }
            case PageAction::OtherStreams: app.push(makeStreamsScreen(a.entry, true)); break;
            case PageAction::Remove:
                removeId = a.entry.metaId;
                removeName = a.entry.name.empty() ? a.item.name : a.entry.name;
                removeBtn = 1;
                break;
            case PageAction::None: break;
        }
    }

    void update(App& app) override {
        const Input& in = app.in;
        if (app.addonsVersion != seenAddons) {
            seenAddons = app.addonsVersion;
            seenHistory = app.historyVersion;
            rebuildHome(app);
            rebuildType(app, pages[1], "movie");
            rebuildType(app, pages[2], "series");
            discover.reset(app);
            if (!query.empty()) startSearch(app, query);
        } else if (app.homeVersion != seenHome) {
            seenHome = app.homeVersion;
            rebuildHome(app);  // sections edited in Settings
        } else if (app.historyVersion != seenHistory || app.libraryVersion != seenLibrary) {
            seenLibrary = app.libraryVersion;
            seenHistory = app.historyVersion;
            refreshFixedRows(app);
        }
        if (loginTask.ready()) {
            auto r = loginTask.take();
            if (!r.first.empty()) {
                app.cfg.authKey = r.first;
                app.cfg.email = pendingEmail;
                app.cfg.save();
                app.reloadAddons();
                app.toast("Logged in! Loading your addons and library...");
            } else {
                app.toast("Login failed: " + r.second);
            }
        }

        if (!removeId.empty()) {  // "Remove from your history?"
            int tid = 0, ta = 0, tb = 0;
            bool tap = app.tapped(tid, ta, tb) && tid == 13;
            if (in.pressed(K_LEFT)) removeBtn = 0;
            if (in.pressed(K_RIGHT)) removeBtn = 1;
            if (tap) removeBtn = ta;
            if (in.pressed(K_A) || tap) {
                if (removeBtn == 0) {
                    app.history.removeTitle(removeId);
                    app.historyVersion++;
                    app.toast("Removed " + removeName);
                }
                removeId.clear();
            }
            if (in.pressed(K_B)) removeId.clear();
            return;
        }
        if (in.pressed(K_PLUS)) {
            app.quit();
            return;
        }
        bool modal = tab == TAB_DOWNLOADS && confirmDelete;
        if (in.pressed(K_L) && !modal) tab = (tab + TAB_COUNT - 1) % TAB_COUNT;
        if (in.pressed(K_R) && !modal) tab = (tab + 1) % TAB_COUNT;
        int tid = 0, ta = 0, tb = 0;
        if (app.tapped(tid, ta, tb) && tid == 1 && !modal && ta < TAB_COUNT) tab = ta;  // touch: nav tabs
        if (tab == TAB_DOWNLOADS && (in.pressed(K_L) || in.pressed(K_R) || tid == 1)) refreshDownloads(true);

        if (BrowsePage* p = pageFor(tab)) {
            handle(app, p->update(app));
        } else if (tab == TAB_DISCOVER) {
            handle(app, discover.update(app));
        } else if (tab == TAB_SEARCH) {
            if (in.pressed(K_Y) || (in.pressed(K_A) && search.rows.empty()) || tid == 30) {
                std::string q;
                if (showKeyboard(app, q, "Search movies and series", query) && !q.empty()) startSearch(app, q);
                return;
            }
            handle(app, search.update(app));
        } else if (tab == TAB_DOWNLOADS) {
            updateDownloads(app);
        } else {
            updateSettings(app);
        }
    }

    void drawSearch(App& app) {
        Gfx& g = app.gfx;
        int bw = 760, bx = (W - bw) / 2;
        g.roundRect(bx, 96, bw, 56, 28, col::card.alpha(230));
        app.hit(bx, 96, bw, 56, 30);
        g.roundOutline(bx, 96, bw, 56, 28, 2, col::white.alpha(40));
        g.roundOutline(bx + 24, 112, 20, 20, 10, 3, col::dim);  // magnifier
        g.rect(bx + 41, 130, 8, 3, col::dim);
        g.text(query.empty() ? "Search movies and series" : query, bx + 62, 110, 22, query.empty() ? col::faint : col::text,
               bw - 140);
        g.roundRect(bx + bw - 54, 110, 30, 28, 14, col::white.alpha(36));
        g.text("Y", bx + bw - 39, 113, 18, col::text, 0, AlignCenter);
        search.draw(app);
        if (search.loading() && search.rows.size() && std::all_of(search.rows.begin(), search.rows.end(), [](const Row& r) { return r.cards.empty(); }))
            app.drawLoading(W / 2, 330, "Searching");
    }

    // ------------------------------------------------ settings

    enum SettingId {
        S_ACCOUNT, S_AUTO_DOWNLOAD, S_UPLOAD, S_SERVER,
        S_PREF_QUALITY, S_PREF_SOURCE, S_AUTOPLAY, S_RESUME_STREAM,
        S_SUB_LANG, S_SUB_ADDONS, S_SUB_SIZE, S_SUB_POS, S_SUB_STYLE, S_SUB_COLOR, S_SUB_DELAY, S_SUB_STEP,
        S_ADD_ADDON, S_REMOVE_ADDONS, S_HOME_SECTIONS, S_RELOAD, S_CLEAR_HISTORY,
    };
    enum SettingKind { Action, Toggle, Choice };
    struct SettingRow {
        SettingId id;
        SettingKind kind;
        std::string label, value, section;
        bool on = false;
    };

    static std::string signedSeconds(int ms) {
        char b[16];
        snprintf(b, sizeof(b), "%+.2f s", ms / 1000.0);
        return b;
    }

    std::vector<SettingRow> settingsRows(App& app) {
        const Config& c = app.cfg;
        static const char* positions[] = {"Bottom", "Slightly higher", "Higher", "Much higher"};
        int posIdx = std::max(0, std::min(3, (100 - c.subPos) / 5));
        return {
            {S_ACCOUNT, Action, c.authKey.empty() ? "Log in with a Stremio account" : "Log out", c.email, "Account"},
            {S_AUTO_DOWNLOAD, Toggle, "Download what I watch", "", "Torrents", c.autoDownload},
            {S_UPLOAD, Choice, "Torrent uploading",
             c.uploadMode == 0 ? "Off" : c.uploadMode == 1 ? "Limited" : "Normal", ""},
            {S_SERVER, Action, "Streaming server", c.server.empty() ? "Built-in engine" : c.server, ""},
            {S_PREF_QUALITY, Choice, "Preferred quality",
             c.prefQuality == "auto" ? "Auto" + (c.lastQuality.empty() ? std::string() : " (" + c.lastQuality + ")") : c.prefQuality,
             "Playback"},
            {S_PREF_SOURCE, Choice, "Prefer streams from",
             c.preferSource == 1 ? "Debrid & links" : c.preferSource == 2 ? "Torrents" : "Any source", ""},
            {S_AUTOPLAY, Toggle, "Autoplay next episode", "", "", c.autoplayNext},
            {S_RESUME_STREAM, Toggle, "Continue with the last stream", "", "", c.resumeSavedStream},
            {S_SUB_LANG, Choice, "Subtitle language", subLanguage(c.subLang).label, "Subtitles"},
            {S_SUB_ADDONS, Toggle, "Subtitles from addons", "", "", c.subAddons},
            {S_SUB_SIZE, Choice, "Size", std::to_string(c.subSize) + "%", ""},
            {S_SUB_POS, Choice, "Position", positions[posIdx], ""},
            {S_SUB_STYLE, Choice, "Style", c.subStyle ? "Dark box" : "Outline", ""},
            {S_SUB_COLOR, Choice, "Color", c.subColor ? "Yellow" : "White", ""},
            {S_SUB_DELAY, Choice, "Default delay", signedSeconds(c.subDelayMs), ""},
            {S_SUB_STEP, Choice, "Timing step in the player (L / R)", signedSeconds(c.subStepMs).substr(1), ""},
            {S_ADD_ADDON, Action, "Add addon by URL", "", "Addons & data"},
            {S_REMOVE_ADDONS, Action, "Manage addons", std::to_string(app.addons.size()) + " active", ""},
            {S_HOME_SECTIONS, Action, "Home sections", "Show, hide and reorder", ""},
            {S_RELOAD, Action, "Reload addons and library", "", ""},
            {S_CLEAR_HISTORY, Action, "Clear watch history", "", ""},
        };
    }

    // Left/right (dir = -1/+1) or A (dir = +1, wrapping) on a choice row.
    void adjust(App& app, SettingId id, int dir, bool wrap) {
        Config& c = app.cfg;
        auto step = [&](int v, int lo, int hi, int by) {
            int n = v + dir * by;
            if (n > hi) n = wrap ? lo : hi;
            if (n < lo) n = wrap ? hi : lo;
            return n;
        };
        switch (id) {
            case S_SUB_LANG: {
                auto& langs = subLanguages();
                int i = 0;
                for (int k = 0; k < (int)langs.size(); k++)
                    if (c.subLang == langs[k].code) i = k;
                i = (i + dir + (int)langs.size()) % (int)langs.size();
                c.subLang = langs[i].code;
                break;
            }
            case S_PREF_QUALITY: {
                static const char* q[] = {"auto", "4K", "1080p", "720p", "SD"};
                int i = 0;
                for (int k = 0; k < 5; k++)
                    if (c.prefQuality == q[k]) i = k;
                i = wrap ? (i + 1) % 5 : std::max(0, std::min(4, i + dir));
                c.prefQuality = q[i];
                break;
            }
            case S_PREF_SOURCE: c.preferSource = step(c.preferSource, 0, 2, 1); break;
            case S_UPLOAD:
                c.uploadMode = step(c.uploadMode, 0, 2, 1);
                if (app.activeTorrent) app.activeTorrent->setUploadMode(c.uploadMode);
                for (auto& t : app.downloads) t->setUploadMode(c.uploadMode);
                break;
            case S_SUB_SIZE: c.subSize = step(c.subSize, 60, 200, 10); break;
            case S_SUB_POS: c.subPos = step(c.subPos, 85, 100, -5); break;
            case S_SUB_STYLE: c.subStyle = 1 - c.subStyle; break;
            case S_SUB_COLOR: c.subColor = 1 - c.subColor; break;
            case S_SUB_DELAY: c.subDelayMs = step(c.subDelayMs, -10000, 10000, 100); break;
            case S_SUB_STEP: {
                static const int steps[] = {50, 100, 250, 500};
                int i = 0;
                for (int k = 0; k < 4; k++)
                    if (c.subStepMs == steps[k]) i = k;
                i = wrap ? (i + 1) % 4 : std::max(0, std::min(3, i + dir));
                c.subStepMs = steps[i];
                break;
            }
            default: return;
        }
        c.save();
    }

    void updateSettings(App& app) {
        const Input& in = app.in;
        auto rows = settingsRows(app);
        int n = (int)rows.size();
        if (in.pressed(K_DOWN) && accSel + 1 < n) accSel++;
        if (in.pressed(K_UP) && accSel > 0) accSel--;
        int tid = 0, ta = 0, tb = 0;
        bool tapPress = false;
        if (app.tapped(tid, ta, tb) && tid == 50 && ta < n) {
            tapPress = accSel == ta || rows[ta].kind != Choice;
            accSel = ta;
        }
        const SettingRow& row = rows[accSel];
        if (row.kind == Choice && (in.pressed(K_LEFT) || in.pressed(K_RIGHT))) adjust(app, row.id, in.pressed(K_RIGHT) ? 1 : -1, false);
        if (row.kind == Toggle && (in.pressed(K_LEFT) || in.pressed(K_RIGHT))) {
            bool on = in.pressed(K_RIGHT);
            if (row.id == S_SUB_ADDONS) app.cfg.subAddons = on;
            if (row.id == S_AUTOPLAY) app.cfg.autoplayNext = on;
            if (row.id == S_RESUME_STREAM) app.cfg.resumeSavedStream = on;
            if (row.id == S_AUTO_DOWNLOAD) app.cfg.autoDownload = on;
            app.cfg.save();
        }
        if (!in.pressed(K_A) && !tapPress) return;

        Config& c = app.cfg;
        switch (row.id) {
            case S_ACCOUNT:
                if (!c.authKey.empty()) {
                    c.authKey.clear();
                    c.email.clear();
                    c.save();
                    app.reloadAddons();
                    app.toast("Logged out");
                } else if (!loginTask.active()) {
                    std::string email, pass;
                    if (!showKeyboard(app, email, "Stremio account e-mail", c.email) || email.empty()) break;
                    if (!showKeyboard(app, pass, "Password", "", true) || pass.empty()) break;
                    pendingEmail = email;
                    loginTask.start([email, pass]() {
                        std::pair<std::string, std::string> r;
                        if (!apiLogin(email, pass, r.first, r.second)) r.first.clear();
                        return r;
                    });
                    app.toast("Logging in...");
                }
                break;
            case S_AUTOPLAY:
                c.autoplayNext = !c.autoplayNext;
                c.save();
                break;
            case S_RESUME_STREAM:
                c.resumeSavedStream = !c.resumeSavedStream;
                c.save();
                break;
            case S_AUTO_DOWNLOAD:
                c.autoDownload = !c.autoDownload;
                c.save();
                app.toast(c.autoDownload ? "Torrents you watch will also be saved to the SD card"
                                         : "Torrents will only be streamed (- in the stream list downloads one)");
                break;
            case S_SUB_ADDONS:
                c.subAddons = !c.subAddons;
                c.save();
                break;
            case S_SERVER: {
                std::string s;
                if (showKeyboard(app, s, "stremio-server URL (empty = built-in engine)", c.server.empty() ? "http://" : c.server)) {
                    while (!s.empty() && (s.back() == ' ' || s.back() == '/')) s.pop_back();
                    c.server = (s == "http:" || s == "http://") ? "" : s;
                    c.save();
                }
                break;
            }
            case S_ADD_ADDON: {
                std::string u;
                if (showKeyboard(app, u, "Addon manifest URL", "https://") && u.size() > 8) {
                    c.addons.push_back(normalizeAddonUrl(u));
                    c.save();
                    app.reloadAddons();
                    app.toast("Addon added, loading...");
                }
                break;
            }
            case S_REMOVE_ADDONS: app.push(makeAddonsScreen()); break;
            case S_HOME_SECTIONS: app.push(makeHomeSectionsScreen()); break;
            case S_RELOAD: app.reloadAddons(); break;
            case S_CLEAR_HISTORY:
                app.history = History();
                app.history.save();
                app.historyVersion++;
                app.toast("Watch history cleared");
                break;
            default: adjust(app, row.id, 1, true); break;  // choices cycle with A too
        }
    }

    float settingsScroll = 0;

    void drawSettings(App& app) {
        Gfx& g = app.gfx;
        auto rows = settingsRows(app);
        const int top = 96, bottom = H - 50, rowH = 50, gap = 8, headH = 40, x = 60, w = 660;

        // layout (content coordinates)
        std::vector<int> ys;
        int y = 0;
        for (auto& r : rows) {
            if (!r.section.empty()) y += headH;
            ys.push_back(y);
            y += rowH + gap;
        }
        float target = std::max(0.0f, (float)ys[accSel] - (bottom - top) * 0.5f + rowH);
        target = std::min(target, (float)std::max(0, y - (bottom - top)));
        settingsScroll = approach(settingsScroll, target, 0.25f);

        g.clip(0, top - 4, x + w + 10, bottom - top + 4);
        for (int i = 0; i < (int)rows.size(); i++) {
            const SettingRow& r = rows[i];
            int ry = top + ys[i] - (int)settingsScroll;
            if (!r.section.empty()) g.text(r.section, x, ry - 34, 21, col::accentLight);
            if (ry + rowH < top || ry > bottom) continue;
            bool s = i == accSel;
            g.roundRect(x, ry, w, rowH, 12, s ? col::cardSel : col::card.alpha(200));
            if (s) g.roundOutline(x, ry, w, rowH, 12, 2, col::accentLight);
            g.text(r.label, x + 24, ry + 12, 21, s ? col::text : col::dim, 400);
            app.hit(x, std::max(ry, top), w, rowH, 50, i);
            int vx = x + w - 24;
            if (r.kind == Toggle) {
                g.roundRect(vx - 52, ry + 12, 52, 26, 13, r.on ? col::accent : col::white.alpha(50));
                g.circle(r.on ? vx - 13 : vx - 39, ry + 25, 10, col::white);
            } else if (r.kind == Choice) {
                int tw = g.textWidth(r.value, 20);
                Color arrow = s ? col::accentLight : col::faint;
                g.triangle(vx - 4, ry + 17, vx - 4, ry + 33, vx + 4, ry + 25, arrow);
                g.text(r.value, vx - 16, ry + 13, 20, s ? col::text : col::dim, 0, AlignRight);
                g.triangle(vx - 28 - tw, ry + 17, vx - 28 - tw, ry + 33, vx - 36 - tw, ry + 25, arrow);
            } else if (!r.value.empty()) {
                g.text(r.value, vx, ry + 13, 19, col::faint, 220, AlignRight);
            }
        }
        g.unclip();

        // Right column: who is logged in, addons, and a live subtitle preview
        int px = 760, pw = 460;
        g.roundRect(px, 96, pw, 250, 16, col::card.alpha(160));
        if (SDL_Texture* logo = g.image("romfs:/logo.png")) g.drawTex(logo, px + 20, 116, 64, 64);
        g.text("StremNX", px + 100, 120, 30, col::text);
        g.text(app.cfg.authKey.empty() ? "Not logged in" : app.cfg.email, px + 100, 158, 18, col::dim, pw - 120);
        g.text("Installed addons  ·  " + std::to_string(app.addons.size()), px + 20, 200, 19, col::text);
        int ay = 230;
        if (app.addonsLoading()) app.drawLoading(px + 20, ay, "Loading", AlignLeft);
        else
            for (size_t i = 0; i < app.addons.size() && ay < 330; i++, ay += 26)
                g.text(app.addons[i].name, px + 20, ay, 17, col::faint, pw - 40);

        bool subsRow = rows[accSel].id >= S_SUB_LANG && rows[accSel].id <= S_SUB_STEP;
        if (subsRow) {
            const Config& c = app.cfg;
            int vy = 366, vh = 240;
            g.rect(px, vy, pw, vh, col::black);
            if (SDL_Texture* bg = g.image(pages[0].hero.empty() ? "" : pages[0].hero[0].background)) g.drawCover(bg, px, vy, pw, vh, 150);
            g.text("Preview", px + 14, vy + 10, 16, col::white.alpha(160));
            int size = 18 * c.subSize / 100;
            std::string sample = c.subLang.empty() ? "(subtitles off)" : "Isso é uma legenda de exemplo.";
            int sy = vy + vh - 26 - size - (100 - c.subPos) * vh / 60;
            int sw = g.textWidth(sample, size);
            Color fg = c.subColor ? Color{255, 230, 0, 255} : col::white;
            if (c.subStyle == 1) g.rect(px + pw / 2 - sw / 2 - 8, sy - 2, sw + 16, size + 10, col::black.alpha(200));
            else
                for (int d = 1; d <= 2; d++) g.text(sample, px + pw / 2 + d, sy + d, size, col::black, 0, AlignCenter);
            g.text(sample, px + pw / 2, sy, size, fg, 0, AlignCenter);
        } else if (rows[accSel].id == S_UPLOAD) {
            int vy = 366;
            g.roundRect(px, vy, pw, 250, 16, col::gold.alpha(28));
            g.roundOutline(px, vy, pw, 250, 16, 1, col::gold.alpha(120));
            g.text("About uploading", px + 20, vy + 16, 20, col::gold);
            g.textWrapped("While you watch a torrent, other people download pieces of it from you. That is how "
                          "BitTorrent works and it keeps downloads fast. In some countries, sharing copyrighted "
                          "files can bring notices or fines from your internet provider. You are responsible for "
                          "what you watch.",
                          px + 20, vy + 46, 16, col::text, pw - 40, 110);
            g.textWrapped("Off: never upload (torrents may slow down or stall).\nLimited: about 100 KB/s to 2 people.\n"
                          "Normal: up to 6 people, no limit (fastest).",
                          px + 20, vy + 162, 16, col::dim, pw - 40, 80);
        } else {
            g.textWrapped(app.cfg.autoDownload
                              ? "Download what I watch is on: torrents you play are also saved completely to the SD "
                                "card and keep downloading after you stop watching. Manage them in the Downloads tab."
                              : "Torrents only stream, nothing is saved. To keep one on the SD card (for a trip), "
                                "press - on it in the stream list. Set a streaming server only to send torrents "
                                "through a PC running Stremio.",
                          px, 370, 17, col::faint, pw, 90);
        }
        g.text("Unofficial client - not affiliated with or endorsed by Stremio.", px + pw, H - 72, 15, col::faint, 0,
               AlignRight);
    }

    // ------------------------------------------------ downloads

    std::vector<DownloadInfo> dls;
    uint32_t dlsAt = 0;
    int dlSel = 0;
    float dlScroll = 0;
    bool confirmDelete = false;
    int confirmBtn = 1;  // 0 delete, 1 cancel
    int64_t freeSpace = 0;

    void refreshDownloads(bool force) {
        if (!force && dlsAt != 0 && SDL_GetTicks() - dlsAt < 2000) return;
        dlsAt = SDL_GetTicks();
        dls = listDownloads();
        freeSpace = sdFreeSpace();
        dlSel = std::max(0, std::min(dlSel, (int)dls.size() - 1));
    }

    void updateDownloads(App& app) {
        const Input& in = app.in;
        refreshDownloads(false);
        if (dls.empty()) return;
        int tid = 0, ta = 0, tb = 0;
        bool tap = app.tapped(tid, ta, tb);
        DownloadInfo d = dls[dlSel];

        if (confirmDelete) {
            if (in.pressed(K_LEFT)) confirmBtn = 0;
            if (in.pressed(K_RIGHT)) confirmBtn = 1;
            if (in.pressed(K_B)) confirmDelete = false;
            if (tap && tid == 41) confirmBtn = ta;
            if (in.pressed(K_A) || (tap && tid == 41)) {
                confirmDelete = false;
                if (confirmBtn == 0) {
                    std::shared_ptr<Torrent> t = app.findDownload(d.infoHash, d.fileIdx);
                    app.stopDownload(d.infoHash, d.fileIdx);
                    if (t) {  // the engine closes the file in its own time: delete after that, off the UI thread
                        DownloadInfo dd = d;
                        startThread([t, dd]() {
                            t->waitStopped(20000);
                            deleteDownload(dd);
                        });
                        dls.erase(dls.begin() + dlSel);
                        dlSel = std::max(0, std::min(dlSel, (int)dls.size() - 1));
                        app.toast("Deleted from the SD card");
                    } else {
                        app.toast(deleteDownload(d) ? "Deleted from the SD card" : "Could not delete the file");
                        refreshDownloads(true);
                    }
                }
            }
            return;
        }

        if (in.pressed(K_DOWN) && dlSel + 1 < (int)dls.size()) dlSel++;
        if (in.pressed(K_UP) && dlSel > 0) dlSel--;
        if (in.pressed(K_Y)) {
            confirmDelete = true;
            confirmBtn = 1;
        }
        bool tapOpen = false;
        if (tap && tid == 40 && ta < (int)dls.size()) {
            tapOpen = dlSel == ta;
            dlSel = ta;
            d = dls[dlSel];
        }
        if (in.pressed(K_A) || tapOpen) {
            if (d.complete && !fileExists(d.file)) {
                app.toast("The file is missing from the SD card");
            } else {  // finished: the file; still downloading: what is saved plays now, peers fill the rest
                Stream s;
                s.addonName = "Downloads";
                s.infoHash = d.infoHash;
                s.fileIdx = d.fileIdx;
                s.trackers = d.trackers;
                playStream(app, s, d.ctx, true, false);
            }
        } else if (in.pressed(K_X) && !d.complete) {
            if (app.findDownload(d.infoHash, d.fileIdx)) {
                app.stopDownload(d.infoHash, d.fileIdx);
                app.toast("Download paused");
            } else {
                auto t = std::make_shared<Torrent>(d.infoHash, d.trackers, d.fileIdx);
                t->setUploadMode(app.cfg.uploadMode);
                t->saveTo(DOWNLOADS_DIR, downloadInfoJson(d.ctx, d.trackers));
                t->start();
                app.keepDownloading(t);
                app.toast("Resuming download");
            }
            refreshDownloads(true);
        }
    }

    void drawDownloads(App& app) {
        Gfx& g = app.gfx;
        g.text("Downloads", 60, 88, 34, col::text);
        if (freeSpace > 0)
            g.text("SD card  ·  " + formatBytes((double)freeSpace) + " free", W - 60, 98, 19, col::dim, 0, AlignRight);

        if (dls.empty()) {
            g.text("No downloads yet", W / 2, 300, 26, col::text, 0, AlignCenter);
            g.text("Press - on a torrent in the stream list (pick an episode or movie first) to download it.",
                   W / 2, 344, 20, col::dim, W - 120, AlignCenter);
            return;
        }

        const int top = 150, rowH = 104, gap = 12, visible = 4;
        float target = (float)std::max(0, dlSel - (visible - 2)) * (rowH + gap);
        dlScroll = approach(dlScroll, target, 0.25f);
        g.clip(0, top - 6, W, H - top - 40);
        for (int i = 0; i < (int)dls.size(); i++) {
            int y = top + i * (rowH + gap) - (int)dlScroll;
            if (y + rowH < top - 6 || y > H) continue;
            const DownloadInfo& d = dls[i];
            bool s = i == dlSel;
            g.roundRect(60, y, W - 120, rowH, 14, s ? col::cardSel.alpha(240) : col::card.alpha(200));
            if (s) g.roundOutline(60, y, W - 120, rowH, 14, 2, col::accentLight);
            app.hit(60, std::max(y, top), W - 120, rowH, 40, i);

            Uint8 fade = 0;
            g.rect(78, y + 10, 56, 84, col::bar);
            if (SDL_Texture* t = g.image(d.ctx.poster, &fade)) g.drawTex(t, 78, y + 10, 56, 84, fade);

            std::string title = d.ctx.name + (d.ctx.episodeLabel.empty() ? "" : "  ·  " + d.ctx.episodeLabel);
            g.text(title, 154, y + 12, 22, col::text, 720);
            g.text(d.fileName.substr(d.fileName.rfind('/') + 1), 154, y + 42, 16, col::faint, 720);

            // Live numbers while it is downloading right now
            std::shared_ptr<Torrent> t = app.findDownload(d.infoHash, d.fileIdx);
            if (!t && app.activeTorrent && app.activeTorrent->infoHash() == d.infoHash) t = app.activeTorrent;
            int64_t savedBytes = d.savedBytes;
            std::string status;
            Color sc = col::dim;
            if (d.complete) {
                status = "Downloaded";
                sc = col::success;
            } else if (t) {
                TorrentStatus st = t->status();
                savedBytes = std::max(savedBytes, st.savedBytes);
                char buf[64];
                snprintf(buf, sizeof(buf), "Downloading  ·  %.1f MB/s", st.speed / (1024 * 1024));
                status = !st.saveError.empty() ? st.saveError : st.hasMetadata ? buf : "Connecting";
                sc = !st.saveError.empty() ? col::error : col::accentLight;
            } else {
                status = "Paused";
            }
            double p = d.size > 0 ? (double)savedBytes / d.size : 0;
            drawProgress(g, 154, y + 76, 760, 6, d.complete ? 1.0 : p);
            g.text(status, W - 84, y + 20, 19, sc, 260, AlignRight);
            std::string sizes = d.complete ? formatBytes((double)d.size)
                                           : formatBytes((double)savedBytes) + " of " + formatBytes((double)d.size) +
                                                 "  ·  " + std::to_string((int)(p * 100)) + "%";
            g.text(sizes, W - 84, y + 66, 17, col::dim, 260, AlignRight);
        }
        g.unclip();

        if (confirmDelete && dlSel < (int)dls.size()) {
            const DownloadInfo& d = dls[dlSel];
            g.rect(0, 0, W, H, col::black.alpha(170));
            int pw = 600, ph = 250, px = (W - pw) / 2, py = (H - ph) / 2;
            g.roundRect(px, py, pw, ph, 20, col::bar);
            g.roundOutline(px, py, pw, ph, 20, 1, col::white.alpha(40));
            g.text("Delete this download?", px + 36, py + 30, 28, col::text);
            g.text(d.ctx.name + (d.ctx.episodeLabel.empty() ? "" : "  ·  " + d.ctx.episodeLabel), px + 36, py + 78,
                   20, col::dim, pw - 72);
            g.text("The file (" + formatBytes((double)(d.complete ? d.size : d.savedBytes)) +
                       ") will be removed from the SD card.",
                   px + 36, py + 108, 18, col::faint, pw - 72);
            int bw0 = buttonWidth(g, "Delete", IconNone), bw1 = buttonWidth(g, "Cancel", IconNone);
            drawButton(g, px + 36, py + 168, bw0, 50, "Delete", confirmBtn == 0, confirmBtn == 0, IconNone);
            drawButton(g, px + 54 + bw0, py + 168, bw1, 50, "Cancel", confirmBtn == 1, false, IconNone);
            app.hit(px + 36, py + 168, bw0, 50, 41, 0);
            app.hit(px + 54 + bw0, py + 168, bw1, 50, 41, 1);
        }
    }

    void drawNav(App& app) {
        Gfx& g = app.gfx;
        static const char* names[TAB_COUNT] = {"Home", "Discover", "Movies", "Series", "Search", "Downloads", "Settings"};
        g.gradientV(0, 0, W, 130, col::bg, 235, 0);
        if (SDL_Texture* logo = g.image("romfs:/logo.png")) g.drawTex(logo, 40, 14, 46, 46);
        int x = 112;
        float selX = 0, selW = 0;
        for (int i = 0; i < TAB_COUNT; i++) {
            bool s = i == tab;
            int w = g.text(names[i], x, 24, 21, s ? col::text : col::dim);
            if (s) selX = (float)x, selW = (float)w;
            if (i == TAB_DOWNLOADS && !app.downloads.empty()) g.circle(x + w + 7, 28, 4, col::success);  // active
            app.hit(x - 12, 12, w + 24, 48, 1, i);
            x += w + 28;
        }
        tabX = tabW == 0 ? selX : approach(tabX, selX, 0.3f);
        tabW = tabW == 0 ? selW : approach(tabW, selW, 0.3f);
        g.roundRect((int)tabX, 56, (int)tabW, 4, 2, col::accent);
    }

    void drawRemoveConfirm(App& app) {
        Gfx& g = app.gfx;
        g.rect(0, 0, W, H, col::black.alpha(170));
        int pw = 640, ph = 240, px = (W - pw) / 2, py = (H - ph) / 2;
        g.roundRect(px, py, pw, ph, 20, col::bar);
        g.roundOutline(px, py, pw, ph, 20, 1, col::white.alpha(40));
        g.text("Remove " + removeName + "?", px + 36, py + 30, 28, col::text, pw - 72);
        g.textWrapped("It leaves Continue Watching and Recently Watched on this Switch, with its progress. "
                      "Watching it again brings it back.",
                      px + 36, py + 80, 18, col::dim, pw - 72, 70);
        int bw0 = buttonWidth(g, "Remove", IconNone), bw1 = buttonWidth(g, "Cancel", IconNone);
        drawButton(g, px + 36, py + 160, bw0, 50, "Remove", removeBtn == 0, removeBtn == 0, IconNone);
        drawButton(g, px + 54 + bw0, py + 160, bw1, 50, "Cancel", removeBtn == 1, false, IconNone);
        app.hit(px + 36, py + 160, bw0, 50, 13, 0);
        app.hit(px + 54 + bw0, py + 160, bw1, 50, 13, 1);
        app.drawFooter("A Confirm   B Cancel");
    }

    void draw(App& app) override {
        switch (tab) {
            case TAB_HOME:
            case TAB_MOVIES:
            case TAB_SERIES:
                pageFor(tab)->draw(app);
                drawNav(app);
                if (tab == TAB_HOME && !removeId.empty())
                    drawRemoveConfirm(app);
                else if (tab == TAB_HOME)
                    app.drawFooter(pages[0].historyFocused()
                                       ? "A Select   X Other stream   Y Details   - Remove   L/R Tabs   + Exit"
                                       : "A Select   X Other stream   Y Details   L/R Tabs   + Exit");
                else
                    app.drawFooter("A Select   Y Details   L/R Tabs   + Exit");
                break;
            case TAB_DISCOVER:
                discover.draw(app);
                drawNav(app);
                app.drawFooter(discover.chipsFocused() ? "A Next option   Y Previous   L/R Tabs   + Exit"
                                                       : "A Open   L/R Tabs   + Exit");
                break;
            case TAB_SEARCH:
                drawSearch(app);
                drawNav(app);
                app.drawFooter("Y Search   A Open   L/R Tabs   + Exit");
                break;
            case TAB_DOWNLOADS:
                drawDownloads(app);
                drawNav(app);
                if (confirmDelete)
                    app.drawFooter("A Confirm   B Cancel");
                else if (dls.empty())
                    app.drawFooter("L/R Tabs   + Exit");
                else
                    app.drawFooter(std::string(dls[dlSel].complete ? "A Play" : "A Watch   X Pause/Resume") +
                                   "   Y Delete   L/R Tabs   + Exit");
                break;
            case TAB_SETTINGS:
                drawSettings(app);
                drawNav(app);
                app.drawFooter("A Select   </> Change   L/R Tabs   + Exit");
                break;
        }
    }
};

}  // namespace

std::unique_ptr<Screen> makeHomeScreen() {
    return std::make_unique<HomeScreen>();
}

// ---------------------------------------------------------------- Settings > Home sections

// Every catalog row of the home screen: switch each one on or off and move it up or down. The home
// screen is rebuilt when leaving, so browsing here doesn't refetch anything.
class HomeSectionsScreen : public Screen {
    std::vector<HomeSection> list;
    bool loaded = false, changed = false;
    int sel = 0;
    float scroll = 0;
    static const int top = 140, rowH = 72, gap = 8, visible = 7;
    static const int toggleX = W - 84 - 56, upX = W - 84 - 56 - 130, downX = W - 84 - 56 - 76;

    void save(App& app) {
        Config& c = app.cfg;
        c.homeOrder.clear();
        c.homeHidden.clear();
        for (auto& s : list) {
            c.homeOrder.push_back(s.key);
            if (!s.on) c.homeHidden.push_back(s.key);
        }
        c.save();
        changed = true;
    }

    void move(App& app, int dir) {
        int to = sel + dir;
        if (to < 0 || to >= (int)list.size()) return;
        std::swap(list[sel], list[to]);
        sel = to;
        save(app);
    }

public:
    void update(App& app) override {
        if (!loaded && !app.addonsLoading()) {
            loaded = true;
            list = homeSections(app);
        }
        const Input& in = app.in;
        int id = 0, a = 0, b = 0;
        bool tap = app.tapped(id, a, b);
        if (in.pressed(K_B) || (tap && id == kHitBack)) {
            if (changed) app.homeVersion++;
            app.pop();
            return;
        }
        int n = (int)list.size();
        if (n == 0) return;
        if (in.pressed(K_DOWN) && sel + 1 < n) sel++;
        if (in.pressed(K_UP) && sel > 0) sel--;
        bool flip = in.pressed(K_A);
        int dir = in.pressed(K_L) ? -1 : in.pressed(K_R) ? 1 : 0;
        if (tap && a < n) {
            if (id == 91) flip = true;            // the switch
            if (id == 92) dir = -1;               // up arrow
            if (id == 93) dir = 1;                // down arrow
            if (id == 90) flip = sel == a;        // a row: first tap selects, the second switches
            sel = a;
        }
        if (flip) {
            list[sel].on = !list[sel].on;
            save(app);
        }
        if (dir) move(app, dir);
    }

    void draw(App& app) override {
        Gfx& g = app.gfx;
        g.text("Home sections", 60, 40, 34, col::text);
        int on = 0;
        for (auto& s : list) on += s.on;
        g.text(!loaded ? "Loading addons..." : std::to_string(on) + " of " + std::to_string(list.size()) + " shown", 60, 90,
               19, col::dim);
        drawBackButton(app);

        float target = (float)std::max(0, sel - (visible - 2)) * (rowH + gap);
        scroll = approach(scroll, target, 0.25f);
        g.clip(0, top - 6, W, H - top - 40);
        for (int i = 0; i < (int)list.size(); i++) {
            int y = top + i * (rowH + gap) - (int)scroll;
            if (y + rowH < top - 6 || y > H) continue;
            const HomeSection& s = list[i];
            bool f = i == sel;
            g.roundRect(60, y, W - 120, rowH, 14, f ? col::cardSel.alpha(240) : col::card.alpha(200));
            if (f) g.roundOutline(60, y, W - 120, rowH, 14, 2, col::accentLight);
            app.hit(60, std::max(y, top - 6), W - 120, rowH, 90, i);
            g.text(std::to_string(i + 1), 84, y + 22, 22, col::faint);
            g.text(s.title, 130, y + 10, 22, s.on ? col::text : col::dim, 760);
            g.text(s.fixed ? s.addonName : typeLabel(s.type) + "  ·  " + s.addonName, 130, y + 40, 15, col::faint, 760);
            // Up / down arrows and the on / off switch
            g.roundRect(upX, y + 20, 44, 32, 16, col::white.alpha(30));
            g.triangle(upX + 12, y + 42, upX + 32, y + 42, upX + 22, y + 30, col::text);
            app.hit(upX, y + 16, 44, 40, 92, i);
            g.roundRect(downX, y + 20, 44, 32, 16, col::white.alpha(30));
            g.triangle(downX + 12, y + 30, downX + 32, y + 30, downX + 22, y + 42, col::text);
            app.hit(downX, y + 16, 44, 40, 93, i);
            g.roundRect(toggleX, y + 22, 56, 28, 14, s.on ? col::accent : col::white.alpha(50));
            g.circle(toggleX + (s.on ? 42.0f : 14.0f), y + 36.0f, 11, col::white);
            app.hit(toggleX - 8, y + 12, 72, 48, 91, i);
        }
        g.unclip();
        if (loaded && list.empty()) g.text("No catalogs: add an addon with catalogs", W / 2, 300, 22, col::dim, 0, AlignCenter);
        app.drawFooter("A Show / hide   L/R Move up / down   B Back");
    }
};

std::unique_ptr<Screen> makeHomeSectionsScreen() {
    return std::make_unique<HomeSectionsScreen>();
}
