#pragma once

#include <switch.h>

#include <cstdint>

enum Key : uint32_t {
    K_UP = 1 << 0,
    K_DOWN = 1 << 1,
    K_LEFT = 1 << 2,
    K_RIGHT = 1 << 3,
    K_A = 1 << 4,
    K_B = 1 << 5,
    K_X = 1 << 6,
    K_Y = 1 << 7,
    K_L = 1 << 8,
    K_R = 1 << 9,
    K_ZL = 1 << 10,
    K_ZR = 1 << 11,
    K_PLUS = 1 << 12,
    K_MINUS = 1 << 13,
    K_LSTICK = 1 << 14,  // left stick click
};

// Controller input (D-pad and left stick are equivalent), with repeat while held.
// Touch screen: swipes act like the D-pad, a short tap is reported through tap().
class Input {
public:
    void init();
    void update();
    bool pressed(uint32_t k) const { return (pressed_ & k) != 0; }
    bool held(uint32_t k) const { return (held_ & k) != 0; }
    // The physical D-pad was pressed this frame (K_UP etc. also come from the sticks).
    bool dpadPressed() const { return dpad_; }
    // A tap that ended this frame (screen coordinates, 1280x720).
    bool tap(int& x, int& y) const {
        if (!tapNow) return false;
        x = tapX;
        y = tapY;
        return true;
    }
    bool touching() const { return tDown; }
    // Finger on the screen: where it is now and where the touch started.
    bool touch(int& x, int& y, int& startX, int& startY) const {
        if (!tDown) return false;
        x = lx;
        y = ly;
        startX = sx;
        startY = sy;
        return true;
    }
    bool dragKeys = true;  // swipes act like the D-pad (off where a drag means something else)

    // Gyro pointer, like a TV "magic remote" (Pro Controller or the Joy-Con pair, not in handheld):
    // moving the controller shows a cursor and A taps where it points. The D-pad, the stick or 4 s
    // without moving hide it, and A goes back to the focused item.
    bool pointer(int& x, int& y) const {
        x = (int)px;
        y = (int)py;
        return pointerOn;
    }
    bool pointerEnabled = true;  // off in the player

private:
    PadState pad;
    uint32_t pressed_ = 0, held_ = 0;
    bool dpad_ = false;
    uint32_t repeatStart[4] = {}, repeatLast[4] = {};
    bool tDown = false, tMoved = false, tapNow = false;
    int sx = 0, sy = 0, lx = 0, ly = 0, accX = 0, accY = 0, tapX = 0, tapY = 0;
    uint32_t tStart = 0;
    HidSixAxisSensorHandle gyro[3] = {};  // Pro Controller, left Joy-Con, right Joy-Con
    bool pointerOn = false;
    float px = 640, py = 360;
    uint32_t pointerMoved = 0, lastGyro = 0;
    void updatePointer(uint32_t now);
};
