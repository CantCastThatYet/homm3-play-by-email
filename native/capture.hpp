#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hota_capture {

// The caller must execute capture on the game's thread at a completed native
// save boundary. This copies existing state; it never calls an engine function.
struct CaptureLimits {
    std::size_t max_registry_entries = 256;
    std::size_t max_stream_bytes = 64u * 1024u * 1024u;
    std::size_t max_total_bytes = 256u * 1024u * 1024u;
    std::size_t max_deferred_packets = 4096;
    std::size_t max_packet_bytes = 1024u * 1024u;
    std::size_t max_deferred_bytes = 16u * 1024u * 1024u;
};

// Optional test reader. Production defaults to ReadProcessMemory on this process.
using MemoryReader = bool (*)(void*, std::uintptr_t, void*, std::size_t);
struct CaptureRequest {
    std::uintptr_t hd_base = 0;
    std::uintptr_t engine_base = 0;
    CaptureLimits limits{};
    MemoryReader reader = nullptr;
    void* reader_context = nullptr;
};

struct Participant {
    std::uint8_t ack = 0, done = 0, role = 0, participates = 0;
    std::int8_t predecessor = -1, successor = -1;
};
struct NativeFlag {
    std::uint32_t static_address = 0;
    std::int32_t value = 0;
};
struct CapturedStream {
    std::string role;
    std::string name;
    // present, absent, empty, busy, invalid, unreadable, limit_exceeded
    std::string status = "absent";
    std::string detail;
    std::uint32_t cursor = 0, length = 0, capacity = 0, open = 0;
    std::vector<std::uint8_t> bytes;
};
struct DeferredPacket {
    std::int32_t sender = -1;
    std::uint32_t message_id = 0;
    std::vector<std::uint8_t> bytes;
};
struct CaptureEnvelope {
    bool native_completion_observed = false, completion_marker_confirmed = false;
    std::uint32_t completion_source_site = 0;
    std::int32_t completion_player = -1;
    // A done marker does not imply that any existing world stream is fresh.
    bool completion_fresh_world_serialized = false;
    bool native_save_observed = false, native_save_name_complete = false;
    std::string native_save_name, native_save_name_bytes_hex;
    std::uint32_t native_save_flags[4]{};
    std::uint32_t native_save_result = 0;
    // Always false: this is evidence/checkpoint material, not a proven restore format.
    bool complete_restore = false;
    bool metadata_valid = false;
    std::string status = "unreadable";
    std::vector<std::string> warnings;
    std::int32_t local_player = -1, current_player = -1, st_day = 0;
    std::int32_t first_participant = -1;
    std::uint16_t world_date_words[3]{};
    bool world_date_valid = false;
    Participant participants[8]{};
    std::vector<NativeFlag> flags;
    std::vector<std::uint8_t> raw_st_metadata;
    std::vector<std::uint8_t> native_world_audit, hd_roster, hero_assignments;
    std::vector<CapturedStream> streams;
    std::string deferred_status = "unreadable";
    std::uint32_t deferred_declared_count = 0, deferred_capacity = 0;
    std::vector<DeferredPacket> deferred_packets;
    // Relay game messages the engine had consumed at capture; -1 without the relay.
    std::int64_t relay_consumed = -1;
    // Game messages this seat had sent through the relay; -1 without the relay.
    std::int64_t relay_sent = -1;
    // Scenario file the world was started from (world+0x1F6D9), e.g. "Judgement Day.h3m".
    std::string map_file;
};

CaptureEnvelope CaptureExistingBranch(const CaptureRequest& request) noexcept;

// Worker-thread only. Creates a NEW output directory (will not overwrite one).
// Writes payloads plus manifest.json last. Returns false if any write fails.
bool WriteEnvelope(const CaptureEnvelope& envelope, const std::wstring& output_directory,
                   std::string* error = nullptr) noexcept;

} // namespace hota_capture
