// Synthetic 32-bit memory fixtures only. No game process, engine calls or injection.
#include "capture.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace hota_capture;
constexpr std::uintptr_t kSt = 0x0120fffa;
constexpr std::uintptr_t kRegistry = 0x0121872c;
constexpr std::uintptr_t kFirstObject = 0x03100000;
constexpr std::uintptr_t kFirstPayload = 0x03200000;
constexpr std::uintptr_t kPacket = 0x03400000;
constexpr const char* kAbcSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

void Require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

struct Memory {
    std::map<std::uintptr_t, std::vector<std::uint8_t>> blocks;
    bool change_metadata_on_second_read = false;
    int metadata_reads = 0;

    void Add(std::uintptr_t address, std::size_t length) { blocks[address].resize(length); }
    void At(std::uintptr_t address, std::size_t offset, std::uint32_t value) {
        auto& bytes = blocks.at(address);
        Require(offset + sizeof value <= bytes.size(), "Fixture write overflow");
        std::memcpy(bytes.data() + offset, &value, sizeof value);
    }
    void Word(std::uintptr_t address, std::uint32_t value) { Add(address, 4); At(address, 0, value); }
    static bool Read(void* context, std::uintptr_t address, void* output, std::size_t length) {
        auto& self = *static_cast<Memory*>(context);
        if (address == kSt && ++self.metadata_reads == 2 && self.change_metadata_on_second_read)
            self.At(kSt, 0x1c, 2);
        for (const auto& entry : self.blocks) {
            if (address >= entry.first && address - entry.first <= entry.second.size() &&
                length <= entry.second.size() - (address - entry.first)) {
                std::memcpy(output, entry.second.data() + (address - entry.first), length);
                return true;
            }
        }
        return false;
    }
    Memory() {
        Add(kSt, 0x9c);
        At(kSt, 0, 0x03300000); At(kSt, 4, 1); At(kSt, 8, 1024); At(kSt, 0x1c, 1);
        const std::uint8_t participant[] = {1, 1, 1, 1, 0, 255};
        std::memcpy(blocks[kSt].data() + 0x56, participant, sizeof participant);
        Word(0x006995a4, 1); Word(0x0069ccf4, 1); Word(0x00699538, 0x02000000);
        Add(0x0201f63e, 6);
        const std::uint16_t date[] = {1, 1, 1};
        std::memcpy(blocks[0x0201f63e].data(), date, sizeof date);
        Add(0x00698840, 13);
        std::memcpy(blocks[0x00698840].data(), "RMTabcRC.BIN", 12);
        Add(kRegistry, 12); At(kRegistry, 0, 0x03000000); At(kRegistry, 4, 4); At(kRegistry, 8, 32);
        Add(0x03000000, 16);
        const char* names[] = {"orig.dat", "orig0.bin", "RMTGEXBS.BIN", "RMTabcRC.BIN"};
        for (int i = 0; i < 4; ++i) {
            const auto object = kFirstObject + i * 0x1000;
            const auto payload = kFirstPayload + i * 0x1000;
            At(0x03000000, i * 4, static_cast<std::uint32_t>(object)); Add(object, 0x11d);
            std::memcpy(blocks[object].data() + 4, names[i], std::strlen(names[i]) + 1);
            At(object, 0x10c, static_cast<std::uint32_t>(payload)); At(object, 0x110, 3); At(object, 0x114, 16);
            Add(payload, 3); std::memcpy(blocks[payload].data(), "abc", 3);
        }
        Word(0x03300000, static_cast<std::uint32_t>(kPacket)); Add(kPacket, 28);
        At(kPacket, 8, 0x41a); At(kPacket, 12, 28);
    }
    CaptureEnvelope Capture(CaptureLimits limits = {}) {
        metadata_reads = 0;
        CaptureRequest request;
        request.hd_base = 0x01000000; request.engine_base = 0x00400000;
        request.limits = limits; request.reader = &Memory::Read; request.reader_context = this;
        return CaptureExistingBranch(request);
    }
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "Output file missing");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void TestCapture() {
    Memory valid;
    const auto result = valid.Capture();
    Require(result.status == "captured" && result.metadata_valid && !result.complete_restore,
            "Valid extraction or restore disclaimer failed");
    Require(result.local_player == 1 && result.current_player == 1 && result.st_day == 1,
            "Native player/day metadata incorrect");
    Require(result.participants[1].done == 1 && result.participants[1].predecessor == 0 &&
            result.participants[1].successor == -1, "Participant metadata incorrect");
    Require(result.world_date_valid && result.world_date_words[0] == 1, "World date missing");
    Require(result.streams.size() == 4, "Expected four stream roles");
    for (const auto& stream : result.streams)
        Require(stream.status == "present" && stream.bytes == std::vector<std::uint8_t>({'a','b','c'}),
                "Stream payload incorrect");
    Require(result.deferred_status == "present" && result.deferred_packets.size() == 1 &&
            result.deferred_packets[0].message_id == 0x41a, "Deferred packet missing");

    Memory missing; missing.At(kRegistry, 4, 3);
    Require(missing.Capture().streams[3].status == "absent", "Missing stream not reported absent");
    Memory cap; cap.At(kFirstObject, 0x110, 17);
    Require(cap.Capture().streams[0].status == "invalid", "Length above capacity accepted");
    Memory cursor; cursor.At(kFirstObject, 0x108, 4);
    Require(cursor.Capture().streams[0].status == "invalid", "Cursor above length accepted");
    Memory busy; busy.At(kFirstObject, 0x118, 1);
    Require(busy.Capture().streams[0].status == "busy", "Open MemFile was copied");
    Memory unreadable; unreadable.blocks.erase(kFirstPayload);
    Require(unreadable.Capture().streams[0].status == "unreadable", "Unmapped payload accepted");
    Memory registry_cap; registry_cap.At(kRegistry, 8, 12);
    Require(registry_cap.Capture().streams[0].status == "unreadable", "Registry capacity overflow accepted");
    Memory registry_count; registry_count.At(kRegistry, 4, 999999);
    Require(registry_count.Capture().streams[0].status == "unreadable", "Registry count limit ignored");
    Memory duplicate; std::memcpy(duplicate.blocks[kFirstObject + 0x1000].data() + 4, "orig.dat", 9);
    Require(duplicate.Capture().streams[0].status == "invalid", "Duplicate stream name accepted");

    CaptureLimits stream_limit; stream_limit.max_stream_bytes = 2;
    Require(valid.Capture(stream_limit).streams[0].status == "limit_exceeded", "Stream byte limit ignored");
    CaptureLimits aggregate_limit; aggregate_limit.max_total_bytes = 5;
    const auto limited = valid.Capture(aggregate_limit);
    Require(limited.streams[0].status == "present" && limited.streams[1].status == "limit_exceeded" &&
            limited.deferred_status == "limit_exceeded", "Aggregate byte budget ignored");
    Memory queue_count; queue_count.At(kSt, 4, 4097); queue_count.At(kSt, 8, 4097 * 4);
    Require(queue_count.Capture().deferred_status == "limit_exceeded", "Queue count limit ignored");
    Memory queue_capacity; queue_capacity.At(kSt, 8, 3);
    Require(queue_capacity.Capture().deferred_status == "invalid", "Queue pointer capacity ignored");
    Memory short_packet; short_packet.At(kPacket, 12, 15);
    Require(short_packet.Capture().deferred_status == "invalid", "Short packet accepted");
    Memory huge_packet; huge_packet.At(kPacket, 12, 2 * 1024 * 1024);
    Require(huge_packet.Capture().deferred_status == "limit_exceeded", "Packet limit ignored");
    CaptureLimits queue_limit; queue_limit.max_deferred_bytes = 27;
    Require(valid.Capture(queue_limit).deferred_status == "limit_exceeded", "Queue byte budget ignored");
    Memory unstable; unstable.change_metadata_on_second_read = true;
    Require(unstable.Capture().status == "unstable", "Changed native metadata accepted as stable");
}

void TestOutput(const std::filesystem::path& directory) {
    Memory fixture; const auto envelope = fixture.Capture();
    std::string error;
    Require(WriteEnvelope(envelope, directory.wstring(), &error), "Envelope write failed");
    Require(error.empty(), "Successful write left error");
    const auto manifest = ReadFile(directory / "manifest.json");
    Require(!std::filesystem::exists(directory / "manifest.json.pending"), "Pending manifest left after publish");
    Require(manifest.find("\"complete_restore\":false") != std::string::npos, "Restore disclaimer missing");
    Require(manifest.find(kAbcSha256) != std::string::npos, "SHA256 known-answer test failed");
    for (int i = 0; i < 4; ++i)
        Require(ReadFile(directory / ("stream-" + std::to_string(i) + ".bin")) == "abc", "Written payload differs");
    Require(ReadFile(directory / "st-metadata.bin").size() == 0x9c, "ST metadata file length incorrect");
    Require(ReadFile(directory / "deferred-0.bin").size() == 28, "Deferred packet file length incorrect");
    Require(!WriteEnvelope(envelope, directory.wstring(), &error) && !error.empty(), "Existing output overwritten");
    Require(ReadFile(directory / "manifest.json") == manifest, "No-overwrite call modified manifest");
    Require(!WriteEnvelope(envelope, (directory / "missing-parent" / "envelope").wstring(), &error),
            "Missing parent failure not returned");
}
} // namespace

int main(int argc, char** argv) {
    try {
        static_assert(sizeof(void*) == 4, "Capture tests must exercise the x86 build");
        TestCapture();
        const auto output = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::temp_directory_path() /
            ("hota-capture-tests-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(output), "Test output directory must be new");
        TestOutput(output / "envelope");
        std::cout << "Native capture tests passed (synthetic x86 memory only). Artifacts: " << output.string() << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Native capture tests FAILED: " << exception.what() << '\n';
        return 1;
    }
}
