// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_game_hooks.h"
#include "bbport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <sys/resource.h>
#include <thread>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include "bbport_settings.h"
#include "bbport_text.h"
#include "bbport_trophy_queue.h"
#ifdef BB_METAL_BACKEND
#include "bbport_overlay_metal.h"
#include "video_core/renderer_metal/mtl_vk_layer.h"
#endif
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// bbport: the menu in the launcher's language (bbport_text: gpu/shim/locales/*.json).
const char* T(const char* key) {
    return BbText::Get(key);
}
const char* T(const std::string& key) {
    return BbText::Get(key.c_str());
}
std::string EffectKey(int effect) {
    return std::string("effects.") + BbSettings::Effects[effect].key;
}

// The settings menu key: Insert, and F1 on macOS (Mac keyboards have no Insert key).
#ifdef __APPLE__
constexpr SDL_Keycode MenuKey = SDLK_F1;
#else
constexpr SDL_Keycode MenuKey = SDLK_INSERT;
#endif

// Embedded fonts (third_party/fonts): Inter for text and Cinzel for headings, as the launcher
// (SIL OFL 1.1), and DejaVu Sans (Bitstream Vera license) for the scripts they lack.
#ifdef __APPLE__
// Mach-O: read-only data section, C symbols with a leading underscore.
#define BB_EMBED(name, path)                                                                       \
    asm(".section __TEXT,__const\n"                                                                \
        ".balign 16\n"                                                                             \
        ".private_extern _" #name "\n"                                                             \
        ".globl _" #name "\n"                                                                      \
        "_" #name ":\n"                                                                            \
        ".incbin \"" path "\"\n"                                                                   \
        ".private_extern _" #name "_end\n"                                                         \
        ".globl _" #name "_end\n"                                                                  \
        "_" #name "_end:\n"                                                                        \
        ".text\n")
#else
#define BB_EMBED(name, path)                                                                       \
    asm(".section .rodata\n"                                                                       \
        ".balign 16\n"                                                                             \
        ".hidden " #name "\n"                                                                      \
        ".global " #name "\n" #name ":\n"                                                          \
        ".incbin \"" path "\"\n"                                                                   \
        ".hidden " #name "_end\n"                                                                  \
        ".global " #name "_end\n" #name "_end:\n"                                                  \
        ".previous\n")
#endif
BB_EMBED(bb_font_ttf, BB_FONT_PATH);
BB_EMBED(bb_font_text_ttf, BB_FONT_TEXT_PATH);
BB_EMBED(bb_font_heading_ttf, BB_FONT_HEADING_PATH);
extern "C" const unsigned char bb_font_ttf[], bb_font_ttf_end[];
extern "C" const unsigned char bb_font_text_ttf[], bb_font_text_ttf_end[];
extern "C" const unsigned char bb_font_heading_ttf[], bb_font_heading_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

struct TrophyText {
    int id;
    const char* language;
    const char* name;
    const char* description;
    const char* grade;
    const char* group;
};
#include "bbport_trophy_data.inc"

BbTrophyQueue::Queue trophy_queue;
float fps_top_right_bottom = 0.0f; ///< present thread: the FPS counter's bottom edge, when top right

const TrophyText* TrophyTextFor(int id, const char* language) {
    const TrophyText* english = nullptr;
    for (const auto& row : kTrophyTexts) {
        if (row.id != id) continue;
        if (std::strcmp(row.language, language) == 0) return &row;
        if (std::strcmp(row.language, "en") == 0) english = &row;
    }
    return english;
}

void QueueTrophy(int id, bool preview) {
    const TrophyText* text = TrophyTextFor(id, "en");
    trophy_queue.Push({.id = id, .at = BbTrophyQueue::Clock::now(), .preview = preview,
                       .platinum = text && std::strcmp(text->grade, "platinum") == 0});
}

// The launcher's design tokens (bloodborne-launcher src/index.css): the menu looks like the app.
namespace Theme {
constexpr ImVec4 Hex(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(float((rgb >> 16) & 0xff) / 255.0f, float((rgb >> 8) & 0xff) / 255.0f,
                  float(rgb & 0xff) / 255.0f, alpha);
}
// The launcher's colours (launcher/src/index.css): blue-slate night, parchment text, rust for what
// is on or chosen, gold for headings and the focus ring.
constexpr ImVec4 Background = Hex(0x07090d, 0.96f);
constexpr ImVec4 Foreground = Hex(0xece4d4);
constexpr ImVec4 Card = Hex(0x0e1218);
constexpr ImVec4 Primary = Hex(0x7a2a26);
constexpr ImVec4 PrimaryHover = Hex(0xa24a3f);
constexpr ImVec4 PrimaryForeground = Hex(0xf6eedd);
constexpr ImVec4 Muted = Hex(0x161b23);
constexpr ImVec4 MutedForeground = Hex(0x979ba4);
constexpr ImVec4 Accent = Hex(0x1f2530);
constexpr ImVec4 AccentHover = Hex(0x2a3242);
constexpr ImVec4 Gold = Hex(0xc9a265);
constexpr ImVec4 Border = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
constexpr ImVec4 Warning = Hex(0xe0a458);
constexpr float Radius = 12.0f; // --radius: 0.75rem
} // namespace Theme

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved 0.3 s after the last change, and on close
std::chrono::steady_clock::time_point dirty_since{};
float base_scale = 1.0f;
ImFont* font_text = nullptr;
ImFont* font_heading = nullptr;
// The game's text dialog (ImeDialog: the character name, passwords): drawn while it is open, with
// an on-screen keyboard for the controller (issues #17, #19: fullscreen has no window title).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;
// The online module's status line (bbgpu_overlay_banner), set from its network threads.
std::mutex banner_mutex;
std::string banner_text;
void (*text_editor)(TextEdit, const char*, void*) = nullptr;
void* text_editor_context = nullptr;
// The connected monitors' names (the window thread reads them when the menu opens).
std::mutex displays_mutex;
std::vector<std::string> display_names;
std::atomic<bool> displays_wanted{true};
// Xbox-style pads get A/B/X/Y hints; PlayStation and others the shapes.
std::atomic<bool> xbox_buttons{false};
std::atomic<bool> keyboard_buttons{false};

// Present rate for the FPS counter, and the last frame times for its graph.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
constexpr int FrameHistory = 120;
float frame_ms_history[FrameHistory]{};
int frame_ms_next = 0;

enum Tab { TabGraphics, TabGame, TabAdvanced, TabCount };
int tab = TabGraphics;
const char* help_text = nullptr; // the focused or hovered row's explanation (footer)

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // The system cursor shows over the menu (window.cpp, from bbport 0.4); ImGui learns where it
    // is now, not at the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        displays_wanted = true;
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// ---- Widgets ------------------------------------------------------------------------------------

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
        dirty_since = std::chrono::steady_clock::now();
    }
}

float S(float px) {
    return px * base_scale;
}

/// The focused (keyboard/controller) or hovered item's explanation goes to the footer.
void Help(const char* text) {
    if (text && (ImGui::IsItemFocused() || ImGui::IsItemHovered())) {
        help_text = text;
    }
}

/// A card: a rounded panel of rows, with an optional heading above it.
bool BeginCard(const char* id, const char* heading = nullptr) {
    if (heading) {
        ImGui::PushFont(font_heading, ImGui::GetFontSize() * 1.1f);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Gold);
        ImGui::TextUnformatted(heading);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(Theme::Radius));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(10)));
    const bool open = ImGui::BeginChild(id, ImVec2(0, 0),
                                        ImGuiChildFlags_AutoResizeY |
                                            ImGuiChildFlags_AlwaysUseWindowPadding |
                                            ImGuiChildFlags_NavFlattened);
    return open;
}

void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(6)));
}

/// A row: the label on the left, the control (drawn after this call) right-aligned in
/// `control_width`, a separator line above all but the first.
void Row(const char* label, float control_width, bool first = false) {
    if (!first) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
            ImGui::GetColorU32(Theme::Border));
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    // The label on the control's baseline (frame padding), the control right-aligned on the line.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                             ImGui::GetWindowContentRegionMax().x - control_width));
}

/// A switch: a pill with a knob, rust when on. The item is a frame tall; the pill is centered in it.
bool Switch(const char* id, bool* value) {
    const float height = ImGui::GetFrameHeight() * 0.85f;
    const float width = height * 1.8f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, ImGui::GetFrameHeight()));
    if (pressed) {
        *value = !*value;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float inset = (ImGui::GetFrameHeight() - height) * 0.5f;
    const ImVec2 a(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y + inset);
    const ImVec2 b(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y - inset);
    const ImVec4 track = *value ? (hovered ? Theme::PrimaryHover : Theme::Primary)
                                : (hovered ? Theme::AccentHover : Theme::Accent);
    draw->AddRectFilled(a, b, ImGui::GetColorU32(track), height * 0.5f);
    const float radius = height * 0.5f - S(3);
    const float x = *value ? b.x - radius - S(3) : a.x + radius + S(3);
    draw->AddCircleFilled(ImVec2(x, (a.y + b.y) * 0.5f), radius,
                          ImGui::GetColorU32(*value ? Theme::PrimaryForeground : Theme::MutedForeground));
    return pressed;
}

float SwitchWidth() {
    return ImGui::GetFrameHeight() * 0.85f * 1.8f;
}

void SwitchSetting(const char* label, std::atomic<bool>& value, const char* help, bool first = false) {
    Row(label, SwitchWidth(), first);
    bool v = value;
    ImGui::PushID(label);
    // The widget runs first: Store must get the value it leaves (argument order is unspecified).
    const bool changed = Switch("##switch", &v);
    Store(value, v, changed);
    Help(help);
    ImGui::PopID();
}

/// Segmented choice: one button per option, the chosen one rust. `enabled` may be null.
bool Segmented(const char* id, const char* const* labels, int count, int* value,
               const bool* enabled = nullptr) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    for (int i = 0; i < count; ++i) {
        if (i) {
            ImGui::SameLine();
        }
        const bool chosen = i == *value;
        const bool on = !enabled || enabled[i];
        ImGui::PushStyleColor(ImGuiCol_Button, chosen ? Theme::Primary : Theme::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chosen ? Theme::PrimaryHover : Theme::AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::PrimaryHover);
        ImGui::PushStyleColor(ImGuiCol_Text, chosen ? Theme::PrimaryForeground : Theme::Foreground);
        ImGui::BeginDisabled(!on);
        ImGui::PushID(i);
        if (ImGui::Button(labels[i]) && i != *value) {
            *value = i;
            changed = true;
        }
        ImGui::PopID();
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

float SegmentedWidth(const char* const* labels, int count) {
    float width = 0.0f;
    for (int i = 0; i < count; ++i) {
        width += ImGui::CalcTextSize(labels[i]).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }
    return width + S(2) * float(count - 1);
}

/// A row choosing one of `labels`: segmented buttons when they fit beside the label, else a
/// stepper (‹ choice ›) as wide as the longest choice (monitor names, long translations).
bool Choice(const char* label, const char* const* labels, int count, int* value, const char* help,
            bool first = false) {
    const float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(label).x - S(24);
    const float segmented = SegmentedWidth(labels, count);
    bool changed = false;
    if (segmented <= room) {
        Row(label, segmented, first);
        changed = Segmented(label, labels, count, value);
        Help(help);
        return changed;
    }
    const float arrow = ImGui::GetFrameHeight();
    float text = 0.0f;
    for (int i = 0; i < count; ++i) {
        text = std::max(text, ImGui::CalcTextSize(labels[i]).x + ImGui::GetStyle().FramePadding.x * 2.0f);
    }
    text = std::max(S(80), std::min(text, room - arrow * 2.0f - S(8)));
    Row(label, text + arrow * 2.0f + S(8), first);
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::AccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::PrimaryHover);
    // The arrows are for the mouse; the controller steps the focused choice with left/right.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::ArrowButton("##previous", ImGuiDir_Left)) {
        *value = (*value + count - 1) % count;
        changed = true;
    }
    ImGui::PopItemFlag();
    Help(help);
    ImGui::SameLine(0.0f, S(4));
    // The choice itself: pressing it, or left/right while it has focus, steps it.
    const int shown = std::clamp(*value, 0, count - 1);
    if (ImGui::Button(labels[shown], ImVec2(text, 0.0f))) {
        *value = (shown + 1) % count;
        changed = true;
    }
    if (ImGui::IsItemFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (shown + count - 1) % count;
            changed = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (shown + 1) % count;
            changed = true;
        }
    }
    Help(help);
    ImGui::SameLine(0.0f, S(4));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
        *value = (*value + 1) % count;
        changed = true;
    }
    ImGui::PopItemFlag();
    Help(help);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

/// A slider; with the controller, left/right on the focused slider steps it.
void SliderSetting(const char* label, std::atomic<float>& value, float lo, float hi, float step,
                   const char* help, bool first = false) {
    const float width = S(220);
    Row(label, width, first);
    float v = value;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::SliderFloat("##slider", &v, lo, hi, "%.2f");
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            v = std::max(lo, v - step);
            changed = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
                   ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            v = std::min(hi, v + step);
            changed = true;
        }
    }
    Help(help);
    ImGui::PopID();
    Store(value, v, changed);
}

/// A line of muted (or warning) text inside a card.
void Note(const char* text, bool warning = false) {
    ImGui::PushStyleColor(ImGuiCol_Text, warning ? Theme::Warning : Theme::MutedForeground);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/// Controller hints: the button's symbol for the connected pad, then what it does.
const char* ButtonName(const char* playstation, const char* xbox) {
    return xbox_buttons ? xbox : playstation;
}

void HintItem(const char* button, const char* action) {
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Foreground);
    ImGui::TextUnformatted(button);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, S(6));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::MutedForeground);
    ImGui::TextUnformatted(action);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, S(18));
}

bool RestartNeeded() {
    const auto& s = BbSettings::Get();
    bool restart = s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart() || BbSettings::LauncherNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const bool live = e < BbSettings::GameEffectCount && e != BbSettings::EffectSsr && BbGameHooks::HotEffects();
        restart |= !live && s.effects[e] != s.startup_effects[e];
    }
    return restart;
}

// ---- Tabs ---------------------------------------------------------------------------------------
// The launcher's pages, with the same sections and words (launcher.* texts, imported from the
// launcher's locales): Graphics, Game and Advanced. The launcher's own options are shown only when
// it keeps the settings (fullscreen always: it switches at once).

#define LT(key) T("launcher." key)

void ChangeLauncher(BbSettings::LauncherKey key, const std::string& value) {
    BbSettings::SetLauncher(key, value);
    dirty = true;
    dirty_since = std::chrono::steady_clock::now();
}

/// A launcher option chosen among `values` (launcher.json's value text), shown as `labels`.
void LauncherChoice(BbSettings::LauncherKey key, const char* label, const char* const* values,
                    const char* const* labels, int count, const char* help, bool first = false) {
    const std::string current = BbSettings::Launcher(key);
    int v = 0;
    for (int i = 0; i < count; ++i) {
        if (current == values[i]) v = i;
    }
    if (Choice(label, labels, count, &v, help, first)) {
        ChangeLauncher(key, values[v]);
    }
}

void LauncherSwitch(BbSettings::LauncherKey key, const char* label, const char* help, bool first = false) {
    Row(label, SwitchWidth(), first);
    bool v = BbSettings::Launcher(key) == "true";
    ImGui::PushID(label);
    if (Switch("##switch", &v)) {
        ChangeLauncher(key, v ? "true" : "false");
    }
    Help(help);
    ImGui::PopID();
}

/// The monitor: the main display, a connected one by name (BB_DISPLAY), or the saved one while it
/// is not connected. The names come from the window thread (ApplyWindowSettings).
void MonitorChoice() {
    std::vector<std::string> names;
    {
        std::scoped_lock lock{displays_mutex};
        names = display_names;
    }
    const std::string saved = BbSettings::Launcher(BbSettings::LauncherDisplay);
    if (!saved.empty() && std::find(names.begin(), names.end(), saved) == names.end()) {
        names.push_back(saved);
    }
    std::vector<const char*> values{""}, labels{LT("graphics.monitorMain")};
    for (const auto& name : names) {
        values.push_back(name.c_str());
        labels.push_back(name.c_str());
    }
    LauncherChoice(BbSettings::LauncherDisplay, LT("graphics.monitor"), values.data(), labels.data(),
                   int(values.size()), LT("graphics.monitorHint"));
}

/// The frame rate limit (0: none), stepped by 5 with left/right.
void FpsLimitSetting() {
    const float width = S(220);
    Row(LT("graphics.fpsLimit"), width);
    int v = std::atoi(BbSettings::Launcher(BbSettings::LauncherFpsLimit).c_str());
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::SliderInt("##fpslimit", &v, 0, 480, v == 0 ? LT("common.off") : "%d",
                                    ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            v = std::max(0, (v - 1) / 5 * 5);
            changed = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            v = std::min(480, v / 5 * 5 + 5);
            changed = true;
        }
    }
    Help(LT("graphics.fpsLimitHint"));
    if (changed) {
        ChangeLauncher(BbSettings::LauncherFpsLimit, std::to_string(v));
    }
}

/// The memory the game uses at its next start with these settings, next to the computer's: as
/// run.sh sizes the game's memory, plus the renderer's (measured). The launcher shows the same
/// (bloodborne-launcher src/lib/data/memory.ts): keep the two in step.
void MemoryEstimate() {
    const auto& s = BbSettings::Get();
    const auto out = BbSettings::CurrentOutput();
    int w = out.width, h = out.height;
    bool patched = false;
    if (s.upscaler != BbSettings::UpscalerTaa && s.live_resolution != 1) {
        const float scale = s.upscaler == BbSettings::UpscalerOff ? 1.0f : BbSettings::PresetScale(s.preset);
        if (w != 1920 || h != 1080 || scale != 1.0f) {
            const auto even = [&](int v) { return std::max(2, int(std::lround(v / scale / 2.0f)) * 2); };
            w = even(w);
            h = even(h);
            if (w == 1920 && h == 1080) {
                w = 1916;
                h = 1078;
            }
            patched = true;
        }
    }
    const double pixels = double(w) * double(h);
    const double extra = patched ? std::max(0.0, (pixels - 1920.0 * 1080.0) * 448.0) : 0.0;
    const double game = 5056.0 + std::ceil(extra / double(64 << 20)) * 64.0 + 1024.0;
    const double used = game + 1000.0 + 360.0 * pixels / 1e6; // MiB
    static const int total = SDL_GetSystemRAM();               // MiB
    const bool high = total > 0 && used > total * 0.75;
    char text[64];
    if (total > 0) {
        std::snprintf(text, sizeof(text), "%.1f / %.1f GB", used / 1024.0, total / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%.1f GB", used / 1024.0);
    }
    Row(LT("graphics.memory"), ImGui::CalcTextSize(text).x);
    ImGui::PushStyleColor(ImGuiCol_Text, high ? Theme::Warning : Theme::MutedForeground);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    Help(high ? LT("graphics.memoryHigh") : LT("graphics.memoryHint"));
}

void GraphicsTab() {
    auto& s = BbSettings::Get();
    const bool launcher = BbSettings::LauncherChannel();
    if (BeginCard("##display", LT("graphics.display"))) {
        // The aspect ratio, then its outputs (bbport 0.6): another ratio keeps the height.
        static const char* aspects[BbSettings::AspectCount] = {};
        aspects[0] = LT("graphics.aspectAuto");
        for (int a = 1; a < BbSettings::AspectCount; ++a) aspects[a] = BbSettings::AspectKey(a);
        int aspect = s.aspect;
        if (Choice(LT("graphics.aspectRatio"), aspects, BbSettings::AspectCount, &aspect,
                   LT("graphics.aspectRatioHint"), true)) {
            const auto kept = BbSettings::NearestOutput(aspect, BbSettings::CurrentOutput());
            Store(s.aspect, aspect, true);
            Store(s.output_res, BbSettings::PackSize(kept), true);
        }
        const auto list = BbSettings::Outputs(s.aspect);
        static std::array<std::string, 12> texts;
        static std::array<const char*, 12> outputs{};
        for (int i = 0; i < list.count; ++i) {
            texts[i] = std::to_string(list.sizes[i].width) + " × " + std::to_string(list.sizes[i].height);
            outputs[i] = texts[i].c_str();
        }
        int output = std::max(0, list.IndexOf(BbSettings::CurrentOutput()));
        const bool output_changed =
            Choice(LT("graphics.outputRes"), outputs.data(), list.count, &output,
                   T(BbSettings::FixedRenderSession() ? "output.fixedHint" : "output.liveHint"));
        if (output_changed) {
            Store(s.output_res, BbSettings::PackSize(list.sizes[output]), true);
        }
        MemoryEstimate();
        if (launcher) {
            MonitorChoice();
        }
        LauncherSwitch(BbSettings::LauncherFullscreen, LT("graphics.fullscreen"), nullptr);
        if (launcher) {
            static const char* modes[] = {"Fifo", "FifoRelaxed", "Mailbox", "Immediate"};
            const char* names[] = {LT("options.present.Fifo"), LT("options.present.FifoRelaxed"),
                                   LT("options.present.Mailbox"), LT("options.present.Immediate")};
            LauncherChoice(BbSettings::LauncherPresentMode, LT("graphics.presentMode"), modes, names, 4,
                           LT("common.restartNote"));
        }
    }
    EndCard();
    if (BeginCard("##framerate", LT("graphics.frameRate"))) {
        if (launcher) {
            static const char* modes[] = {"uncap", "60", "90", "30"};
            const char* names[] = {LT("options.fps.uncap"), LT("options.fps.60"), LT("options.fps.90"),
                                   LT("options.fps.30")};
            const std::string mode = BbSettings::Launcher(BbSettings::LauncherFpsMode);
            LauncherChoice(BbSettings::LauncherFpsMode, LT("graphics.fpsMode"), modes, names, 4,
                           mode == "uncap" ? LT("options.fps.uncapHint")
                           : mode == "30"  ? LT("options.fps.30Hint")
                                           : LT("common.restartNote"),
                           true);
            FpsLimitSetting();
        }
        SwitchSetting(LT("graphics.showFps"), s.show_fps, nullptr, !launcher);
        ImGui::BeginDisabled(!s.show_fps);
        {
            const char* sizes[] = {LT("options.fpsSize.small"), LT("options.fpsSize.big")};
            static_assert(sizeof(sizes) / sizeof(sizes[0]) == BbSettings::FpsSizeCount);
            int size = s.fps_size;
            const bool size_changed = Choice(LT("graphics.fpsSize"), sizes, BbSettings::FpsSizeCount, &size, nullptr);
            Store(s.fps_size, size, size_changed);
            const char* positions[] = {LT("options.fpsPosition.top_left"),     LT("options.fpsPosition.top_center"),
                                       LT("options.fpsPosition.top_right"),    LT("options.fpsPosition.bottom_left"),
                                       LT("options.fpsPosition.bottom_center"), LT("options.fpsPosition.bottom_right")};
            static_assert(sizeof(positions) / sizeof(positions[0]) == BbSettings::FpsPositionCount);
            int position = s.fps_position;
            const bool position_changed =
                Choice(LT("graphics.fpsPosition"), positions, BbSettings::FpsPositionCount, &position, nullptr);
            Store(s.fps_position, position, position_changed);
        }
        ImGui::EndDisabled();
#ifdef __APPLE__
        if (launcher) {
            LauncherSwitch(BbSettings::LauncherMetalHud, LT("graphics.metalHud"), LT("graphics.metalHudHint"));
        }
#endif
    }
    EndCard();
    if (BeginCard("##upscaling", LT("graphics.upscaling"))) {
        // The upscalers this GPU offers (FSR 4, DLSS and XeSS need other GPUs).
        std::vector<int> ids;
        std::vector<const char*> names;
        const char* all[BbSettings::UpscalerCount] = {
            T("upscaler.off"), "FSR 3.1", "FSR 4", "FSR 4.1", "TAA", "DLSS",
            LT("options.upscaler.metalfx"), "FSR 4.1 Lossy", "FSR 4.1.2 Lite"};
        // The 4.1.x modes in the order the menu describes them: the new Lite model first, then
        // the two 4.1 profiles; the other upscalers keep their place.
        const int order[] = {BbSettings::UpscalerOff, BbSettings::UpscalerFsr3,
                             BbSettings::UpscalerFsr4, BbSettings::UpscalerFsr412,
                             BbSettings::UpscalerFsr411Lossy, BbSettings::UpscalerFsr411,
                             BbSettings::UpscalerTaa, BbSettings::UpscalerDlss,
                             BbSettings::UpscalerMetalFx};
        for (int i : order) {
            const bool supported = i == BbSettings::UpscalerFsr4          ? s.fsr4_supported.load()
                                   : i == BbSettings::UpscalerFsr411      ? s.fsr411_supported.load()
                                   : i == BbSettings::UpscalerFsr411Lossy ? s.fsr411_lossy_supported.load()
                                   : i == BbSettings::UpscalerFsr412      ? s.fsr412_supported.load()
                                   : i == BbSettings::UpscalerDlss        ? s.dlss_supported.load()
                                   : i == BbSettings::UpscalerMetalFx     ? s.metalfx_supported.load()
                                   : i == BbSettings::UpscalerFsr3        ? s.fsr3_supported.load()
                                                                          : true;
#ifndef __APPLE__
            if (i == BbSettings::UpscalerMetalFx) continue; // macOS only
#endif
            if (supported || i == s.upscaler) {
                ids.push_back(i);
                names.push_back(all[i]);
            }
        }
        int chosen = int(std::find(ids.begin(), ids.end(), s.upscaler.load()) - ids.begin());
        if (chosen == int(ids.size())) chosen = 0; // a hidden mode: show the first entry
        const char* hint = s.upscaler == BbSettings::UpscalerTaa    ? T("upscaler.taa")
                           : s.upscaler == BbSettings::UpscalerDlss ? T("dlss.hint")
                           : s.upscaler == BbSettings::UpscalerMetalFx
                               ? LT("options.upscaler.metalfxHint")
                           : s.upscaler == BbSettings::UpscalerFsr411 ? T("fsr4.hint411")
                           : s.upscaler == BbSettings::UpscalerFsr411Lossy ? T("fsr4.hint411lossy")
                           : s.upscaler == BbSettings::UpscalerFsr412 ? T("fsr4.hint412")
                           : BbSettings::IsFsr4(s.upscaler) ? T("fsr4.hint")
                                                            : LT("graphics.upscalingHint");
        if (Choice(LT("graphics.upscaler"), names.data(), int(names.size()), &chosen, hint, true)) {
            Store(s.upscaler, ids[chosen], true);
        }
        if (const char* problem = s.dlss_problem.load(); problem && s.upscaler == BbSettings::UpscalerDlss) {
            char line[192];
            std::snprintf(line, sizeof(line), "DLSS: %s", problem);
            Note(line, true);
        }
#ifdef __APPLE__
        if (const char* problem = s.metalfx_problem.load();
            problem && s.upscaler == BbSettings::UpscalerMetalFx) {
            char line[192];
            std::snprintf(line, sizeof(line), "MetalFX: %s", problem);
            Note(line, true);
        }
#endif
        if (const char* problem = s.fsr4_problem.load()) {
            char line[192];
            std::snprintf(line, sizeof(line), T("fsr4.unavailable"), problem);
            Note(line, true);
        }
        const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
        const bool taa = s.upscaler == BbSettings::UpscalerTaa;
        // The preset: the render scale, with the resolution it renders at below.
        ImGui::BeginDisabled(!upscaler_on || taa);
        const char* presets[BbSettings::PresetCount] = {LT("options.preset.0"), LT("options.preset.1"),
                                                        LT("options.preset.2"), LT("options.preset.3"),
                                                        LT("options.preset.4")};
        int preset = taa ? BbSettings::NativeAA : s.preset.load();
        if (Choice(LT("graphics.preset"), presets, BbSettings::PresetCount, &preset,
                   T(BbSettings::FixedRenderSession() ? "preset.fixedHint" : "preset.hint"))) {
            Store(s.preset, preset, true);
        }
        ImGui::EndDisabled();
        char active[96];
        std::snprintf(active, sizeof(active), T("preset.active"), s.active_render_width.load(),
                      s.active_render_height.load());
        Note(taa ? T("preset.taaNote") : active);
        ImGui::BeginDisabled(!upscaler_on);
        SwitchSetting(LT("graphics.sharpen"), s.sharpen, T("sharpen.hint"));
        ImGui::BeginDisabled(!s.sharpen);
        SliderSetting(LT("graphics.sharpness"), s.sharpness, 0.0f, 2.0f, 0.05f, T("sharpen.hint"));
        ImGui::EndDisabled();
        SwitchSetting(LT("graphics.objectMotion"), s.object_motion, LT("graphics.objectMotionHint"));
        ImGui::EndDisabled();
    }
    EndCard();
    if (BeginCard("##detail", LT("graphics.detail"))) {
        const char* lods[] = {LT("options.lod.-2"), LT("options.lod.0"), LT("options.lod.1"), LT("options.lod.2")};
        static constexpr int lod_values[] = {-2, 0, 1, 2};
        int lod = 1;
        for (int i = 0; i < 4; ++i) {
            if (lod_values[i] == s.model_lod) lod = i;
        }
        if (Choice(LT("graphics.modelLod"), lods, 4, &lod, LT("common.restartNote"), true)) {
            Store(s.model_lod, lod_values[lod], true);
        }
    }
    EndCard();
    if (BeginCard("##effects", LT("graphics.effects"))) {
        Note(LT("graphics.effectsHint"));
        for (int e = 0; e < BbSettings::GameEffectCount; ++e) {
            // Switched live by the game hooks (bbport 0.6): no restart note.
            const bool live = e != BbSettings::EffectSsr && BbGameHooks::HotEffects();
            SwitchSetting(T(EffectKey(e)), s.effects[e],
                          e == BbSettings::EffectSsr ? LT("options.effects.effect_ssrHint")
                          : live                     ? nullptr
                                                     : LT("common.restartNote"));
        }
    }
    EndCard();
}

void GameTab() {
    auto& s = BbSettings::Get();
    if (BeginCard("##extras", LT("game.extras"))) {
        SwitchSetting(LT("game.skipIntro"), s.effects[BbSettings::EffectSkipIntro], LT("common.restartNote"), true);
    }
    EndCard();
}

void AdvancedTab() {
    auto& s = BbSettings::Get();
    const bool launcher = BbSettings::LauncherChannel();
    if (BeginCard("##performance", LT("advanced.performance"))) {
#ifdef __APPLE__
        if (launcher) {
            static const char* renderers[] = {"moltenvk", "metal"};
            const char* renderer_names[] = {LT("options.renderer.moltenvk"), LT("options.renderer.metal")};
            LauncherChoice(BbSettings::LauncherRenderer, LT("advanced.renderer"), renderers, renderer_names, 2,
                           LT("advanced.rendererHint"), true);
        }
#endif
        if (launcher) {
            static const char* pipe[] = {"", "1", "0"};
            const char* pipe_names[] = {LT("options.drawPipe.auto"), LT("options.drawPipe.on"), LT("options.drawPipe.off")};
#ifdef __APPLE__
            constexpr bool first = false;
#else
            constexpr bool first = true;
#endif
            LauncherChoice(BbSettings::LauncherDrawPipe, LT("advanced.drawPipe"), pipe, pipe_names, 3,
                           LT("options.drawPipe.autoHint"), first);
            static const char* readbacks[] = {"", "2", "0"};
            const char* readback_names[] = {LT("options.readbacks.relaxed"), LT("options.readbacks.precise"),
                                            LT("options.readbacks.off")};
            LauncherChoice(BbSettings::LauncherReadbacks, LT("advanced.readbacks"), readbacks, readback_names, 3,
                           LT("common.restartNote"));
            static const char* preupload[] = {"", "2", "0"};
            const char* preupload_names[] = {LT("options.preupload.normal"), LT("options.preupload.full"),
                                             LT("options.preupload.off")};
            LauncherChoice(BbSettings::LauncherPreupload, LT("advanced.preupload"), preupload, preupload_names, 3,
                           LT("options.preupload.fullHint"));
        }
        const char* live_modes[] = {LT("options.live.0"), LT("options.live.1"), LT("options.live.auto")};
        int live = s.live_resolution < 0 ? 2 : s.live_resolution.load();
        if (Choice(LT("advanced.liveResolution"), live_modes, 3, &live, LT("advanced.liveResolutionHint"),
                   !launcher)) {
            Store(s.live_resolution, live == 2 ? -1 : live, true);
        }
    }
    EndCard();
    if (BeginCard("##diagnostics", LT("advanced.diagnostics"))) {
        SwitchSetting(LT("advanced.debugCamera"), s.effects[BbSettings::EffectDebugCamera],
                      LT("advanced.debugCameraHint"), true);
        SwitchSetting(LT("advanced.debugMenu"), s.effects[BbSettings::EffectDebugMenu], LT("advanced.debugMenuHint"));
    }
    EndCard();
    // The upscaler's finer settings: the menu's own (not in the launcher).
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    if (BeginCard("##tuning", T("reactive.section"))) {
        ImGui::BeginDisabled(s.upscaler == BbSettings::UpscalerOff);
        SwitchSetting(T("jitter.label"), s.jitter, T("jitter.hint"), true);
        if (BbSettings::IsFsr4(s.upscaler)) {
            SwitchSetting(T("fsr4.autoExposure"), s.fsr4_auto_exposure, T("fsr4.checkHint"));
            SwitchSetting(T("fsr4.invertJitter"), s.fsr4_invert_jitter, T("fsr4.checkHint"));
        }
        ImGui::BeginDisabled(taa);
        SwitchSetting(T("reactive.enable"), s.reactive, T("reactive.hint"));
        ImGui::BeginDisabled(!s.reactive);
        SliderSetting(T("reactive.scale"), s.reactive_scale, 0.0f, 4.0f, 0.1f, T("reactive.hint"));
        SliderSetting(T("reactive.threshold"), s.reactive_threshold, 0.0f, 1.0f, 0.05f, T("reactive.hint"));
        SliderSetting(T("reactive.max"), s.reactive_max, 0.0f, 1.0f, 0.05f, T("reactive.hint"));
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
    EndCard();
    if (BeginCard("##debug", T("tab.debugViews"))) {
        bool mask = s.debug_view == BbSettings::DebugReactive;
        Row(T("reactive.show"), SwitchWidth(), true);
        if (Switch("##showmask", &mask)) {
            s.debug_view = mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
        }
        Help(T("reactive.hint"));
        bool motion = s.debug_view == BbSettings::DebugMotion;
        Row(T("motion.show"), SwitchWidth());
        if (Switch("##showmotion", &motion)) {
            s.debug_view = motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
        }
        Help(T("motion.showHint"));
        Note(T("effects.debugHint"));
    }
    EndCard();
}

// ---- The menu -----------------------------------------------------------------------------------

void Menu() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(S(760), viewport->WorkSize.x - S(32));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const float max_height = std::min(S(880), viewport->WorkSize.y - S(32));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, max_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    if (!ImGui::Begin("##bbport_menu", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();
    help_text = nullptr;

    // Q/E follow the game's keyboard shoulder bindings; don't steal text while editing a value.
    const bool keys_for_tabs = !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
        (keys_for_tabs && ImGui::IsKeyPressed(ImGuiKey_Q, false))) {
        tab = (tab + TabCount - 1) % TabCount;
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
               (keys_for_tabs && ImGui::IsKeyPressed(ImGuiKey_E, false))) {
        tab = (tab + 1) % TabCount;
    }
    // Circle (B) leaves the menu when nothing is being edited.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive()) {
        SetOpen(false);
    }

    // Header: the title in the launcher's display face, the frame rate in a pill.
    ImGui::PushFont(font_heading, ImGui::GetFontSize() * 1.55f);
    ImGui::TextUnformatted(T("menu.heading"));
    ImGui::PopFont();
    {
        char fps[64];
        std::snprintf(fps, sizeof(fps), "%.0f FPS  %.1f ms",
                      frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);
        const ImVec2 size = ImGui::CalcTextSize(fps);
        const float pad = S(10);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - size.x - pad * 2.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y = p.y + S(6);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y),
                                                  ImVec2(p.x + size.x + pad * 2.0f, y + size.y + S(8)),
                                                  ImGui::GetColorU32(Theme::Muted), S(999));
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad, y + S(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::MutedForeground);
        ImGui::TextUnformatted(fps);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    // The tab row: the chosen tab bright, with a gold line under it.
    const char* tab_names[TabCount] = {T("launcher.graphics.title"), T("launcher.game.title"),
                                       T("launcher.advanced.title")};
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        for (int i = 0; i < TabCount; ++i) {
            if (i) {
                ImGui::SameLine(0.0f, S(22));
            }
            const bool chosen = i == tab;
            ImGui::PushStyleColor(ImGuiCol_Text, chosen ? Theme::Foreground : Theme::MutedForeground);
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
            const ImVec2 size = ImGui::CalcTextSize(tab_names[i]);
            ImGui::PushID(i);
            if (ImGui::Selectable(tab_names[i], chosen, ImGuiSelectableFlags_NoAutoClosePopups, size)) {
                tab = i;
            }
            ImGui::PopID();
            ImGui::PopStyleColor(4);
            if (chosen) {
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x, b.y + S(5)),
                                                          ImVec2(b.x, b.y + S(7)),
                                                          ImGui::GetColorU32(Theme::Gold), S(1));
            }
        }
        const ImVec2 line = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x, line.y + S(4)),
                                            ImVec2(start.x + ImGui::GetContentRegionAvail().x, line.y + S(4)),
                                            ImGui::GetColorU32(Theme::Border));
        ImGui::Dummy(ImVec2(0, S(14)));
    }

    // The tab's cards, scrolling inside when taller than the panel.
    // The cards take their own height, up to what the panel leaves them (then they scroll).
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f + S(40);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(FLT_MAX, max_height - (ImGui::GetCursorPosY() + footer)));
    ImGui::BeginChild("##content", ImVec2(width - S(44), 0), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY);
    switch (tab) {
    case TabGraphics: GraphicsTab(); break;
    case TabGame: GameTab(); break;
    default: AdvancedTab(); break;
    }
    ImGui::EndChild();

    // Footer: the focused option's explanation, the restart notice, the buttons and the hints.
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::MutedForeground);
    ImGui::PushTextWrapPos(0.0f);
    const float help_top = ImGui::GetCursorPosY();
    ImGui::TextUnformatted(help_text                       ? help_text
                           : BbSettings::LauncherChannel() ? T("menu.syncedLauncher")
                                                           : T("menu.savedIn"));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), help_top + ImGui::GetTextLineHeightWithSpacing() * 2.0f));

    const bool restart = RestartNeeded();
    if (restart) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warning);
        ImGui::TextUnformatted(T("restart.note"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    // Buttons, right-aligned: Restart (when needed), Quit, Close.
    static bool quit_armed;
    const char* quit_label = T(quit_armed ? "menu.quitAgain" : "menu.quit");
    const float pad = ImGui::GetStyle().FramePadding.x * 2.0f;
    float buttons = ImGui::CalcTextSize(quit_label).x + ImGui::CalcTextSize(T("menu.close")).x + pad * 2.0f + S(8);
    if (restart) {
        buttons += ImGui::CalcTextSize(T("restart.button")).x + pad + S(8);
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttons));
    if (restart) {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Primary);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::PrimaryHover);
        if (ImGui::Button(T("restart.button"))) {
            BbSettings::Save();
            BbSettings::ExportLauncherEnvironment();
            runtime_restart();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, S(8));
    }
    // Issues #74, #116: leaving the game from the pad. A second press confirms; the window
    // thread then closes like the window's own close button.
    if (ImGui::Button(quit_label)) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemFocused() && !ImGui::IsItemHovered()) {
        quit_armed = false;
    }
    ImGui::SameLine(0.0f, S(8));
    bool close = ImGui::Button(T("menu.close"));

    ImGui::Dummy(ImVec2(0, S(2)));
    const bool keys = keyboard_buttons.load(std::memory_order_relaxed);
    HintItem(keys ? "Enter" : ButtonName("✕", "A"), T("hints.select"));
    HintItem(keys ? "Esc" : ButtonName("○", "B"), T("hints.back"));
    HintItem(keys ? "Q E" : ButtonName("L1 R1", "LB RB"), T("hints.tabs"));
    HintItem(keys ? "F1" : "L3+R3", T("hints.close"));
    ImGui::NewLine();
    ImGui::End();
    if (close) {
        SetOpen(false);
    }
}

// The CPU layer (bloodborne-recomp, linked into bb-probe): weak, so tests without it still load.
extern "C" __attribute__((weak)) size_t bbcpu_recomp_functions(void);
extern "C" __attribute__((weak)) int bbcpu_enabled(void);

/// What runs the game's code: the recompiled library, the translator (x86-64 to arm64 as it runs),
/// the interpreter, or the CPU itself (x86-64 builds without bbcpu).
const char* CpuName() {
    if (bbcpu_recomp_functions && bbcpu_recomp_functions() > 0) {
        return "Recompiled";
    }
    if (bbcpu_enabled && !bbcpu_enabled()) {
        return "x86-64 native";
    }
    const char* jit = std::getenv("BB_JIT");
    return jit && jit[0] == '0' ? "Interpreter" : "Translator";
}

/// The process's CPU time over wall time (100% = one core), sampled once a second (present thread).
float CpuPercent() {
    static double last_wall = 0.0, last_cpu = 0.0;
    static float percent = 0.0f;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (wall - last_wall < 1.0) {
        return percent;
    }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec +
                       usage.ru_stime.tv_usec / 1e6;
    if (last_wall > 0.0) {
        percent = float((cpu - last_cpu) / (wall - last_wall) * 100.0);
    }
    last_wall = wall;
    last_cpu = cpu;
    return percent;
}

/// What draws the frame: the Metal backend's frame path (BB_METAL_FRAME=1), MoltenVK, or Vulkan.
const char* RendererName() {
#ifdef BB_METAL_BACKEND
    return MetalLayer::FrameEnabled() ? "Metal" : "MoltenVK";
#elif defined(__APPLE__)
    return "MoltenVK";
#else
    return "Vulkan";
#endif
}

/// The FPS counter, where the player put it. Small: the frame rate, colored by how healthy it is,
/// and the frame time on one line. Big: also what runs the game (GPU and CPU path), the upscaler
/// and resolutions, and a graph of the last frame times (stutters stand out). Fixed widths, so it
/// never moves as the numbers change.
/// BB_TEST_TITLE (tools/test_run.sh): what a test run is testing, at the top of the screen, so
/// whoever sees the window knows; BB_TEST_DETAIL a second, smaller line. Not in game-frame
/// grabs (only in --screen ones).
const char* TestTitle() {
    static const char* title = [] {
        const char* env = std::getenv("BB_TEST_TITLE");
        return env && env[0] ? env : nullptr;
    }();
    return title;
}

/// The test's clock: how long it has run, or, once the runner wrote the time it will close the game
/// (seconds since 1970) into BB_TEST_CLOCK's file, the time left. Read once a second. Returns the
/// seconds left, or -1 while it counts up.
long long TestClock(char* text, size_t size) {
    static const char* path = std::getenv("BB_TEST_CLOCK");
    static const auto started = std::chrono::steady_clock::now();
    static auto last_read = std::chrono::steady_clock::time_point{};
    static long long end = 0;
    const auto now = std::chrono::steady_clock::now();
    if (path && path[0] && now - last_read >= std::chrono::seconds(1)) {
        last_read = now;
        if (FILE* file = std::fopen(path, "r")) {
            long long value = 0;
            end = std::fscanf(file, "%lld", &value) == 1 ? value : 0;
            std::fclose(file);
        }
    }
    if (end > 0) {
        const long long left = std::max(0LL, end - (long long)std::time(nullptr));
        std::snprintf(text, size, "closes in %lld:%02lld", left / 60, left % 60);
        return left;
    }
    const long long ran = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
    std::snprintf(text, size, "%lld:%02lld", ran / 60, ran % 60);
    return -1;
}

/// Text wrapped at `wrap`, each line centered in the window (an auto-sized window is as wide as its
/// widest line, known from the frame before).
void CenteredText(const char* text, float wrap) {
    const float row = ImGui::GetContentRegionAvail().x;
    const float left = ImGui::GetCursorPosX();
    const char* const text_end = text + std::strlen(text);
    for (const char* line = text; line < text_end;) {
        const char* end = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), line, text_end, wrap);
        if (end == line) {
            ++end; // a word wider than the wrap: at least one character a line
        }
        const float width = ImGui::CalcTextSize(line, end).x;
        ImGui::SetCursorPosX(left + std::max(0.0f, (row - width) * 0.5f));
        ImGui::TextUnformatted(line, end);
        line = end;
        while (line < text_end && (*line == ' ' || *line == '\n')) {
            ++line;
        }
    }
}

void TestBanner() {
    static const char* detail = [] {
        const char* env = std::getenv("BB_TEST_DETAIL");
        return env && env[0] ? env : nullptr;
    }();
    char clock[32];
    const long long left = TestClock(clock, sizeof(clock));
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + S(14)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(Theme::Radius));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(8)));
    ImGui::Begin("##test", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    // Every line centered: the title, the detail (wrapped at 60% of the screen) and the clock.
    const float wrap = viewport->WorkSize.x * 0.6f;
    ImGui::PushFont(font_heading, ImGui::GetFontSize() * 1.2f);
    CenteredText(TestTitle(), wrap);
    ImGui::PopFont();
    if (detail) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::MutedForeground);
        CenteredText(detail, wrap);
        ImGui::PopStyleColor();
    }
    // The clock: light while it counts up, amber counting down, red in the last 10 seconds.
    ImGui::PushStyleColor(ImGuiCol_Text, left < 0 ? Theme::Foreground : left > 10 ? Theme::Warning : Theme::PrimaryHover);
    CenteredText(clock, wrap);
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// The online status line, centered above the bottom of the screen while there is one.
void OnlineBanner() {
    std::string message;
    {
        std::scoped_lock lock{banner_mutex};
        message = banner_text;
    }
    if (message.empty()) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - S(80)),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.86f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(Theme::Radius));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(12)));
    ImGui::Begin("##online_status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(message.c_str());
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void FpsCounter() {
    const auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool big = s.fps_size == BbSettings::FpsBig;
    const int position = std::clamp(s.fps_position.load(), 0, BbSettings::FpsPositionCount - 1);
    const float pad = S(14);
    const float column = float(position % 3) * 0.5f; // left, center, right
    const bool bottom = position >= BbSettings::FpsBottomLeft;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + pad + (viewport->WorkSize.x - pad * 2.0f) * column,
               bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(column, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(big ? 0.78f : 0.6f);
    const float panel = big ? ImGui::CalcTextSize("FSR 4.1.2 Lite  3840 x 2160 > 3840 x 2160").x + S(24)
                            : ImGui::CalcTextSize("000 FPS  000.0 ms").x + S(16);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panel, 0.0f), ImVec2(panel, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(big ? Theme::Radius : 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, big ? ImVec2(S(12), S(8)) : ImVec2(S(8), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(2)));
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    if (position == BbSettings::FpsTopRight) {
        fps_top_right_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    }
    const float fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    const ImVec4 health = fps >= 55.0f ? Theme::Foreground : fps >= 30.0f ? Theme::Warning : Theme::PrimaryHover;
    // The number right-aligned in a three-digit slot: the label after it stays in place.
    if (big) {
        ImGui::PushFont(font_text, ImGui::GetFontSize() * 1.45f);
    }
    char number[16];
    std::snprintf(number, sizeof(number), "%.0f", std::min(fps, 999.0f));
    const float slot = ImGui::CalcTextSize("000").x;
    const float left = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(left + slot - ImGui::CalcTextSize(number).x);
    ImGui::PushStyleColor(ImGuiCol_Text, health);
    ImGui::TextUnformatted(number);
    ImGui::PopStyleColor();
    if (big) {
        ImGui::PopFont();
    }
    ImGui::SameLine(left + slot + S(big ? 8 : 4));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::MutedForeground);
    if (big) {
        ImGui::AlignTextToFramePadding();
    }
    ImGui::Text("FPS  %.1f ms", frame_ms_avg);
    if (!big) {
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }
    const char* upscaler = s.upscaler == BbSettings::UpscalerFsr3        ? "FSR 3.1"
                           : s.upscaler == BbSettings::UpscalerFsr4      ? "FSR 4"
                           : s.upscaler == BbSettings::UpscalerFsr411    ? "FSR 4.1"
                           : s.upscaler == BbSettings::UpscalerFsr411Lossy ? "FSR 4.1 Lossy"
                           : s.upscaler == BbSettings::UpscalerFsr412    ? "FSR 4.1.2 Lite"
                           : s.upscaler == BbSettings::UpscalerTaa       ? "TAA"
                           : s.upscaler == BbSettings::UpscalerDlss      ? "DLSS"
                           : s.upscaler == BbSettings::UpscalerMetalFx   ? "MetalFX"
                                                                         : nullptr;
    const auto output = BbSettings::CurrentOutput();
    // What runs it: the GPU path and the CPU path.
    ImGui::Text("%s  ·  %s", RendererName(), CpuName());
    // CPU use and the hottest die sensor (amber from 70 °C, red from 85 °C); without sensors, the
    // system's thermal state (amber when it throttles).
    ImGui::Text("CPU %.0f%%", CpuPercent());
    const float celsius = Temperature();
    const int thermal = ThermalState();
    const char* name = ThermalName(thermal);
    if (celsius > 0.0f || name) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted("  ·  ");
        ImGui::SameLine(0.0f, 0.0f);
        if (celsius > 0.0f) {
            ImGui::PushStyleColor(ImGuiCol_Text, celsius >= 85.0f   ? Theme::PrimaryHover
                                                 : celsius >= 70.0f ? Theme::Warning
                                                                    : Theme::MutedForeground);
            ImGui::Text("%.0f °C", celsius);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, thermal >= 2   ? Theme::PrimaryHover
                                                 : thermal == 1 ? Theme::Warning
                                                                : Theme::MutedForeground);
            ImGui::Text("thermal %s", name);
        }
        ImGui::PopStyleColor();
    }
    if (upscaler) {
        ImGui::Text("%s  %d x %d > %d x %d", upscaler, s.active_render_width.load(), s.active_render_height.load(),
                    output.width, output.height);
    } else {
        ImGui::Text("%d x %d", output.width, output.height);
    }
    ImGui::PopStyleColor();
    // Frame times, newest on the right: bars above 1.5x the average in amber, 2x+ in red.
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = S(26);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float top = 0.0f;
    for (float ms : frame_ms_history) top = std::max(top, ms);
    top = std::max(top, frame_ms_avg * 2.0f);
    const float bar = width / FrameHistory;
    for (int i = 0; i < FrameHistory; ++i) {
        const float ms = frame_ms_history[(frame_ms_next + i) % FrameHistory];
        if (ms <= 0.0f || top <= 0.0f) continue;
        const float h = std::max(S(1), height * std::min(ms / top, 1.0f));
        const ImVec4 color = ms > frame_ms_avg * 2.0f   ? Theme::PrimaryHover
                             : ms > frame_ms_avg * 1.5f ? Theme::Warning
                                                        : ImVec4(Theme::MutedForeground.x, Theme::MutedForeground.y,
                                                                 Theme::MutedForeground.z, 0.7f);
        draw->AddRectFilled(ImVec2(a.x + i * bar, a.y + height - h), ImVec2(a.x + (i + 1) * bar - S(0.5f), a.y + height),
                            ImGui::GetColorU32(color));
    }
    ImGui::Dummy(ImVec2(width, height));
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ---- The on-screen keyboard (the game's text dialog) --------------------------------------------

/// Keys: letters (with Shift) and symbols pages; the bottom row's commands span several columns.
enum KeyCommand { KeyShift = 1, KeyPage, KeySpace, KeyDelete, KeyDone };
struct KeyboardState {
    int row = 1, column = 0;
    bool shift = false, symbols = false;
    float stick_x = 0.0f, stick_y = 0.0f; // the left stick, for one step per push
} keyboard;
struct KeyboardKeyHit {
    ImVec2 min{}, max{};
    int row = 0, column = 0;
};
std::array<KeyboardKeyHit, 50> keyboard_hits{};
int keyboard_hit_count = 0; // rebuilt with the prompt's current size and position each frame
constexpr int KeyColumns = 10, KeyRows = 5;
const char* const LetterRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"};
const char* const SymbolRows[4] = {"!@#$%^&*()", "-_=+[]{};:", "\"/?<>|\\~`'", ",.!?&#*-_+"};
// The bottom row: Shift (2 columns), page (2), Space (3), Delete (1+), Done (2).
constexpr int BottomCommands[KeyColumns] = {KeyShift, KeyShift, KeyPage, KeyPage, KeySpace,
                                            KeySpace, KeySpace, KeyDelete, KeyDone, KeyDone};

void Edit(TextEdit edit, const char* text = nullptr) {
    if (text_editor) {
        text_editor(edit, text, text_editor_context);
    }
}

void PressKey(int row, int column) {
    if (row < 4) {
        const char c = (keyboard.symbols ? SymbolRows : LetterRows)[row][column];
        char typed[2] = {keyboard.shift && !keyboard.symbols ? char(std::toupper(static_cast<unsigned char>(c))) : c, 0};
        Edit(TextEdit::Append, typed);
        keyboard.shift = false;
        return;
    }
    switch (BottomCommands[column]) {
    case KeyShift: keyboard.shift = !keyboard.shift; break;
    case KeyPage: keyboard.symbols = !keyboard.symbols; break;
    case KeySpace: Edit(TextEdit::Append, " "); break;
    case KeyDelete: Edit(TextEdit::Backspace); break;
    case KeyDone: Edit(TextEdit::Done); break;
    }
}

void MoveKey(int dx, int dy) {
    if (dy) {
        keyboard.row = (keyboard.row + dy + KeyRows) % KeyRows;
    }
    if (dx) {
        if (keyboard.row == 4) { // jump over the rest of a wide key
            const int command = BottomCommands[keyboard.column];
            int c = keyboard.column;
            do {
                c = (c + dx + KeyColumns) % KeyColumns;
            } while (BottomCommands[c] == command && c != keyboard.column);
            // land on the first column of the wide key
            while (c > 0 && BottomCommands[c - 1] == BottomCommands[c]) --c;
            keyboard.column = c;
        } else {
            keyboard.column = (keyboard.column + dx + KeyColumns) % KeyColumns;
        }
    }
}

/// Window thread, under imgui_mutex: a pad event while the text dialog is open.
bool KeyboardEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        const float value = float(event.gaxis.value) / 32767.0f;
        float& last = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? keyboard.stick_x
                      : event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? keyboard.stick_y
                                                                 : keyboard.stick_x;
        if (event.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && event.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) {
            return true;
        }
        const int was = last > 0.6f ? 1 : last < -0.6f ? -1 : 0;
        const int now = value > 0.6f ? 1 : value < -0.6f ? -1 : 0;
        last = value;
        if (now != 0 && now != was) {
            event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? MoveKey(now, 0) : MoveKey(0, now);
        }
        return true;
    }
    if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        return event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    switch (event.gbutton.button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: MoveKey(0, -1); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: MoveKey(0, 1); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: MoveKey(-1, 0); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: MoveKey(1, 0); break;
    case SDL_GAMEPAD_BUTTON_SOUTH: PressKey(keyboard.row, keyboard.column); break;
    case SDL_GAMEPAD_BUTTON_WEST: Edit(TextEdit::Backspace); break;
    case SDL_GAMEPAD_BUTTON_NORTH: Edit(TextEdit::Append, " "); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: keyboard.shift = !keyboard.shift; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: keyboard.symbols = !keyboard.symbols; break;
    case SDL_GAMEPAD_BUTTON_START: Edit(TextEdit::Done); break;
    case SDL_GAMEPAD_BUTTON_EAST: Edit(TextEdit::Cancel); break;
    default: break;
    }
    return true;
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float key = std::min(S(52), (viewport->WorkSize.x - S(80)) / KeyColumns);
    const float gap = S(5);
    const float board = key * KeyColumns + gap * (KeyColumns - 1);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(18)));
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    ImGui::PushFont(font_heading, ImGui::GetFontSize() * 1.25f);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(4)));
    // The text field.
    {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + board, a.y + ImGui::GetFrameHeight() + S(10));
        draw->AddRectFilled(a, b, ImGui::GetColorU32(Theme::Card), S(10));
        draw->AddRect(a, b, ImGui::GetColorU32(Theme::Border), S(10));
        const bool caret = (int(ImGui::GetTime() * 2.0) & 1) == 0;
        const std::string shown = text + (caret ? "|" : " ");
        draw->AddText(ImVec2(a.x + S(12), a.y + (b.y - a.y - ImGui::GetTextLineHeight()) * 0.5f),
                      ImGui::GetColorU32(Theme::Foreground), shown.c_str());
        ImGui::Dummy(ImVec2(board, b.y - a.y));
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    // The keys. Keep the hit rectangles in screen pixels for HandleEvent on the window thread;
    // drawing and event delivery both hold imgui_mutex.
    keyboard_hit_count = 0;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const char* const* rows = keyboard.symbols ? SymbolRows : LetterRows;
    for (int r = 0; r < KeyRows; ++r) {
        for (int c = 0; c < KeyColumns; ++c) {
            int span = 1;
            std::string label;
            if (r < 4) {
                const char ch = rows[r][c];
                label.assign(1, keyboard.shift && !keyboard.symbols ? char(std::toupper(static_cast<unsigned char>(ch))) : ch);
            } else {
                if (c > 0 && BottomCommands[c - 1] == BottomCommands[c]) {
                    continue; // drawn with its first column
                }
                while (c + span < KeyColumns && BottomCommands[c + span] == BottomCommands[c]) ++span;
                switch (BottomCommands[c]) {
                case KeyShift: label = T("keyboard.shift"); break;
                case KeyPage: label = T(keyboard.symbols ? "keyboard.letters" : "keyboard.symbols"); break;
                case KeySpace: label = T("keyboard.space"); break;
                case KeyDelete: label = T("keyboard.delete"); break;
                case KeyDone: label = T("keyboard.done"); break;
                }
            }
            const ImVec2 a(origin.x + c * (key + gap), origin.y + r * (key + gap));
            const ImVec2 b(a.x + key * span + gap * (span - 1), a.y + key);
            keyboard_hits[keyboard_hit_count++] = {a, b, r, c};
            const bool selected = r == keyboard.row &&
                                  (r < 4 ? c == keyboard.column
                                         : BottomCommands[c] == BottomCommands[keyboard.column]);
            const bool lit = (r == 4 && BottomCommands[c] == KeyShift && keyboard.shift) ||
                             (r == 4 && BottomCommands[c] == KeyDone);
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool hovered = mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
            draw->AddRectFilled(a, b,
                                ImGui::GetColorU32(selected ? Theme::Primary
                                                   : hovered ? Theme::AccentHover
                                                   : lit    ? Theme::AccentHover
                                                            : Theme::Accent),
                                S(8));
            if (selected) {
                draw->AddRect(a, b, ImGui::GetColorU32(Theme::Gold), S(8), 0, S(2));
            }
            const ImVec2 size = ImGui::CalcTextSize(label.c_str());
            draw->AddText(ImVec2((a.x + b.x - size.x) * 0.5f, (a.y + b.y - size.y) * 0.5f),
                          ImGui::GetColorU32(selected ? Theme::PrimaryForeground : Theme::Foreground),
                          label.c_str());
        }
    }
    ImGui::Dummy(ImVec2(board, KeyRows * key + (KeyRows - 1) * gap + S(10)));
    // Hints: the controller's buttons, and the keyboard's.
    HintItem(ButtonName("✕", "A"), T("hints.select"));
    HintItem(ButtonName("□", "X"), T("keyboard.delete"));
    HintItem(ButtonName("△", "Y"), T("keyboard.space"));
    HintItem(ButtonName("R1", "RB"), T("keyboard.shift"));
    HintItem(ButtonName("L1", "LB"), T("keyboard.symbols"));
    ImGui::NewLine();
    HintItem(ButtonName("OPTIONS", "MENU"), T("keyboard.done"));
    HintItem(ButtonName("○", "B"), T("keyboard.cancel"));
    HintItem(T("keyboard.keys"), T("keyboard.keysHint"));
    ImGui::NewLine();
    ImGui::End();
}

/// BB_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1,
/// consumed when the file appears (it is removed), one key press per frame.
void ScriptedKeys() {
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point last_check{};
    static std::vector<std::string> queue;
    static bool release = false;
    static ImGuiKey held = ImGuiKey_None;
    if (!path) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (release) {
        io.AddKeyEvent(held, false);
        release = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (queue.empty() && now - last_check > std::chrono::milliseconds(100)) {
        last_check = now;
        if (FILE* f = std::fopen(path, "r")) {
            char token[32];
            while (std::fscanf(f, "%31s", token) == 1) {
                queue.emplace_back(token);
            }
            std::fclose(f);
            std::remove(path);
        }
    }
    if (queue.empty()) {
        return;
    }
    const std::string token = queue.front();
    queue.erase(queue.begin());
    if (token == "toggle") {
        SetOpen(!menu_open);
        return;
    }
    held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (held != ImGuiKey_None) {
        io.AddKeyEvent(held, true);
        release = true;
    }
}

void ApplyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 18.0f;
    style.ChildRounding = Theme::Radius;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.FramePadding = ImVec2(12.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.GrabMinSize = 14.0f;
    style.ScrollbarSize = 10.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Theme::Foreground;
    c[ImGuiCol_TextDisabled] = Theme::MutedForeground;
    c[ImGuiCol_WindowBg] = Theme::Background;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = Theme::Card;
    c[ImGuiCol_Border] = Theme::Border;
    c[ImGuiCol_FrameBg] = Theme::Accent;
    c[ImGuiCol_FrameBgHovered] = Theme::AccentHover;
    c[ImGuiCol_FrameBgActive] = Theme::AccentHover;
    c[ImGuiCol_SliderGrab] = Theme::Primary;
    c[ImGuiCol_SliderGrabActive] = Theme::PrimaryHover;
    c[ImGuiCol_Button] = Theme::Accent;
    c[ImGuiCol_ButtonHovered] = Theme::AccentHover;
    c[ImGuiCol_ButtonActive] = Theme::Primary;
    c[ImGuiCol_Header] = Theme::Accent;
    c[ImGuiCol_HeaderHovered] = Theme::AccentHover;
    c[ImGuiCol_HeaderActive] = Theme::Primary;
    c[ImGuiCol_Separator] = Theme::Border;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = Theme::Accent;
    c[ImGuiCol_ScrollbarGrabHovered] = Theme::AccentHover;
    c[ImGuiCol_ScrollbarGrabActive] = Theme::Primary;
    c[ImGuiCol_CheckMark] = Theme::PrimaryForeground;
    c[ImGuiCol_NavCursor] = Theme::Gold;
}

void TrophyToast(std::chrono::steady_clock::time_point now) {
    const BbTrophyQueue::Notice* notice = trophy_queue.Update(now);
    if (!notice) return;

    const TrophyText* text = TrophyTextFor(notice->id, BbText::Language());
    const TrophyText* english = TrophyTextFor(notice->id, "en");
    if (!text || !english) return;
    const char* description = text->description[0] ? text->description : english->description;
    const ImVec4 metal = std::strcmp(text->grade, "platinum") == 0 ? Theme::Hex(0xcfdee4)
                         : std::strcmp(text->grade, "gold") == 0 ? Theme::Hex(0xe8c976)
                         : std::strcmp(text->grade, "silver") == 0 ? Theme::Hex(0xd7dbdd)
                         : Theme::Hex(0xce9e78);
    const float elapsed = std::chrono::duration<float>(now - trophy_queue.Started()).count();
    const float left = std::chrono::duration<float>(trophy_queue.Until() - now).count();
    const float alpha = std::clamp(std::min(elapsed / 0.3f, left / 0.3f), 0.0f, 1.0f);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(S(420), viewport->WorkSize.x - S(24));
    // Below the FPS counter when it is in the same corner (its height depends on its size).
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - S(14),
                                   fps_top_right_bottom > 0.0f ? fps_top_right_bottom + S(10)
                                                               : viewport->WorkPos.y + S(14)),
                            ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Card);
    ImGui::PushStyleColor(ImGuiCol_Border, metal);
    ImGui::Begin("##trophy_notice", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 badge = ImGui::GetCursorScreenPos();
    const float cx = badge.x + S(23), cy = badge.y + S(27);
    const ImU32 metal_color = ImGui::GetColorU32(metal);
    draw->AddCircleFilled(ImVec2(cx, cy), S(22), metal_color);
    draw->AddCircleFilled(ImVec2(cx, cy), S(18), ImGui::GetColorU32(Theme::Card));
    // A cup silhouette for every grade; the metallic tint comes from the trophy's grade.
    draw->AddBezierCubic(ImVec2(cx - S(10), cy - S(9)), ImVec2(cx - S(22), cy - S(13)),
                         ImVec2(cx - S(18), cy + S(3)), ImVec2(cx - S(8), cy + S(3)),
                         metal_color, S(2.5f));
    draw->AddBezierCubic(ImVec2(cx + S(10), cy - S(9)), ImVec2(cx + S(22), cy - S(13)),
                         ImVec2(cx + S(18), cy + S(3)), ImVec2(cx + S(8), cy + S(3)),
                         metal_color, S(2.5f));
    const ImVec2 cup[] = {{cx - S(11), cy - S(11)}, {cx + S(11), cy - S(11)},
                          {cx + S(7), cy + S(3)}, {cx - S(7), cy + S(3)}};
    draw->AddConvexPolyFilled(cup, 4, metal_color);
    draw->AddRectFilled(ImVec2(cx - S(2), cy + S(3)), ImVec2(cx + S(2), cy + S(12)), metal_color);
    draw->AddRectFilled(ImVec2(cx - S(8), cy + S(12)), ImVec2(cx + S(8), cy + S(15)), metal_color, S(1));
    ImGui::Dummy(ImVec2(S(54), S(55)));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, metal);
    ImGui::TextUnformatted(T(notice->preview ? "trophies.preview" : "trophies.earned"));
    ImGui::PopStyleColor();
    ImGui::PushFont(font_heading, ImGui::GetFontSize() * 1.1f);
    ImGui::TextWrapped("%s", text->name);
    ImGui::PopFont();
    if (description[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::MutedForeground);
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, metal);
    ImGui::Text("%s · %s", T(std::string("trophies.") + text->grade),
                T(std::string("trophies.") + text->group));
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

} // namespace

void SetBanner(const char* message) {
    std::scoped_lock lock{banner_mutex};
    banner_text = message ? message : "";
}

void TrophyUnlocked(const char* title, int id) {
    if (!title || std::strcmp(title, "CUSA03173") || id < 0 || id >= 40) return;
    const char* off = std::getenv("BB_TROPHY_OVERLAY");
    if (off && std::strcmp(off, "0") == 0) return;
    QueueTrophy(id, false);
}

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    if (active && !prompt_active) {
        keyboard = KeyboardState{}; // each dialog starts on the letters, at Q
    }
    prompt_active = active;
}

void SetTextEditor(void (*edit)(TextEdit, const char*, void*), void* context) {
    text_editor = edit;
    text_editor_context = context;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";
    ApplyTheme();

    // Text: Inter, then DejaVu for the scripts Inter lacks; headings: Cinzel (Latin capitals),
    // then Inter for the rest. CJK: a system font merged into both.
    const auto add = [&](const unsigned char* begin, const unsigned char* end, bool merge) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.MergeMode = merge;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(begin), int(end - begin), 18.0f,
                                              &config);
    };
    const auto add_cjk = [&]() {
        if (!BbText::NeedsCjkFont()) {
            return;
        }
        const std::string_view language = BbText::Language();
        std::vector<const char*> candidates;
        if (language == "ja") {
            candidates = {"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"};
        } else if (language == "ko") {
            candidates = {"/System/Library/Fonts/AppleSDGothicNeo.ttc",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"};
        } else if (language == "zh-TW") {
            candidates = {"/System/Library/Fonts/STHeiti Medium.ttc",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"};
        } else {
            candidates = {"/System/Library/Fonts/Hiragino Sans GB.ttc",
                          "/System/Library/Fonts/STHeiti Medium.ttc",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"};
        }
        ImFontConfig cjk_config;
        cjk_config.MergeMode = true;
        for (const char* path : candidates) {
            std::error_code error;
            if (std::filesystem::exists(path, error) && io.Fonts->AddFontFromFileTTF(path, 18.0f, &cjk_config)) {
                return;
            }
        }
        std::fprintf(stderr, "Menu: no %s font found, its text shows as boxes\n", BbText::Language());
    };
    font_text = add(bb_font_text_ttf, bb_font_text_ttf_end, false);
    add(bb_font_ttf, bb_font_ttf_end, true);
    add_cjk();
    font_heading = add(bb_font_heading_ttf, bb_font_heading_ttf_end, false);
    add(bb_font_text_ttf, bb_font_text_ttf_end, true);
    add(bb_font_ttf, bb_font_ttf_end, true);
    add_cjk();
    io.FontDefault = font_text;

#ifdef BB_METAL_BACKEND
    if (MetalLayer::FrameEnabled()) {
        // The Metal frame's presenter draws the overlay with ImGui's Metal backend.
        if (!MetalBackend::Init(MetalLayer::MtlDevice())) {
            std::printf("Overlay: ImGui Metal backend init failed\n");
            ImGui::DestroyContext();
            return;
        }
        initialized = true;
        std::printf("Overlay: menu ready (F1 or L3+R3), drawn through Metal\n");
        return;
    }
#endif
    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    const char* notices_off = std::getenv("BB_TROPHY_OVERLAY");
    if (const char* preview = std::getenv("BB_TROPHY_OVERLAY_PREVIEW");
        preview && std::strcmp(preview, "1") == 0 &&
        (!notices_off || std::strcmp(notices_off, "0") != 0)) {
        for (int id : {21, 7, 1, 0, 35})
            QueueTrophy(id, true);
    }
#ifdef __APPLE__
    std::printf("Overlay: menu ready (F1 or L3+R3)\n");
#else
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
#endif
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

void ApplyWindowSettings(SDL_Window* window) {
    if (const int fullscreen = BbSettings::TakeFullscreenRequest(); fullscreen >= 0) {
        SDL_SetWindowFullscreen(window, fullscreen == 1);
    }
    if (displays_wanted.exchange(false)) {
        std::vector<std::string> names;
        int count = 0;
        if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
            for (int i = 0; i < count; ++i) {
                if (const char* name = SDL_GetDisplayName(ids[i]); name && *name) {
                    names.emplace_back(name);
                }
            }
            SDL_free(ids);
        }
        std::scoped_lock lock{displays_mutex};
        display_names = std::move(names);
    }
}

bool HandleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_DISPLAY_ADDED || event.type == SDL_EVENT_DISPLAY_REMOVED) {
        displays_wanted = true;
    }
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    // The connected pad's family, for the button hints.
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_ADDED) {
        const SDL_JoystickID id = event.type == SDL_EVENT_GAMEPAD_ADDED ? event.gdevice.which : event.gbutton.which;
        const SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
        xbox_buttons = type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE;
    }
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down) keyboard_buttons = true;
        if (down && !event.key.repeat &&
            (event.key.key == MenuKey || event.key.key == SDLK_INSERT ||
             (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == MenuKey || event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        // The text dialog's keyboard follows the left stick too.
        return prompt_active && !is_open ? KeyboardEvent(event) : false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (down) keyboard_buttons = false;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            // The game's text dialog: the pad types on the on-screen keyboard.
            return prompt_active ? KeyboardEvent(event) : false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open && !prompt_active) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        keyboard_buttons = true;
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open && !prompt_active) {
            return false;
        }
        if (prompt_active && !is_open) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    Edit(TextEdit::Cancel);
                } else if (event.button.button == SDL_BUTTON_LEFT) {
                    const float density = PixelDensity(event.button.windowID);
                    const float x = event.button.x * density, y = event.button.y * density;
                    for (int i = 0; i < keyboard_hit_count; ++i) {
                        const auto& hit = keyboard_hits[i];
                        if (x >= hit.min.x && x < hit.max.x && y >= hit.min.y && y < hit.max.y) {
                            keyboard.row = hit.row;
                            keyboard.column = hit.column;
                            PressKey(hit.row, hit.column);
                            break;
                        }
                    }
                }
            }
            return true;
        }
        keyboard_buttons = true;
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        keyboard_buttons = true;
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    // BB_MENU_TRIGGER: creating that file opens (or closes) the menu, for screenshots of a game
    // running in the background (tools/grab.sh --screen).
    static const char* menu_trigger = std::getenv("BB_MENU_TRIGGER");
    if (initialized && menu_trigger && std::remove(menu_trigger) == 0) {
        SetOpen(!menu_open);
    }
    if (!initialized) return false;
    std::scoped_lock lock{banner_mutex};
    return menu_open || prompt_active || BbSettings::Get().show_fps || trophy_queue.Pending() ||
           TestTitle() || !banner_text.empty();
}


int ThermalState() {
#ifdef __APPLE__
    // [[NSProcessInfo processInfo] thermalState]: no autoreleased objects, safe on any thread.
    static const Class info_class = objc_getClass("NSProcessInfo");
    if (!info_class) {
        return -1;
    }
    const auto send = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    const auto state = reinterpret_cast<long (*)(id, SEL)>(objc_msgSend);
    const id info = send(reinterpret_cast<id>(info_class), sel_registerName("processInfo"));
    return info ? int(state(info, sel_registerName("thermalState"))) : -1;
#else
    return -1;
#endif
}

#ifdef __APPLE__
namespace {
// IOKit's HID event system (private, loaded at run time): the temperature sensors' services
// (usage page 0xff00, usage 5) and their temperature events, as system monitors read them.
using HidClient = void*;
struct HidApi {
    HidClient (*create)(CFAllocatorRef);
    int (*set_matching)(HidClient, CFDictionaryRef);
    CFArrayRef (*copy_services)(HidClient);
    CFTypeRef (*copy_event)(void* service, int64_t type, int32_t options, int64_t timestamp);
    double (*float_value)(CFTypeRef event, int32_t field);
    CFTypeRef (*copy_property)(void* service, CFStringRef key);
};

bool LoadHid(HidApi& api) {
    void* iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!iokit) {
        return false;
    }
    api.create = reinterpret_cast<decltype(api.create)>(dlsym(iokit, "IOHIDEventSystemClientCreate"));
    api.set_matching =
        reinterpret_cast<decltype(api.set_matching)>(dlsym(iokit, "IOHIDEventSystemClientSetMatching"));
    api.copy_services =
        reinterpret_cast<decltype(api.copy_services)>(dlsym(iokit, "IOHIDEventSystemClientCopyServices"));
    api.copy_event = reinterpret_cast<decltype(api.copy_event)>(dlsym(iokit, "IOHIDServiceClientCopyEvent"));
    api.float_value = reinterpret_cast<decltype(api.float_value)>(dlsym(iokit, "IOHIDEventGetFloatValue"));
    api.copy_property =
        reinterpret_cast<decltype(api.copy_property)>(dlsym(iokit, "IOHIDServiceClientCopyProperty"));
    return api.create && api.set_matching && api.copy_services && api.copy_event && api.float_value &&
           api.copy_property;
}

CFDictionaryRef TemperatureMatching() {
    int page = 0xff00, usage = 5;
    CFNumberRef numbers[] = {CFNumberCreate(nullptr, kCFNumberIntType, &page),
                             CFNumberCreate(nullptr, kCFNumberIntType, &usage)};
    const void* keys[] = {CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage")};
    CFDictionaryRef matching = CFDictionaryCreate(nullptr, keys, reinterpret_cast<const void**>(numbers), 2,
                                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(numbers[0]);
    CFRelease(numbers[1]);
    return matching;
}

/// The hottest die sensor ("tdie" in its name: the CPU and GPU dies), else the hottest "SOC" or
/// "PMU" one, in °C; 0 when there is none. The first call prints what it found.
float ReadTemperature(const HidApi& api, HidClient client) {
    constexpr int64_t TemperatureEvent = 15;
    CFArrayRef services = api.copy_services(client);
    if (!services) {
        return 0.0f;
    }
    float die = 0.0f, soc = 0.0f;
    int dies = 0, socs = 0;
    const CFIndex count = CFArrayGetCount(services);
    for (CFIndex i = 0; i < count; ++i) {
        void* service = const_cast<void*>(CFArrayGetValueAtIndex(services, i));
        char name[128] = "";
        if (CFTypeRef product = api.copy_property(service, CFSTR("Product"))) {
            if (CFGetTypeID(product) == CFStringGetTypeID()) {
                CFStringGetCString(static_cast<CFStringRef>(product), name, sizeof(name), kCFStringEncodingUTF8);
            }
            CFRelease(product);
        }
        CFTypeRef event = api.copy_event(service, TemperatureEvent, 0, 0);
        if (!event) {
            continue;
        }
        const float value = float(api.float_value(event, int32_t(TemperatureEvent << 16)));
        CFRelease(event);
        if (value <= 0.0f || value >= 150.0f) {
            continue;
        }
        if (std::strstr(name, "tdie")) {
            die = std::max(die, value);
            ++dies;
        } else if (std::strstr(name, "SOC") || std::strstr(name, "PMU")) {
            soc = std::max(soc, value);
            ++socs;
        }
    }
    CFRelease(services);
    static bool printed = false;
    if (!printed) {
        printed = true;
        std::printf("Temperature: %ld sensors, %d die and %d SoC/PMU ones read\n", long(count), dies, socs);
    }
    return dies ? die : soc;
}

std::atomic<float> temperature{0.0f};
} // namespace
#endif

float Temperature() {
#ifdef __APPLE__
    // A thread of its own, every 2 s: reading every sensor takes milliseconds.
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            HidApi api{};
            if (!LoadHid(api)) {
                std::printf("Temperature: IOKit's HID event system is not available\n");
                return;
            }
            HidClient client = api.create(kCFAllocatorDefault);
            if (!client) {
                return;
            }
            CFDictionaryRef matching = TemperatureMatching();
            api.set_matching(client, matching);
            CFRelease(matching);
            for (;;) {
                temperature.store(ReadTemperature(api, client), std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }).detach();
    });
    return temperature.load(std::memory_order_relaxed);
#else
    return 0.0f;
#endif
}

const char* ThermalName(int state) {
    static const char* const names[] = {"nominal", "fair", "serious", "critical"};
    return state >= 0 && state < 4 ? names[state] : nullptr;
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || prompt_active;
}

namespace {
/// Every frame: the present interval for the FPS readout (measured also while nothing is drawn),
/// scripted keys and the live effects. Whether anything is drawn.
bool BeginFrame(float& ms, std::chrono::steady_clock::time_point& now) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    now = std::chrono::steady_clock::now();
    ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
        frame_ms_history[frame_ms_next] = ms;
        frame_ms_next = (frame_ms_next + 1) % FrameHistory;
    }
    if (std::getenv("BB_MENU_KEYS_FILE") && initialized) {
        std::scoped_lock lock{imgui_mutex};
        ScriptedKeys();
    }
    // Effects changed in the menu or by the launcher reach the game (bbport 0.6's live effects).
    BbGameHooks::ApplyGameEffects();
    return Visible();
}

/// Under imgui_mutex: the display size and scale, saving changes.
void SetupFrame(float ms, std::chrono::steady_clock::time_point now, u32 width, u32 height) {
    // Changes are saved 0.3 s after the last one (and on close): a crash keeps them.
    if (dirty && now - dirty_since > std::chrono::milliseconds(300)) {
        dirty = false;
        BbSettings::Save();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1). BB_UI_SCALE multiplies it: a small screen
    // seen close (a phone: the iOS proof of concept sets 1.75) needs more than its pixel height.
    static const float ui_factor = [] {
        const char* env = std::getenv("BB_UI_SCALE");
        const float f = env ? std::strtof(env, nullptr) : 1.0f;
        return f >= 0.5f && f <= 4.0f ? f : 1.0f;
    }();
    const float scale = std::max(float(height) / 1080.0f, 0.75f) * ui_factor;
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }
}

/// Under imgui_mutex, after the backend's new frame: the menu, FPS counter, prompt and notices.
void BuildUi(std::chrono::steady_clock::time_point now) {
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    fps_top_right_bottom = 0.0f;
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    TrophyToast(now);
    OnlineBanner();
    if (TestTitle() && !menu_open) {
        TestBanner();
    }
    ImGui::Render();
}
} // namespace

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    float ms{};
    std::chrono::steady_clock::time_point now;
    if (!BeginFrame(ms, now)) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    SetupFrame(ms, now, extent.width, extent.height);
    ImGui_ImplVulkan_NewFrame();
    BuildUi(now);

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

#ifdef BB_METAL_BACKEND
void RenderMetal(void* command_buffer, void* target, u32 width, u32 height) {
    float ms{};
    std::chrono::steady_clock::time_point now;
    if (!BeginFrame(ms, now) || !initialized) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    SetupFrame(ms, now, width, height);
    MetalBackend::Frame(command_buffer, target, [now] { BuildUi(now); });
}
#endif

} // namespace BbOverlay
