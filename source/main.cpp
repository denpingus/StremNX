#include <switch.h>

#include "app.hpp"
#include "updater.hpp"

int main(int argc, char** argv) {
    if (argc > 0) updaterSetSelfPath(argv[0]);
    socketInitializeDefault();
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
    socketExit();
    return 0;
}
