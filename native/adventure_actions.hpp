#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;
namespace hota_adventure {
enum class Status : unsigned { Idle, Queued, Dispatching, Returned, Refused, Expired };
enum class Error : unsigned { None, NotInstalled, Busy, InvalidRequest, Expired,
    Manager, Modal, Combat, World, Owner, Date, SimultaneousState, Button, MessageWrite, GameplayBlocked };
struct Request {
    std::uint32_t token=0;
    int expected_local=-1;
    std::uint16_t day=0,week=0,month=0;
};
struct Snapshot {
    bool installed=false;
    Status status=Status::Idle;
    Error error=Error::None;
    std::uint32_t token=0, thread_id=0, native_result=0;
};
// Install on the game thread during adapter startup, after pinned module checks.
bool Install(PatcherInstance*,std::uintptr_t hd_base,std::string* error=nullptr) noexcept;
bool Uninstall(std::string* error=nullptr) noexcept;
using Predicate=bool(*)(void*) noexcept;
// Configure on the installed game thread outside any adventure callback.
// A true gate skips the entire native dispatch, including idle timeout/AI work.
bool ConfigureInputGate(Predicate,void* context,std::string* error=nullptr) noexcept;
// Called only before the outermost original idle dispatch. Returning true skips
// that entire dispatch. The callback may latch a permanent departure hold.
bool ConfigureIdleBoundary(Predicate,void* context,std::string* error=nullptr) noexcept;
bool IsAtIdleBoundary() noexcept;
bool IsProcessingIdle() noexcept;
// Worker-safe; one request. Expires after10seconds. Dispatch happens only in an
// idle adventure callback with matching owner/date and an unfinished ST branch.
bool QueueEndTurn(const Request&,std::string* error=nullptr) noexcept;
bool CancelQueued() noexcept;
Snapshot GetSnapshot() noexcept;
const char* StatusName(Status) noexcept;
const char* ErrorName(Error) noexcept;
// Returned means native event handler returned, not that HD completed the turn.
// Existing completion observer/manifest is the only completion evidence.
}
