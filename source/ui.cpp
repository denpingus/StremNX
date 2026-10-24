#include "ui.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <tuple>
#include <vector>

#include "http.hpp"
#include "log.hpp"
#include "thread.hpp"

// ---------------------------------------------------------------- ImageLoader

struct ImageLoader::Shared {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::deque<std::tuple<std::string, SDL_Surface*, bool>> done;
};

static const size_t kMaxQueue = 64;

void ImageLoader::start(int threads) {
    sh = std::make_shared<Shared>();
    for (int i = 0; i < threads; i++) {
        std::shared_ptr<Shared> s = sh;
        startThread([s]() {
            for (;;) {
                std::string url;
                {
                    std::unique_lock<std::mutex> lock(s->m);
                    s->cv.wait(lock, [&] { return !s->queue.empty(); });
                    url = s->queue.front();  // LIFO: newest (visible) request first
                    s->queue.pop_front();
                }
                std::string body, err;
                SDL_Surface* surf = nullptr;
                if (httpGet(url, body, err, 20)) {
                    SDL_RWops* rw = SDL_RWFromConstMem(body.data(), (int)body.size());
                    surf = IMG_Load_RW(rw, 1);
                    if (!surf) logMsg("IMG_Load failed (%s): %s", url.c_str(), IMG_GetError());
                }
                std::lock_guard<std::mutex> lock(s->m);
                s->done.emplace_back(url, surf, false);
            }
        });
    }
}

void ImageLoader::request(const std::string& url) {
    std::lock_guard<std::mutex> lock(sh->m);
    sh->queue.push_front(url);
    while (sh->queue.size() > kMaxQueue) {
        sh->done.emplace_back(sh->queue.back(), nullptr, true);
        sh->queue.pop_back();
    }
    sh->cv.notify_one();
}

bool ImageLoader::pop(std::string& url, SDL_Surface*& surf, bool& dropped) {
    std::lock_guard<std::mutex> lock(sh->m);
    if (sh->done.empty()) return false;
    std::tie(url, surf, dropped) = sh->done.front();
    sh->done.pop_front();
    return true;
}

// ---------------------------------------------------------------- Gfx

static const size_t kMaxTextCache = 800;
static const size_t kMaxImageCache = 110;
static const int kFadeFrames = 12;

bool Gfx::init(SDL_Window* w) {
    win = w;
    Result rc = plGetSharedFontByType(&fontData, PlSharedFontType_Standard);
    if (R_FAILED(rc)) {
        logMsg("plGetSharedFontByType failed: 0x%x", rc);
        return false;
    }
    logMsg("init: system font ok (%u bytes)", (unsigned)fontData.size);
    loader.start(4);
    logMsg("init: image loader threads ok");
    if (!restoreRenderer()) return false;
    logMsg("init: renderer ok");
    return true;
}

void Gfx::makeGradients() {
    // Eased ramp so fades look smooth instead of banded.
    auto ramp = [](int i) { return (Uint8)std::lround(255.0 * std::pow(i / 255.0, 1.3)); };
    SDL_Surface* h = SDL_CreateRGBSurfaceWithFormat(0, 256, 1, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Surface* v = SDL_CreateRGBSurfaceWithFormat(0, 1, 256, 32, SDL_PIXELFORMAT_RGBA32);
    if (h && v) {
        for (int i = 0; i < 256; i++) {
            Uint8* ph = (Uint8*)h->pixels + i * 4;
            Uint8* pv = (Uint8*)v->pixels + i * v->pitch;
            ph[0] = ph[1] = ph[2] = 255;
            pv[0] = pv[1] = pv[2] = 255;
            ph[3] = pv[3] = ramp(i);
        }
        gradH = SDL_CreateTextureFromSurface(r, h);
        gradV = SDL_CreateTextureFromSurface(r, v);
        SDL_SetTextureBlendMode(gradH, SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(gradV, SDL_BLENDMODE_BLEND);
    }
    if (h) SDL_FreeSurface(h);
    if (v) SDL_FreeSurface(v);
}

bool Gfx::restoreRenderer() {
    SDL_GL_ResetAttributes();
    requestColorDepth();
    r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) {
        logMsg("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    disableGlDither();  // the renderer's GL context is current now
    int rb = 0, gb = 0, bb = 0;
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &rb);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &gb);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &bb);
    logMsg("ui: framebuffer %d/%d/%d bits", rb, gb, bb);
    makeGradients();
    return true;
}

void Gfx::dropTextures() {
    for (auto& kv : textCache) SDL_DestroyTexture(kv.second.tex);
    for (auto& kv : imageCache) SDL_DestroyTexture(kv.second.tex);
    textCache.clear();
    imageCache.clear();
    if (gradH) SDL_DestroyTexture(gradH);
    if (gradV) SDL_DestroyTexture(gradV);
    gradH = gradV = nullptr;
}

void Gfx::releaseRenderer() {
    dropTextures();
    if (r) SDL_DestroyRenderer(r);
    r = nullptr;
}

void Gfx::destroy() {
    releaseRenderer();
    for (auto& kv : fonts) TTF_CloseFont(kv.second);
    fonts.clear();
}

TTF_Font* Gfx::font(int size) {
    auto it = fonts.find(size);
    if (it != fonts.end()) return it->second;
    SDL_RWops* rw = SDL_RWFromConstMem(fontData.address, (int)fontData.size);
    TTF_Font* f = TTF_OpenFontRW(rw, 1, size);
    if (!f) logMsg("TTF_OpenFontRW failed: %s", TTF_GetError());
    fonts[size] = f;
    return f;
}

std::string Gfx::fontFamily() {
    TTF_Font* f = font(20);
    const char* name = f ? TTF_FontFaceFamilyName(f) : nullptr;
    return name ? name : "";
}

void Gfx::beginFrame() {
    frame++;
}

void Gfx::pumpImages() {
    std::string url;
    SDL_Surface* surf;
    bool dropped;
    for (int i = 0; i < 4 && loader.pop(url, surf, dropped); i++) {
        pending.erase(url);
        if (dropped) continue;
        if (!surf) {
            failed[url] = SDL_GetTicks();
            continue;
        }
        SDL_Texture* t = r ? SDL_CreateTextureFromSurface(r, surf) : nullptr;
        if (t) imageCache[url] = Cached{t, surf->w, surf->h, frame, frame};
        SDL_FreeSurface(surf);
    }
    evict(imageCache, kMaxImageCache);
    evict(textCache, kMaxTextCache);
}

void Gfx::evict(std::unordered_map<std::string, Cached>& cache, size_t max) {
    if (cache.size() <= max) return;
    // Drop the least recently used entries (never ones used this frame).
    std::vector<std::pair<uint64_t, std::string>> order;
    order.reserve(cache.size());
    for (auto& kv : cache) order.emplace_back(kv.second.lastUse, kv.first);
    std::sort(order.begin(), order.end());
    size_t toRemove = cache.size() - max;
    for (size_t i = 0; i < order.size() && toRemove > 0; i++) {
        if (order[i].first >= frame) break;
        auto it = cache.find(order[i].second);
        SDL_DestroyTexture(it->second.tex);
        cache.erase(it);
        toRemove--;
    }
}

void Gfx::clear(Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderClear(r);
}

void Gfx::present() {
    SDL_RenderPresent(r);
}

// ---------------------------------------------------------------- shapes

void Gfx::rect(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    SDL_Rect rc{x, y, w, h};
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rc);
}

void Gfx::outline(int x, int y, int w, int h, int t, Color c) {
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y + t, t, h - 2 * t, c);
    rect(x + w - t, y + t, t, h - 2 * t, c);
}

// Triangle fan for a quarter circle centered at (cx, cy), starting at angle a0.
static void quarter(std::vector<SDL_Vertex>& v, float cx, float cy, float rad, float a0, SDL_Color c) {
    const int steps = 8;
    for (int i = 0; i < steps; i++) {
        float t0 = a0 + (float)M_PI / 2 * i / steps, t1 = a0 + (float)M_PI / 2 * (i + 1) / steps;
        v.push_back({{cx, cy}, c, {0, 0}});
        v.push_back({{cx + std::cos(t0) * rad, cy + std::sin(t0) * rad}, c, {0, 0}});
        v.push_back({{cx + std::cos(t1) * rad, cy + std::sin(t1) * rad}, c, {0, 0}});
    }
}

void Gfx::roundRect(int x, int y, int w, int h, int rad, Color c) {
    if (w <= 0 || h <= 0) return;
    rad = std::min(rad, std::min(w, h) / 2);
    if (rad <= 0) {
        rect(x, y, w, h, c);
        return;
    }
    rect(x + rad, y, w - 2 * rad, h, c);
    rect(x, y + rad, rad, h - 2 * rad, c);
    rect(x + w - rad, y + rad, rad, h - 2 * rad, c);
    std::vector<SDL_Vertex> v;
    SDL_Color sc{c.r, c.g, c.b, c.a};
    float R = (float)rad;
    quarter(v, x + R, y + R, R, (float)M_PI, sc);
    quarter(v, x + w - R, y + R, R, (float)M_PI * 1.5f, sc);
    quarter(v, x + w - R, y + h - R, R, 0, sc);
    quarter(v, x + R, y + h - R, R, (float)M_PI * 0.5f, sc);
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
}

void Gfx::roundOutline(int x, int y, int w, int h, int rad, int t, Color c) {
    // Approximation: outer rounded rect minus nothing (draw edges + corner arcs as thin fans).
    rad = std::min(rad, std::min(w, h) / 2);
    rect(x + rad, y, w - 2 * rad, t, c);
    rect(x + rad, y + h - t, w - 2 * rad, t, c);
    rect(x, y + rad, t, h - 2 * rad, c);
    rect(x + w - t, y + rad, t, h - 2 * rad, c);
    std::vector<SDL_Vertex> v;
    SDL_Color sc{c.r, c.g, c.b, c.a};
    auto arc = [&](float cx, float cy, float a0) {
        const int steps = 8;
        for (int i = 0; i < steps; i++) {
            float t0 = a0 + (float)M_PI / 2 * i / steps, t1 = a0 + (float)M_PI / 2 * (i + 1) / steps;
            float ro = (float)rad, ri = (float)std::max(0, rad - t);
            SDL_FPoint o0{cx + std::cos(t0) * ro, cy + std::sin(t0) * ro}, o1{cx + std::cos(t1) * ro, cy + std::sin(t1) * ro};
            SDL_FPoint i0{cx + std::cos(t0) * ri, cy + std::sin(t0) * ri}, i1{cx + std::cos(t1) * ri, cy + std::sin(t1) * ri};
            v.push_back({o0, sc, {0, 0}});
            v.push_back({o1, sc, {0, 0}});
            v.push_back({i0, sc, {0, 0}});
            v.push_back({i0, sc, {0, 0}});
            v.push_back({o1, sc, {0, 0}});
            v.push_back({i1, sc, {0, 0}});
        }
    };
    float R = (float)rad;
    arc(x + R, y + R, (float)M_PI);
    arc(x + w - R, y + R, (float)M_PI * 1.5f);
    arc(x + w - R, y + h - R, 0);
    arc(x + R, y + h - R, (float)M_PI * 0.5f);
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
}

void Gfx::circle(float cx, float cy, float rad, Color c) {
    std::vector<SDL_Vertex> v;
    SDL_Color sc{c.r, c.g, c.b, c.a};
    for (int q = 0; q < 4; q++) quarter(v, cx, cy, rad, (float)M_PI / 2 * q, sc);
    SDL_RenderGeometry(r, nullptr, v.data(), (int)v.size(), nullptr, 0);
}

void Gfx::triangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
    SDL_Color sc{c.r, c.g, c.b, c.a};
    SDL_Vertex v[3] = {{{x1, y1}, sc, {0, 0}}, {{x2, y2}, sc, {0, 0}}, {{x3, y3}, sc, {0, 0}}};
    SDL_RenderGeometry(r, nullptr, v, 3, nullptr, 0);
}

void Gfx::gradientV(int x, int y, int w, int h, Color c, Uint8 a0, Uint8 a1) {
    if (w <= 0 || h <= 0) return;
    Uint8 base = std::min(a0, a1);
    if (base) rect(x, y, w, h, c.alpha(base));
    if (a0 == a1 || !gradV) return;
    SDL_SetTextureColorMod(gradV, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(gradV, (Uint8)std::abs(a1 - a0));
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopyEx(r, gradV, nullptr, &dst, 0, nullptr, a1 > a0 ? SDL_FLIP_NONE : SDL_FLIP_VERTICAL);
}

void Gfx::gradientH(int x, int y, int w, int h, Color c, Uint8 a0, Uint8 a1) {
    if (w <= 0 || h <= 0) return;
    Uint8 base = std::min(a0, a1);
    if (base) rect(x, y, w, h, c.alpha(base));
    if (a0 == a1 || !gradH) return;
    SDL_SetTextureColorMod(gradH, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(gradH, (Uint8)std::abs(a1 - a0));
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopyEx(r, gradH, nullptr, &dst, 0, nullptr, a1 > a0 ? SDL_FLIP_NONE : SDL_FLIP_HORIZONTAL);
}

void Gfx::clip(int x, int y, int w, int h) {
    SDL_Rect rc{x, y, w, h};
    SDL_RenderSetClipRect(r, &rc);
}

void Gfx::unclip() {
    SDL_RenderSetClipRect(r, nullptr);
}

// ---------------------------------------------------------------- text

std::string Gfx::fit(const std::string& s, int size, int maxW) {
    TTF_Font* f = font(size);
    int w = 0;
    if (maxW <= 0 || !f) return s;
    TTF_SizeUTF8(f, s.c_str(), &w, nullptr);
    if (w <= maxW) return s;
    std::string t = s;
    while (!t.empty()) {
        size_t i = t.size() - 1;
        while (i > 0 && ((unsigned char)t[i] & 0xC0) == 0x80) --i;  // don't cut a UTF-8 character in half
        t.erase(i);
        std::string cand = t + "...";
        TTF_SizeUTF8(f, cand.c_str(), &w, nullptr);
        if (w <= maxW) return cand;
    }
    return "...";
}

Gfx::Cached* Gfx::textTex(const std::string& key, const std::string& s, int size, Color c, int maxW, int wrapW) {
    auto it = textCache.find(key);
    if (it != textCache.end()) {
        it->second.lastUse = frame;
        return &it->second;
    }
    TTF_Font* f = font(size);
    if (!f) return nullptr;
    SDL_Color sc{c.r, c.g, c.b, 255};
    SDL_Surface* surf = wrapW > 0 ? TTF_RenderUTF8_Blended_Wrapped(f, s.c_str(), sc, wrapW)
                                  : TTF_RenderUTF8_Blended(f, fit(s, size, maxW).c_str(), sc);
    if (!surf) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, surf);
    Cached cached{t, surf->w, surf->h, frame, frame};
    SDL_FreeSurface(surf);
    if (!t) return nullptr;
    if (c.a != 255) SDL_SetTextureAlphaMod(t, c.a);
    return &(textCache[key] = cached);
}

static std::string cacheKey(char kind, int size, Color c, int w, const std::string& s) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%c%d|%02x%02x%02x%02x|%d|", kind, size, c.r, c.g, c.b, c.a, w);
    return buf + s;
}

int Gfx::text(const std::string& s, int x, int y, int size, Color c, int maxW, Align align) {
    if (s.empty()) return 0;
    Cached* t = textTex(cacheKey('L', size, c, maxW, s), s, size, c, maxW, 0);
    if (!t) return 0;
    if (align == AlignCenter) x -= t->w / 2;
    else if (align == AlignRight) x -= t->w;
    SDL_Rect dst{x, y, t->w, t->h};
    SDL_RenderCopy(r, t->tex, nullptr, &dst);
    return t->w;
}

int Gfx::textWrapped(const std::string& s, int x, int y, int size, Color c, int wrapW, int maxH) {
    if (s.empty()) return 0;
    Cached* t = textTex(cacheKey('W', size, c, wrapW, s), s, size, c, 0, wrapW);
    if (!t) return 0;
    int h = maxH > 0 ? std::min(t->h, maxH) : t->h;
    SDL_Rect src{0, 0, t->w, h};
    SDL_Rect dst{x, y, t->w, h};
    SDL_RenderCopy(r, t->tex, &src, &dst);
    return h;
}

int Gfx::textWidth(const std::string& s, int size) {
    int w = 0;
    TTF_Font* f = font(size);
    if (f) TTF_SizeUTF8(f, s.c_str(), &w, nullptr);
    return w;
}

// ---------------------------------------------------------------- images

SDL_Texture* Gfx::image(const std::string& url, Uint8* fade) {
    if (fade) *fade = 0;
    if (url.empty()) return nullptr;
    auto it = imageCache.find(url);
    if (it != imageCache.end()) {
        it->second.lastUse = frame;
        if (fade) *fade = (Uint8)std::min<uint64_t>(255, (frame - it->second.born) * 255 / kFadeFrames);
        return it->second.tex;
    }
    // A failed image is tried again after a while: most failures are a busy network (a torrent
    // downloading), and a poster that failed once must not stay blank for good.
    auto f = failed.find(url);
    if (f != failed.end()) {
        if (SDL_GetTicks() - f->second < 10000) return nullptr;
        failed.erase(f);
    }
    if (url.compare(0, 6, "romfs:") == 0) {
        SDL_Surface* s = IMG_Load(url.c_str());
        SDL_Texture* t = s ? SDL_CreateTextureFromSurface(r, s) : nullptr;
        if (!t) {
            failed[url] = SDL_GetTicks();
            if (s) SDL_FreeSurface(s);
            return nullptr;
        }
        imageCache[url] = Cached{t, s->w, s->h, frame, 0};
        SDL_FreeSurface(s);
        if (fade) *fade = 255;
        return t;
    }
    if (!pending.count(url)) {
        pending.insert(url);
        loader.request(url);
    }
    return nullptr;
}

void Gfx::drawTex(SDL_Texture* t, int x, int y, int w, int h, Uint8 alpha) {
    SDL_Rect dst{x, y, w, h};
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_RenderCopy(r, t, nullptr, &dst);
}

void Gfx::drawCover(SDL_Texture* t, int x, int y, int w, int h, Uint8 alpha) {
    int tw = 0, th = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    SDL_Rect src{0, 0, tw, th};
    if ((float)tw / th > (float)w / h) {
        src.w = (int)((float)th * w / h);
        src.x = (tw - src.w) / 2;
    } else {
        src.h = (int)((float)tw * h / w);
        src.y = (th - src.h) / 4;  // keep faces: bias toward the top
    }
    SDL_Rect dst{x, y, w, h};
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_RenderCopy(r, t, &src, &dst);
}

SDL_Rect Gfx::drawFit(SDL_Texture* t, int x, int y, int maxW, int maxH, Align align, bool bottom, Uint8 alpha) {
    int tw = 0, th = 0;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    SDL_Rect dst{x, y, 0, 0};
    if (tw <= 0 || th <= 0) return dst;
    float s = std::min((float)maxW / tw, (float)maxH / th);
    dst.w = (int)(tw * s);
    dst.h = (int)(th * s);
    if (align == AlignCenter) dst.x = x + (maxW - dst.w) / 2;
    else if (align == AlignRight) dst.x = x + maxW - dst.w;
    if (bottom) dst.y = y + maxH - dst.h;
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_RenderCopy(r, t, nullptr, &dst);
    return dst;
}
