#include "solo_journal.hpp"
#include "common.hpp"
#include <deque>
#include <vector>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>

namespace hota_solo_journal {
namespace {
struct OwnedPacket {
    PacketView info;
    std::uint64_t ordinal = 0;
    std::vector<std::uint8_t> bytes;
};
SRWLOCK lock = SRWLOCK_INIT;
struct Guard { Guard() { AcquireSRWLockExclusive(&lock); } ~Guard() { ReleaseSRWLockExclusive(&lock); } };
std::deque<OwnedPacket> queue;
Snapshot state;
std::wstring root;
HANDLE wake = nullptr, worker = nullptr;
DWORD producer = 0;
std::uint32_t session_token = 0;
std::uint64_t last_sequence = 0, accepted_bytes = 0;

void Fail(const char* message) noexcept {
    try { Guard guard; state.fatal_error = message; } catch (...) { }
}
void CheckParents(const std::wstring& path) {
    auto parent = hp::Parent(path);
    if (parent.empty()) throw std::runtime_error("journal_parent_missing");
    for (;;) {
        if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
        const DWORD attributes = GetFileAttributesW(parent.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("journal_parent_missing_or_linked");
        if (parent.size() == 3 && parent[1] == L':') break;
        const auto next = hp::Parent(parent);
        if (next.empty() || next == parent) break;
        parent = next;
    }
}
void WriteNew(const std::wstring& path, const void* bytes, std::size_t length) {
    hp::Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!file) throw std::runtime_error("journal_create_file_failed");
    const auto* cursor = static_cast<const std::uint8_t*>(bytes);
    while (length) {
        const DWORD amount = static_cast<DWORD>((std::min)(length, static_cast<std::size_t>(1024 * 1024)));
        DWORD written = 0;
        if (!WriteFile(file.value, cursor, amount, &written, nullptr) || written != amount)
            throw std::runtime_error("journal_write_failed");
        cursor += amount; length -= amount;
    }
    if (!FlushFileBuffers(file.value)) throw std::runtime_error("journal_flush_failed");
}
void Persist(const OwnedPacket& packet) {
    CheckParents(root + L"\\entry"); // Recheck root/ancestors before each worker write.
    std::wostringstream name; name << L"\\packet-" << std::setfill(L'0') << std::setw(20) << packet.ordinal;
    const auto directory = root + name.str();
    if (!CreateDirectoryW(directory.c_str(), nullptr)) throw std::runtime_error("journal_packet_directory_exists_or_failed");
    const auto payload = directory + L"\\payload.bin";
    WriteNew(payload, packet.bytes.data(), packet.bytes.size());
    const auto hash = hp::Sha256(payload);
    if (hash.size() != 64) throw std::runtime_error("journal_hash_failed");
    const auto& p = packet.info;
    std::ostringstream json;
    json << "{\"schema\":\"hota-solo-packet-v1\",\"token\":" << p.token
         << ",\"sequence\":" << p.sequence << ",\"ordinal\":" << packet.ordinal
         << ",\"local_color\":" << p.local_color << ",\"recipient_color\":" << p.recipient_color
         << ",\"sender_dpid\":" << p.sender_dpid << ",\"receiver_dpid\":" << p.receiver_dpid
         << ",\"reliable\":" << p.reliable << ",\"size\":" << p.size
         << ",\"file\":\"payload.bin\",\"sha256\":\"" << hash
         << "\",\"durability\":\"local_files_flushed\",\"server_stored\":false,\"protocol_ack\":false}";
    const auto content = json.str();
    const auto pending = directory + L"\\manifest.json.pending";
    WriteNew(pending, content.data(), content.size());
    // Final manifest is the commit marker. No overwrite, no consumer-visible success before it.
    if (!MoveFileExW(pending.c_str(), (directory + L"\\manifest.json").c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("journal_manifest_publish_failed");
}
DWORD WINAPI Work(void*) noexcept {
    for (;;) {
        OwnedPacket packet; bool available = false;
        {
            Guard guard;
            if (!queue.empty()) { packet = std::move(queue.front()); queue.pop_front(); available = true; }
            else if (state.stopping) return 0;
        }
        if (!available) { if (WaitForSingleObject(wake, INFINITE) != WAIT_OBJECT_0) { Fail("journal_wait_failed"); return 1; } continue; }
        bool saved = false;
        try { Persist(packet); saved = true; }
        catch (const std::exception& error) { Fail(error.what()); }
        catch (...) { Fail("journal_unknown_write_failure"); }
        {
            Guard guard;
            state.pending_bytes -= packet.bytes.size();
            if (saved) ++state.durable;
            else {
                ++state.failed;
                state.failed += queue.size();
                for (const auto& queued : queue) state.pending_bytes -= queued.bytes.size();
                queue.clear(); state.stopping = true; return 1;
            }
        }
    }
}
}

bool Initialize(const std::wstring& output_directory, std::string* error) noexcept {
    try {
        Guard guard;
        if (worker || state.initialized) throw std::runtime_error("journal_already_initialized");
        if (output_directory.empty() || output_directory.size() > 220) throw std::runtime_error("journal_path_invalid");
        wchar_t absolute[MAX_PATH]{};
        const DWORD length = GetFullPathNameW(output_directory.c_str(), MAX_PATH, absolute, nullptr);
        if (!length || length >= MAX_PATH || output_directory.size() < 3 || output_directory[1] != L':' ||
            (output_directory[2] != L'\\' && output_directory[2] != L'/')) throw std::runtime_error("journal_absolute_local_path_required");
        root = absolute;
        CheckParents(root);
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw std::runtime_error("journal_directory_must_be_new");
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake) throw std::runtime_error("journal_event_create_failed");
        state = Snapshot{}; state.initialized = true;
        producer = 0; session_token = 0; last_sequence = 0; accepted_bytes = 0;
        worker = CreateThread(nullptr, 0, Work, nullptr, 0, nullptr);
        if (!worker) { CloseHandle(wake); wake = nullptr; state.initialized = false; throw std::runtime_error("journal_worker_create_failed"); }
        return true;
    } catch (const std::exception& ex) { if (error) try { *error = ex.what(); } catch (...) { } return false; }
      catch (...) { if (error) try { *error = "journal_initialize_failed"; } catch (...) { } return false; }
}

bool Accept(const PacketView& packet) noexcept {
    Guard guard;
    if (!state.initialized || state.stopping || !state.fatal_error.empty() || !worker ||
        !packet.token || !packet.sequence || !packet.data || !packet.size || packet.size > MaxPacketBytes ||
        packet.local_color < 0 || packet.local_color > 7 || packet.recipient_color < -1 || packet.recipient_color > 7 ||
        (producer && producer != GetCurrentThreadId()) || (session_token && session_token != packet.token) ||
        packet.sequence <= last_sequence || state.pending_bytes + packet.size > MaxPendingBytes ||
        accepted_bytes + packet.size > MaxJournalBytes || state.accepted >= MaxRecords) { ++state.rejected; return false; }
    try {
        OwnedPacket owned; owned.info = packet; owned.info.data = nullptr; owned.ordinal = state.accepted + 1;
        owned.bytes.resize(packet.size);
        SIZE_T copied = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), packet.data, owned.bytes.data(), packet.size, &copied) || copied != packet.size) {
            ++state.rejected; return false;
        }
        queue.push_back(std::move(owned));
        producer = GetCurrentThreadId(); session_token = packet.token; last_sequence = packet.sequence;
        ++state.accepted; state.pending_bytes += packet.size; accepted_bytes += packet.size;
        SetEvent(wake); return true;
    } catch (...) { ++state.rejected; return false; }
}

Snapshot GetSnapshot() noexcept {
    try { Guard guard; return state; }
    catch (...) { Snapshot failure; failure.failed = 1; return failure; }
}

void Shutdown() noexcept {
    HANDLE joining = nullptr;
    { Guard guard; if (!worker) return; state.stopping = true; joining = worker; SetEvent(wake); }
    if (GetCurrentThreadId() == GetThreadId(joining)) return; // Never self-join; caller contract forbids this.
    WaitForSingleObject(joining, INFINITE);
    { Guard guard; CloseHandle(worker); worker = nullptr; CloseHandle(wake); wake = nullptr; state.initialized = false; }
}
}
