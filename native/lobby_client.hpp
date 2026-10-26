#pragma once
#include <windows.h>
#include <string>
#include <vector>

// The in-game lobby's connection to the Heroes Post server: HeroesPost.ini in the HotA
// folder names the server, the player signs in to their account there (the sign-in is kept
// for the Windows user, not in the HotA folder), and a worker thread keeps a connection to the
// server's game relay, which pushes the lobby as it changes. No game-thread work and no game memory here; text handed to the UI is
// already in the game's own character set.
namespace hp_lobby_client {
struct OpenGame { std::string id, name, host; bool started = false, password = false; int players = 0, max_players = 0; };
// One player of a game: state is Online, Away or StandIn (the server plays the seat). away_since is
// Unix seconds, 0 when unknown. Names are raw HotA bytes.
struct Seat { std::string name, state; long long away_since = 0; };
struct MyGame { std::string id, name, seat, state, date; bool started = false; int turn_seconds = 0; std::vector<Seat> seats; };
struct ChatLine { long long id = 0; std::string name, text; };

struct Snapshot {
    unsigned version = 0;           // changes whenever anything below changes
    bool online = false;            // connected, and the server has sent the lobby
    std::string status;             // what the lobby shows under its title
    std::string server;             // server URL from HeroesPost.ini, empty when none is set
    std::string player;             // this player's name on the server
    bool signed_in = false;         // this Windows user has a sign-in for the server (checked once connected)
    bool needs_account = false;     // signed in as a player from before accounts, who has no name and password yet
    bool signing_in = false;        // a SignIn is under way
    std::string account;            // the account name signed in with
    std::string remembered_account; // the last account signed in to this server, kept after signing out
    std::string sign_in_error;      // why the last SignIn failed, empty when it did not
    std::string relay_host, ticket; // where and as whom HotA's DirectPlay connects for Heroes Post games
    int relay_port = 0, default_turn_seconds = 0;
    std::vector<MyGame> mine;
    std::vector<OpenGame> games;
    std::vector<ChatLine> chat;     // newest last, at most 100 lines
};

// Reads HeroesPost.ini from the folder; posts a harmless mouse move to `window` when the snapshot changes.
void Start(const std::wstring& folder, HWND window);
// The lobby window opened or closed. The first Open connects, and the connection then stays
// up (redialling when it drops) so the lobby is current whenever the window opens again. The
// name is the one typed in HotA's multiplayer screen, which the relay logs.
void Open(const std::string& player_name);
void Close();
// Keeps the connection up without the window, e.g. during a Heroes Post game for its seat states.
void Connect(const std::string& player_name);
Snapshot Get();
// Replaces the status line under the lobby's title.
void SetStatus(const std::string& text);
// Signs in to an account, or registers one (`create`), without blocking: the snapshot's signing_in,
// signed_in and sign_in_error report how it went. Text is in the game's character set.
void SignIn(const std::string& account, const std::string& password, bool create);
// Forgets this server's sign-in for the Windows user and ends it on the server.
void SignOut();

// An authenticated call to this player's server (HeroesPost.ini), with the player's token. Blocking:
// never from the game thread. `limit` caps the response body.
struct Reply { int status = 0; std::string body, error; };
Reply Call(const char* method, const std::string& path, const std::string& body = {}, const wchar_t* type = L"application/json",
    size_t limit = 1024 * 1024, int timeout_seconds = 30);
// What went wrong with a call, in the game's character set.
std::string ProblemText(const Reply& reply);
void SendChat(const std::string& text);
void SetTurnLimit(const std::string& game_id, int seconds);

// Text conversions shared with the lobby window.
std::string GameText(const std::string& utf8);     // server text -> HotA's ANSI code page
std::string Utf8Text(const std::string& game);     // HotA text -> UTF-8
std::string BytesText(const std::string& utf8);    // relay session names: Latin-1 on the server, raw bytes in HotA
}
