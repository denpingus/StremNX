#include "updater.hpp"

#include <curl/curl.h>
#include <switch.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "http.hpp"
#include "json.hpp"
#include "log.hpp"
#include "thread.hpp"

namespace {

std::string selfPath;

const char* kDefaultNro = "sdmc:/switch/stremnx/stremnx.nro";
const char* kPartPath = APP_DIR "/update.nro.part";

bool endsWithNro(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".nro") == 0;
}

std::string targetPath() {
    if (!selfPath.empty() && endsWithNro(selfPath)) return selfPath;
    return kDefaultNro;
}

size_t fileWrite(char* p, size_t size, size_t n, void* ud) {
    return fwrite(p, size, n, static_cast<FILE*>(ud)) * size;
}

int progressCb(void* ud, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
    auto* d = static_cast<UpdateDownload*>(ud);
    if (dlTotal > 0) d->total = (int64_t)dlTotal;
    d->done = (int64_t)dlNow;
    return 0;
}

// An .nro has "NRO0" at offset 0x10.
bool looksLikeNro(const char* path, int64_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char head[0x14] = {};
    bool ok = fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head + 0x10, "NRO0", 4) == 0;
    fseek(f, 0, SEEK_END);
    size = (int64_t)ftell(f);
    fclose(f);
    return ok;
}

bool download(UpdateDownload& d, const std::string& url, std::string& err) {
    FILE* f = fopen(kPartPath, "wb");
    if (!f) {
        err = "cannot write to the SD card";
        return false;
    }
    CURL* c = curl_easy_init();
    if (!c) {
        fclose(f);
        err = "curl_easy_init failed";
        return false;
    }
    std::string ua = "StremNX/" APP_VERSION " (Nintendo Switch)";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);  // GitHub redirects assets to its CDN
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fileWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &d);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    bool closed = fclose(f) == 0;
    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        return false;
    }
    if (!closed) {
        err = "the SD card is full or failed";
        return false;
    }
    return true;
}

// FAT cannot rename over an existing file: move the old one aside first, and put it back on failure.
bool replaceFile(const std::string& from, const std::string& to, std::string& err) {
    std::string bak = to + ".old";
    remove(bak.c_str());
    bool hadOld = fileExists(to);
    if (hadOld && rename(to.c_str(), bak.c_str()) != 0) {
        err = "cannot move the old version aside";
        return false;
    }
    if (rename(from.c_str(), to.c_str()) != 0) {
        if (hadOld) rename(bak.c_str(), to.c_str());
        err = "cannot put the new version in place";
        return false;
    }
    if (hadOld) remove(bak.c_str());
    return true;
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    const char* p = a.c_str();
    const char* q = b.c_str();
    if (*p == 'v' || *p == 'V') p++;
    if (*q == 'v' || *q == 'V') q++;
    while (*p || *q) {
        long x = strtol(p, const_cast<char**>(&p), 10);
        long y = strtol(q, const_cast<char**>(&q), 10);
        if (x != y) return x < y ? -1 : 1;
        // skip to the next numeric part (".", "-beta"...)
        while (*p && !isdigit((unsigned char)*p)) p++;
        while (*q && !isdigit((unsigned char)*q)) q++;
    }
    return 0;
}

UpdateInfo checkForUpdate() {
    UpdateInfo info;
    std::string body;
    if (!httpGet("https://api.github.com/repos/" UPDATE_REPO "/releases/latest", body, info.error, 20)) {
        logMsg("update: check failed: %s", info.error.c_str());
        return info;
    }
    bool ok = false;
    Json j = Json::parse(body, &ok);
    if (!ok || !j.isObject()) {
        info.error = "invalid answer from GitHub";
        return info;
    }
    info.ok = true;
    std::string tag = j["tag_name"].str();
    info.version = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
    info.notes = j["body"].str();
    for (auto& a : j["assets"].items()) {
        if (!endsWithNro(a["name"].str())) continue;
        info.nroUrl = a["browser_download_url"].str();
        info.size = (int64_t)a["size"].num();
        break;
    }
    info.available = !info.version.empty() && !info.nroUrl.empty() && compareVersions(info.version, APP_VERSION) > 0;
    logMsg("update: latest release %s (running %s)%s", info.version.c_str(), APP_VERSION,
           info.available ? ", update available" : "");
    return info;
}

std::shared_ptr<UpdateDownload> startUpdateDownload(const UpdateInfo& info) {
    auto d = std::make_shared<UpdateDownload>();
    d->total = info.size;
    d->path = targetPath();
    std::string url = info.nroUrl, version = info.version;
    int64_t expected = info.size;
    startThread([d, url, version, expected]() {
        std::string err;
        logMsg("update: downloading %s to %s", version.c_str(), d->path.c_str());
        bool ok = download(*d, url, err);
        int64_t size = 0;
        if (ok && (!looksLikeNro(kPartPath, size) || (expected > 0 && size != expected))) {
            ok = false;
            err = "the downloaded file is incomplete or not an .nro";
        }
        if (ok) ok = replaceFile(kPartPath, d->path, err);
        if (!ok) remove(kPartPath);
        logMsg("update: %s%s", ok ? "installed " : "failed: ", ok ? version.c_str() : err.c_str());
        d->error = err;
        d->ok = ok;
        d->finished = true;
    });
    return d;
}

void updaterSetSelfPath(const char* argv0) {
    if (!argv0 || !*argv0) return;
    selfPath = argv0;
    if (selfPath[0] == '/') selfPath = "sdmc:" + selfPath;  // some loaders leave the device out
}

std::string updaterSelfPath() {
    return targetPath();
}

bool updaterRelaunch(const std::string& nro) {
    if (!envHasNextLoad()) return false;
    std::string args = "\"" + nro + "\"";
    return R_SUCCEEDED(envSetNextLoad(nro.c_str(), args.c_str()));
}
