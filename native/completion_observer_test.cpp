// Tests only byte matching, relocation and snapshot parsing. No hooks are applied.
#include "completion_observer.hpp"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using namespace hota_completion;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Memory {
    std::map<std::uintptr_t, std::vector<std::uint8_t>> blocks;
    std::uintptr_t base = 0x04000000;
    int st_reads = 0;
    bool unstable = false;
    std::uintptr_t Hd(std::uint32_t address) const { return base + address - 0x01000000; }
    void Add(std::uintptr_t address, std::size_t count) { blocks[address].resize(count); }
    void Put(std::uintptr_t address, std::size_t offset, std::uint32_t value) {
        std::memcpy(blocks.at(address).data() + offset, &value, 4);
    }
    void Word(std::uintptr_t address, std::uint32_t value) { Add(address, 4); Put(address, 0, value); }
    static bool Read(void* context, std::uintptr_t address, void* output, std::size_t size) {
        auto& self = *static_cast<Memory*>(context);
        if (address == self.Hd(0x0120fffa) && ++self.st_reads == 2 && self.unstable)
            self.Put(address, 0x1c, 2);
        for (const auto& block : self.blocks) {
            if (address >= block.first && address - block.first <= block.second.size() &&
                size <= block.second.size() - (address - block.first)) {
                std::memcpy(output, block.second.data() + address - block.first, size); return true;
            }
        }
        return false;
    }
    Memory() {
        blocks[Hd(0x0117af2d)] = {0xc6,0x04,0x45,0,0,0,0,1,0xc7,5,0xd8,0x77,0x69,0,0,0,0,0};
        blocks[Hd(0x0117afc4)] = {0xc6,0x04,0x45,0,0,0,0,1,0x8b,0x0d,0x38,0x95,0x69,0};
        Put(Hd(0x0117af2d), 3, static_cast<std::uint32_t>(Hd(0x0121004b)));
        Put(Hd(0x0117afc4), 3, static_cast<std::uint32_t>(Hd(0x0121004b)));
        Add(Hd(0x0120fffa), 0x9c); Put(Hd(0x0120fffa), 0x1c, 1); Put(Hd(0x0120fffa), 0x48, 1);
        auto& st = blocks[Hd(0x0120fffa)];
        st[0x56 + 1] = 1; st[0x56 + 2] = 1; st[0x56 + 3] = 1; st[0x56 + 5] = 255;
        Word(0x00699538, 0x02000000); Word(0x006995a4, 1); Word(0x0069ccf4, 1); Word(0x0069959c, 1);
        Add(0x0201f63e, 6); blocks[0x0201f63e][0] = 1; blocks[0x0201f63e][2] = 1; blocks[0x0201f63e][4] = 1;
    }
    Snapshot SnapshotAt(SourceSite site = SourceSite::SecondaryAfterDone, int eax = 3) {
        st_reads = 0; return ReadSnapshot(site, base, eax, Read, this);
    }
};
}

int main() {
    try {
        static_assert(sizeof(void*) == 4, "Use x86 tests");
        Memory memory;
        Require(ValidatePinnedSites(memory.base, Memory::Read, &memory), "Relocated pinned bytes rejected");
        memory.blocks[memory.Hd(0x0117af2d)][3] ^= 1;
        Require(!ValidatePinnedSites(memory.base, Memory::Read, &memory), "Wrong relocated store accepted");
        memory.blocks[memory.Hd(0x0117af2d)][3] ^= 1;
        memory.blocks[memory.Hd(0x0117afc4)][8] ^= 1;
        Require(!ValidatePinnedSites(memory.base, Memory::Read, &memory), "Changed hook instruction accepted");
        Require(!ValidatePinnedSites(0xfffffff0u, Memory::Read, &memory), "Overflow base accepted");
        const auto secondary = memory.SnapshotAt();
        Require(secondary.marker_confirmed && secondary.local_player == 1 && secondary.site_player == 1 &&
                secondary.participants[1].successor == -1 && !secondary.fresh_world_serialized,
                "Secondary marker parsing failed");
        Require(!memory.SnapshotAt(SourceSite::HeadAfterDone).marker_confirmed, "Wrong role accepted");
        memory.blocks[memory.Hd(0x0120fffa)][0x56 + 2] = 0;
        Require(memory.SnapshotAt(SourceSite::HeadAfterDone).marker_confirmed, "Head marker parsing failed");
        Require(!memory.SnapshotAt(SourceSite::HeadAfterDone, 4).marker_confirmed, "Non-slot EAX accepted");
        Require(!memory.SnapshotAt(SourceSite::HeadAfterDone, 24).marker_confirmed, "Out-of-range EAX accepted");
        memory.Word(0x006995a4, 2);
        Require(!memory.SnapshotAt(SourceSite::HeadAfterDone).marker_confirmed, "Peer identity mismatch accepted");
        memory.Word(0x006995a4, 1); memory.unstable = true;
        Require(!memory.SnapshotAt(SourceSite::HeadAfterDone).metadata_stable, "Metadata race accepted");
        memory.unstable = false; memory.blocks.erase(0x00699538);
        Require(!memory.SnapshotAt().metadata_readable, "Missing world pointer accepted");
        Require(!HasOwnedHooks(), "Synthetic tests unexpectedly installed hooks");
        std::cout << "Completion observer parsing tests passed; no native execution or hook install tested.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Completion observer tests FAILED: " << error.what() << '\n'; return 1;
    }
}
