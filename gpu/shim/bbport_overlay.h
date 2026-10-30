// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Window thread, once per poll: fullscreen switched from the menu or the launcher, and the
/// monitor names the menu offers.
void ApplyWindowSettings(SDL_Window* window);

/// Guest thread: a newly persisted unlock, after the edited-save and duplicate checks.
void TrophyUnlocked(const char* title, int id);
/// Any thread: the online module's status line under the game's HUD; null or empty clears it.
void SetBanner(const char* message);

/// Whether anything is drawn this frame (menu, FPS counter or trophy notice).
bool Visible();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);
/// The same with the Metal frame: draws into `target` (id<MTLTexture>, width x height) on
/// `command_buffer` (id<MTLCommandBuffer>). Called every frame (timing and live effects).
void RenderMetal(void* command_buffer, void* target, u32 width, u32 height);

/// The system's thermal state (NSProcessInfo.thermalState on Apple platforms: 0 nominal, 1 fair,
/// 2 serious, 3 critical; the system throttles from fair up), or -1 where there is none.
int ThermalState();
/// The hottest die sensor in °C (Apple platforms, read every 2 s on a thread of its own), or 0
/// where the system gives none.
float Temperature();
/// Its name for the overlay and the logs ("nominal" ... "critical"), or null for -1.
const char* ThermalName(int state);

/// The menu or the text dialog is open: the game's input is held neutral.
bool CapturesInput();
/// Opens the settings menu (the touch controls' Settings button).
void OpenMenu();

/// The settings menu is open: the window shows the system cursor over it.
bool MenuOpen();

/// Window thread: the game's text dialog (ImeDialog) state, drawn as a box over the frame.
void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

/// What the on-screen keyboard does to the text dialog's text (window thread, from HandleEvent).
enum class TextEdit { Append, Backspace, Done, Cancel };
/// The window's handler for those edits: the same text a physical keyboard types into.
void SetTextEditor(void (*edit)(TextEdit edit, const char* text, void* context), void* context);

} // namespace BbOverlay
