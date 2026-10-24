#include "stremio.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstring>

#include "http.hpp"
#include "log.hpp"

static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static bool getJson(const std::string& url, Json& out, std::string& err) {
    std::string body;
    if (!httpGet(url, body, err)) return false;
    bool ok = false;
    out = Json::parse(body, &ok);
    if (!ok) {
        err = "invalid JSON";
        logMsg("invalid JSON from %s", url.c_str());
        return false;
    }
    return true;
}

static std::string firstLine(const std::string& s) {
    size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::string normalizeAddonUrl(const std::string& url) {
    std::string u = url;
    while (!u.empty() && (u.back() == ' ' || u.back() == '/')) u.pop_back();
    if (u.compare(0, 10, "stremio://") == 0) u = "https://" + u.substr(10);
    // The link on an addon's page is often its configure page: the manifest sits next to it.
    if (endsWith(u, "/configure/manifest.json")) u.resize(u.size() - strlen("/configure/manifest.json"));
    if (endsWith(u, "/configure")) u.resize(u.size() - strlen("/configure"));
    if (!endsWith(u, "/manifest.json")) u += "/manifest.json";
    return u;
}

bool addonFromManifest(const std::string& url, const Json& manifest, Addon& out) {
    if (!manifest.isObject() || !manifest.has("id")) return false;
    out.transportUrl = normalizeAddonUrl(url);
    out.base = out.transportUrl.substr(0, out.transportUrl.size() - strlen("/manifest.json"));
    out.manifest = manifest;
    out.id = manifest["id"].str();
    out.name = manifest["name"].str(out.id);
    return true;
}

bool fetchManifest(const std::string& url, Addon& out, std::string& err) {
    Json m;
    std::string u = normalizeAddonUrl(url);
    if (!getJson(u, m, err)) return false;
    if (!addonFromManifest(u, m, out)) {
        err = "invalid manifest";
        return false;
    }
    return true;
}

static bool listHas(const Json& arr, const std::string& v) {
    for (auto& x : arr.items())
        if (x.str() == v) return true;
    return false;
}

bool addonSupports(const Addon& a, const std::string& resource, const std::string& type, const std::string& id) {
    const Json& m = a.manifest;
    for (auto& r : m["resources"].items()) {
        const Json* types = &m["types"];
        const Json* prefixes = &m["idPrefixes"];
        if (r.isString()) {
            if (r.str() != resource) continue;
        } else if (r.isObject()) {
            if (r["name"].str() != resource) continue;
            if (r.has("types")) types = &r["types"];
            if (r.has("idPrefixes")) prefixes = &r["idPrefixes"];
        } else {
            continue;
        }
        if (!listHas(*types, type)) continue;
        if (!prefixes->isArray() || prefixes->size() == 0) return true;
        for (auto& p : prefixes->items()) {
            const std::string pre = p.str();
            if (id.compare(0, pre.size(), pre) == 0) return true;
        }
    }
    return false;
}

std::vector<CatalogInfo> listCatalogs(const std::vector<Addon>& addons) {
    std::vector<CatalogInfo> out;
    for (size_t i = 0; i < addons.size(); i++) {
        for (auto& c : addons[i].manifest["catalogs"].items()) {
            CatalogInfo ci;
            ci.addon = (int)i;
            ci.type = c["type"].str();
            ci.id = c["id"].str();
            ci.name = c["name"].str(ci.id);
            // New format: extra: [{name, isRequired}]
            for (auto& e : c["extra"].items()) {
                std::string n = e["name"].str();
                if (n == "search") ci.search = true;
                if (n == "skip") ci.skip = true;
                if (n == "genre")
                    for (auto& o : e["options"].items()) ci.genres.push_back(o.str());
                if (e["isRequired"].boolean()) ci.required = true;
            }
            for (auto& o : c["genres"].items()) ci.genres.push_back(o.str());  // old format
            // Old format: extraSupported / extraRequired
            if (listHas(c["extraSupported"], "search")) ci.search = true;
            if (listHas(c["extraSupported"], "skip")) ci.skip = true;
            if (c["extraRequired"].size() > 0) ci.required = true;
            if (!ci.type.empty() && !ci.id.empty()) out.push_back(ci);
        }
    }
    return out;
}

// Cinemeta-style IDs have artwork on metahub even when the addon doesn't send it.
static std::string metahub(const std::string& kind, const std::string& id) {
    if (id.compare(0, 2, "tt") != 0) return "";
    return "https://images.metahub.space/" + kind + "/medium/" + id + "/img";
}

static std::vector<std::string> genreList(const Json& m) {
    std::vector<std::string> out;
    const Json& g = m.has("genres") ? m["genres"] : m["genre"];
    for (auto& x : g.items())
        if (!x.str().empty()) out.push_back(x.str());
    return out;
}

static MetaItem parseMetaItem(const Json& j) {
    MetaItem m;
    m.id = j["id"].str();
    m.type = j["type"].str();
    m.name = j["name"].str();
    m.poster = j["poster"].str(metahub("poster", m.id));
    m.background = j["background"].str(metahub("background", m.id));
    m.logo = j["logo"].str(metahub("logo", m.id));
    m.description = j["description"].str();
    m.imdbRating = j["imdbRating"].str();
    m.releaseInfo = j["releaseInfo"].str(j["year"].str());
    m.genres = genreList(j);
    m.posterShape = j["posterShape"].str();
    return m;
}

std::vector<MetaItem> fetchCatalog(const Addon& a, const CatalogInfo& c, const std::string& extra, std::string& err) {
    std::vector<MetaItem> out;
    std::string url = a.base + "/catalog/" + urlEncode(c.type) + "/" + urlEncode(c.id);
    if (!extra.empty()) url += "/" + extra;
    url += ".json";
    Json j;
    if (!getJson(url, j, err)) return out;
    for (auto& m : j["metas"].items()) {
        MetaItem mi = parseMetaItem(m);
        if (mi.type.empty()) mi.type = c.type;
        if (!mi.id.empty()) out.push_back(mi);
    }
    return out;
}

bool fetchMeta(const Addon& a, const std::string& type, const std::string& id, Meta& out, std::string& err) {
    std::string url = a.base + "/meta/" + urlEncode(type) + "/" + urlEncode(id) + ".json";
    Json j;
    if (!getJson(url, j, err)) return false;
    const Json& m = j["meta"];
    if (!m.isObject()) {
        err = "empty meta";
        return false;
    }
    out.id = m["id"].str(id);
    out.type = m["type"].str(type);
    out.name = m["name"].str();
    out.poster = m["poster"].str(metahub("poster", out.id));
    out.background = m["background"].str(metahub("background", out.id));
    out.logo = m["logo"].str(metahub("logo", out.id));
    out.description = m["description"].str();
    out.releaseInfo = m["releaseInfo"].str(m["year"].str());
    out.runtime = m["runtime"].str();
    out.imdbRating = m["imdbRating"].str();

    out.genreList = genreList(m);
    auto names = [](const Json& v, size_t max) {
        std::string s;
        if (v.isString()) return v.str();
        for (size_t i = 0; i < v.size() && i < max; i++) s += (i ? ", " : "") + v[i].str();
        return s;
    };
    out.cast = names(m["cast"], 4);
    out.director = names(m["director"], 2);
    for (size_t i = 0; i < out.genreList.size() && i < 3; i++) {
        if (i) out.genres += ", ";
        out.genres += out.genreList[i];
    }

    for (auto& v : m["videos"].items()) {
        Video vd;
        vd.id = v["id"].str();
        vd.title = v["name"].str(v["title"].str());
        vd.thumbnail = v["thumbnail"].str();
        vd.overview = v["overview"].str(v["description"].str());
        vd.season = (int)v["season"].num(0);
        vd.episode = (int)v["episode"].num(v["number"].num(0));
        std::string rel = v["released"].str();
        vd.released = rel.substr(0, std::min<size_t>(10, rel.size()));
        if (!vd.id.empty()) out.videos.push_back(vd);
    }
    return true;
}

std::vector<Stream> fetchStreams(const Addon& a, const std::string& type, const std::string& id, std::string& err) {
    std::vector<Stream> out;
    std::string url = a.base + "/stream/" + urlEncode(type) + "/" + urlEncode(id) + ".json";
    Json j;
    if (!getJson(url, j, err)) return out;
    for (auto& s : j["streams"].items()) {
        Stream st;
        st.addonName = a.name;
        st.name = s["name"].str();
        st.title = s["title"].str(s["description"].str());
        st.url = s["url"].str();
        st.infoHash = s["infoHash"].str();
        st.fileIdx = (int)s["fileIdx"].num(-1);
        for (auto& src : s["sources"].items()) {
            std::string v = src.str();
            if (v.compare(0, 8, "tracker:") == 0) st.trackers.push_back(v.substr(8));
        }
        // ytId / externalUrl can't be played here.
        if (st.url.empty() && st.infoHash.empty()) continue;
        if (st.name.empty()) st.name = firstLine(a.name);
        out.push_back(st);
    }
    return out;
}

std::string streamPlayUrl(const Stream& s, const std::string& server) {
    if (!s.url.empty()) return s.url;
    if (!s.infoHash.empty() && !server.empty()) {
        std::string b = server;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b + "/" + s.infoHash + "/" + std::to_string(s.fileIdx);
    }
    return "";
}

static bool apiCall(const std::string& method, const Json& body, Json& result, std::string& err) {
    std::string resp;
    if (!httpPostJson(std::string(STREMIO_API) + "/" + method, body.dump(), resp, err)) return false;
    bool ok = false;
    Json j = Json::parse(resp, &ok);
    if (!ok) {
        err = "invalid API response";
        return false;
    }
    if (j.has("error") && !j["error"].isNull()) {
        const Json& e = j["error"];
        err = e.isObject() ? e["message"].str("API error") : e.str("API error");
        return false;
    }
    result = j["result"];
    return true;
}

bool apiLogin(const std::string& email, const std::string& password, std::string& authKey, std::string& err) {
    Json body = Json::object();
    body.set("type", "Login").set("email", email).set("password", password).set("facebook", false);
    Json r;
    if (!apiCall("login", body, r, err)) return false;
    authKey = r["authKey"].str();
    if (authKey.empty()) {
        err = "login returned no authKey";
        return false;
    }
    return true;
}

bool apiAddonCollection(const std::string& authKey, std::vector<AddonEntry>& out, std::string& err) {
    Json body = Json::object();
    body.set("type", "AddonCollectionGet").set("authKey", authKey).set("update", true);
    Json r;
    if (!apiCall("addonCollectionGet", body, r, err)) return false;
    for (auto& a : r["addons"].items()) {
        AddonEntry e;
        e.url = a["transportUrl"].str();
        e.manifest = a["manifest"];
        if (!e.url.empty()) out.push_back(e);
    }
    return true;
}

bool apiDatastoreGet(const std::string& authKey, Json& items, std::string& err) {
    Json body = Json::object();
    body.set("authKey", authKey).set("collection", "libraryItem").set("ids", Json::array()).set("all", true);
    return apiCall("datastoreGet", body, items, err);
}

bool apiDatastorePut(const std::string& authKey, const Json& item, std::string& err) {
    Json changes = Json::array();
    changes.push(item);
    Json body = Json::object();
    body.set("authKey", authKey).set("collection", "libraryItem").set("changes", changes);
    Json r;
    return apiCall("datastorePut", body, r, err);
}

std::vector<HistoryEntry> progressFromLibrary(const Json& items) {
    std::vector<HistoryEntry> out;
    for (auto& it : items.items()) {
        // "temp" items are titles watched without adding them to the library: they count too.
        if (it["removed"].boolean() && !it["temp"].boolean()) continue;
        const Json& st = it["state"];
        std::string lastWatched = st["lastWatched"].str();
        double offset = st["timeOffset"].num() / 1000.0;
        double duration = st["duration"].num() / 1000.0;
        bool watched = st["timesWatched"].num() > 0 || st["flaggedWatched"].num() > 0 || st["flaggedWatched"].boolean();
        if (lastWatched.empty() && offset <= 0 && !watched) continue;  // in the library but never played
        if (offset <= 0 && watched) {  // Stremio resets the offset once a video is finished
            duration = duration > 0 ? duration : 1;
            offset = duration;
        }
        HistoryEntry h;
        h.metaId = it["_id"].str();
        h.type = it["type"].str();
        h.name = it["name"].str();
        h.poster = it["poster"].str(metahub("poster", h.metaId));
        h.background = metahub("background", h.metaId);
        h.logo = metahub("logo", h.metaId);
        h.videoId = st["video_id"].str();
        if (h.videoId.empty()) h.videoId = h.metaId;
        h.episodeLabel = episodeLabelFromId(h.videoId);
        h.position = offset;
        h.duration = duration;
        h.updated = parseIsoTime(lastWatched.empty() ? it["_mtime"].str() : lastWatched);
        if (!h.metaId.empty()) out.push_back(h);
    }
    logMsg("library: %zu items in the account, %zu with watch history", items.size(), out.size());
    return out;
}

std::vector<MetaItem> libraryFromItems(const Json& items) {
    std::vector<std::pair<int64_t, MetaItem>> v;
    for (auto& it : items.items()) {
        if (it["removed"].boolean() || it["temp"].boolean()) continue;
        MetaItem m;
        m.id = it["_id"].str();
        m.type = it["type"].str();
        m.name = it["name"].str();
        m.poster = it["poster"].str(metahub("poster", m.id));
        m.background = metahub("background", m.id);
        m.logo = metahub("logo", m.id);
        if (!m.id.empty()) v.emplace_back(parseIsoTime(it["_ctime"].str(it["_mtime"].str())), m);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<MetaItem> out;
    for (auto& p : v) out.push_back(p.second);
    return out;
}

std::string isoTime(int64_t t) {
    // civil from days (Howard Hinnant)
    int64_t days = t / 86400, secs = t % 86400;
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char buf[64];
    snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lld.000Z", (long long)y, m, d,
             (long long)(secs / 3600), (long long)(secs / 60 % 60), (long long)(secs % 60));
    return buf;
}

Json libraryItem(const Json* existing, const std::string& id, const std::string& type, const std::string& name,
                 const std::string& poster) {
    int64_t now = (int64_t)time(nullptr);
    Json it = existing && existing->isObject() ? *existing : Json::object();
    if (!it.has("_id")) {
        Json st = Json::object();
        st.set("lastWatched", isoTime(now)).set("timeWatched", 0).set("timeOffset", 0).set("overallTimeWatched", 0);
        st.set("timesWatched", 0).set("flaggedWatched", 0).set("duration", 0).set("video_id", Json());
        st.set("watched", Json()).set("noNotif", false);
        Json hints = Json::object();
        hints.set("defaultVideoId", Json()).set("featuredVideoId", Json()).set("hasScheduledVideos", false);
        it.set("_id", id).set("removed", true).set("temp", true).set("_ctime", isoTime(now)).set("state", st);
        it.set("behaviorHints", hints).set("posterShape", "poster");
    }
    it.set("name", name.empty() ? it["name"].str(id) : name).set("type", type.empty() ? it["type"].str() : type);
    if (!poster.empty()) it.set("poster", poster);
    it.set("_mtime", isoTime(now));
    return it;
}

// ---------------------------------------------------------------- subtitles

std::vector<SubtitleTrack> fetchSubtitles(const Addon& a, const std::string& type, const std::string& id,
                                          std::string& err) {
    std::vector<SubtitleTrack> out;
    std::string url = a.base + "/subtitles/" + urlEncode(type) + "/" + urlEncode(id) + ".json";
    Json j;
    if (!getJson(url, j, err)) return out;
    for (auto& s : j["subtitles"].items()) {
        SubtitleTrack t;
        t.id = s["id"].str();
        t.url = s["url"].str();
        t.lang = s["lang"].str();
        t.addonName = a.name;
        if (!t.url.empty()) out.push_back(t);
    }
    return out;
}

const std::vector<SubLanguage>& subLanguages() {
    static const std::vector<SubLanguage> langs = {
        {"", "Off", {}},
        {"pob", "Português (BR)", {"pob", "pt-br", "ptbr", "pt_br", "pb", "por", "pt"}},
        {"por", "Português (PT)", {"por", "pt", "pt-pt", "pob"}},
        {"eng", "English", {"eng", "en", "en-us", "en-gb"}},
        {"spa", "Español", {"spa", "es", "spl", "es-419", "es-es"}},
        {"fre", "Français", {"fre", "fra", "fr"}},
        {"ger", "Deutsch", {"ger", "deu", "de"}},
        {"ita", "Italiano", {"ita", "it"}},
        {"jpn", "日本語", {"jpn", "ja"}},
    };
    return langs;
}

const SubLanguage& subLanguage(const std::string& code) {
    for (auto& l : subLanguages())
        if (code == l.code) return l;
    return subLanguages()[0];
}

bool subLangMatches(const SubLanguage& l, const std::string& lang) {
    std::string low;
    for (char c : lang) low += (char)tolower((unsigned char)c);
    for (auto& m : l.match)
        if (low == m) return true;
    return false;
}
