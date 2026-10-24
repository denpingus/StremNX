#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>

static std::mutex g_logMutex;
static FILE* g_log = nullptr;

void logOpen(const char* path) {
    g_log = fopen(path, "w");
}

void logClose() {
    std::lock_guard<std::mutex> lock(g_logMutex);  // other threads may be logging right now
    if (g_log) fclose(g_log);
    g_log = nullptr;
}

void logMsg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool writeFile(const std::string& path, const void* data, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

void ensureDir(const std::string& path) {
    mkdir(path.c_str(), 0777);
}

// ---------------------------------------------------------------- watchdog

#include <atomic>
#include <chrono>
#include <thread>

#include <SDL2/SDL.h>

#include "thread.hpp"

namespace {
struct WatchState {
    std::atomic<uint32_t> beat{0};
    std::atomic<const char*> stage{"start"};
    std::atomic<bool> idle{true};
};
WatchState g_watch[WatchSlots];
const char* const kSlotNames[WatchSlots] = {"main thread", "torrent engine"};
}  // namespace

void watchBeat(int slot, const char* stage) {
    g_watch[slot].stage = stage;
    g_watch[slot].beat = SDL_GetTicks();
    g_watch[slot].idle = false;
}

void watchIdle(int slot) {
    g_watch[slot].idle = true;
}

void watchdogStart() {
    startThread(
        []() {
            bool reported[WatchSlots] = {};
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                uint32_t now = SDL_GetTicks();
                for (int s = 0; s < WatchSlots; s++) {
                    WatchState& w = g_watch[s];
                    uint32_t last = w.beat;
                    bool stuck = !w.idle && last != 0 && now - last > 4000;
                    if (stuck && !reported[s]) {
                        logMsg("WATCHDOG: %s stuck for %u ms at stage '%s'", kSlotNames[s], now - last, (const char*)w.stage);
                        reported[s] = true;
                    } else if (!stuck && reported[s]) {
                        logMsg("WATCHDOG: %s recovered (stage '%s')", kSlotNames[s], (const char*)w.stage);
                        reported[s] = false;
                    }
                }
            }
        },
        64 * 1024);
}
