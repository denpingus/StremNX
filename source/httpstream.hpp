#pragma once

#include <mpv/stream_cb.h>

#include <string>

// HTTP(S) streams for mpv through libcurl. The FFmpeg in devkitPro's portlibs has no "https"
// protocol, so mpv cannot open debrid links (Real-Debrid, AllDebrid...) or any https:// URL on its
// own. Remote URLs are handed to mpv as "nxhttp://<original url>" and read here instead, with
// range requests for seeking.

#define HTTPSTREAM_PROTO "nxhttp"

// "https://x" -> "nxhttp://https://x"; anything else is returned unchanged.
std::string httpStreamUrl(const std::string& url);

// mpv_stream_cb_open_ro_fn for HTTPSTREAM_PROTO. userData: a const char* user agent (may be null).
int httpStreamOpen(void* userData, char* uri, mpv_stream_cb_info* info);
