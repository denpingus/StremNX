#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "history.hpp"
#include "input.hpp"
#include "library.hpp"
#include "stremio.hpp"
#include "task.hpp"
#include "torrent.hpp"
#include "ui.hpp"

constexpr int SCREEN_W = 1280;
constexpr int SCREEN_H = 720;

// Saved to sdmc:/switch/stremnx/config.json
struct Config {
    std::string authKey, email;
    std::string server;               // stremio-server for torrents, e.g. http://192.168.0.10:11470
    std::vector<std::string> addons;  // extra addons added by URL
    std::vector<std::string> disabledAddons;  // addons switched off on this Switch (any kind)
    // Home sections (catalog keys "addonId/type/catalogId"): the order picked and the ones switched off.
    std::vector<std::string> homeOrder, homeHidden;
    bool autoDownload = false;        // save every torrent played to the SD card (off: only by hand)
    int uploadMode = 1;               // torrent uploading: 0 off, 1 limited, 2 normal
    // Subtitles
    std::string subLang = "pob";  // SubLanguage code, "" = off
    bool subAddons = true;        // fetch subtitles from addons (OpenSubtitles...)
    int subSize = 100;            // percent
    int subPos = 100;             // mpv sub-pos: 100 = bottom
    int subStyle = 0;             // 0 outline + shadow, 1 dark box
    int subColor = 0;             // 0 white, 1 yellow
    int subDelayMs = 0;           // default offset applied to every video
    int subStepMs = 100;          // L/R step in the player
    // Playback
    std::string prefQuality = "auto";  // auto (learn from my picks), 4K, 1080p, 720p, SD
    std::string lastQuality;           // learned: quality of the last stream picked by hand
    int preferSource = 0;              // 0 any, 1 debrid/direct links, 2 torrents
    bool autoplayNext = true;          // play the next episode at the end
    bool resumeSavedStream = true;     // Continue watching reuses the stream picked before
    int volume = 100;                  // player volume in percent (the last one picked in the player)
    // mpv dithering: off by default. Its noise pattern is fixed on screen, per color channel, and on the
    // OLED panel it shows as a still film of red and green specks over dark scenes.
    bool dither = false;
    void load();
    void save() const;
};

class App;

class Screen {
public:
    virtual ~Screen() {}
    virtual void update(App& app) = 0;
    virtual void draw(App& app) = 0;
};

std::unique_ptr<Screen> makeHomeScreen();

struct AddonsResult {
    std::vector<Addon> addons;
    std::vector<std::string> errors;
    std::map<std::string, std::string> failed;  // addon URL -> why it did not load
    std::vector<HistoryEntry> library;  // account progress, when logged in
    bool accountOk = false;             // datastore read worked
    Json accountItems;                  // raw account library items
    std::vector<MetaItem> libraryItems; // titles in the account library
};

struct PlayResult {
    std::string error;
    double position = 0, duration = 0;
    bool ended = false;     // reached the end of the file
    bool playNext = false;  // "Up next" chosen (or counted down)
};

class App {
public:
    bool init();
    void run();
    void shutdown();

    // Screen stack changes are applied at the end of the frame.
    void push(std::unique_ptr<Screen> s);
    void pop();
    void quit() { running = false; }
    void toast(const std::string& msg);
    // resume: start from the saved position, if any.
    void requestPlay(const std::string& url, const PlayContext& ctx, bool resume = true);
    void reloadAddons();
    // Background SD card downloads (torrents that keep running after leaving the player).
    void keepDownloading(const std::shared_ptr<Torrent>& t);
    std::shared_ptr<Torrent> findDownload(const std::string& infoHash, int fileIdx);
    // The last streamed torrent stays alive a few minutes after leaving it (RAM cache and peers kept,
    // nothing new downloaded): opening the same stream again starts at once. nullptr drops it.
    void keepWarm(const std::shared_ptr<Torrent>& t);
    std::shared_ptr<Torrent> takeWarm(const std::string& infoHash, int fileIdx);
    void stopDownload(const std::string& infoHash, int fileIdx);
    std::vector<std::shared_ptr<Torrent>> downloads;

    Gfx gfx;
    Input in;
    Config cfg;
    History history;
    Library library;
    int libraryVersion = 0;
    std::map<std::string, Json> accountItems;  // raw library items by id (to update without losing fields)
    void setInLibrary(const MetaItem& m, bool in);
    void syncProgress(const PlayContext& ctx, double position, double duration, bool ended);
    void onResume();  // back from sleep / HOME: wake up network users

    // Touch: screens register tappable rectangles while drawing; a tap is matched against the
    // rectangles drawn in the previous frame (what the viewer saw).
    struct Hit {
        SDL_Rect r;
        int id, a, b;
    };
    void hit(int x, int y, int w, int h, int id, int a = 0, int b = 0);
    bool tapped(int& id, int& a, int& b);
    std::vector<Hit> hits;
    SDL_Window* win = nullptr;

    std::shared_ptr<Torrent> activeTorrent;  // read by the player through the torrent:// protocol

    std::vector<Addon> addons;
    int addonsVersion = 0;   // bumped whenever the addon list changes
    int homeVersion = 0;     // bumped when the home sections are edited
    std::map<std::string, std::string> addonErrors;  // addon URL -> why it did not load (last reload)
    int historyVersion = 0;  // bumped whenever the watch history changes
    bool addonsLoading() const { return addonsTask.active(); }

    // Footer with button hints.
    void drawFooter(const std::string& hints);
    void drawLoading(int x, int y, const std::string& label, Align align = AlignCenter);
    void drawSpinner(int cx, int cy, int radius);
    void drawPointer();  // the gyro pointer, when it is showing

private:
    void applyPending();
    void dumpSystemFont();

    std::vector<std::unique_ptr<Screen>> stack;
    std::vector<std::unique_ptr<Screen>> toPush;
    int toPop = 0;
    bool running = true;
    std::string toastMsg;
    uint32_t toastUntil = 0;
    std::string playUrl;
    PlayContext playCtx;
    bool playResume = true;
    Task<AddonsResult> addonsTask;
    std::shared_ptr<Torrent> warm;
    uint32_t warmUntil = 0;
};

// startAt: seconds to resume from (0 = beginning).
PlayResult runPlayer(App& app, const std::string& url, const PlayContext& ctx, double startAt);
