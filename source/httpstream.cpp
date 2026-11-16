#include "httpstream.hpp"

#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "log.hpp"

namespace {

const size_t kPrefixLen = sizeof(HTTPSTREAM_PROTO "://") - 1;

struct HttpStream {
    std::string url;  // final URL after redirects (debrid "resolve" links redirect to the file)
    std::string ua;
    int64_t size = -1;
    bool ranges = false;

    int64_t pos = 0;  // offset of the next byte handed to mpv
    std::atomic<bool> cancel{false};

    CURLM* multi = nullptr;
    CURL* easy = nullptr;
    std::string buf;
    size_t bufOff = 0;
    bool done = false, badRange = false;
    CURLcode result = CURLE_OK;
    int retries = 0;
};

std::string hostOf(const std::string& url) {
    size_t a = url.find("://");
    a = a == std::string::npos ? 0 : a + 3;
    size_t b = url.find_first_of("/?#", a);
    return url.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

void commonOpts(CURL* c, const std::string& url, const std::string& ua) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
}

// ---------------------------------------------------------------- probe

struct Probe {
    int64_t total = -1;  // from Content-Range
    bool gotBody = false;
};

size_t probeHeader(char* p, size_t size, size_t n, void* ud) {
    auto* pr = static_cast<Probe*>(ud);
    size_t len = size * n;
    std::string line(p, len);
    if (line.compare(0, 5, "HTTP/") == 0) pr->total = -1;  // a new response (after a redirect)
    std::string low = line;
    for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
    if (low.compare(0, 14, "content-range:") == 0) {
        size_t slash = line.rfind('/');
        if (slash != std::string::npos && slash + 1 < line.size() && line[slash + 1] != '*')
            pr->total = strtoll(line.c_str() + slash + 1, nullptr, 10);
    }
    return len;
}

size_t probeWrite(char*, size_t size, size_t n, void* ud) {
    auto* pr = static_cast<Probe*>(ud);
    pr->gotBody = true;
    // A one-byte range answers 206 with one byte; a server without ranges sends the whole file
    // (200), so stop it here: the status and length are already known.
    return size * n <= 1 ? size * n : 0;
}

// Follows redirects and finds out the size and whether ranges (seeking) work.
bool probe(HttpStream& s, const std::string& url, std::string& err) {
    CURL* c = curl_easy_init();
    if (!c) {
        err = "curl_easy_init failed";
        return false;
    }
    Probe pr;
    commonOpts(c, url, s.ua);
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probeHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &pr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probeWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &pr);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    char* eff = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    curl_off_t len = -1;
    curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    bool ok = false;
    if (code == 206) {
        ok = rc == CURLE_OK || rc == CURLE_WRITE_ERROR;
        s.ranges = pr.total > 0;
        s.size = pr.total;
    } else if (code == 200 && (rc == CURLE_OK || rc == CURLE_WRITE_ERROR)) {
        ok = true;
        s.ranges = false;
        s.size = len > 0 ? (int64_t)len : -1;
    }
    if (ok) {
        s.url = eff ? eff : url;
    } else {
        err = code >= 400 ? "HTTP " + std::to_string(code) : curl_easy_strerror(rc);
    }
    curl_easy_cleanup(c);
    return ok;
}

// ---------------------------------------------------------------- transfer

size_t streamWrite(char* p, size_t size, size_t n, void* ud) {
    auto* s = static_cast<HttpStream*>(ud);
    if (s->cancel) return 0;
    if (s->buf.empty() && s->pos > 0) {  // first data of a range request: it must really be a range
        long code = 0;
        curl_easy_getinfo(s->easy, CURLINFO_RESPONSE_CODE, &code);
        if (code != 206) {
            s->badRange = true;
            return 0;
        }
    }
    s->buf.append(p, size * n);
    return size * n;
}

void stop(HttpStream& s) {
    if (s.easy) {
        curl_multi_remove_handle(s.multi, s.easy);
        curl_easy_cleanup(s.easy);
        s.easy = nullptr;
    }
    s.buf.clear();
    s.bufOff = 0;
    s.done = false;
    s.badRange = false;
    s.result = CURLE_OK;
}

bool start(HttpStream& s) {
    stop(s);
    s.easy = curl_easy_init();
    if (!s.easy) return false;
    commonOpts(s.easy, s.url, s.ua);
    curl_easy_setopt(s.easy, CURLOPT_BUFFERSIZE, 256L * 1024);
    // A link that stops sending data for 30 s is dropped and reopened where it stopped.
    curl_easy_setopt(s.easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(s.easy, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(s.easy, CURLOPT_WRITEFUNCTION, streamWrite);
    curl_easy_setopt(s.easy, CURLOPT_WRITEDATA, &s);
    if (s.pos > 0) {
        std::string range = std::to_string(s.pos) + "-";
        curl_easy_setopt(s.easy, CURLOPT_RANGE, range.c_str());  // copied by curl
    }
    curl_multi_add_handle(s.multi, s.easy);
    return true;
}

int64_t streamRead(void* cookie, char* out, uint64_t n) {
    auto* s = static_cast<HttpStream*>(cookie);
    for (;;) {
        if (s->cancel) return -1;
        if (s->bufOff < s->buf.size()) {
            size_t k = std::min<size_t>((size_t)n, s->buf.size() - s->bufOff);
            memcpy(out, s->buf.data() + s->bufOff, k);
            s->bufOff += k;
            s->pos += (int64_t)k;
            if (s->bufOff == s->buf.size()) {
                s->buf.clear();
                s->bufOff = 0;
            }
            s->retries = 0;
            return (int64_t)k;
        }
        if (s->size >= 0 && s->pos >= s->size) return 0;
        if (!s->easy && !start(*s)) return -1;

        if (s->done) {
            bool complete = s->result == CURLE_OK && !s->badRange;
            if (complete && s->size < 0) return 0;  // unknown length: the server closing is the end
            // Error, or the connection closed early: reopen at the current position.
            bool canResume = s->pos == 0 || s->ranges;
            if (!canResume || s->retries >= 4) {
                logMsg("httpstream: %s at %lld, giving up", s->badRange ? "range ignored" : curl_easy_strerror(s->result),
                       (long long)s->pos);
                return -1;
            }
            s->retries++;
            logMsg("httpstream: %s at %lld, reconnecting (%d)", complete ? "closed early" : curl_easy_strerror(s->result),
                   (long long)s->pos, s->retries);
            for (int i = 0; i < 5 * s->retries && !s->cancel; i++) svcSleepThread(100'000'000LL);
            stop(*s);
            continue;
        }

        int running = 0;
        curl_multi_perform(s->multi, &running);
        int left = 0;
        while (CURLMsg* m = curl_multi_info_read(s->multi, &left))
            if (m->msg == CURLMSG_DONE && m->easy_handle == s->easy) {
                s->done = true;
                s->result = m->data.result;
            }
        if (s->buf.empty() && !s->done) curl_multi_poll(s->multi, nullptr, 0, 200, nullptr);
    }
}

int64_t streamSeek(void* cookie, int64_t off) {
    auto* s = static_cast<HttpStream*>(cookie);
    if (off < 0 || (s->size >= 0 && off > s->size)) return MPV_ERROR_GENERIC;
    if (off == s->pos) return off;
    // Short jump forward inside what is already buffered.
    if (off > s->pos && off - s->pos <= (int64_t)(s->buf.size() - s->bufOff)) {
        s->bufOff += (size_t)(off - s->pos);
        s->pos = off;
        return off;
    }
    stop(*s);
    s->pos = off;
    return off;
}

int64_t streamSize(void* cookie) {
    auto* s = static_cast<HttpStream*>(cookie);
    return s->size >= 0 ? s->size : MPV_ERROR_UNSUPPORTED;
}

void streamClose(void* cookie) {
    auto* s = static_cast<HttpStream*>(cookie);
    stop(*s);
    if (s->multi) curl_multi_cleanup(s->multi);
    delete s;
}

void streamCancel(void* cookie) {
    static_cast<HttpStream*>(cookie)->cancel = true;
}

}  // namespace

std::string httpStreamUrl(const std::string& url) {
    if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0)
        return HTTPSTREAM_PROTO "://" + url;
    return url;
}

int httpStreamOpen(void* userData, char* uri, mpv_stream_cb_info* info) {
    if (strncmp(uri, HTTPSTREAM_PROTO "://", kPrefixLen) != 0) return MPV_ERROR_LOADING_FAILED;
    std::string url = uri + kPrefixLen;

    auto* s = new HttpStream;
    s->ua = userData ? static_cast<const char*>(userData) : "StremNX";
    std::string err;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {  // DNS / TLS hiccups right after wake-up
        if (attempt) svcSleepThread(700'000'000LL * attempt);
        ok = probe(*s, url, err);
        if (!ok && err.compare(0, 5, "HTTP ") == 0) break;  // the server said no; retrying won't help
    }
    if (!ok) {
        logMsg("httpstream: cannot open %s: %s", hostOf(url).c_str(), err.c_str());
        delete s;
        return MPV_ERROR_LOADING_FAILED;
    }
    s->multi = curl_multi_init();
    if (!s->multi) {
        delete s;
        return MPV_ERROR_LOADING_FAILED;
    }
    logMsg("httpstream: %s, %lld bytes, %s", hostOf(s->url).c_str(), (long long)s->size,
           s->ranges ? "seekable" : "not seekable");

    info->cookie = s;
    info->read_fn = streamRead;
    info->seek_fn = s->ranges ? streamSeek : nullptr;
    info->size_fn = streamSize;
    info->close_fn = streamClose;
    info->cancel_fn = streamCancel;
    return 0;
}
