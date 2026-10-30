// SPDX-License-Identifier: GPL-2.0-or-later
// On-screen pad for touch screens (the iPhone, Android, a touch display): the sticks, the face
// buttons, the d-pad, L1/L2/R1/R2, L3/R3, Options, the touchpad and the settings menu. A touch
// outside every control turns the camera. The design follows ducvd89/Bloodborne-Android's
// TouchControllerOverlay (GPL-2.0): translucent controls, the camera a right stick anchored where
// the finger landed. The state reaches the game through runtime_pad.c (bbgpu_touch_pad); the
// overlay draws the controls with its theme (bbport_overlay.cpp).
#pragma once

#include <SDL3/SDL_events.h>
#include <vector>

namespace BbTouch {

/// Window thread: fingers on the controls, and the mouse events SDL makes from them while the
/// controls are shown (a tap is not a click that takes the mouse). True: the event is used.
bool HandleEvent(const SDL_Event& event);
/// Whether the controls are shown: a touch screen (or BB_TOUCH_CONTROLS=1), no gamepad, the
/// in-game menu closed. BB_TOUCH_CONTROLS=0: never.
bool Shown();

/// One control as drawn, in the overlay's pixels.
struct View {
    enum Kind { Button, Pill, Face, Dpad, Stick } kind;
    float x, y;     ///< centre
    float w, h;     ///< half sizes (a circle: w == h, its radius)
    const char* label;
    int symbol;     ///< Face: 0 triangle, 1 circle, 2 cross, 3 square; Dpad: the pressed directions
    bool pressed;
    float knob_x, knob_y; ///< Stick: the knob's centre
};
/// The controls for a WIDTH x HEIGHT overlay, with what is held now.
std::vector<View> Views(float width, float height);

} // namespace BbTouch
