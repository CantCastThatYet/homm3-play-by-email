#pragma once
#include <cstdint>
#include <string>
class PatcherInstance;
namespace hota_port {
enum class Error : unsigned { None, NotInstalled, WrongThread, AlreadyArmed, InvalidPort,
    UnexpectedNativeFrame, InvalidElements, InvalidService, InvalidAddress,
    UnexpectedSequence, StackWriteFailed };
struct Snapshot {
    bool installed=false, armed=false;
    std::uint16_t port=0;
    unsigned query_applied=0, fill_applied=0;
    Error error=Error::None;
    bool Applied() const noexcept { return query_applied==1&&fill_applied==1&&error==Error::None; }
};
// Install only on the game thread after the adapter's pinned module checks.
bool Install(PatcherInstance*,std::string* error=nullptr) noexcept;
bool Uninstall(std::string* error=nullptr) noexcept;
// Scope narrowly around the native TCP configuration/prelude, before Host/Join.
// After Disarm, inspect Applied(); a requested port not applied must fail closed.
bool Arm(std::uint16_t port) noexcept;
Snapshot Disarm() noexcept;
Snapshot GetSnapshot() noexcept;
const char* ErrorName(Error) noexcept;
}
