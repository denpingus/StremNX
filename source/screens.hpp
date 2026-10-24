#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app.hpp"

constexpr int W = SCREEN_W;
constexpr int H = SCREEN_H;

struct CatalogResult {
    std::vector<MetaItem> items;
    std::string err;
};

// ---------------------------------------------------------------- text helpers

std::string cleanText(const std::string& s);  // strips emojis the system font can't draw
std::vector<std::string> splitLines(const std::string& s);
std::string joinFrom(const std::vector<std::string>& v, size_t from, const char* sep);
std::string joinGenres(const std::vector<std::string>& g, size_t max);
std::string formatClock(double seconds);     // 1:02:03
std::string formatLeft(double seconds);      // "1h 12m left"
std::string formatBytes(double bytes);
void keepVisible(int sel, int& first, int visible);
float approach(float current, float target, float speed);  // frame-rate friendly lerp

// ---------------------------------------------------------------- shared drawing

enum ButtonIcon { IconNone, IconPlay, IconInfo, IconRestart, IconList, IconAdd, IconCheck };

// Rounded pill button. primary = white with dark text (Netflix "Play"), otherwise translucent.
void drawButton(Gfx& g, int x, int y, int w, int h, const std::string& label, bool focused, bool primary,
                ButtonIcon icon);
int buttonWidth(Gfx& g, const std::string& label, ButtonIcon icon);
// Full-screen artwork with the cinematic left + bottom fades used on the hero and details pages.
void drawBackdrop(Gfx& g, const std::string& url, int y = 0, Uint8 strength = 255);
// Small "IMDb 7.8" badge. Returns its width.
int drawRating(Gfx& g, int x, int y, const std::string& rating);
void drawProgress(Gfx& g, int x, int y, int w, int h, double progress);

// ---------------------------------------------------------------- navigation

PlayContext contextForMovie(const MetaItem& item);
PlayContext contextForEpisode(const Meta& meta, const Video& v);
MetaItem itemFromEntry(const HistoryEntry& e);

std::unique_ptr<Screen> makeDetailScreen(const MetaItem& item);
std::unique_ptr<Screen> makeStreamsScreen(const PlayContext& ctx, bool resume = true);

// ---------------------------------------------------------------- streams

struct StreamInfo {
    std::string quality, hdr, size, seeds, source, title, subtitle;
    int rank = 0;  // 4K = 4, 1080p = 3, 720p = 2, SD = 1, unknown = 0
    double bytes = 0;
    int seedCount = 0;
};
StreamInfo describeStream(const Stream& s);
// Higher is better: preferred quality, fast sources, seeders, and the addon used last time.
int scoreStream(const Stream& s, const StreamInfo& i, const Config& cfg, const SavedStream* previous);
int pickBestStream(const std::vector<Stream>& streams, const Config& cfg, const SavedStream* previous);
SavedStream savedFrom(const Stream& s, const StreamInfo& i);
Stream streamFromSaved(const SavedStream& s);
bool sameStream(const Stream& s, const SavedStream& saved);
// Plays a stream (link, SD card file or torrent) and remembers it in the context.
// byHand: picked in the list (teaches the preferred quality).
void playStream(App& app, const Stream& s, PlayContext ctx, bool resume, bool byHand);

std::unique_ptr<Screen> makeTorrentScreen(App& app, const Stream& s, const PlayContext& ctx, bool resume);
std::unique_ptr<Screen> makeNextEpisodeScreen(const PlayContext& finished);
std::unique_ptr<Screen> makeAddonsScreen();  // Settings > Manage addons
std::unique_ptr<Screen> makeHomeSectionsScreen();  // Settings > Home sections

// Touch: a "Back" pill at the top-left of sub-screens (tap id kHitBack).
constexpr int kHitBack = 2;
void drawBackButton(App& app);
