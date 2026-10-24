#pragma once

#include <string>
#include <vector>

#include "history.hpp"
#include "json.hpp"

// Stremio addon protocol:
// https://github.com/Stremio/stremio-addon-sdk/blob/master/docs/protocol.md

#define CINEMETA_URL "https://v3-cinemeta.strem.io/manifest.json"
#define OPENSUBTITLES_URL "https://opensubtitles-v3.strem.io/manifest.json"  // official subtitles addon
#define STREMIO_API "https://api.strem.io/api"

struct Addon {
    std::string transportUrl;  // .../manifest.json
    std::string base;          // transportUrl without /manifest.json
    std::string id, name;
    Json manifest;
};

struct CatalogInfo {
    int addon = 0;  // index into App::addons
    std::string type, id, name;
    bool search = false;    // supports the "search" extra
    bool skip = false;      // supports "skip" pagination
    bool required = false;  // needs some extra (not usable on the home screen)
    std::vector<std::string> genres;  // options of the "genre" extra, if supported
};

struct MetaItem {
    std::string id, type, name, poster, releaseInfo;
    std::string background, logo, description, imdbRating;
    std::string posterShape;  // "poster" (2:3, default), "landscape" (16:9) or "square", as the addon says
    std::vector<std::string> genres;
};

struct Video {
    std::string id, title, released, thumbnail, overview;
    int season = 0, episode = 0;
};

struct Meta {
    std::string id, type, name, poster, background, logo, description;
    std::string releaseInfo, runtime, genres, imdbRating;  // genres: display text
    std::string cast, director;  // display text
    std::vector<std::string> genreList;
    std::vector<Video> videos;
};

struct Stream {
    std::string addonName, name, title, url, infoHash;
    int fileIdx = -1;
    std::vector<std::string> trackers;  // from "sources": ["tracker:udp://...", "dht:..."]
};

struct AddonEntry {
    std::string url;
    Json manifest;  // may come ready-made from the account's collection
};

std::string normalizeAddonUrl(const std::string& url);
bool addonFromManifest(const std::string& url, const Json& manifest, Addon& out);
bool fetchManifest(const std::string& url, Addon& out, std::string& err);
bool addonSupports(const Addon& a, const std::string& resource, const std::string& type, const std::string& id);
std::vector<CatalogInfo> listCatalogs(const std::vector<Addon>& addons);

std::vector<MetaItem> fetchCatalog(const Addon& a, const CatalogInfo& c, const std::string& extra, std::string& err);
bool fetchMeta(const Addon& a, const std::string& type, const std::string& id, Meta& out, std::string& err);
std::vector<Stream> fetchStreams(const Addon& a, const std::string& type, const std::string& id, std::string& err);

// URL for mpv. Torrents need a stremio-server (e.g. http://192.168.0.10:11470).
std::string streamPlayUrl(const Stream& s, const std::string& server);

bool apiLogin(const std::string& email, const std::string& password, std::string& authKey, std::string& err);
bool apiAddonCollection(const std::string& authKey, std::vector<AddonEntry>& out, std::string& err);

// Account library (Stremio "libraryItem" collection): raw items, progress, and updates.
bool apiDatastoreGet(const std::string& authKey, Json& items, std::string& err);
bool apiDatastorePut(const std::string& authKey, const Json& item, std::string& err);
std::vector<HistoryEntry> progressFromLibrary(const Json& items);
std::vector<MetaItem> libraryFromItems(const Json& items);  // titles added to the library
// New or updated library item; keeps the fields of `existing` that we do not know about.
Json libraryItem(const Json* existing, const std::string& id, const std::string& type, const std::string& name,
                 const std::string& poster);
std::string isoTime(int64_t unixTime);

// ---------------------------------------------------------------- subtitles

struct SubtitleTrack {
    std::string id, url, lang, addonName;
};

std::vector<SubtitleTrack> fetchSubtitles(const Addon& a, const std::string& type, const std::string& id,
                                          std::string& err);

struct SubLanguage {
    const char* code;   // stored in the config ("" = subtitles off)
    const char* label;
    std::vector<std::string> match;  // language codes addons and files use for it
};
const std::vector<SubLanguage>& subLanguages();
const SubLanguage& subLanguage(const std::string& code);
bool subLangMatches(const SubLanguage& l, const std::string& lang);
