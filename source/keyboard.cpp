#include "keyboard.hpp"

#include <cctype>
#include <cmath>
#include <vector>

#include "app.hpp"
#include "log.hpp"

// Laid out and colored like the Switch system keyboard (dark theme): character keys on the left, a
// tall OK on the right, cyan focus outline, and the same buttons (B delete, Y space, + OK, L/R cursor,
// left stick shift). Keys are big enough to aim at with the gyro pointer when docked.

namespace {

enum KeyAction { KChar, KShift, KSpace, KBack, KDone, KCancel, KPage, KLeft, KRight };

struct KbKey {
    std::string label;  // the character it types (KChar)
    KeyAction act;
    int row, rows, col, span;  // 12 character columns; col 12 is the wide right column
};

namespace kb {
constexpr Color bgTop{30, 30, 30, 255}, panel{45, 45, 45, 255}, key{72, 72, 72, 255}, keyAlt{58, 58, 58, 255};
constexpr Color ok{0, 162, 200, 255}, focus{0, 195, 227, 255}, text{255, 255, 255, 255}, dim{165, 165, 165, 255};
}  // namespace kb

constexpr int kX0 = 61, kUnit = 84, kGap = 6, kRightW = 150, kY0 = 302, kStep = 72, kKeyH = 64;

std::vector<std::string> chars(const std::string& s) {  // UTF-8 characters
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

std::vector<KbKey> layout(bool symbols) {
    static const char* letters[] = {"1234567890-=", "qwertyuiop@/", "asdfghjkl:'_", "zxcvbnm,.?!"};
    static const char* syms[] = {"!@#$%^&*()+=", "[]{}<>|~`;\",", "áéíóúàâêôãõç", "ÁÉÍÓÚÂÊÔÃÕÇ"};
    std::vector<KbKey> keys;
    for (int r = 0; r < 4; r++) {
        int c = r == 3 ? 1 : 0;  // row 3 starts after Shift
        for (auto& ch : chars(symbols ? syms[r] : letters[r])) keys.push_back({ch, KChar, r, 1, c++, 1});
    }
    keys.push_back({"", KShift, 3, 1, 0, 1});
    keys.push_back({symbols ? "abc" : "#+=", KPage, 4, 1, 0, 2});
    keys.push_back({"Space", KSpace, 4, 1, 2, 8});
    keys.push_back({"", KLeft, 4, 1, 10, 1});
    keys.push_back({"", KRight, 4, 1, 11, 1});
    keys.push_back({"", KBack, 0, 1, 12, 1});
    keys.push_back({"Cancel", KCancel, 1, 1, 12, 1});
    keys.push_back({"OK", KDone, 2, 3, 12, 1});
    return keys;
}

SDL_Rect keyRect(const KbKey& k) {
    int y = kY0 + k.row * kStep, h = k.rows * kStep - (kStep - kKeyH);
    if (k.col == 12) return {kX0 + 12 * kUnit, y, kRightW, h};
    return {kX0 + k.col * kUnit, y, k.span * kUnit - kGap, h};
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

size_t prevChar(const std::string& s, size_t at) {
    if (at == 0) return 0;
    do at--;
    while (at > 0 && ((unsigned char)s[at] & 0xC0) == 0x80);
    return at;
}

size_t nextChar(const std::string& s, size_t at) {
    if (at >= s.size()) return s.size();
    do at++;
    while (at < s.size() && ((unsigned char)s[at] & 0xC0) == 0x80);
    return at;
}

void hint(Gfx& g, int& x, int y, const std::string& button, const std::string& label) {  // right to left
    int lw = g.textWidth(label, 18);
    x -= lw;
    g.text(label, x, y - 11, 18, kb::dim);
    int bw = std::max(24, g.textWidth(button, 15) + 12);
    x -= bw + 8;
    g.roundRect(x, y - 12, bw, 24, 12, kb::text.alpha(230));
    g.text(button, x + bw / 2, y - 9, 15, kb::panel, 0, AlignCenter);
    x -= 26;
}

}  // namespace

bool showKeyboard(App& app, std::string& out, const std::string& guide, const std::string& initial, bool password) {
    Gfx& g = app.gfx;
    Input& in = app.in;
    bool symbols = false, shift = false;
    std::vector<KbKey> keys = layout(symbols);
    std::string text = initial;
    size_t cursor = text.size();  // byte offset, on a character boundary
    int sel = 12, selRow = 1;     // "q"; selRow: the row the D-pad is on (the tall OK spans three)

    auto covers = [](const KbKey& k, int row) { return row >= k.row && row < k.row + k.rows; };
    auto centerX = [](const KbKey& k) {
        SDL_Rect r = keyRect(k);
        return r.x + r.w / 2;
    };

    while (appletMainLoop()) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) return false;
        watchBeat(WatchMain, "keyboard");
        in.update();
        g.beginFrame();
        g.pumpImages();

        // D-pad: left/right along the row, up/down to the key under the same spot.
        int dx = in.pressed(K_RIGHT) ? 1 : in.pressed(K_LEFT) ? -1 : 0;
        if (dx) {
            int cx = centerX(keys[sel]), best = -1, bestD = 1 << 30;
            for (int i = 0; i < (int)keys.size(); i++) {
                int d = (centerX(keys[i]) - cx) * dx;
                if (covers(keys[i], selRow) && d > 0 && d < bestD) bestD = d, best = i;
            }
            if (best >= 0) sel = best;
        }
        int dy = in.pressed(K_DOWN) ? 1 : in.pressed(K_UP) ? -1 : 0;
        if (dy && selRow + dy >= 0 && selRow + dy <= 4) {
            int cx = centerX(keys[sel]), best = -1, bestD = 1 << 30;
            for (int i = 0; i < (int)keys.size(); i++) {
                int d = std::abs(centerX(keys[i]) - cx);
                if (covers(keys[i], selRow + dy) && d < bestD) bestD = d, best = i;
            }
            if (best >= 0) {
                sel = best;
                selRow += dy;
            }
        }

        // A (or a touch / pointer click on a key) presses the key; the buttons do what the system does.
        int press = in.pressed(K_A) ? sel : -1;
        int tx = 0, ty = 0;
        if (in.tap(tx, ty))
            for (int i = 0; i < (int)keys.size(); i++) {
                SDL_Rect r = keyRect(keys[i]);
                if (tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h) {
                    press = sel = i;
                    selRow = std::max(keys[i].row, std::min(selRow, keys[i].row + keys[i].rows - 1));
                }
            }
        KeyAction act = press >= 0 ? keys[press].act : KChar;
        if (press < 0) {
            if (in.pressed(K_B)) act = text.empty() ? KCancel : KBack;
            else if (in.pressed(K_Y)) act = KSpace;
            else if (in.pressed(K_PLUS)) act = KDone;
            else if (in.pressed(K_MINUS)) act = KCancel;
            else if (in.pressed(K_X)) act = KPage;
            else if (in.pressed(K_LSTICK)) act = KShift;
            else if (in.pressed(K_L)) act = KLeft;
            else if (in.pressed(K_R)) act = KRight;
            else act = KeyAction(-1);
        }
        switch (act) {
            case KChar: {
                std::string ch = shift ? upper(keys[press].label) : keys[press].label;
                text.insert(cursor, ch);
                cursor += ch.size();
                shift = false;
                break;
            }
            case KSpace:
                text.insert(cursor, " ");
                cursor++;
                break;
            case KBack: {
                size_t p = prevChar(text, cursor);
                text.erase(p, cursor - p);
                cursor = p;
                break;
            }
            case KShift: shift = !shift; break;
            case KPage:
                symbols = !symbols;
                keys = layout(symbols);  // same positions on both pages, so the focus stays put
                break;
            case KLeft: cursor = prevChar(text, cursor); break;
            case KRight: cursor = nextChar(text, cursor); break;
            case KDone: out = text; return true;
            case KCancel: return false;
            default: break;
        }

        // ------------------------------------------------ drawing
        g.clear(kb::bgTop);
        g.text(guide, 60, 40, 22, kb::dim, 1160);
        std::string shown = password ? std::string(chars(text).size(), '*') : text;
        size_t shownCursor = password ? chars(text.substr(0, cursor)).size() : cursor;
        // Scroll so the cursor stays in view.
        const int fieldX = 60, fieldW = 1160;
        int caret = g.textWidth(shown.substr(0, shownCursor), 38);
        int scroll = std::max(0, caret - (fieldW - 20));
        g.clip(fieldX, 96, fieldW, 70);
        if (!shown.empty()) g.text(shown, fieldX - scroll, 104, 38, kb::text);
        if (SDL_GetTicks() / 500 % 2) g.rect(fieldX - scroll + caret + 1, 102, 3, 52, kb::focus);
        g.unclip();
        g.rect(fieldX, 172, fieldW, 2, kb::focus);
        g.text(std::to_string(chars(text).size()) + "/500", fieldX + fieldW, 184, 16, kb::dim, 0, AlignRight);

        g.rect(0, 284, SCREEN_W, SCREEN_H - 284, kb::panel);
        for (int i = 0; i < (int)keys.size(); i++) {
            const KbKey& k = keys[i];
            SDL_Rect r = keyRect(k);
            bool focused = i == sel;
            Color bg = k.act == KDone ? kb::ok : k.act == KChar || k.act == KSpace ? kb::key : kb::keyAlt;
            if (k.act == KShift && shift) bg = kb::focus.alpha(150);
            g.roundRect(r.x, r.y, r.w, r.h, 8, bg);
            if (focused) {
                g.roundOutline(r.x - 3, r.y - 3, r.w + 6, r.h + 6, 10, 3, kb::focus);
                g.roundRect(r.x, r.y, r.w, r.h, 8, kb::text.alpha(18));
            }
            int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
            switch (k.act) {
                case KChar:
                    g.text(shift ? upper(k.label) : k.label, cx, cy - 18, 30, kb::text, 0, AlignCenter);
                    break;
                case KShift:  // arrow up on a stem
                    g.triangle(cx - 12, cy + 2, cx + 12, cy + 2, cx, cy - 12, kb::text);
                    g.rect(cx - 5, cy + 2, 10, 10, kb::text);
                    break;
                case KLeft: g.triangle(cx + 7, cy - 10, cx + 7, cy + 10, cx - 9, cy, kb::text); break;
                case KRight: g.triangle(cx - 7, cy - 10, cx - 7, cy + 10, cx + 9, cy, kb::text); break;
                case KBack:  // a key shape pointing left, plus the B hint
                    g.triangle(cx - 30, cy, cx - 16, cy - 12, cx - 16, cy + 12, kb::text);
                    g.rect(cx - 16, cy - 12, 26, 24, kb::text);
                    g.roundRect(cx + 22, cy - 12, 24, 24, 12, kb::text.alpha(230));
                    g.text("B", cx + 34, cy - 9, 15, kb::panel, 0, AlignCenter);
                    break;
                case KDone:
                    g.text("OK", cx, cy - 22, 34, kb::text, 0, AlignCenter);
                    g.roundRect(cx - 12, cy + 22, 24, 24, 12, kb::text.alpha(230));
                    g.text("+", cx, cy + 25, 15, kb::panel, 0, AlignCenter);
                    break;
                default: g.text(k.label, cx, cy - 13, 22, kb::text, 0, AlignCenter); break;
            }
        }
        int hx = SCREEN_W - 40;
        hint(g, hx, 692, "+", "OK");
        hint(g, hx, 692, "B", "Delete");
        hint(g, hx, 692, "Y", "Space");
        hint(g, hx, 692, "X", symbols ? "Letters" : "Symbols");
        hint(g, hx, 692, "L/R", "Move cursor");
        hint(g, hx, 692, "-", "Cancel");
        app.drawPointer();
        g.present();
    }
    return false;
}
