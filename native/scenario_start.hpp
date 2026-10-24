#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;
namespace hota_start {
enum class Action : std::uint32_t { HostStart, PeerAccept, SoloScaffoldStart };
enum class Status : std::uint32_t { Idle, Queued, WaitingForNativeSetup, PeerArmed, Starting,
    ConfiguringOptions, OptionsAccepted, HostStartReturned, PeerAccepted, Refused, Cancelled };
struct Request {
    std::uint32_t token=0;
    Action action=Action::HostStart;
    std::uint32_t peer_ids[2]{};
    std::string map_basename;
    std::uint32_t date_mwd=211; // Decimal month/week/day, not absolute day.
};
struct Snapshot {
    Status status=Status::Idle;
    std::uint32_t token=0, thread_id=0, scenario=0, peer_ids[2]{};
    std::int32_t colors[2]{-1,-1};
    std::uint32_t assigned_humans=0, pending_transfers=0, options_mode=UINT32_MAX;
    std::uint32_t checkbox=0, date_mwd=0, native_result=0;
    bool setup_ready=false, start_event_sent=false, options_accepted=false;
    // Return from Start is not proof that gameplay/world initialization finished.
    bool gameplay_verified=false;
    char detail[128]{};
};
// Fixed engine base00400000; caller fingerprints pinned engine+HD and owns process.
// Install/uninstall on game thread, outside these hooks. No worker engine calls.
bool Install(PatcherInstance*,std::uintptr_t hd_base,std::string* error=nullptr) noexcept;
bool Uninstall(std::string* error=nullptr) noexcept;
bool HasOwnedHooks() noexcept;
// Thread-safe one immutable request. Peer must arm before host Start.
bool Queue(const Request&,std::string* error=nullptr) noexcept;
bool CancelWaiting() noexcept;
Snapshot GetSnapshot() noexcept;
const char* StatusName(Status) noexcept;
}
