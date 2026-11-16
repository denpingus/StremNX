#include "http.hpp"

#include <curl/curl.h>

#include <cctype>

#include "log.hpp"

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

void httpInit() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void httpCleanup() {
    curl_global_cleanup();
}

static bool perform(const std::string& url, const std::string* body, std::string& out,
                    std::string& err, long timeoutSec) {
    out.clear();
    CURL* c = curl_easy_init();
    if (!c) {
        err = "curl_easy_init failed";
        return false;
    }
    struct curl_slist* headers = nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "StremNX/" APP_VERSION " (Nintendo Switch)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body->size());
    }

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        logMsg("HTTP error %s: %s", url.c_str(), err.c_str());
        return false;
    }
    if (code < 200 || code >= 300) {
        err = "HTTP " + std::to_string(code);
        logMsg("HTTP %ld on %s", code, url.c_str());
        return false;
    }
    return true;
}

bool httpGet(const std::string& url, std::string& out, std::string& err, long timeoutSec) {
    return perform(url, nullptr, out, err, timeoutSec);
}

bool httpPostJson(const std::string& url, const std::string& body, std::string& out, std::string& err) {
    return perform(url, &body, out, err, 30);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        // ':' stays literal because Stremio IDs (tt123:1:2) are used that way in routes.
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
