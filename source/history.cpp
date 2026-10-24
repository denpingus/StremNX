#include "history.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>

#include "json.hpp"
#include "log.hpp"

static const char* kHistoryPath = APP_DIR "/history.json";
static const size_t kMaxEntries = 300;

void History::load() {
    std::string text;
    if (!readFile(kHistoryPath, text)) return;
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok) {
        logMsg("invalid history.json, starting fresh");
        return;
    }
    entries.clear();
    removed.clear();
    for (auto& r : j["removed"].items()) removed[r["metaId"].str()] = (int64_t)r["at"].num();
    for (auto& e : j["entries"].items()) {
        HistoryEntry h;
        h.metaId = e["metaId"].str();
        h.type = e["type"].str();
        h.name = e["name"].str();
        h.poster = e["poster"].str();
        h.background = e["background"].str();
        h.logo = e["logo"].str();
        h.videoId = e["videoId"].str();
        h.episodeLabel = e["episodeLabel"].str();
        for (auto& g : e["genres"].items()) h.genres.push_back(g.str());
        const Json& s = e["stream"];
        h.stream.url = s["url"].str();
        h.stream.infoHash = s["infoHash"].str();
        h.stream.title = s["title"].str();
        h.stream.addonName = s["addon"].str();
        h.stream.quality = s["quality"].str();
        h.stream.source = s["source"].str();
        h.stream.fileIdx = (int)s["fileIdx"].num(-1);
        for (auto& t : s["trackers"].items()) h.stream.trackers.push_back(t.str());
        h.nextVideoId = e["nextVideoId"].str();
        h.nextEpisodeLabel = e["nextEpisodeLabel"].str();
        h.position = e["position"].num();
        h.duration = e["duration"].num();
        h.updated = (int64_t)e["updated"].num();
        if (!h.metaId.empty() && !h.videoId.empty()) entries.push_back(h);
    }
}

void History::save() const {
    Json arr = Json::array();
    for (auto& h : entries) {
        Json e = Json::object();
        Json genres = Json::array();
        for (auto& g : h.genres) genres.push(g);
        e.set("metaId", h.metaId).set("type", h.type).set("name", h.name).set("poster", h.poster);
        e.set("background", h.background).set("logo", h.logo).set("videoId", h.videoId);
        e.set("episodeLabel", h.episodeLabel).set("genres", genres);
        e.set("nextVideoId", h.nextVideoId).set("nextEpisodeLabel", h.nextEpisodeLabel);
        if (h.stream.valid()) {
            Json s = Json::object(), tr = Json::array();
            for (auto& t : h.stream.trackers) tr.push(t);
            s.set("url", h.stream.url).set("infoHash", h.stream.infoHash).set("title", h.stream.title);
            s.set("addon", h.stream.addonName).set("quality", h.stream.quality).set("source", h.stream.source);
            s.set("fileIdx", h.stream.fileIdx).set("trackers", tr);
            e.set("stream", s);
        }
        e.set("position", h.position).set("duration", h.duration).set("updated", (double)h.updated);
        arr.push(e);
    }
    Json gone = Json::array();
    for (auto& kv : removed) {
        Json r = Json::object();
        r.set("metaId", kv.first).set("at", (double)kv.second);
        gone.push(r);
    }
    Json j = Json::object();
    j.set("entries", arr).set("removed", gone);
    std::string text = j.dump();
    if (!writeFile(kHistoryPath, text.data(), text.size())) logMsg("failed to save history");
}

void History::record(const PlayContext& ctx, double position, double duration) {
    if (ctx.metaId.empty() || ctx.videoId.empty() || duration <= 0) return;
    removed.erase(ctx.metaId);  // watched again: it belongs in the lists again
    HistoryEntry h;
    static_cast<PlayContext&>(h) = ctx;
    h.position = position;
    h.duration = duration;
    h.updated = (int64_t)time(nullptr);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const HistoryEntry& e) { return e.videoId == ctx.videoId; }),
                  entries.end());
    entries.insert(entries.begin(), h);
    if (entries.size() > kMaxEntries) entries.resize(kMaxEntries);
    save();
}

void History::mergeAccount(const std::vector<HistoryEntry>& items) {
    for (auto& a : items) {
        auto gone = removed.find(a.metaId);
        if (gone != removed.end() && a.updated <= gone->second) continue;  // removed by hand, not watched since
        auto it = std::find_if(entries.begin(), entries.end(),
                               [&](const HistoryEntry& e) { return e.videoId == a.videoId; });
        if (it == entries.end()) {
            entries.push_back(a);
        } else if (a.updated > it->updated) {
            it->position = a.position;
            it->duration = a.duration;
            it->updated = a.updated;
        }
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const HistoryEntry& x, const HistoryEntry& y) { return x.updated > y.updated; });
    if (entries.size() > kMaxEntries) entries.resize(kMaxEntries);
}

void History::removeTitle(const std::string& metaId) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const HistoryEntry& e) { return e.metaId == metaId; }),
                  entries.end());
    removed[metaId] = (int64_t)time(nullptr);
    save();
}

const HistoryEntry* History::findVideo(const std::string& videoId) const {
    for (auto& e : entries)
        if (e.videoId == videoId) return &e;
    return nullptr;
}

const HistoryEntry* History::resumePoint(const PlayContext& ctx) const {
    const HistoryEntry* h = findVideo(ctx.videoId);
    const SavedStream& s = ctx.stream;
    for (size_t i = 0; !h && s.valid() && i < entries.size(); i++) {
        const SavedStream& e = entries[i].stream;
        if (!s.url.empty() ? e.url == s.url : e.infoHash == s.infoHash && e.fileIdx == s.fileIdx) h = &entries[i];
    }
    return h && h->position > 30 && !h->finished() ? h : nullptr;
}

const HistoryEntry* History::latestForMeta(const std::string& metaId) const {
    const HistoryEntry* best = nullptr;
    for (auto& e : entries)
        if (e.metaId == metaId && (!best || e.updated > best->updated)) best = &e;
    return best;
}

std::vector<HistoryEntry> History::recentTitles(size_t max) const {
    std::vector<HistoryEntry> out;
    std::set<std::string> seen;
    for (auto& e : entries) {  // entries are kept newest first
        if (out.size() >= max) break;
        if (seen.insert(e.metaId).second) out.push_back(*latestForMeta(e.metaId));
    }
    return out;
}

std::vector<HistoryEntry> History::continueWatching(size_t max) const {
    std::vector<HistoryEntry> out;
    for (auto& e : recentTitles(max * 2)) {
        if (out.size() >= max) break;
        if (!e.finished() && e.position > 10) out.push_back(e);
    }
    return out;
}

int64_t parseIsoTime(const std::string& iso) {
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
    // days from civil (Howard Hinnant)
    y -= mo <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

std::string episodeLabelFromId(const std::string& videoId) {
    size_t a = videoId.find(':');
    if (a == std::string::npos) return "";
    size_t b = videoId.find(':', a + 1);
    if (b == std::string::npos) return "";
    return "S" + videoId.substr(a + 1, b - a - 1) + ":E" + videoId.substr(b + 1);
}

std::string guessNextEpisode(const std::string& videoId, bool nextSeason) {
    size_t a = videoId.find(':');
    if (a == std::string::npos) return "";
    size_t b = videoId.find(':', a + 1);
    if (b == std::string::npos) return "";
    int season = atoi(videoId.substr(a + 1, b - a - 1).c_str()), episode = atoi(videoId.substr(b + 1).c_str());
    if (nextSeason) return videoId.substr(0, a) + ":" + std::to_string(season + 1) + ":1";
    return videoId.substr(0, a) + ":" + std::to_string(season) + ":" + std::to_string(episode + 1);
}
