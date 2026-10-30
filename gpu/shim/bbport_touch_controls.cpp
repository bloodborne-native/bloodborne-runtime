// SPDX-License-Identifier: GPL-2.0-or-later
// On-screen pad for touch screens (bbport_touch_controls.h). The design follows
// ducvd89/Bloodborne-Android's TouchControllerOverlay (GPL-2.0): translucent controls, and a touch
// outside every control turns the camera as a right stick anchored where the finger landed. This
// file's layout, logic and drawing (bbport_overlay.cpp, with its theme) are this project's.
#include "bbport_touch_controls.h"

#include "bbport_overlay.h"
#include "../bbgpu.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace BbTouch {
namespace {

// libScePad's button bits (runtime_pad.c).
enum : uint32_t {
    L3 = 0x2, R3 = 0x4, Options = 0x8, Up = 0x10, Right = 0x20, Down = 0x40, Left = 0x80, L2 = 0x100,
    R2 = 0x200, L1 = 0x400, R1 = 0x800, Triangle = 0x1000, Circle = 0x2000, Cross = 0x4000,
    Square = 0x8000, Touchpad = 0x100000,
    MenuBit = 0x80000000u, // not a pad button: the settings menu
};

// The layout, in units of the window's height from the safe area's edges (Edge), so it keeps its
// shape on every screen and stays clear of a notch or the Dynamic Island.
enum Edge { FromLeft, FromRight, FromCentre };
struct Def {
    View::Kind kind;
    Edge edge;
    float x, y, w, h;
    uint32_t bit;
    const char* label;
    int symbol;
};
constexpr float FaceSpread = 0.105f, FaceRadius = 0.060f;
constexpr Def Defs[] = {
    // The shoulders stacked at each side, below the game's HUD (health, stamina, insight).
    {View::Pill, FromLeft, 0.13f, 0.25f, 0.075f, 0.040f, L2, "L2", 0},
    {View::Pill, FromLeft, 0.13f, 0.365f, 0.075f, 0.040f, L1, "L1", 0},
    {View::Pill, FromRight, 0.13f, 0.25f, 0.075f, 0.040f, R2, "R2", 0},
    {View::Pill, FromRight, 0.13f, 0.365f, 0.075f, 0.040f, R1, "R1", 0},
    // The system row at the top centre.
    {View::Pill, FromCentre, 0.0f, 0.065f, 0.105f, 0.032f, Touchpad, "Touchpad", 0},
    {View::Pill, FromCentre, 0.25f, 0.065f, 0.075f, 0.032f, Options, "Options", 0},
    {View::Pill, FromCentre, -0.25f, 0.065f, 0.075f, 0.032f, MenuBit, "Settings", 0},
    {View::Button, FromLeft, 0.62f, 0.90f, 0.045f, 0.045f, L3, "L3", 0},
    {View::Button, FromRight, 0.62f, 0.90f, 0.045f, 0.045f, R3, "R3", 0},
    {View::Face, FromRight, 0.30f, 0.64f - FaceSpread, FaceRadius, FaceRadius, Triangle, nullptr, 0},
    {View::Face, FromRight, 0.30f - FaceSpread, 0.64f, FaceRadius, FaceRadius, Circle, nullptr, 1},
    {View::Face, FromRight, 0.30f, 0.64f + FaceSpread, FaceRadius, FaceRadius, Cross, nullptr, 2},
    {View::Face, FromRight, 0.30f + FaceSpread, 0.64f, FaceRadius, FaceRadius, Square, nullptr, 3},
};
constexpr float DpadX = 0.36f, DpadY = 0.52f, DpadRadius = 0.13f;
constexpr float StickX = 0.30f, StickY = 0.80f, StickRadius = 0.12f;
constexpr float CameraRange = 0.10f; // the drag that turns the camera at full speed
constexpr float StickZoneTop = 0.42f, StickZoneRight = 0.45f; // of the height; of the width

enum class Use { None, Control, Dpad, Stick, Camera };
struct Finger {
    SDL_FingerID id = 0;
    Use use = Use::None;
    int control = -1;
    float ox = 0, oy = 0, x = 0, y = 0; // in height units: where it landed, where it is
};

std::mutex mutex;
std::array<Finger, 10> fingers{};
float aspect = 16.0f / 9.0f;      // the screen's width / height (the window's, the overlay's)
float inset_left = 0.0f, inset_right = 0.0f; // the safe area's margins, in height units
std::atomic<bool> touch_seen{false};
std::atomic<int> gamepads{-1}; // -1 until counted

int Forced() {
    static const int forced = [] {
        const char* env = std::getenv("BB_TOUCH_CONTROLS");
        return !env || !*env ? -1 : env[0] == '0' ? 0 : 1;
    }();
    return forced;
}

float CentreX(const Def& d) {
    return d.edge == FromLeft ? inset_left + d.x : d.edge == FromRight ? aspect - inset_right - d.x : aspect / 2 + d.x;
}
float DpadCentreX() { return inset_left + DpadX; }

/// The control under (x, y), height units; -1 none.
int ControlAt(float x, float y) {
    for (int i = 0; i < int(std::size(Defs)); ++i) {
        const Def& d = Defs[i];
        const float dx = x - CentreX(d), dy = y - d.y;
        // A little larger than drawn: a thumb is not a pointer.
        const bool inside = d.kind == View::Pill
                                ? std::abs(dx) <= d.w * 1.15f && std::abs(dy) <= d.h * 1.4f
                                : dx * dx + dy * dy <= d.w * d.w * 1.35f * 1.35f;
        if (inside) return i;
    }
    return -1;
}
bool OnDpad(float x, float y) {
    const float dx = x - DpadCentreX(), dy = y - DpadY;
    return dx * dx + dy * dy <= DpadRadius * DpadRadius * 1.2f;
}
uint32_t DpadBits(float x, float y) {
    const float dx = x - DpadCentreX(), dy = y - DpadY, length = std::hypot(dx, dy);
    if (length < DpadRadius * 0.18f) return 0;
    uint32_t bits = 0;
    if (std::abs(dx) > length * 0.38f) bits |= dx < 0 ? Left : Right; // diagonals: both
    if (std::abs(dy) > length * 0.38f) bits |= dy < 0 ? Up : Down;
    return bits;
}
uint8_t Axis(float offset, float range) {
    return uint8_t(std::clamp(std::lround(128.0f + offset / range * 127.0f), 0L, 255L));
}

void ReleaseAll() {
    for (Finger& f : fingers) f = Finger{};
}

/// Under mutex: the window's shape and safe area's margins, from the event's window.
void UpdateShape(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    int w = 0, h = 0;
    if (!window || !SDL_GetWindowSize(window, &w, &h) || w <= 0 || h <= 0) return;
    aspect = float(w) / float(h);
    SDL_Rect safe{};
    if (SDL_GetWindowSafeArea(window, &safe) && safe.w > 0) {
        inset_left = float(safe.x) / float(h);
        inset_right = float(w - safe.x - safe.w) / float(h);
    } else {
        inset_left = inset_right = 0.0f;
    }
}

} // namespace

bool Shown() {
    const int forced = Forced();
    if (forced == 0 || BbOverlay::CapturesInput()) return false;
    if (forced == 1) return true;
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
    const bool touch_screen = true;
#else
    const bool touch_screen = touch_seen.load(std::memory_order_relaxed);
#endif
    return touch_screen && gamepads.load(std::memory_order_relaxed) <= 0;
}

bool HandleEvent(const SDL_Event& event) {
    if (gamepads.load(std::memory_order_relaxed) < 0) {
        int count = 0;
        SDL_free(SDL_GetGamepads(&count));
        gamepads.store(count, std::memory_order_relaxed);
    }
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) gamepads.fetch_add(1, std::memory_order_relaxed);
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED && gamepads.load(std::memory_order_relaxed) > 0) {
        gamepads.fetch_sub(1, std::memory_order_relaxed);
    }
    const bool finger = event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
                        event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;
    if (finger) touch_seen.store(true, std::memory_order_relaxed);
    std::scoped_lock lock{mutex};
    if (!Shown()) {
        ReleaseAll(); // the menu opened, or a gamepad came: nothing stays held
        return false;
    }
    // The mouse events SDL makes from touches: a tap is not a click that takes the mouse.
    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event.button.which == SDL_TOUCH_MOUSEID) {
        return true;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which == SDL_TOUCH_MOUSEID) return true;
    if (!finger) return false;

    UpdateShape(event.tfinger.windowID);
    const float x = event.tfinger.x * aspect, y = event.tfinger.y;
    Finger* f = nullptr;
    for (Finger& candidate : fingers) {
        if (candidate.use != Use::None && candidate.id == event.tfinger.fingerID) f = &candidate;
    }
    if (event.type == SDL_EVENT_FINGER_DOWN) {
        if (!f) {
            for (Finger& candidate : fingers) {
                if (candidate.use == Use::None) {
                    f = &candidate;
                    break;
                }
            }
        }
        if (!f) return true; // more than ten fingers
        *f = Finger{event.tfinger.fingerID, Use::None, -1, x, y, x, y};
        if (const int control = ControlAt(x, y); control >= 0) {
            f->use = Use::Control, f->control = control;
            if (Defs[control].bit == MenuBit) {
                ReleaseAll();
                BbOverlay::OpenMenu();
            }
        } else if (OnDpad(x, y)) {
            f->use = Use::Dpad;
        } else if (x < aspect * StickZoneRight && y > StickZoneTop) {
            f->use = Use::Stick; // the stick's centre is where the thumb landed
        } else {
            f->use = Use::Camera;
        }
        return true;
    }
    if (!f) return true;
    if (event.type == SDL_EVENT_FINGER_MOTION) {
        f->x = x, f->y = y;
        // A thumb slides from one button to the next (between the face buttons, the shoulders).
        if (f->use == Use::Control) {
            const int control = ControlAt(x, y);
            if (control >= 0 && Defs[control].bit != MenuBit) f->control = control;
        }
        return true;
    }
    *f = Finger{}; // up or cancelled
    return true;
}

std::vector<View> Views(float width, float height) {
    std::scoped_lock lock{mutex};
    std::vector<View> views;
    // The overlay covers the window: its shape is the layout's (also before the first touch).
    if (width > 0 && height > 0) aspect = width / height;
    const float k = height; // height units -> pixels
    uint32_t held = 0, dpad = 0;
    const Finger* stick = nullptr;
    for (const Finger& f : fingers) {
        if (f.use == Use::Control) held |= Defs[f.control].bit;
        if (f.use == Use::Dpad) dpad |= DpadBits(f.x, f.y);
        if (f.use == Use::Stick) stick = &f;
    }
    for (const Def& d : Defs) {
        views.push_back({d.kind, CentreX(d) * k, d.y * k, d.w * k, d.h * k, d.label, d.symbol, (held & d.bit) != 0, 0, 0});
    }
    views.push_back({View::Dpad, DpadCentreX() * k, DpadY * k, DpadRadius * k, DpadRadius * k, nullptr, int(dpad),
                     dpad != 0, 0, 0});
    // The stick: where the thumb landed while held, else its place at rest.
    const float sx = stick ? stick->ox : inset_left + StickX, sy = stick ? stick->oy : StickY;
    float kx = sx, ky = sy;
    if (stick) {
        const float dx = stick->x - sx, dy = stick->y - sy, length = std::hypot(dx, dy);
        const float scale = length > StickRadius ? StickRadius / length : 1.0f;
        kx = sx + dx * scale, ky = sy + dy * scale;
    }
    views.push_back({View::Stick, sx * k, sy * k, StickRadius * k, StickRadius * k, nullptr, 0, stick != nullptr,
                     kx * k, ky * k});
    return views;
}

} // namespace BbTouch

/// runtime_pad.c, at every pad read: the touch controls' buttons (ORed in) and sticks (-1: not
/// held). 1 while the controls are shown.
extern "C" int bbgpu_touch_pad(uint32_t* buttons, int* sticks) {
    using namespace BbTouch;
    for (int i = 0; i < 4; ++i) sticks[i] = -1;
    *buttons = 0;
    if (!Shown()) return 0;
    std::scoped_lock lock{mutex};
    for (const Finger& f : fingers) {
        switch (f.use) {
        case Use::Control:
            if (Defs[f.control].bit != MenuBit) *buttons |= Defs[f.control].bit;
            break;
        case Use::Dpad:
            *buttons |= DpadBits(f.x, f.y);
            break;
        case Use::Stick: {
            const float dx = f.x - f.ox, dy = f.y - f.oy, length = std::hypot(dx, dy);
            const float scale = length > StickRadius ? StickRadius / length : 1.0f;
            sticks[0] = Axis(dx * scale, StickRadius), sticks[1] = Axis(dy * scale, StickRadius);
            break;
        }
        case Use::Camera: {
            const float dx = f.x - f.ox, dy = f.y - f.oy, length = std::hypot(dx, dy);
            const float scale = length > CameraRange ? CameraRange / length : 1.0f;
            sticks[2] = Axis(dx * scale, CameraRange), sticks[3] = Axis(dy * scale, CameraRange);
            break;
        }
        case Use::None:
            break;
        }
    }
    return 1;
}
