#pragma once

#include <string>

class App;

// On-screen keyboard drawn by the app (the system one is a separate applet where touch and the gyro
// pointer don't reach). Blocks until the viewer confirms (true) or cancels (false).
bool showKeyboard(App& app, std::string& out, const std::string& guide, const std::string& initial = "",
                  bool password = false);
