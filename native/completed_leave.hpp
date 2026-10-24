#pragma once
#include <cstdint>
#include <string>
namespace hota_leave {
enum class Status : std::uint32_t { Idle, Armed, Refused, Closing, Closed, TeardownIncomplete, Cancelled };
struct Request {
    std::uint32_t token=0,checkpoint_token=0,world_address=0,local_dpid=0,remote_dpid=0;
    std::int32_t local_color=-1,remote_color=-1;
    std::uint16_t date_words[3]{}; // day,week,month of verified completed branch
};
struct Snapshot {
    Status status=Status::Idle;
    std::uint32_t token=0,checkpoint_token=0,thread_id=0,world=0;
    std::uint32_t transport_before=0,transport_after=0,network_before=0,network_after=0;
    std::uint32_t local_dpid_before=0,local_dpid_after=0,remote_dpid=0;
    std::int32_t local_color=-1,current_color=-1;
    std::uint16_t date_words[3]{};
    std::uint8_t participants[8][6]{};
    bool native_call_started=false,native_call_returned=false,post_state_readable=false;
    bool gameplay_blocked=false,transport_null=false,network_mode_zero=false;
    // Upstream must establish checkpoint durability. Closing locally proves
    // neither remote receipt nor recoverability/rejoin.
    bool durable_checkpoint_verified=false,remote_notified=false,resume_supported=false;
    char detail[96]{};
};
// Required: explicit outer pre-dispatch idle boundary, balanced save/load/turn
// depths, no reentrant native work, and a verified durable marker capture bound
// to the armed request. Must not throw or pump messages/call engine routines.
using GuardCallback=bool (*)(const Request&,void*) noexcept;
// On game thread after caller pins entire binaries. Refuses unknown entry detours.
// No native calls occur during Install or Arm.
bool Install(std::uintptr_t hd_base,GuardCallback,void* context=nullptr,std::string* error=nullptr) noexcept;
bool Arm(const Request&,std::string* error=nullptr) noexcept;
// Called before ANY previous adventure408710 chain. True means skip the whole
// chain now and on every subsequent event. Calls only native5548A0 once.
bool TryAtAdventureBoundary() noexcept;
bool IsGameplayBlocked() noexcept;
bool CancelArmed() noexcept;
Snapshot GetSnapshot() noexcept;
const char* StatusName(Status) noexcept;
}
