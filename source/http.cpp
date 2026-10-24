#include "http.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <thread>

#include "log.hpp"

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::atomic<int> g_foreground{0};
static std::atomic<int64_t> g_foregroundDone{-100000};  // ms

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool httpForegroundBusy() {
    return g_foreground > 0 || nowMs() - g_foregroundDone < 1500;  // the gap between two requests too
}

void httpInit() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static bool perform(const std::string& url, const std::string* body, std::string& out,
                    std::string& err, long timeoutSec, bool background) {
    struct Foreground {  // counts the request while it runs
        bool on;
        explicit Foreground(bool o) : on(o) {
            if (on) g_foreground++;
        }
        ~Foreground() {
            if (!on) return;
            g_foregroundDone = nowMs();
            g_foreground--;
        }
    } fg(!background);
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
    curl_easy_setopt(c, CURLOPT_USERAGENT, "StremNX/0.1 (Nintendo Switch)");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body->size());
    }

    CURLcode rc;
    for (int attempt = 0;; attempt++) {
        rc = curl_easy_perform(c);
        // A failed connect is often momentary (socket memory busy with torrent peers that are just
        // stepping aside, Wi-Fi waking up): try again before reporting it.
        if (rc != CURLE_COULDNT_CONNECT || attempt == 2) break;
        out.clear();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
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

bool httpGet(const std::string& url, std::string& out, std::string& err, long timeoutSec, bool background) {
    return perform(url, nullptr, out, err, timeoutSec, background);
}

bool httpPostJson(const std::string& url, const std::string& body, std::string& out, std::string& err) {
    return perform(url, &body, out, err, 30, false);
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
