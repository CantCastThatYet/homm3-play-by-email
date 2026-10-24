#pragma once
#include <cstdint>
#include <string>

namespace hota_scenario {
enum class Status { NotRequested, Applied, Refused, Failed };
struct Result {
    Status status = Status::NotRequested;
    bool applied = false, native_select_called = false;
    std::uint32_t map_count = 0, selected_index = UINT32_MAX;
    std::string selected_basename, error;
};
// Call ONLY from the owned ScenarioRun584EC0 hook after the native constructor
// has returned, before passing through to the preceding Run chain. Caller must
// already enforce pinned build/module hashes. No pointers survive this call.
// Applies a fixed map for the native TCP host; no player config/ready/start.
Result ApplyRequestedMap(void* native_dialog, const std::string& basename,
                         std::uint32_t expected_game_thread, bool solo_scaffold=false) noexcept;
const char* StatusName(Status) noexcept;
}
