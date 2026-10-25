#pragma once
#include "common.hpp"
#include <cstdint>
#include <string>

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
    std::uint32_t rejoin_from = 0; // messages the captured seat had consumed; the relay replays the rest
    std::wstring log_path;     // empty disables logging
};

// Reads key=value lines (host, port, ticket, name, rejoin, seat). Missing file is not an error:
// returns false with an empty error so callers keep stock DirectPlay.
bool ReadConfig(const std::wstring& path, Config& config, std::string* error);

// Redirects the main module's CoCreateInstance import for CLSID_DirectPlay and
// CLSID_DirectPlayLobby. Other classes are passed through unchanged.
bool Install(const Config& config, std::string* error);
bool Installed() noexcept;
// True when this client resumes a held seat in a running game (rejoin= and seat=).
bool Rejoining() noexcept;
// Game messages from the relay the engine has taken with Receive, counted from the
// start of the session (a rejoin continues the count it resumed from).
std::uint32_t ConsumedCount() noexcept;
// A rejoining client restores its captured seat before it may see game messages:
// Receive withholds them (system messages still flow) until this is called.
void ReleaseGameMessages() noexcept;
// Name of a player in the game a rejoining client entered; the engine's player
// names are their network names. False if unknown.
bool RejoinPlayerName(std::uint32_t id, std::string& name);
std::string StatusJson();

// Test seams used by the native relay test without a game process.
HRESULT CreateDirectPlay(const Config& config, void** out);
HRESULT CreateLobby(void** out);
}
