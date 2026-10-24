#pragma once

#include <string>

void httpInit();

// Return true on HTTP 2xx. On failure, `err` gets a short description.
// background: the request is not something the viewer waits for (torrent trackers).
bool httpGet(const std::string& url, std::string& out, std::string& err, long timeoutSec = 30,
             bool background = false);
// The app is fetching something for the viewer (catalogs, details, artwork...), or just was: background
// downloads step aside meanwhile.
bool httpForegroundBusy();
bool httpPostJson(const std::string& url, const std::string& body, std::string& out, std::string& err);

std::string urlEncode(const std::string& s);
