// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbMacMenu {
/// macOS: removes the Command shortcuts of the menu bar SDL made (bbport_mac_menu.mm). Called
/// once the window exists, on the main thread.
void ClearCommandShortcuts();
} // namespace BbMacMenu
