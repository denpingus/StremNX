#include <switch.h>

#include "app.hpp"

int main(int argc, char** argv) {
    // Blocking socket calls each hold one of these sessions. With the default 3, tracker requests (up to
    // 8 s each) left the torrent engine, the catalogs and the artwork waiting in line: frozen screens
    // and rows that never load. 16 is the libnx maximum; more buffers serve the torrent peers.
    SocketInitConfig sock = *socketGetDefaultInitConfig();
    sock.num_bsd_sessions = 16;
    sock.sb_efficiency = 8;
    socketInitialize(&sock);
    nxlinkStdio();  // logs to the PC when launched with `nxlink -s`
    plInitialize(PlServiceType_User);
    romfsInit();  // romfs:/logo.png

    {
        App app;
        if (app.init()) app.run();
        app.shutdown();
    }

    romfsExit();
    plExit();
    return 0;  // no socketExit(): see App::shutdown
}
