#include "menu_dispatch.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include "vendor/patcher_x86.hpp"

static_assert(sizeof(void*) == 4, "Native menu dispatch requires x86");
namespace hota_menu {
namespace {
struct State {
    HiHook* menu = nullptr;
    LoHook* boundary = nullptr;
    Callback callback = nullptr;
    void* context = nullptr;
    DWORD thread = 0;
    std::uint32_t claimed = 0;
} state;
std::atomic<bool> active{false}, owned{false};
std::atomic<std::uint32_t> pending{0};
std::atomic<Status> status{Status::Idle};
std::atomic_flag request_lock = ATOMIC_FLAG_INIT;
thread_local bool inside = false;
void Error(std::string* error, const char* value) noexcept {
    if (error) { try { *error = value; } catch (...) {} }
}
bool Read(std::uintptr_t address, void* result, SIZE_T size) noexcept {
    SIZE_T copied = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), result,
                                       size, &copied) && copied == size;
}
bool U32(std::uintptr_t address, std::uint32_t& result) noexcept { return Read(address, &result, 4); }
bool ZeroByte(std::uintptr_t address) noexcept { unsigned char v = 1; return Read(address, &v, 1) && !v; }
bool Bytes(std::uintptr_t address, const unsigned char* expected, SIZE_T count) noexcept {
    unsigned char bytes[64]{};
    return count <= sizeof bytes && Read(address, bytes, count) && !std::memcmp(bytes, expected, count);
}
bool Validate() noexcept {
    // Entry may already be chained by HD. Validate the unmodified native close
    // tail plus caller/cleanup and exact unhooked low-hook span instead.
    const unsigned char tail[] = {0xb8,0x0a,0,0,0,0x5f,0xc7,0x03,0,0x02,0,0,0x89,0x43,0x08,
                                  0x89,0x43,0x04,0x5e,0xb8,0x02,0,0,0,0x5b};
    const unsigned char boundary[] = {0x8b,0x0d,0xd0,0x92,0x69,0,0x89,0x3d,0x20,0x8a,0x69,0,
                                     0x8b,0x51,0x38,0x89,0x55,0xe0,0x8b,0x45,0xe0};
    const unsigned char call[] = {0xe8,0x12,0xcb,0,0,0x8d,0x8d,0x80,0xfd,0xff,0xff,
                                 0x89,0x75,0xfc,0xe8,0x84,0xca,0,0};
    return Bytes(0x4fc1d0, tail, sizeof tail) && Bytes(0x4ef26c, boundary, sizeof boundary) &&
           Bytes(0x4ef259, call, sizeof call);
}
template<class T> bool Destroy(T*& hook) noexcept {
    if (!hook) return true;
    try { if (!hook->Destroy()) return false; hook = nullptr; return true; }
    catch (...) { return false; }
}
bool Rollback() noexcept {
    active.store(false);
    const bool a = Destroy(state.boundary), b = Destroy(state.menu);
    owned.store(state.boundary || state.menu);
    if (a && b) { state = State{}; pending.store(0); status.store(Status::Idle); }
    return a && b;
}
void Refuse() noexcept { state.claimed = 0; status.store(Status::RefusedPhase); pending.store(0); }

unsigned __stdcall MenuHook(HiHook* hook, std::uint32_t* message) {
    using Native = unsigned (__thiscall*)(std::uint32_t*);
    // Preserve full preceding HD/HotA chain. Never call setup from this callback.
    const unsigned result = reinterpret_cast<Native>(hook->GetDefaultFunc())(message);
    __declspec(align(16)) unsigned char fp[512]; _fxsave(fp);
    const DWORD last_error = GetLastError();
    unsigned returned = result;
    if (active.load() && !inside && GetCurrentThreadId() == state.thread && !state.claimed &&
        !request_lock.test_and_set()) {
        const auto token = pending.load();
        if (token) {
            inside = true;
            std::uint32_t wm=0, menu=0, depth=0, in_menu=0, msg[8]{};
            const bool phase = result == 1 && U32(0x6992d0,wm) && wm && wm <= UINT32_MAX-0x60 &&
                U32(0x6996b0,menu) && menu && U32(0x6aad88,depth) && depth==1 &&
                U32(0x698a20,in_menu) && in_menu==1 && ZeroByte(0x69d85c) &&
                Read(reinterpret_cast<std::uintptr_t>(message),msg,sizeof msg) && msg[7]==menu;
            if (phase) {
                // Native close protocol: selector is separate from close message.
                // This exact contract is checked by602AE0 after our return.
                *reinterpret_cast<std::uint32_t*>(wm+0x38)=0x65;
                message[0]=0x200; message[1]=10; message[2]=10;
                state.claimed=token; status.store(Status::ClosingNativeMenu); returned=2;
            } else Refuse();
            inside = false;
        }
        request_lock.clear();
    }
    SetLastError(last_error); _fxrstor(fp);
    return returned;
}
int __stdcall BoundaryHook(LoHook*, HookContext* registers) {
    __declspec(align(16)) unsigned char fp[512]; _fxsave(fp);
    const DWORD last_error=GetLastError();
    if (active.load() && !inside && state.claimed && GetCurrentThreadId()==state.thread) {
        inside=true;
        Dispatch dispatch; dispatch.token=state.claimed; dispatch.thread_id=state.thread;
        std::uint32_t menu=1,in_menu=1,choice=0;
        const bool phase = registers && U32(0x6992d0,dispatch.window_manager) && dispatch.window_manager &&
            dispatch.window_manager<=UINT32_MAX-0x60 && static_cast<std::uint32_t>(registers->ecx)==dispatch.window_manager &&
            U32(0x6996b0,menu) && menu==0 && U32(0x6aad88,dispatch.modal_depth) && dispatch.modal_depth==0 &&
            U32(0x698a20,in_menu) && in_menu==0 && ZeroByte(0x69d85c) &&
            U32(dispatch.window_manager+0x38,choice) && choice==0x65 && U32(0x699538,dispatch.world_object);
        if (phase) {
            // Callback may prepare the next native stage, but must not unload hooks.
            state.callback(dispatch,state.context);
            // Request remains occupied throughout the callback. Queue/Cancel never
            // modify it while ClosingNativeMenu, so publication cannot race them.
            state.claimed=0; pending.store(0); status.store(Status::DispatchReady);
        } else Refuse();
        inside=false;
    }
    SetLastError(last_error); _fxrstor(fp);
    return EXEC_DEFAULT; // All native registers/flags and flow remain unchanged.
}
}
bool Install(PatcherInstance* patcher, Callback callback, void* context, std::string* error) noexcept {
    if (!patcher || !callback || owned.load()) { Error(error,"invalid_or_already_installed"); return false; }
    if (!Validate()) { Error(error,"native_menu_sites_mismatch"); return false; }
    try {
        state.callback=callback; state.context=context; state.thread=GetCurrentThreadId();
        state.menu=patcher->CreateHiHook(0x4fbda0,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(MenuHook));
        owned.store(state.menu!=nullptr);
        if (!state.menu) { Rollback(); Error(error,"menu_hook_create_failed"); return false; }
        state.boundary=patcher->CreateLoHook(0x4ef278,BoundaryHook);
        owned.store(state.menu || state.boundary);
        if (!state.boundary || state.boundary->Apply()<0 || state.menu->Apply()<0 ||
            !state.boundary->IsApplied() || !state.menu->IsApplied()) {
            Error(error,Rollback()?"menu_hook_apply_failed":"menu_hook_rollback_incomplete"); return false;
        }
        active.store(true); Error(error,""); return true;
    } catch (...) { Error(error,Rollback()?"menu_hook_exception":"menu_hook_rollback_incomplete"); return false; }
}
bool Uninstall(std::string* error) noexcept {
    if (inside || (owned.load() && GetCurrentThreadId()!=state.thread) || state.claimed) {
        Error(error,"uninstall_requires_idle_game_thread"); return false;
    }
    if (request_lock.test_and_set()) { Error(error,"request_publication_in_progress"); return false; }
    const bool ok=Rollback(); request_lock.clear();
    Error(error,ok?"":"menu_hook_rollback_incomplete"); return ok;
}
bool HasOwnedHooks() noexcept { return owned.load(); }
bool IsReady() noexcept {
    std::uint32_t menu=0, depth=0, in_menu=0;
    return active.load() && !pending.load() && U32(0x6996b0,menu) && menu &&
        U32(0x6aad88,depth) && depth==1 && U32(0x698a20,in_menu) && in_menu==1 && ZeroByte(0x69d85c);
}
bool Queue(std::uint32_t token) noexcept {
    if (!token || !active.load() || request_lock.test_and_set()) return false;
    std::uint32_t menu=0, depth=0, in_menu=0;
    const bool menu_phase = U32(0x6996b0,menu) && menu && U32(0x6aad88,depth) && depth==1 &&
                           U32(0x698a20,in_menu) && in_menu==1 && ZeroByte(0x69d85c);
    const bool available = active.load() && menu_phase && !pending.load() && status.load()!=Status::ClosingNativeMenu;
    if (available) { pending.store(token); status.store(Status::Queued); }
    request_lock.clear(); return available;
}
bool CancelQueued() noexcept {
    if (request_lock.test_and_set()) return false;
    const bool queued=status.load()==Status::Queued;
    if (queued) { pending.store(0); status.store(Status::Cancelled); }
    request_lock.clear(); return queued;
}
Status GetStatus() noexcept { return status.load(); }
}
