#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// In-app updates from the GitHub releases of the project: the latest release's tag is compared with
// APP_VERSION and its .nro asset replaces the running .nro on the SD card.

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#define UPDATE_REPO "denpingus/StremNX"

struct UpdateInfo {
    bool ok = false;         // the check worked
    bool available = false;  // a newer version with a .nro asset exists
    std::string version;     // "0.3.3"
    std::string notes;       // release text
    std::string nroUrl;
    int64_t size = 0;
    std::string error;
};

// Blocking: run it on a background thread.
UpdateInfo checkForUpdate();

struct UpdateDownload {
    std::atomic<int64_t> done{0}, total{0};
    std::atomic<bool> finished{false}, ok{false};
    std::string error;  // valid once finished
    std::string path;   // the .nro that was replaced
};

// Downloads the .nro and replaces the running one, on a background thread.
std::shared_ptr<UpdateDownload> startUpdateDownload(const UpdateInfo& info);

// argv[0] from main(): where the running .nro lives.
void updaterSetSelfPath(const char* argv0);
std::string updaterSelfPath();

// Asks the homebrew loader to start `nro` after the app exits. False when the app was not started by
// it (then the viewer has to reopen StremNX by hand).
bool updaterRelaunch(const std::string& nro);

// "1.2.10" > "1.2.9"; a leading "v" is ignored.
int compareVersions(const std::string& a, const std::string& b);
