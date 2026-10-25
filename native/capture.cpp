#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "capture.hpp"
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hota_capture {
namespace {
constexpr std::uint32_t kHdPreferred = 0x01000000;
constexpr std::uint32_t kEnginePreferred = 0x00400000;
constexpr std::size_t kHardTotal = 512u * 1024u * 1024u;
// Layout evidence: re/merge-stream-lifecycle.md. Caller fingerprints the supported
// HD/HotA build before invoking this code; these offsets are not version universal.

bool NativeRead(void*, std::uintptr_t address, void* target, std::size_t length) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             target, length, &copied) != FALSE && copied == length;
}

class Reader {
public:
    explicit Reader(const CaptureRequest& request) : request_(request) {}
    bool bytes(std::uintptr_t address, void* output, std::size_t length) const {
        // All addresses belong to the known 32-bit target, including test fixtures.
        if (!address || address > UINT32_MAX || length > UINT32_MAX ||
            length > static_cast<std::size_t>(UINT32_MAX - address) + 1u) return false;
        return (request_.reader ? request_.reader : NativeRead)(
            request_.reader_context, address, output, length);
    }
    template<class T> bool value(std::uintptr_t address, T& output) const {
        return bytes(address, &output, sizeof output);
    }
    std::uintptr_t hd(std::uint32_t address) const {
        return relocate(request_.hd_base, address - kHdPreferred);
    }
    std::uintptr_t engine(std::uint32_t address) const {
        return relocate(request_.engine_base, address - kEnginePreferred);
    }
private:
    static std::uintptr_t relocate(std::uintptr_t base, std::uint32_t offset) {
        return base && base <= UINT32_MAX && offset <= UINT32_MAX - base ? base + offset : 0;
    }
    const CaptureRequest& request_;
};

std::uint32_t U32(const std::uint8_t* p) { std::uint32_t n; std::memcpy(&n, p, 4); return n; }
std::int32_t I32(const std::uint8_t* p) { std::int32_t n; std::memcpy(&n, p, 4); return n; }

bool SameName(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
        if (lower(static_cast<unsigned char>(left[i])) !=
            lower(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

struct RegistryEntry {
    std::uint32_t address = 0;
    std::string name;
    std::uint8_t header[0x11d]{};
};

bool Budget(std::size_t amount, std::size_t used, std::size_t maximum) {
    return used <= maximum && amount <= maximum - used;
}

void CaptureStream(const Reader& reader, const CaptureLimits& limits,
                   const std::vector<RegistryEntry>& entries, bool registry_complete,
                   CapturedStream& stream, std::size_t& used) {
    const RegistryEntry* found = nullptr;
    for (const auto& entry : entries) {
        if (!SameName(entry.name, stream.name)) continue;
        if (found) { stream.status = "invalid"; stream.detail = "duplicate registry name"; return; }
        found = &entry;
    }
    if (!found) {
        stream.status = registry_complete ? "absent" : "unreadable";
        if (!registry_complete) stream.detail = "registry incomplete; absence cannot be established";
        return;
    }
    const auto* h = found->header;
    const auto payload = U32(h + 0x10c);
    stream.cursor = U32(h + 0x108);
    stream.length = U32(h + 0x110);
    stream.capacity = U32(h + 0x114);
    stream.open = U32(h + 0x118);
    if (stream.length > stream.capacity || stream.cursor > stream.length ||
        stream.length > INT32_MAX || stream.capacity > INT32_MAX || stream.open > 1 ||
        (stream.capacity != 0 && payload == 0)) {
        stream.status = "invalid"; stream.detail = "invalid MemFile bounds or flags"; return;
    }
    if (stream.open) { stream.status = "busy"; stream.detail = "MemFile remains open"; return; }
    if (stream.length > limits.max_stream_bytes ||
        !Budget(stream.length, used, limits.max_total_bytes)) {
        stream.status = "limit_exceeded"; return;
    }
    if (!stream.length) { stream.status = "empty"; return; }
    stream.bytes.resize(stream.length);
    if (!reader.bytes(payload, stream.bytes.data(), stream.bytes.size())) {
        stream.bytes.clear(); stream.status = "unreadable"; return;
    }
    std::uint8_t after[0x11d]{};
    if (!reader.bytes(found->address, after, sizeof after) ||
        std::memcmp(after, h, sizeof after) != 0) {
        stream.bytes.clear(); stream.status = "invalid";
        stream.detail = "MemFile header changed during capture"; return;
    }
    used += stream.bytes.size();
    stream.status = "present";
}

void CaptureQueue(const Reader& reader, const CaptureLimits& limits,
                  CaptureEnvelope& envelope, std::size_t& used) {
    const auto* st = envelope.raw_st_metadata.data();
    const auto pointer = U32(st);
    const auto count = I32(st + 4);
    const auto capacity = I32(st + 8);
    envelope.deferred_declared_count = static_cast<std::uint32_t>(count);
    envelope.deferred_capacity = static_cast<std::uint32_t>(capacity);
    if (count < 0 || capacity < 0 || static_cast<std::uint64_t>(count) * 4 >
        static_cast<std::uint32_t>(capacity) || (count && !pointer)) {
        envelope.deferred_status = "invalid"; return;
    }
    if (static_cast<std::size_t>(count) > limits.max_deferred_packets) {
        envelope.deferred_status = "limit_exceeded"; return;
    }
    if (!count) { envelope.deferred_status = "empty"; return; }
    std::vector<std::uint32_t> pointers(static_cast<std::size_t>(count));
    if (!reader.bytes(pointer, pointers.data(), pointers.size() * sizeof(std::uint32_t))) {
        envelope.deferred_status = "unreadable"; return;
    }
    envelope.deferred_status = "present";
    std::size_t queue_bytes = 0;
    for (const auto packet_address : pointers) {
        std::uint8_t header[16]{};
        if (!reader.bytes(packet_address, header, sizeof header)) {
            envelope.deferred_status = "unreadable"; break;
        }
        const auto length = U32(header + 12);
        if (length < sizeof header) { envelope.deferred_status = "invalid"; break; }
        if (length > limits.max_packet_bytes || !Budget(length, queue_bytes, limits.max_deferred_bytes) ||
            !Budget(length, used, limits.max_total_bytes)) {
            envelope.deferred_status = "limit_exceeded"; break;
        }
        DeferredPacket packet;
        packet.sender = I32(header);
        packet.message_id = U32(header + 8);
        packet.bytes.resize(length);
        if (!reader.bytes(packet_address, packet.bytes.data(), length) ||
            std::memcmp(header, packet.bytes.data(), sizeof header) != 0) {
            envelope.deferred_status = "unreadable"; break;
        }
        queue_bytes += length;
        used += length;
        envelope.deferred_packets.push_back(std::move(packet));
    }
    std::vector<std::uint32_t> after(pointers.size());
    if (!reader.bytes(pointer, after.data(), after.size() * sizeof(std::uint32_t)) || after != pointers)
        envelope.deferred_status = "unstable";
    if (envelope.deferred_status != "present")
        envelope.warnings.push_back("Deferred packet capture is incomplete; copied packets are diagnostic only.");
}

std::string Json(const std::string& text) {
    std::ostringstream out; out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { out << '\\' << static_cast<char>(c); }
        else if (c < 0x20 || c >= 0x80) {
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else out << static_cast<char>(c);
    }
    out << '"'; return out.str();
}

std::string Sha256(const std::vector<std::uint8_t>& bytes) {
    struct Handles {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<std::uint8_t> object;
        ~Handles() {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    DWORD object_length = 0, returned = 0;
    if (BCryptGetProperty(handles.algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_length), sizeof object_length, &returned, 0) < 0 ||
        returned != sizeof object_length || object_length > 1024u * 1024u)
        throw std::runtime_error("SHA256 provider size invalid");
    handles.object.resize(object_length);
    if (BCryptCreateHash(handles.algorithm, &handles.hash, handles.object.data(), object_length, nullptr, 0, 0) < 0)
        throw std::runtime_error("SHA256 initialization failed");
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto length = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1024u * 1024u));
        if (BCryptHashData(handles.hash, const_cast<PUCHAR>(bytes.data() + offset), length, 0) < 0)
            throw std::runtime_error("SHA256 update failed");
        offset += length;
    }
    std::uint8_t digest[32]{};
    if (BCryptFinishHash(handles.hash, digest, sizeof digest, 0) < 0)
        throw std::runtime_error("SHA256 finalization failed");
    // The provider's hash object buffer must remain alive until DestroyHash.
    BCryptDestroyHash(handles.hash); handles.hash = nullptr;
    std::string output; output.reserve(64);
    constexpr char hex[] = "0123456789abcdef";
    for (auto value : digest) { output.push_back(hex[value >> 4]); output.push_back(hex[value & 15]); }
    return output;
}

bool WriteBytes(const std::filesystem::path& path, const void* data, std::size_t length) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto* next = static_cast<const std::uint8_t*>(data);
    bool success = true;
    while (length) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length, 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, next, chunk, &written, nullptr) || written != chunk) {
            success = false; break;
        }
        next += written; length -= written;
    }
    if (success && !FlushFileBuffers(handle)) success = false;
    if (!CloseHandle(handle)) success = false;
    return success;
}
} // namespace

CaptureEnvelope CaptureExistingBranch(const CaptureRequest& request) noexcept {
    CaptureEnvelope result;
    try {
        const auto& limits = request.limits;
        if (!request.hd_base || !request.engine_base || !limits.max_registry_entries ||
            limits.max_registry_entries > 4096 || limits.max_stream_bytes > kHardTotal ||
            limits.max_total_bytes > kHardTotal || limits.max_deferred_packets > 65536 ||
            limits.max_packet_bytes > 16u * 1024u * 1024u || limits.max_deferred_bytes > kHardTotal) {
            result.status = "invalid"; result.warnings.push_back("Invalid bases or capture limits."); return result;
        }
        Reader reader(request);
        result.raw_st_metadata.resize(0x9c);
        if (!reader.bytes(reader.hd(0x0120fffa), result.raw_st_metadata.data(), 0x9c) ||
            !reader.value(reader.engine(0x006995a4), result.local_player) ||
            !reader.value(reader.engine(0x0069ccf4), result.current_player)) {
            result.raw_st_metadata.clear(); return result;
        }
        const auto* st = result.raw_st_metadata.data();
        result.st_day = I32(st + (0x01210016 - 0x0120fffa));
        result.first_participant = I32(st + (0x0121008a - 0x0120fffa));
        for (std::uint32_t address = 0x01210006; address <= 0x01210046; address += 4)
            result.flags.push_back({address, I32(st + (address - 0x0120fffa))});
        for (const auto address : {0x0121007au, 0x0121007eu, 0x01210082u, 0x01210086u,
                                   0x0121008au, 0x0121008eu, 0x01210092u})
            result.flags.push_back({address, I32(st + (address - 0x0120fffa))});
        for (int i = 0; i < 8; ++i) {
            const auto* p = st + (0x0121004a - 0x0120fffa) + i * 6;
            result.participants[i] = {p[0], p[1], p[2], p[3], static_cast<std::int8_t>(p[4]),
                                      static_cast<std::int8_t>(p[5])};
        }
        std::uint32_t world = 0;
        if (reader.value(reader.engine(0x00699538), world) && world && world <= UINT32_MAX - 0x1f644)
            result.world_date_valid = reader.bytes(world + 0x1f63e, result.world_date_words, 6) &&
                result.world_date_words[0] >= 1 && result.world_date_words[0] <= 7 &&
                result.world_date_words[1] >= 1 && result.world_date_words[1] <= 4 && result.world_date_words[2] >= 1;
        result.metadata_valid = true;

        std::uint32_t registry[3]{};
        std::vector<RegistryEntry> entries;
        bool registry_complete = reader.bytes(reader.hd(0x0121872c), registry, sizeof registry);
        if (registry_complete && (registry[1] > limits.max_registry_entries ||
            static_cast<std::uint64_t>(registry[1]) * 4 > registry[2] || (registry[1] && !registry[0])))
            registry_complete = false;
        if (registry_complete) {
            std::vector<std::uint32_t> pointers(registry[1]);
            if (!pointers.empty() && !reader.bytes(registry[0], pointers.data(), pointers.size() * 4))
                registry_complete = false;
            else for (const auto address : pointers) {
                RegistryEntry entry; entry.address = address;
                if (!reader.bytes(address, entry.header, sizeof entry.header)) {
                    registry_complete = false; continue;
                }
                const auto* name = reinterpret_cast<const char*>(entry.header + 4);
                const auto* end = static_cast<const char*>(std::memchr(name, 0, 0x104));
                if (!end) { registry_complete = false; continue; }
                entry.name.assign(name, end); entries.push_back(std::move(entry));
            }
        }
        if (!registry_complete) result.warnings.push_back("MemFile registry is unreadable or fails bounds validation.");
        result.streams.push_back({"baseline_orig", "orig.dat"});
        result.streams.push_back({"baseline_day_one", "orig0.bin"});
        result.streams.push_back({"local_snapshot", "RMTGEXBS.BIN"});
        CapturedStream incoming; incoming.role = "incoming";
        char receive_name[13]{}; // Engine receive buffer ends at next name, 0069884D.
        if (reader.bytes(reader.engine(0x00698840), receive_name, sizeof receive_name) &&
            receive_name[0] && std::memchr(receive_name, 0, sizeof receive_name)) incoming.name = receive_name;
        else { incoming.status = "unreadable"; incoming.detail = "receive-name buffer unavailable or unterminated"; }
        result.streams.push_back(std::move(incoming));
        std::size_t used = 0;
        for (auto& stream : result.streams)
            if (!stream.name.empty()) CaptureStream(reader, limits, entries, registry_complete, stream, used);
        CaptureQueue(reader, limits, result, used);

        std::uint8_t after[0x9c]{};
        std::uint32_t registry_after[3]{};
        std::int32_t local_after = -1, current_after = -1;
        const bool stable = reader.bytes(reader.hd(0x0120fffa), after, sizeof after) &&
            std::memcmp(after, st, sizeof after) == 0 &&
            reader.bytes(reader.hd(0x0121872c), registry_after, sizeof registry_after) &&
            std::memcmp(registry_after, registry, sizeof registry) == 0 &&
            reader.value(reader.engine(0x006995a4), local_after) && local_after == result.local_player &&
            reader.value(reader.engine(0x0069ccf4), current_after) && current_after == result.current_player;
        result.status = stable ? "captured" : "unstable";
        if (!stable) result.warnings.push_back("Native metadata changed during capture; do not use as a checkpoint.");
        result.warnings.push_back("Existing streams can be stale; capture does not establish their branch/day provenance.");
        result.warnings.push_back("Not a complete restore format. Raw ST metadata includes process-local values.");
    } catch (...) { result.status = "error"; result.metadata_valid = false; }
    return result;
}

bool WriteEnvelope(const CaptureEnvelope& envelope, const std::wstring& output_directory,
                   std::string* error) noexcept {
    try {
        const std::filesystem::path directory(output_directory);
        if (output_directory.empty() || !std::filesystem::create_directory(directory)) {
            if (error) *error = "Capture directory must be new and its parent must exist.";
            return false;
        }
        const auto write = [&](const std::string& name, const std::vector<std::uint8_t>& data) {
            return WriteBytes(directory / name, data.data(), data.size());
        };
        std::ostringstream json;
        json << "{\n\"schema\":\"hota-native-capture-v1\",\"complete_restore\":false,"
             << "\"native_completion\":{\"observed\":" << (envelope.native_completion_observed ? "true" : "false")
             << ",\"source_site\":" << envelope.completion_source_site
             << ",\"player\":" << envelope.completion_player
             << ",\"marker_confirmed\":" << (envelope.completion_marker_confirmed ? "true" : "false")
             << ",\"fresh_world_serialized\":" << (envelope.completion_fresh_world_serialized ? "true" : "false") << "},"
             << "\"native_save\":{\"observed\":" << (envelope.native_save_observed ? "true" : "false")
             << ",\"name\":" << Json(envelope.native_save_name)
             << ",\"name_bytes_hex\":" << Json(envelope.native_save_name_bytes_hex)
             << ",\"name_complete\":" << (envelope.native_save_name_complete ? "true" : "false")
             << ",\"flags\":[" << envelope.native_save_flags[0] << ',' << envelope.native_save_flags[1] << ','
             << envelope.native_save_flags[2] << ',' << envelope.native_save_flags[3]
             << "],\"raw_result\":" << envelope.native_save_result
             << ",\"return_byte\":" << (envelope.native_save_result & 255u)
             << ",\"world_artifact_included\":false},"
             << "\"status\":" << Json(envelope.status) << ",\"metadata_valid\":"
             << (envelope.metadata_valid ? "true" : "false") << ",\"local_player\":" << envelope.local_player
             << ",\"current_player\":" << envelope.current_player << ",\"st_day\":" << envelope.st_day
             << ",\"first_participant\":" << envelope.first_participant << ",\"world_date_valid\":"
             << (envelope.world_date_valid ? "true" : "false") << ",\"world_date_words\":["
             << envelope.world_date_words[0] << ',' << envelope.world_date_words[1] << ','
             << envelope.world_date_words[2] << "],\n\"participants\":[";
        for (int i = 0; i < 8; ++i) {
            const auto& p = envelope.participants[i]; if (i) json << ',';
            json << "{\"player\":" << i << ",\"ack\":" << unsigned(p.ack) << ",\"done\":" << unsigned(p.done)
                 << ",\"role\":" << unsigned(p.role) << ",\"participates\":" << unsigned(p.participates)
                 << ",\"predecessor\":" << int(p.predecessor) << ",\"successor\":" << int(p.successor) << '}';
        }
        json << "],\n\"flags\":[";
        for (std::size_t i = 0; i < envelope.flags.size(); ++i) {
            if (i) json << ',';
            json << "{\"static_address\":" << envelope.flags[i].static_address << ",\"value\":"
                 << envelope.flags[i].value << '}';
        }
        json << "],\n\"relay_consumed\":" << envelope.relay_consumed << ",\"raw_st_metadata\":";
        if (envelope.raw_st_metadata.empty()) json << "null";
        else {
            if (!write("st-metadata.bin", envelope.raw_st_metadata)) throw std::runtime_error("ST metadata write failed");
            json << "\"st-metadata.bin\"";
        }
        json << ",\"raw_st_metadata_sha256\":" << Json(Sha256(envelope.raw_st_metadata));
        const auto extra = [&](const char* key, const char* name, const std::vector<std::uint8_t>& bytes) {
            json << ",\"" << key << "\":";
            if(bytes.empty()) { json << "null"; return; }
            if(!write(name,bytes)) throw std::runtime_error("Native recovery metadata write failed");
            json << "{\"file\":" << Json(name) << ",\"bytes\":" << bytes.size()
                 << ",\"sha256\":" << Json(Sha256(bytes)) << '}';
        };
        extra("world_audit","world-audit.bin",envelope.native_world_audit);
        extra("hd_roster","hd-roster.bin",envelope.hd_roster);
        extra("hero_assignments","hero-assignments.bin",envelope.hero_assignments);
        json << ",\n\"streams\":[";
        for (std::size_t i = 0; i < envelope.streams.size(); ++i) {
            if (i) json << ',';
            const auto& s = envelope.streams[i];
            json << "{\"role\":" << Json(s.role) << ",\"name\":" << Json(s.name)
                 << ",\"status\":" << Json(s.status) << ",\"detail\":" << Json(s.detail)
                 << ",\"cursor\":" << s.cursor << ",\"length\":" << s.length << ",\"capacity\":"
                 << s.capacity << ",\"open\":" << s.open << ",\"captured_bytes\":" << s.bytes.size()
                 << ",\"file\":";
            if (s.bytes.empty()) json << "null";
            else {
                const auto name = "stream-" + std::to_string(i) + ".bin";
                if (!write(name, s.bytes)) throw std::runtime_error("Stream write failed");
                json << Json(name);
            }
            json << ",\"sha256\":" << Json(Sha256(s.bytes)) << '}';
        }
        json << "],\n\"deferred\":{\"status\":" << Json(envelope.deferred_status)
             << ",\"declared_count\":" << envelope.deferred_declared_count << ",\"capacity_bytes\":"
             << envelope.deferred_capacity << ",\"copied_count\":" << envelope.deferred_packets.size()
             << ",\"packets\":[";
        for (std::size_t i = 0; i < envelope.deferred_packets.size(); ++i) {
            if (i) json << ',';
            const auto& p = envelope.deferred_packets[i];
            const auto name = "deferred-" + std::to_string(i) + ".bin";
            if (!write(name, p.bytes)) throw std::runtime_error("Deferred packet write failed");
            json << "{\"sender\":" << p.sender << ",\"message_id\":" << p.message_id
                 << ",\"bytes\":" << p.bytes.size() << ",\"file\":" << Json(name)
                 << ",\"sha256\":" << Json(Sha256(p.bytes)) << '}';
        }
        json << "]},\n\"warnings\":[";
        for (std::size_t i = 0; i < envelope.warnings.size(); ++i) {
            if (i) json << ','; json << Json(envelope.warnings[i]);
        }
        json << "]\n}\n";
        const auto manifest = json.str();
        if (!WriteBytes(directory / "manifest.json.pending", manifest.data(), manifest.size()))
            throw std::runtime_error("Manifest write failed");
        if (!MoveFileExW((directory / "manifest.json.pending").c_str(),
                         (directory / "manifest.json").c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Manifest publish failed");
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) { try { *error = exception.what(); } catch (...) {} }
    } catch (...) { if (error) { try { *error = "Capture write failed."; } catch (...) {} } }
    return false;
}
} // namespace hota_capture
