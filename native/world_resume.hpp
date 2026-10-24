#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hota_resume {
struct PlayerAudit {
    std::uint32_t transport_id=0;
    std::uint8_t local=0,human=0;
    std::array<std::int32_t,7> resources{};
};
struct HeroAudit {
    std::int32_t id=0,max_movement=0,movement=0;
    std::int16_t x=0,y=0;
    std::uint8_t layer=0,owner=0;
};
struct Audit {
    std::uint32_t world=0,adventure=0,map=0,hero_offset=0;
    std::int32_t width=0,height=0,map_size=0,local=-1,current=-1;
    std::uint8_t underground=0;
    std::array<std::uint16_t,3> date{};
    std::array<PlayerAudit,8> players{};
    std::vector<HeroAudit> heroes;
};
struct Frames {
    // Supplied by installed hooks, not inferred from a native eligibility flag.
    unsigned save_depth=0,turn_depth=0,merge_depth=0,receive_depth=0,battle_depth=0;
    bool at_outer_idle_boundary=false;
    // True only when ALL gameplay dispatch (including nested calls) and incoming
    // gameplay delivery will remain blocked while IsGameplayBlocked() is true.
    bool input_gate_installed=false,receive_gate_installed=false;
};
struct Callbacks {
    void* context=nullptr;
    bool (*read_frames)(void*,Frames&) noexcept=nullptr;
};
struct Request {
    std::uint32_t token=0;
    int expected_local=-1;
    std::array<std::uint16_t,3> expected_date{};
    std::int32_t map_size=0;
    std::uint8_t underground=0;
    std::vector<std::uint8_t> local_snapshot;
    // Exact captured HD protection roster (187 bytes), bound to source audit.
    // Required for cold recovery; empty only for same-roster diagnostic reloads.
    std::vector<std::uint8_t> source_roster;
    // Optional source-world audit captured with these exact checkpoint bytes.
    // Absence leaves the outcome awaiting external audit, never verified.
    bool has_source_audit=false;
    Audit source_audit;
};
enum class Status { Uninitialized, Idle, Queued, Loading, Refused, Failed,
                    AwaitingAudit, SourceAuditMatched, Released };
struct Snapshot {
    Status status=Status::Uninitialized;
    std::uint32_t token=0;
    bool input_blocked=false,stream_replaced=false,load_invoked=false,
         load_balanced=false,load_success=false,audit_read=false,source_audit_matched=false;
    bool local_owner_staged=false,local_owner_rollback_succeeded=false;
    bool scaffold_render_cleanup_invoked=false;
    bool membership_reconstruction_attempted=false,membership_reconstructed=false;
    unsigned load_calls_before=0,load_returns_before=0,load_calls_after=0,load_returns_after=0;
    std::string error;
    Audit before,after,loaded_before_membership;
};
// Initialize on the installed game thread. Checks pinned disk images, actual
// wrapper instructions and importer ABI. Does not install any hooks itself.
bool Initialize(std::uint32_t hd_base,std::uint32_t game_thread,Callbacks,
                std::string* error=nullptr) noexcept;
// Worker-safe, deep copy, one request, max64MiB; caller must authenticate/bind
// token, checkpoint bytes and metadata before handing them to this module.
bool Queue(const Request&,std::string* error=nullptr) noexcept;
// Invoke from adventure's OUTERMOST pre-dispatch idle callback. Returns true
// when original gameplay dispatch must be skipped. Never calls EndTurn/setup.
bool TryAtAdventureBoundary() noexcept;
bool IsGameplayBlocked() noexcept;
Snapshot GetSnapshot();
// Game-thread audit, usable during checkpoint capture. No world mutation.
bool ReadAudit(Audit&,std::string* error=nullptr) noexcept;
// Versioned little-endian pointer-free audit. Decode rejects trailing bytes,
// oversized counts and invalid geometry. JSON includes the portable audit_hex
// alongside readable fields; callers may extract that field and DecodeAuditHex.
std::string SerializeAudit(const Audit&);
std::vector<std::uint8_t> EncodeAudit(const Audit&);
bool DecodeAudit(const std::vector<std::uint8_t>&,Audit&,std::string* error=nullptr) noexcept;
std::string EncodeAuditHex(const Audit&);
bool DecodeAuditHex(const std::string&,Audit&) noexcept;
bool SameWorldContent(const Audit&,const Audit&) noexcept;
// Pre-membership reconstruction audit only. Ignores exactly playerC8(DPID),
// playerE1(local controller), playerE2(human), retaining resources/heroes/date
// and session local/current owner checks. Full audit remains mandatory afterward.
bool SameGameplayBeforeMembership(const Audit&,const Audit&) noexcept;
// Exactly one local human controller, on the authenticated expected_local seat.
bool ValidSourceMembership(const Audit&,int expected_local) noexcept;
// x86 LEA uses modulo2^32 addition for the encoded disp32. The resulting
// contiguous array must still fit in the process address space without wrapping.
bool ResolveHeroArrayAddress(std::uint32_t world,std::uint32_t displacement,
                             unsigned count,std::uint32_t& begin) noexcept;
using RecoveryAuthorization=bool(*)(void*,std::uint32_t,const Audit&) noexcept;
// Game-thread outer idle boundary only, after source audit matched. Caller
// certifies transport/ST/UI transaction success; this independently re-reads
// and matches expected rebound world, retaining all source gameplay values.
bool ReleaseAfterRebind(std::uint32_t token,const Audit& expected_rebound,
                       RecoveryAuthorization,void* context=nullptr,std::string* error=nullptr) noexcept;
// Root's installed004BEFF0 hook MUST call these around its original chain,
// preserving actual AL. Result is matched to world/name/arguments/thread.
void ObserveLoadEnter(std::uint32_t world,const char* name,unsigned a,unsigned b) noexcept;
void ObserveLoadReturn(unsigned eax) noexcept;
// Matched world audit leaves native ST/transport rebinding and UI repair to the
// enclosing recovery transaction until ReleaseAfterRebind. Failed loads can have
// partially reset the world and require disposal of this experimental client.
const char* StatusName(Status) noexcept;
}
