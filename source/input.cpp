#include "input.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

// Gyro pointer tuning (angular velocity comes in turns per second). Calibration knobs: a sign flips an
// axis, the speed is screen pixels per full turn of the controller.
constexpr float kPointerSpeed = 13000;
constexpr float kPointerSignX = -1, kPointerSignY = -1;
constexpr float kPointerWake = 0.04f;  // movement that shows the pointer (ignores hand tremor)
// Soft dead zone: below this speed movement fades in gradually (a tenth of the speed moves a tenth as
// much, squared), so a still hand and the leftover reading after a quick move barely slide the pointer,
// while slow aiming still moves it. No hard edge where it suddenly freezes.
constexpr float kPointerSoft = 0.015f;

static uint32_t mapButtons(u64 b) {
    uint32_t k = 0;
    // Both sticks work as the D-pad, and the right stick click as -: the right Joy-Con alone
    // (or held as a remote) controls everything.
    if (b & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) k |= K_UP;
    if (b & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) k |= K_DOWN;
    if (b & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)) k |= K_LEFT;
    if (b & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)) k |= K_RIGHT;
    if (b & HidNpadButton_A) k |= K_A;
    if (b & HidNpadButton_B) k |= K_B;
    if (b & HidNpadButton_X) k |= K_X;
    if (b & HidNpadButton_Y) k |= K_Y;
    if (b & HidNpadButton_L) k |= K_L;
    if (b & HidNpadButton_R) k |= K_R;
    if (b & HidNpadButton_ZL) k |= K_ZL;
    if (b & HidNpadButton_ZR) k |= K_ZR;
    if (b & HidNpadButton_Plus) k |= K_PLUS;
    if (b & (HidNpadButton_Minus | HidNpadButton_StickR)) k |= K_MINUS;
    if (b & HidNpadButton_StickL) k |= K_LSTICK;
    return k;
}

void Input::init() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    hidGetSixAxisSensorHandles(&gyro[0], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&gyro[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    for (auto& h : gyro) hidStartSixAxisSensor(h);
}

void Input::updatePointer(uint32_t now) {
    float dt = lastGyro ? (now - lastGyro) / 1000.0f : 0;
    lastGyro = now;
    u32 style = padGetStyleSet(&pad);
    const HidSixAxisSensorHandle* g = style & HidNpadStyleTag_NpadFullKey  ? &gyro[0]
                                      : style & HidNpadStyleTag_NpadJoyDual ? &gyro[2]
                                                                            : nullptr;
    HidSixAxisSensorState s = {};
    if (!pointerEnabled || padIsHandheld(&pad) || !g || !hidGetSixAxisSensorStates(*g, &s, 1)) {
        pointerOn = false;
        return;
    }
    // Held pointing at the screen: turning left/right is the yaw (z), up/down the pitch (x).
    float vx = kPointerSignX * s.angular_velocity.z, vy = kPointerSignY * s.angular_velocity.x;
    if (std::fabs(vx) + std::fabs(vy) > kPointerWake) {
        pointerOn = true;
        pointerMoved = now;
    }
    if (held_ & (K_UP | K_DOWN | K_LEFT | K_RIGHT) || now - pointerMoved > 4000) pointerOn = false;
    if (!pointerOn) return;
    auto moved = [](float v) { return std::fabs(v) >= kPointerSoft ? v : v * std::fabs(v) / kPointerSoft; };
    px = std::clamp(px + moved(vx) * kPointerSpeed * dt, 0.0f, 1279.0f);
    py = std::clamp(py + moved(vy) * kPointerSpeed * dt, 0.0f, 719.0f);
    if (pressed_ & K_A) {  // A clicks where it points, the same as touching there
        pressed_ &= ~K_A;
        tapNow = true;
        tapX = (int)px;
        tapY = (int)py;
        pointerMoved = now;
    }
}

void Input::update() {
    padUpdate(&pad);
    u64 down = padGetButtonsDown(&pad);
    pressed_ = mapButtons(down);
    dpad_ = down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right);
    held_ = mapButtons(padGetButtons(&pad));

    const uint32_t now = SDL_GetTicks();
    const uint32_t dirs[4] = {K_UP, K_DOWN, K_LEFT, K_RIGHT};
    for (int i = 0; i < 4; i++) {
        if (pressed_ & dirs[i]) {
            repeatStart[i] = repeatLast[i] = now;
        } else if ((held_ & dirs[i]) && now - repeatStart[i] > 350 && now - repeatLast[i] > 70) {
            pressed_ |= dirs[i];
            repeatLast[i] = now;
        }
    }

    // Touch
    tapNow = false;
    HidTouchScreenState ts = {};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        int x = (int)ts.touches[0].x, y = (int)ts.touches[0].y;
        if (!tDown) {
            tDown = true;
            tMoved = false;
            sx = lx = x;
            sy = ly = y;
            accX = accY = 0;
            tStart = now;
        } else {
            accX += x - lx;
            accY += y - ly;
            lx = x;
            ly = y;
            if (std::abs(x - sx) > 24 || std::abs(y - sy) > 24) tMoved = true;
            // Dragging the content: finger to the left shows what is on the right, and so on.
            if (dragKeys) {
                for (; accX <= -110; accX += 110) pressed_ |= K_RIGHT;
                for (; accX >= 110; accX -= 110) pressed_ |= K_LEFT;
                for (; accY <= -90; accY += 90) pressed_ |= K_DOWN;
                for (; accY >= 90; accY -= 90) pressed_ |= K_UP;
            }
        }
    } else if (tDown) {
        tDown = false;
        if (!tMoved && now - tStart < 500) {
            tapNow = true;
            tapX = lx;
            tapY = ly;
        }
    }
    updatePointer(now);
}
