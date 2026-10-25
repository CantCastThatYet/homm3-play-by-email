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
    std::wstring log_path;     // empty disables logging
};

// Reads key=value lines (host, port, ticket, name). Missing file is not an error:
// returns false with an empty error so callers keep stock DirectPlay.
bool ReadConfig(const std::wstring& path, Config& config, std::string* error);

// Redirects the main module's CoCreateInstance import for CLSID_DirectPlay and
// CLSID_DirectPlayLobby. Other classes are passed through unchanged.
bool Install(const Config& config, std::string* error);
bool Installed() noexcept;
std::string StatusJson();

// Test seams used by the native relay test without a game process.
HRESULT CreateDirectPlay(const Config& config, void** out);
HRESULT CreateLobby(void** out);
}
