#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;
namespace hota_departure {
enum class Status : std::uint32_t { Idle, Armed, Held, Refused, Cancelled };
struct Request {
    std::uint32_t token=0, checkpoint_token=0, world_address=0;
    std::uint32_t local_dpid=0, remote_dpid=0;
    std::int32_t local_color=-1, remote_color=-1;
    std::uint16_t date_words[3]{}; // day,week,month; exact current world date.
};
struct Snapshot {
    Status status=Status::Idle;
    std::uint32_t token=0, checkpoint_token=0, thread_id=0, world=0, transport=0;
    std::uint32_t queue_count=0, queue_cursor=0, packet_address=0, packet_words[6]{};
    std::uint32_t local_dpid=0, remote_dpid=0, network=0, st_enabled=0, exit_pending=0, protected_exit=0;
    std::int32_t local_color=-1, current_color=-1;
    std::uint16_t date_words[3]{};
    std::uint8_t participants[8][6]{}, player_records[2][0x1b]{}, hd_roster[0xbb]{};
    bool gameplay_blocked=false, pre_mutation_verified=false;
    // checkpoint_token is an upstream reference, not a durability verification.
    bool durable_checkpoint_verified=false, safe_logout=false, resume_supported=false;
    char detail[96]{};
};
// Both run on the game thread, no throw, no disk/native save/load/message pumping.
// Guard must verify the explicit PRE-DISPATCH adventure boundary (before any
// previous native/HD chain), no active save/load/battle/reentrant dispatch, and that the
// adapter can block ALL adventure input and turn progression when Held.
using GuardCallback=bool (*)(void*) noexcept;
using LatchedCallback=void (*)(const Snapshot&,void*) noexcept;
// Caller pins entire engine/HD identities. Install is not arming. Only fresh
// isolated experiments; refuses any existing patch at553440.
bool Install(PatcherInstance*,std::uintptr_t hd_base,GuardCallback,LatchedCallback,
             void* context=nullptr,std::string* error=nullptr) noexcept;
bool Arm(const Request&,std::string* error=nullptr) noexcept;
// Only call at the OUTER adventure408710 callback, before invoking any previous
// chain. If true, skip the entire chain for this event. No engine calls occur.
// PollHook only enforces an already-held queue; it never acquires a first hold.
bool TryHoldAtAdventureBoundary() noexcept;
bool CancelArmed() noexcept;
Snapshot GetSnapshot() noexcept;
bool IsGameplayBlocked() noexcept;
bool HasOwnedHooks() noexcept;
// No release/resume API. Uninstall refuses a latched hold; keep DLL loaded.
bool Uninstall(std::string* error=nullptr) noexcept;
const char* StatusName(Status) noexcept;
}
