#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;
namespace hota_intro {
enum class Status : unsigned { Idle, Queued, Dispatching, Returned, Refused, Expired };
struct Request {
    std::uint32_t token=0,expected_world=0;
    int expected_local=-1;
    std::uint16_t day=0,week=0,month=0;
    // Caller binds this to its owned validated native bootstrap map selection.
    std::string map_basename;
};
struct Snapshot {
    bool installed=false;
    Status status=Status::Idle;
    std::uint32_t token=0,callback_calls=0,native_result=0;
    bool native_ok_confirmed=false;
    const char* error="";
};
bool Install(PatcherInstance*,std::string* error=nullptr) noexcept;
bool Uninstall(std::string* error=nullptr) noexcept;
// Only the observed166-byte Judgement Day Russian intro is recognized. No
// arbitrary dialog text, pointer, button or native function comes from IPC.
bool QueueKnownIntro(const Request&,std::string* error=nullptr) noexcept;
bool CancelQueued() noexcept;
Snapshot GetSnapshot() noexcept;
const char* StatusName(Status) noexcept;
}
