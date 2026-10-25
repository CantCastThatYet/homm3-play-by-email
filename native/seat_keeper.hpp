#pragma once
#include <functional>
#include <string>
#include <vector>

// The game patch's own care of a Heroes Post seat, with no app beside the game:
// - it saves the seat on the server while the player plays: at End Turn, once each new day and
//   after the player has acted, so leaving at any moment loses at most a few seconds;
// - Continue downloads an away seat and restarts HotA into it; the restarted HotA resumes it;
// - it collects notices about the game (other players leaving and returning, the connection,
//   saves) for the adventure screen.
namespace hp_keeper {
// The adapter's own command handler: takes {"command":"..."} and answers JSON (probe, capture_*, ...).
using Command = std::function<std::string(const std::string& request)>;

// Patch mode, once the hooks are in: starts the keeper thread.
void Start(Command command, const std::wstring& folder);

// The lobby's Continue (game thread): claims the seat on a worker thread and prepares the restart.
// Progress and errors go to the lobby's status line; ReadyToRestart turns true when all is in place.
void Continue(const std::string& game_id);
bool Continuing() noexcept;
bool ReadyToRestart() noexcept;
// Game thread: starts a fresh HotA that resumes the prepared seat, and ends this one.
void Restart();

// HeroesPost.keeper.log in the HotA folder: what the keeper did, for when something goes wrong.
void Log(const std::string& text);

// Lines for the adventure screen, oldest first; the game thread takes them.
void Notice(const std::string& text);
std::vector<std::string> TakeNotices();

// Store-only ZIP of every file in `directory`, manifest.json first: a seat capture as the server takes it.
bool ZipDirectory(const std::wstring& directory, std::string& zip, std::string& error);
}
