#pragma once
#include <string>

// A capture sent as a difference from the seat's previous upload. Captures are stored zip archives
// of about two megabytes on a 72x72 map, and one hero's move changes a few hundred bytes of them,
// though not in place: the world stream grows or shrinks, so the rest of it shifts.
//
// Frame ("HPD1", little-endian): magic, version 1, the base's SHA-256 and length, the result's
// SHA-256 and length, then operations to the end: 0 = copy (u32 base offset, u32 length),
// 1 = literal (u32 length, bytes). HeroesLobby.Server's CaptureDelta reads it.
namespace hp_delta {
// target as a difference from base. Its size tells whether sending it pays.
std::string Encode(const std::string& base, const std::string& target);
// Rebuilds the target from base; false for a malformed frame, another base or a wrong result.
bool Decode(const std::string& base, const std::string& frame, std::string& target);
}
