#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "history.hpp"
#include "log.hpp"

#define DOWNLOADS_DIR APP_DIR "/downloads"

// A torrent file saved (or being saved) to the SD card, read from its progress file.
struct DownloadInfo {
    std::string sidecar, file, infoHash, fileName;
    int fileIdx = -1;
    int64_t size = 0, savedBytes = 0, updated = 0;
    bool complete = false;
    PlayContext ctx;
    std::vector<std::string> trackers;
    double progress() const { return size > 0 ? (double)savedBytes / size : 0; }
};

std::vector<DownloadInfo> listDownloads();  // newest first
// The SD card download (finished or not) of this torrent file, if there is one.
bool findSavedDownload(const std::string& infoHash, int fileIdx, DownloadInfo& out);
bool deleteDownload(const DownloadInfo& d);
int64_t sdFreeSpace();
// Progress-file JSON describing what is being saved, for the Downloads screen.
std::string downloadInfoJson(const PlayContext& ctx, const std::vector<std::string>& trackers);
