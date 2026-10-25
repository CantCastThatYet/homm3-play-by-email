#pragma once
#include <string>

// The in-game Heroes Post lobby: a button in HotA's multiplayer screen opening our own window.
namespace hp_lobby {
// Game thread, patch mode only. Chains onto the multiplayer dialog's message procedure.
bool Install(std::string* error);
// Development: the multiplayer dialog acts as if its Heroes Post button was clicked on its next message.
void RequestClick() noexcept;
// Development: the open lobby window acts as if button `button` was pressed (0 Create, 1 Join the selected or first game).
void RequestPress(int button) noexcept;
}
