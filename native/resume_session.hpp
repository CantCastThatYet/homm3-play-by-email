#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;
namespace hota_completion {struct Snapshot;}
namespace resume_session {
struct Snapshot { bool initialized=false,queued=false,started=false,verified=false,failed=false,completed_input_held=false,completion_marker_observed=false,completion_tail_returned=false,completion_tail_audit_passed=false; std::uint32_t token=0; std::string stage,error; };
bool Initialize(PatcherInstance*,std::uint32_t hd_base,std::uint32_t game_thread,std::string* error=nullptr) noexcept;
bool QueueFile(const std::wstring&,std::uint32_t token,std::string* error=nullptr) noexcept;
bool TryAtAdventureBoundary() noexcept;
bool IsGameplayBlocked() noexcept;
// Observation only: marker holds future/nested gameplay and receive dispatch,
// while the existing native EndTurn tail returns normally. Never calls EndTurn.
void ObserveCompletion(const hota_completion::Snapshot&) noexcept;
// Root calls after its actual outermost native turn hook returns and depth is0.
void ObserveTurnReturned() noexcept;
Snapshot GetSnapshot();
std::string SerializeSnapshot();
}
