#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class PatcherInstance;

namespace hota_completion {
enum class SourceSite : std::uint32_t {
    SecondaryAfterDone = 0x0117af35,
    HeadAfterDone = 0x0117afcc
};

struct Participant {
    std::uint8_t ack = 0, done = 0, role = 0, participates = 0;
    std::int8_t predecessor = -1, successor = -1;
};
struct Snapshot {
    SourceSite source_site = SourceSite::SecondaryAfterDone;
    std::uint32_t thread_id = 0;
    std::uint32_t world_address = 0;
    std::int32_t site_player = -1; // EAX / 3 at either marker.
    std::int32_t local_player = -1, current_player = -1, st_day = 0;
    std::int32_t multiplayer = 0, st_enabled = 0, first_participant = -1;
    std::int32_t merge_succeeded = 0, merge_failed = 0;
    // Raw0121008E: receive/merge eligibility, possibly retained after receive.
    // Historical field name; NOT a reliable indication of an active receive call.
    std::int32_t receive_active = 0;
    std::int32_t barrier_pending = 0, ack_pending = 0, ui_phase = 0, stop_pending = 0;
    std::uint16_t world_date_words[3]{};
    Participant participants[8]{};
    bool metadata_readable = false, metadata_stable = false, world_date_valid = false;
    bool marker_confirmed = false;
    // Marker does not itself serialize the world or prove disk durability.
    bool fresh_world_serialized = false;
};

using Callback = void (*)(const Snapshot&, void* context) noexcept;
using MemoryReader = bool (*)(void*, std::uintptr_t, void*, std::size_t);

// Caller must first fingerprint the pinned binaries and require engine base00400000.
// Install and Uninstall must run on the game thread, outside these callbacks.
// Singleton installation; owns and destroys only the two hooks it creates.
bool Install(PatcherInstance* patcher, std::uintptr_t hd_base, Callback callback,
             void* callback_context = nullptr, std::string* error = nullptr) noexcept;
bool Uninstall(std::string* error = nullptr) noexcept;
bool HasOwnedHooks() noexcept;

// Read-only helpers, also used by synthetic tests. Null reader uses this process.
bool ValidatePinnedSites(std::uintptr_t hd_base, MemoryReader reader = nullptr,
                         void* reader_context = nullptr, std::string* error = nullptr) noexcept;
Snapshot ReadSnapshot(SourceSite site, std::uintptr_t hd_base, std::int32_t eax,
                      MemoryReader reader = nullptr, void* reader_context = nullptr) noexcept;
} // namespace hota_completion
