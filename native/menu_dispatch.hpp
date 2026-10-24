#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;

namespace hota_menu {
enum class Status : std::uint32_t { Idle, Queued, ClosingNativeMenu, DispatchReady, RefusedPhase, Cancelled };
struct Dispatch {
    std::uint32_t token = 0, thread_id = 0;
    std::uint32_t source_site = 0x004ef278;
    std::uint32_t native_choice = 0x65; // New Game; network game-type selection is a later stage.
    std::uint32_t window_manager = 0, world_object = 0;
    std::uint32_t modal_depth = 0;
};
using Callback = void (*)(const Dispatch&, void*) noexcept;
// Fingerprint engine/HD first. Fixed engine base00400000 only. Install/uninstall
// on the game thread outside callbacks. Does not invoke setup/world functions.
bool Install(PatcherInstance*, Callback, void* context = nullptr, std::string* error = nullptr) noexcept;
bool Uninstall(std::string* error = nullptr) noexcept;
bool HasOwnedHooks() noexcept;
bool IsReady() noexcept;
// IPC-safe, one nonzero token outstanding. Acceptance means queued, not executed.
// Native callback revalidates the phase; inspect GetStatus for later refusal.
bool Queue(std::uint32_t token) noexcept;
bool CancelQueued() noexcept;
Status GetStatus() noexcept;
}
