#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The stream the viewer picked, so "Continue watching" can go straight back to it.
struct SavedStream {
    std::string url, infoHash, title, addonName, quality, source;
    int fileIdx = -1;
    std::vector<std::string> trackers;
    bool valid() const { return !url.empty() || !infoHash.empty(); }
};

// What is being played, so the player can save progress and the home screen can show it.
struct PlayContext {
    std::string metaId, type, name, poster, background, logo;
    std::string videoId;       // movie id or episode id (tt123:1:2)
    std::string episodeLabel;  // "S1:E3 · Title", empty for movies
    std::vector<std::string> genres;
    SavedStream stream;
    std::string nextVideoId, nextEpisodeLabel;  // series: what "Up next" plays
    bool fromSaved = false;                      // playing the remembered stream (not saved to disk)
};

// Next episode id by numbering when the full episode list is not at hand: tt1:2:5 -> tt1:2:6.
std::string guessNextEpisode(const std::string& videoId, bool nextSeason);

struct HistoryEntry : PlayContext {
    double position = 0, duration = 0;  // seconds
    int64_t updated = 0;                // unix time
    double progress() const { return duration > 0 ? position / duration : 0; }
    bool finished() const { return duration > 0 && position >= duration * 0.94; }
};

// Watch history, saved to sdmc:/switch/stremnx/history.json (one entry per video).
class History {
public:
    void load();
    void save() const;

    void record(const PlayContext& ctx, double position, double duration);
    // Takes a title out of Continue Watching and Recently Watched for good, on this Switch: the account
    // sync doesn't bring it back unless it is watched again (here or on another device).
    void removeTitle(const std::string& metaId);
    void mergeAccount(const std::vector<HistoryEntry>& items);

    const HistoryEntry* findVideo(const std::string& videoId) const;
    // Where to resume this video: by its id or, failing that, by the file (a title can have another
    // id in another addon's catalog). Null when it was not started or is finished.
    const HistoryEntry* resumePoint(const PlayContext& ctx) const;
    const HistoryEntry* latestForMeta(const std::string& metaId) const;

    // One entry per title, newest first, not finished.
    std::vector<HistoryEntry> continueWatching(size_t max = 20) const;
    // One entry per title, newest first.
    std::vector<HistoryEntry> recentTitles(size_t max) const;
    bool empty() const { return entries.empty(); }

private:
    std::vector<HistoryEntry> entries;
    std::map<std::string, int64_t> removed;  // metaId -> when it was removed
};

int64_t parseIsoTime(const std::string& iso);
std::string episodeLabelFromId(const std::string& videoId);  // tt123:1:2 -> "S1:E2"
