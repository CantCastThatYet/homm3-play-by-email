#pragma once
#include <cstdint>
#include <string>

namespace hota_solo_journal {
struct PacketView {
    std::uint32_t token = 0;
    std::uint64_t sequence = 0;
    std::int32_t local_color = -1, recipient_color = -1;
    std::uint32_t sender_dpid = 0, receiver_dpid = 0, reliable = 0;
    const std::uint8_t* data = nullptr;
    std::uint32_t size = 0;
};
struct Snapshot {
    bool initialized = false, stopping = false;
    std::uint64_t accepted = 0, durable = 0, failed = 0, rejected = 0, pending_bytes = 0;
    std::string fatal_error;
};
constexpr std::uint32_t MaxPacketBytes = 16u * 1024 * 1024;
constexpr std::uint64_t MaxPendingBytes = 64ull * 1024 * 1024;
constexpr std::uint64_t MaxJournalBytes = 1024ull * 1024 * 1024;
constexpr std::uint64_t MaxRecords = 65536;

// Outside hooks. Requires a NEW directory under an existing, non-linked absolute parent.
// Initialize owns one worker; Shutdown drains accepted packets and waits outside engine hooks.
bool Initialize(const std::wstring& output_directory, std::string* error = nullptr) noexcept;
// One game-thread producer. Copies bounded bytes; true means accepted in RAM only.
// Never creates protocol acknowledgments and never reports remote/server persistence.
// One nonzero token per initialization; sequences must strictly increase (gaps allowed).
bool Accept(const PacketView& packet) noexcept;
Snapshot GetSnapshot() noexcept;
void Shutdown() noexcept;
}
