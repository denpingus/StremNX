#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct TorrentStatus {
    bool hasMetadata = false;
    int peersConnected = 0;  // handshake done
    int peersSending = 0;    // connected and not choking us
    int peersKnown = 0;
    int64_t downloaded = 0;
    double speed = 0;  // bytes/s
    std::string fileName;
    int64_t fileSize = 0;
    std::string error;
    // SD card saving
    bool saving = false, complete = false;
    int64_t savedBytes = 0;
    std::string saveError, savePath;
};

// Minimal BitTorrent client built for streaming one file of a torrent:
// trackers (HTTP/UDP) + PEX for peers, ut_metadata for the info dict, and a
// piece picker that follows the player's read position. Pieces live in a
// bounded RAM cache, nothing is written to the SD card.
class Torrent {
public:
    Torrent(const std::string& infoHashHex, const std::vector<std::string>& trackers, int fileIdx);
    ~Torrent();

    void start();
    void stop();
    bool stopped() const;
    // Waits for the engine thread to close its files after stop().
    bool waitStopped(int timeoutMs);
    // After sleep: connections are dead, so forget failed attempts and ask for peers again.
    void wakeUp();
    // 0: never upload, 1: limited (~100 KB/s, 2 peers), 2: normal (6 peers, no limit).
    void setUploadMode(int mode);
    // While on, no new pieces are requested (those in flight still arrive): leaves the connection to
    // what the viewer is waiting for, like artwork, when this is a background download.
    void yieldNetwork(bool on);
    // Off for a background download: it keeps a small RAM cache (the pieces are on the SD card), so
    // several downloads don't hold 256 MB each.
    void setWatched(bool on);

    // Call before start(): saves the selected file into `dir` and downloads all of it, resuming a
    // previous partial download. `infoJson` (title, artwork...) is stored in the progress file.
    void saveTo(const std::string& dir, const std::string& infoJson);
    // Saving to the SD card and not finished yet.
    bool keepsDownloading();
    const std::string& infoHash() const;
    int fileIndex() const;

    TorrentStatus status();
    bool hasMetadata();
    int64_t fileSize();
    std::string fileName();

    // Fetches the part at `fraction` of the file early (the resume point, before the player seeks there).
    void prefetch(double fraction);

    // True once the start and the end of the file are downloaded. pct = progress toward that.
    bool initialBufferReady(int* pct, bool* startReady = nullptr);

    // Reads from the selected file. Blocks until the data arrives; -1 on stop/cancel, 0 at EOF.
    int64_t read(int64_t fileOffset, char* buf, int64_t n, const std::atomic<bool>& cancel);

    struct Impl;

private:
    std::shared_ptr<Impl> d;
};
