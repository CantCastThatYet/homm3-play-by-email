#include "completion_observer.hpp"
#include "supported_build.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include "vendor/patcher_x86.hpp"
#include <atomic>
#include <cstring>

static_assert(sizeof(void*) == 4, "Completion observer requires x86");
namespace hota_completion {
namespace {
constexpr std::uintptr_t kPreferredHd = 0x01000000;
struct State {
    LoHook* secondary = nullptr;
    LoHook* head = nullptr;
    std::uintptr_t hd_base = 0;
    Callback callback = nullptr;
    void* context = nullptr;
    DWORD thread = 0;
    bool active = false;
} state;
std::atomic<bool> owned_hooks{false};
thread_local bool in_callback = false;

void Error(std::string* output, const char* message) noexcept {
    if (output) { try { *output = message; } catch (...) {} }
}
std::uintptr_t Address(std::uintptr_t base, std::uint32_t address) noexcept {
    const auto offset = hp_build::Rva(address - kPreferredHd);
    return base && base <= UINT32_MAX && offset <= UINT32_MAX - base ? base + offset : 0;
}
bool NativeRead(void*, std::uintptr_t address, void* output, std::size_t size) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output,
                             size, &copied) && copied == size;
}
bool Read(MemoryReader reader, void* context, std::uintptr_t address, void* output, std::size_t size) {
    if (!address || address > UINT32_MAX || size > UINT32_MAX || size > UINT32_MAX - address + 1u)
        return false;
    return (reader ? reader : NativeRead)(context, address, output, size);
}
std::int32_t I32(const std::uint8_t* value) {
    std::int32_t result; std::memcpy(&result, value, sizeof result); return result;
}

// Owns only these handles. On failure retain a handle so callers must not unload.
bool Destroy(LoHook*& hook) noexcept {
    if (!hook) return true;
    try {
        if (!hook->Destroy()) return false;
        hook = nullptr; return true;
    } catch (...) { return false; }
}
bool Rollback() noexcept {
    state.active = false;
    const bool head_removed = Destroy(state.head);
    const bool secondary_removed = Destroy(state.secondary);
    owned_hooks.store(state.head || state.secondary);
    if (head_removed && secondary_removed) state = State{};
    return head_removed && secondary_removed;
}

int Observe(SourceSite site, HookContext* context) noexcept {
    // Patcher restores general registers/EFLAGS from the untouched HookContext.
    // Preserve floating-point/SSE state and Win32 last-error across our C++ observer.
    __declspec(align(16)) unsigned char fp_state[512];
    _fxsave(fp_state);
    const DWORD last_error = GetLastError();
    if (context && state.active && !in_callback && GetCurrentThreadId() == state.thread) {
        in_callback = true;
        const auto snapshot = ReadSnapshot(site, state.hd_base, context->eax);
        if (state.callback) state.callback(snapshot, state.context);
        in_callback = false;
    }
    SetLastError(last_error);
    _fxrstor(fp_state);
    return EXEC_DEFAULT;
}
int __stdcall SecondaryHook(LoHook*, HookContext* context) {
    return Observe(SourceSite::SecondaryAfterDone, context);
}
int __stdcall HeadHook(LoHook*, HookContext* context) {
    return Observe(SourceSite::HeadAfterDone, context);
}
} // namespace

bool ValidatePinnedSites(std::uintptr_t hd_base, MemoryReader reader,
                         void* reader_context, std::string* error) noexcept {
    try {
        const auto done_address = Address(hd_base, 0x0121004b);
        if (!done_address) { Error(error, "invalid_hd_base"); return false; }
        // Both preceding instructions are MOV BYTE PTR [EAX*2+done],1 (8 bytes).
        // Validate the instruction to be relocated by LoHook as well, not just its address.
        std::uint8_t secondary[] = {0xc6,0x04,0x45,0,0,0,0,0x01,
                                    0xc7,0x05,0xd8,0x77,0x69,0,0,0,0,0};
        std::uint8_t head[] = {0xc6,0x04,0x45,0,0,0,0,0x01,0x8b,0x0d,0x38,0x95,0x69,0};
        const auto done32 = static_cast<std::uint32_t>(done_address);
        std::memcpy(secondary + 3, &done32, 4); std::memcpy(head + 3, &done32, 4);
        std::uint8_t actual[sizeof secondary]{};
        if (!Read(reader, reader_context, Address(hd_base, 0x0117af2d), actual, sizeof secondary) ||
            std::memcmp(actual, secondary, sizeof secondary) != 0) {
            Error(error, "secondary_completion_site_bytes_mismatch"); return false;
        }
        if (!Read(reader, reader_context, Address(hd_base, 0x0117afc4), actual, sizeof head) ||
            std::memcmp(actual, head, sizeof head) != 0) {
            Error(error, "head_completion_site_bytes_mismatch"); return false;
        }
        Error(error, ""); return true;
    } catch (...) { Error(error, "site_validation_exception"); return false; }
}

Snapshot ReadSnapshot(SourceSite site, std::uintptr_t hd_base, std::int32_t eax,
                      MemoryReader reader, void* reader_context) noexcept {
    Snapshot snapshot;
    snapshot.source_site = site;
    snapshot.thread_id = GetCurrentThreadId();
    try {
        if (site != SourceSite::SecondaryAfterDone && site != SourceSite::HeadAfterDone) return snapshot;
        if (eax >= 0 && eax <= 21 && eax % 3 == 0) snapshot.site_player = eax / 3;
        std::uint8_t metadata[0x9c]{};
        if (!Read(reader, reader_context, Address(hd_base, 0x0120fffa), metadata, sizeof metadata) ||
            !Read(reader, reader_context, 0x00699538, &snapshot.world_address, 4) ||
            !Read(reader, reader_context, 0x006995a4, &snapshot.local_player, 4) ||
            !Read(reader, reader_context, 0x0069ccf4, &snapshot.current_player, 4) ||
            !Read(reader, reader_context, 0x0069959c, &snapshot.multiplayer, 4)) return snapshot;
        const auto flag = [&](std::uint32_t address) { return I32(metadata + address - 0x0120fffa); };
        snapshot.st_day = flag(0x01210016); snapshot.st_enabled = flag(0x01210042);
        snapshot.first_participant = flag(0x0121008a);
        snapshot.merge_succeeded = flag(0x0121001a); snapshot.merge_failed = flag(0x01210026);
        snapshot.receive_active = flag(0x0121008e); snapshot.barrier_pending = flag(0x0121001e);
        snapshot.ack_pending = flag(0x01210022); snapshot.ui_phase = flag(0x0121002a);
        snapshot.stop_pending = flag(0x0121002e);
        for (int i = 0; i < 8; ++i) {
            const auto* p = metadata + 0x50 + i * 6;
            snapshot.participants[i] = {p[0],p[1],p[2],p[3],static_cast<std::int8_t>(p[4]),static_cast<std::int8_t>(p[5])};
        }
        snapshot.metadata_readable = true;
        if (snapshot.world_address && snapshot.world_address <= UINT32_MAX - 0x1f644)
            snapshot.world_date_valid = Read(reader, reader_context, snapshot.world_address + 0x1f63e,
                                             snapshot.world_date_words, sizeof snapshot.world_date_words);
        std::uint8_t after[sizeof metadata]{};
        std::int32_t local_after = -1, current_after = -1;
        std::uint32_t world_after = 0;
        snapshot.metadata_stable = Read(reader, reader_context, Address(hd_base, 0x0120fffa), after, sizeof after) &&
            std::memcmp(after, metadata, sizeof after) == 0 &&
            Read(reader, reader_context, 0x006995a4, &local_after, 4) && local_after == snapshot.local_player &&
            Read(reader, reader_context, 0x0069ccf4, &current_after, 4) && current_after == snapshot.current_player &&
            Read(reader, reader_context, 0x00699538, &world_after, 4) && world_after == snapshot.world_address;
        if (snapshot.site_player >= 0 && snapshot.local_player == snapshot.site_player) {
            const auto& p = snapshot.participants[snapshot.site_player];
            const bool role_matches = site == SourceSite::HeadAfterDone ? p.role == 0 : p.role != 0;
            snapshot.marker_confirmed = snapshot.metadata_stable && snapshot.world_date_valid &&
                snapshot.multiplayer != 0 && snapshot.st_enabled != 0 && p.done == 1 &&
                p.participates != 0 && role_matches;
        }
    } catch (...) { snapshot.metadata_stable = false; snapshot.marker_confirmed = false; }
    return snapshot;
}

bool Install(PatcherInstance* patcher, std::uintptr_t hd_base, Callback callback,
             void* callback_context, std::string* error) noexcept {
    if (!patcher || !callback) { Error(error, "missing_patcher_or_callback"); return false; }
    if (owned_hooks.load()) { Error(error, "completion_hooks_already_owned"); return false; }
    if (!ValidatePinnedSites(hd_base, nullptr, nullptr, error)) return false;
    try {
        state.hd_base = hd_base; state.callback = callback; state.context = callback_context;
        state.thread = GetCurrentThreadId();
        state.secondary = patcher->CreateLoHook(static_cast<_ptr_>(Address(hd_base, 0x0117af35)), SecondaryHook);
        owned_hooks.store(state.secondary != nullptr);
        if (!state.secondary) { Error(error, "secondary_hook_create_failed"); Rollback(); return false; }
        state.head = patcher->CreateLoHook(static_cast<_ptr_>(Address(hd_base, 0x0117afcc)), HeadHook);
        owned_hooks.store(state.head || state.secondary);
        if (!state.head || state.secondary->Apply() < 0 || state.head->Apply() < 0 ||
            !state.secondary->IsApplied() || !state.head->IsApplied()) {
            const bool rolled_back = Rollback();
            Error(error, rolled_back ? "completion_hook_apply_failed" : "completion_hook_rollback_incomplete");
            return false;
        }
        state.active = true;
        Error(error, ""); return true;
    } catch (...) {
        const bool rolled_back = Rollback();
        Error(error, rolled_back ? "completion_hook_exception" : "completion_hook_rollback_incomplete");
        return false;
    }
}

bool Uninstall(std::string* error) noexcept {
    if (owned_hooks.load() && GetCurrentThreadId() != state.thread) {
        Error(error, "uninstall_requires_installing_game_thread"); return false;
    }
    if (in_callback) { Error(error, "cannot_uninstall_in_callback"); return false; }
    const bool result = Rollback();
    Error(error, result ? "" : "completion_hook_rollback_incomplete"); return result;
}
bool HasOwnedHooks() noexcept { return owned_hooks.load(); }
} // namespace hota_completion
