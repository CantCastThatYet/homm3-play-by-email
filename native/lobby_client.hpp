#pragma once
#include <windows.h>
#include <string>
#include <vector>

// The in-game lobby's connection to the Heroes Post server: HeroesPost.ini in the HotA
// folder names the server, the identity is created on first use and remembered there,
// and a worker thread polls the lobby while the window is open. No game-thread work and
// no game memory here; text handed to the UI is already in the game's own character set.
namespace hp_lobby_client {
struct OpenGame { std::string id, name, host; bool started = false, password = false; int players = 0, max_players = 0; };
struct MyGame { std::string id, name, seat, state, date; bool started = false; int turn_seconds = 0; };
struct ChatLine { long long id = 0; std::string name, text; };

struct Snapshot {
    unsigned version = 0;           // changes whenever anything below changes
    bool online = false;            // the last poll succeeded
    std::string status;             // what the lobby shows under its title
    std::string server;             // server URL from HeroesPost.ini, empty when none is set
    std::string player;             // this player's name on the server
    std::string relay_host, ticket; // where and as whom HotA's DirectPlay connects for Heroes Post games
    int relay_port = 0, default_turn_seconds = 0;
    std::vector<MyGame> mine;
    std::vector<OpenGame> games;
    std::vector<ChatLine> chat;     // newest last, at most 100 lines
};

// Reads HeroesPost.ini from the folder; posts a harmless mouse move to `window` when the snapshot changes.
void Start(const std::wstring& folder, HWND window);
// The lobby window opened (polls every 2 seconds) or closed (stops polling). The name is the
// one typed in HotA's multiplayer screen and only matters the first time this server is used.
void Open(const std::string& player_name);
void Close();
Snapshot Get();
void SendChat(const std::string& text);
void SetTurnLimit(const std::string& game_id, int seconds);

// Text conversions shared with the lobby window.
std::string GameText(const std::string& utf8);     // server text -> HotA's ANSI code page
std::string Utf8Text(const std::string& game);     // HotA text -> UTF-8
std::string BytesText(const std::string& utf8);    // relay session names: Latin-1 on the server, raw bytes in HotA
}
