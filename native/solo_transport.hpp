#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
class PatcherInstance;
namespace hota_solo_transport {
struct Snapshot { bool installed=false,active=false,fault=false; std::uint32_t token=0,transport=0,local_dpid=0; std::array<std::uint32_t,8> ids{}; std::uint64_t packets=0; std::string error; };
bool Install(PatcherInstance*,std::uint32_t hd_base,std::uint32_t game_thread,std::string* error=nullptr) noexcept;
// The caller holds the whole adventure dispatcher and has loaded/audited the exact
// checkpoint. This rebinds logical transport identity, never Ready/ACK/game rules.
bool ActivateAtBoundary(std::uint32_t token,std::uint32_t world,int local,int current,
                        std::uint8_t human_mask,const std::vector<std::uint8_t>& source_roster,std::string* error=nullptr) noexcept;
bool IsGameplayBlocked() noexcept;
Snapshot GetSnapshot();
}
