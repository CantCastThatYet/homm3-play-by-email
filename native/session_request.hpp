#pragma once
#include <string>
#include <cstdint>

namespace hota_request {
struct Settings { std::string action, player_name, session_name, address, map_basename; std::uint16_t port = 0; };
struct StartSettings { std::string action, map_basename; std::uint32_t peer_ids[2]{}, date_mwd = 211; };
// Reads one bounded UTF-16LE request atomically from an already selected owned root.
// This is setup input only; it carries no credentials or process pointers.
bool Read(const std::wstring& path, Settings& output, std::string* error = nullptr) noexcept;
bool Parse(const std::string& ascii_ini, Settings& output, std::string* error = nullptr) noexcept;
bool ReadStart(const std::wstring& path, StartSettings& output, std::string* error = nullptr) noexcept;
bool ParseStart(const std::string& ascii_ini, StartSettings& output, std::string* error = nullptr) noexcept;
}
