#pragma once
#include <cstdint>
#include <string>

// The in-game Heroes Post lobby: a button in HotA's multiplayer screen opening our own window.
namespace hp_lobby {
// Game thread, patch mode only. Chains onto the multiplayer dialog's message procedure.
bool Install(std::uintptr_t hd_base, std::string* error);
// Game thread, at the adventure map's idle boundary: shows the keeper's notices in the screen chat
// and opens the players window when F7 was pressed. True when it consumed the event.
bool AtAdventureBoundary() noexcept;
// Development: open the players window at the next idle moment.
void RequestPlayers() noexcept;
// Development: the multiplayer dialog acts as if its Heroes Post button was clicked on its next message.
void RequestClick() noexcept;
// Development: the open lobby window acts as if button `button` was pressed (0 Create, 1 Join the selected or first game).
void RequestPress(int button) noexcept;
// Development: the open sign-in window signs in (or registers) with this name and password.
void RequestSignIn(const std::string& account, const std::string& password, bool create) noexcept;
// Game thread, after HD has loaded its saved PvP options: a Heroes Post host's game gets simultaneous turns.
void PvpOptionsLoaded() noexcept;
}
