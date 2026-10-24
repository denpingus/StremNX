#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

// 8 bits per color for the window and every GL context. SDL's minimum is 3/3/2, so the system could hand
// out a 16-bit surface: mpv then dithered heavily for 5-6 bit color, which shows as grain in dark scenes.
inline void requestColorDepth() {
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
}

// GL_DITHER is on by default in OpenGL: below 8 bits per color the GPU dithers everything drawn with a
// fixed per-channel pattern, a still film of red / green specks over dark areas. Off: at worst banding.
// Call with the context current.
inline void disableGlDither() {
    using Cap = void (*)(unsigned);
    if (auto off = (Cap)SDL_GL_GetProcAddress("glDisable")) off(0x0BD0);  // GL_DITHER
}

struct Color {
    Uint8 r, g, b, a;
    Color alpha(Uint8 na) const { return Color{r, g, b, na}; }
};

namespace col {
constexpr Color bg{11, 10, 18, 255};
constexpr Color bar{22, 20, 34, 255};
constexpr Color card{30, 27, 46, 255};
constexpr Color cardSel{52, 44, 92, 255};
constexpr Color accent{123, 91, 245, 255};
constexpr Color accentLight{166, 140, 255, 255};
constexpr Color text{244, 243, 250, 255};
constexpr Color dim{160, 157, 181, 255};
constexpr Color faint{98, 95, 118, 255};
constexpr Color white{255, 255, 255, 255};
constexpr Color black{0, 0, 0, 255};
constexpr Color gold{245, 197, 24, 255};
constexpr Color error{240, 90, 90, 255};
constexpr Color success{80, 200, 140, 255};
constexpr Color shade{11, 10, 18, 210};
}  // namespace col

enum Align { AlignLeft, AlignCenter, AlignRight };

// Downloads and decodes images (posters, backgrounds) on background threads.
class ImageLoader {
public:
    struct Shared;
    void start(int threads);
    void request(const std::string& url);
    // Hands back a finished result. surf == nullptr with dropped == false means it failed.
    bool pop(std::string& url, SDL_Surface*& surf, bool& dropped);

private:
    std::shared_ptr<Shared> sh;
};

class Gfx {
public:
    bool init(SDL_Window* win);
    void releaseRenderer();  // before opening the player (mpv takes over the GL context)
    bool restoreRenderer();
    void destroy();

    void beginFrame();
    uint64_t frameCount() const { return frame; }
    bool loadingImages() const { return !pending.empty(); }
    void pumpImages();
    void clear(Color c);
    void present();

    void rect(int x, int y, int w, int h, Color c);
    void outline(int x, int y, int w, int h, int thickness, Color c);
    void roundRect(int x, int y, int w, int h, int radius, Color c);
    void roundOutline(int x, int y, int w, int h, int radius, int thickness, Color c);
    void circle(float cx, float cy, float radius, Color c);
    void triangle(float x1, float y1, float x2, float y2, float x3, float y3, Color c);
    // Linear fades of one color between two alphas.
    void gradientV(int x, int y, int w, int h, Color c, Uint8 alphaTop, Uint8 alphaBottom);
    void gradientH(int x, int y, int w, int h, Color c, Uint8 alphaLeft, Uint8 alphaRight);
    void clip(int x, int y, int w, int h);
    void unclip();

    // Draws one line of text (cut with "..." if maxW > 0). Returns the width.
    int text(const std::string& s, int x, int y, int size, Color c, int maxW = 0, Align align = AlignLeft);
    // Word-wrapped text, clipped at maxH. Returns the drawn height.
    int textWrapped(const std::string& s, int x, int y, int size, Color c, int wrapW, int maxH);
    int textWidth(const std::string& s, int size);
    std::string fontFamily();

    // nullptr until the image has arrived. `fade` gets 0-255 for a short fade-in after arrival.
    // "romfs:/..." paths are loaded synchronously from the app's romfs.
    SDL_Texture* image(const std::string& url, Uint8* fade = nullptr);
    void drawTex(SDL_Texture* t, int x, int y, int w, int h, Uint8 alpha = 255);
    // Scales to fill the box and crops the overflow (CSS object-fit: cover).
    void drawCover(SDL_Texture* t, int x, int y, int w, int h, Uint8 alpha = 255);
    // Scales to fit inside the box keeping the aspect ratio; returns the drawn size.
    SDL_Rect drawFit(SDL_Texture* t, int x, int y, int maxW, int maxH, Align align, bool bottom, Uint8 alpha = 255);

    SDL_Renderer* r = nullptr;
    SDL_Window* win = nullptr;
    PlFontData fontData{};

private:
    struct Cached {
        SDL_Texture* tex;
        int w, h;
        uint64_t lastUse;
        uint64_t born;
    };

    TTF_Font* font(int size);
    std::string fit(const std::string& s, int size, int maxW);
    Cached* textTex(const std::string& key, const std::string& s, int size, Color c, int maxW, int wrapW);
    void evict(std::unordered_map<std::string, Cached>& cache, size_t max);
    void dropTextures();
    void makeGradients();

    std::unordered_map<int, TTF_Font*> fonts;
    std::unordered_map<std::string, Cached> textCache, imageCache;
    std::unordered_set<std::string> pending;
    std::unordered_map<std::string, uint32_t> failed;  // url -> when it failed (tried again later)
    ImageLoader loader;
    uint64_t frame = 0;
    SDL_Texture* gradH = nullptr;  // 256x1, alpha 0 -> 255 left to right
    SDL_Texture* gradV = nullptr;  // 1x256, alpha 0 -> 255 top to bottom
};
