#pragma once
#include "common.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Replacement for the DirectPlay 4 COM objects the engine creates with
// CoCreateInstance. All session traffic goes through a Heroes Post relay
// server instead of DirectPlay's peer-to-peer TCP/IP provider. The engine's
// own network wrapper, HD's simultaneous turns and every game message are
// unchanged; only the transport underneath differs.
namespace hp_relay {
struct Config {
    std::string host;          // relay host name or IPv4
    std::uint16_t port = 0;    // relay TCP port
    std::string ticket;        // opaque credential forwarded to the relay
    std::string name;          // diagnostic client label
    std::string rejoin_session; // running game to rejoin (name prefix); empty for a normal client
    std::string rejoin_seat;    // held seat (player name) this client takes over
    std::uint32_t rejoin_seat_id = 0; // and its relay id, when known (names need not be unique)
    std::uint32_t rejoin_from = 0; // messages the captured seat had consumed; the relay replays the rest
    std::uint32_t rejoin_sent = 0; // messages the captured seat had sent; the relay compares it with its own count
    // Game messages the captured engine had received but HD's simultaneous-turn queue still held
    // (deferred= file of u32 length + message). Re-delivered once, first, when game messages are released.
    std::vector<std::vector<std::uint8_t>> rejoin_deferred;
    // When set (GUID text), session enumeration reports only this game: the in-game lobby's Join.
    std::string only_instance;
    std::wstring log_path;     // empty disables logging
};

// Reads key=value lines (host, port, ticket, name, rejoin, seat, from, sent, deferred, only). Missing file is not an error:
// returns false with an empty error so callers keep stock DirectPlay.
bool ReadConfig(const std::wstring& path, Config& config, std::string* error);

// Redirects the main module's CoCreateInstance import for CLSID_DirectPlay and
// CLSID_DirectPlayLobby. Other classes are passed through unchanged.
bool Install(const Config& config, std::string* error);
// The game patch: redirect CoCreateInstance now, but pass every call through to DirectPlay
// until Activate. A DirectPlay object the engine already holds keeps its transport.
bool InstallPassive(const std::wstring& log_path, std::string* error);
void Activate(const Config& config);
void Deactivate();
// True while new DirectPlay objects go through the relay.
bool Installed() noexcept;
// True while a dropped relay connection is being redialled to take the seat back. The engine is
// not told; its messages queue and are sent once the relay has the seat again.
bool Reconnecting() noexcept;
// True while a relay session is open; its instance GUID as text ("xxxxxxxx-xxxx-..."), or empty.
bool InGame() noexcept;
std::string CurrentInstance();
unsigned ReconnectCount() noexcept;
// True when this client resumes a held seat in a running game (rejoin= and seat=).
bool Rejoining() noexcept;
// Messages this rejoining client will re-deliver to HD's simultaneous-turn queue.
std::uint32_t RejoinDeferredCount() noexcept;
// Game messages from the relay the engine has taken with Receive, counted from the
// start of the session (a rejoin continues the count it resumed from).
std::uint32_t ConsumedCount() noexcept;
// Game messages this seat has sent through the relay, continuing a rejoined seat's captured count.
std::uint32_t SentCount() noexcept;
// A rejoining client restores its captured seat before it may see game messages:
// Receive withholds them (system messages still flow) until this is called.
void ReleaseGameMessages() noexcept;
// True from the moment a rejoining client claims its seat until it tells the relay it has caught
// up; the relay ignores the seat's messages until then, so the player's commands wait.
bool CatchingUp() noexcept;
// Name of a player in the game a rejoining client entered; the engine's player
// names are their network names. False if unknown.
bool RejoinPlayerName(std::uint32_t id, std::string& name);
std::string StatusJson();

// Test seams used by the native relay test without a game process.
HRESULT CreateDirectPlay(const Config& config, void** out);
HRESULT CreateLobby(void** out);
// Cuts a DirectPlay object's relay connection as a network failure would.
void TestDropConnection(void* directplay);
}
