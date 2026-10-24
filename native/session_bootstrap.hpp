#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;

namespace hota_session {
enum class Role { Host, Join, SoloScaffold };
enum class Status { Idle, Queued, Dispatching, SelectingNetwork, EstablishingNetwork,
                    NetworkEstablished, AwaitingScenarioConfiguration, Failed };
struct Request {
    std::uint32_t token = 0;
    Role role = Role::Host;
    std::string player_name, session_name, address, map_basename;
    std::uint16_t port = 0; // zero preserves the native provider's default
};
struct Snapshot {
    Status status = Status::Idle;
    std::uint32_t token = 0, local_player_id = 0;
    bool native_host = false, map_applied = false, game_started = false;
    std::uint32_t native_hresult = 0, native_call_result = 0;
    bool native_hresult_valid = false;
    std::string native_operation;
    std::string error;
};
// Install on the game thread after pinned executable/module fingerprints pass.
// Establishes TCP/IP and optionally selects a fixed map through the native host.
// Player preferences, ready/start and asynchronous reconnection are separate.
bool Install(PatcherInstance*, std::string* error = nullptr) noexcept;
bool Uninstall(std::string* error = nullptr) noexcept;
bool Prepare(const Request&, std::string* error = nullptr) noexcept; // IPC thread
bool CancelQueued() noexcept;
bool CancelPrepared(std::uint32_t token) noexcept;
bool Begin(std::uint32_t token) noexcept; // clean menu_dispatch boundary, game thread
Snapshot GetSnapshot();
const char* StatusName(Status) noexcept;
// Internal original-engine SP initialization only. Keep adventure input gated
// until checkpoint load, original human/ST state and solo transport are audited.
bool IsScaffoldRestorePending() noexcept;
// After checkpoint world load, at the owned pre-dispatch adventure boundary.
// Constructs/configures/destroys native network dialog without running its UI;
// retains the resulting real TCP host transport for the logical-player adapter.
bool EstablishSoloTransportAtBoundary(std::uint32_t token,std::string* error=nullptr) noexcept;
bool ReleaseScaffoldGate(std::uint32_t token,std::string* error=nullptr) noexcept;
}
