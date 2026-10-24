#include "solo_journal.hpp"
#include "common.hpp"
#include <cassert>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>

static std::string Read(const std::wstring& path) {
    std::ifstream input(path, std::ios::binary); assert(input);
    std::ostringstream contents; contents << input.rdbuf(); return contents.str();
}
int wmain() {
    static_assert(sizeof(void*) == 4, "Use the x86 toolchain");
    using namespace hota_solo_journal;
    wchar_t temporary[MAX_PATH]{}; assert(GetTempPathW(MAX_PATH, temporary));
    const std::wstring parent = std::wstring(temporary) + L"heroes-journal-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    assert(CreateDirectoryW(parent.c_str(), nullptr));
    const std::uint8_t bytes[] = { 'a', 'b', 'c' };
    PacketView packet{ 42, 1, 0, 1, 10, 20, 1, bytes, 3 };
    assert(!Accept(packet));
    std::string error;
    assert(!Initialize(L"relative-journal", &error));
    assert(!Initialize(parent, &error)); // Existing directories cannot be reused/overwritten.
    const auto output = parent + L"\\success";
    assert(Initialize(output, &error));
    assert(Accept(packet));
    assert(!Accept(packet)); // Duplicate sequence cannot overwrite an accepted record.
    auto invalid = packet; invalid.sequence = 2; invalid.token = 43; assert(!Accept(invalid));
    invalid = packet; invalid.sequence = 2; invalid.size = MaxPacketBytes + 1; assert(!Accept(invalid));
    invalid = packet; invalid.sequence = 2; invalid.data = reinterpret_cast<const std::uint8_t*>(1); assert(!Accept(invalid));
    packet.sequence = 2;
    bool other_thread_accepted = true;
    std::thread other([&] { other_thread_accepted = Accept(packet); }); other.join(); assert(!other_thread_accepted);
    assert(Accept(packet));
    Shutdown();
    auto status = GetSnapshot();
    assert(status.accepted == 2 && status.durable == 2 && status.failed == 0 && status.pending_bytes == 0 && status.rejected == 5);
    const auto first = output + L"\\packet-00000000000000000001";
    assert(Read(first + L"\\payload.bin") == "abc");
    const auto manifest = Read(first + L"\\manifest.json");
    assert(manifest.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != std::string::npos);
    assert(manifest.find("\"server_stored\":false") != std::string::npos);
    assert(manifest.find("\"protocol_ack\":false") != std::string::npos);
    assert(GetFileAttributesW((first + L"\\manifest.json.pending").c_str()) == INVALID_FILE_ATTRIBUTES);

    const auto failed = parent + L"\\failure";
    assert(Initialize(failed, &error));
    assert(CreateDirectoryW((failed + L"\\packet-00000000000000000001").c_str(), nullptr));
    packet.sequence = 1; assert(Accept(packet));
    Shutdown(); status = GetSnapshot();
    assert(status.accepted == 1 && status.durable == 0 && status.failed == 1 && status.pending_bytes == 0 && !status.fatal_error.empty());
    assert(GetFileAttributesW((failed + L"\\packet-00000000000000000001\\manifest.json").c_str()) == INVALID_FILE_ATTRIBUTES);
    assert(!Accept(packet));
    std::wcout << L"Journal tests passed; artifacts: " << parent << L"\n";
    return 0;
}
