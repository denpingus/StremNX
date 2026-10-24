#include "downloads.hpp"

#include <dirent.h>
#include <strings.h>
#include <switch.h>

#include <algorithm>
#include <cstdio>

#include "json.hpp"

std::vector<DownloadInfo> listDownloads() {
    std::vector<DownloadInfo> out;
    DIR* dir = opendir(DOWNLOADS_DIR);
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
        std::string name = e->d_name;
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        std::string path = std::string(DOWNLOADS_DIR "/") + name, text;
        if (!readFile(path, text)) continue;
        bool ok = false;
        Json j = Json::parse(text, &ok);
        if (!ok) continue;
        DownloadInfo d;
        d.sidecar = path;
        d.file = j["file"].str();
        d.infoHash = j["infoHash"].str();
        d.fileName = j["fileName"].str();
        d.fileIdx = (int)j["fileIdx"].num(-1);
        d.size = (int64_t)j["size"].num();
        d.savedBytes = (int64_t)j["savedBytes"].num();
        d.updated = (int64_t)j["updated"].num();
        d.complete = j["complete"].boolean();
        d.ctx.metaId = j["metaId"].str();
        d.ctx.type = j["type"].str();
        d.ctx.name = j["name"].str(d.fileName);
        d.ctx.poster = j["poster"].str();
        d.ctx.background = j["background"].str();
        d.ctx.logo = j["logo"].str();
        d.ctx.videoId = j["videoId"].str();
        d.ctx.episodeLabel = j["episodeLabel"].str();
        for (auto& g : j["genres"].items()) d.ctx.genres.push_back(g.str());
        for (auto& t : j["trackers"].items()) d.trackers.push_back(t.str());
        if (!d.infoHash.empty()) out.push_back(d);
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const DownloadInfo& a, const DownloadInfo& b) { return a.updated > b.updated; });
    return out;
}

bool findSavedDownload(const std::string& infoHash, int fileIdx, DownloadInfo& out) {
    for (auto& d : listDownloads())
        if (d.fileIdx == fileIdx && strcasecmp(d.infoHash.c_str(), infoHash.c_str()) == 0) {
            out = d;
            return true;
        }
    return false;
}

bool deleteDownload(const DownloadInfo& d) {
    bool ok = true;
    if (!d.file.empty() && fileExists(d.file)) ok = remove(d.file.c_str()) == 0;
    if (remove(d.sidecar.c_str()) != 0) ok = false;
    remove((std::string(DOWNLOADS_DIR "/") + d.infoHash + ".info").c_str());  // saved torrent metadata
    logMsg("downloads: deleted %s (%s)", d.fileName.c_str(), ok ? "ok" : "failed");
    return ok;
}

int64_t sdFreeSpace() {
    s64 free = 0;
    if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) fsFsGetFreeSpace(fs, "/", &free);
    return free;
}

std::string downloadInfoJson(const PlayContext& ctx, const std::vector<std::string>& trackers) {
    Json j = Json::object();
    Json genres = Json::array(), tr = Json::array();
    for (auto& g : ctx.genres) genres.push(g);
    for (auto& t : trackers) tr.push(t);
    j.set("metaId", ctx.metaId).set("type", ctx.type).set("name", ctx.name).set("poster", ctx.poster);
    j.set("background", ctx.background).set("logo", ctx.logo).set("videoId", ctx.videoId);
    j.set("episodeLabel", ctx.episodeLabel).set("genres", genres).set("trackers", tr);
    return j.dump();
}
