// The HotA builds Heroes Post is made for: HotA 1.8.1 (its game executable and HotA.dll), with one of
// the HD Mod builds below. The game patch checks the loaded modules against these hashes and stays
// off otherwise; the installer checks the files before it installs anything.
//
// HD Mod updates itself, and every update moves code and data inside HD_HOTA.dll, where the patch
// hooks and reads. The code names every HD_HOTA.dll address as it is in 5.8 R22 and passes it
// through Rva(), which gives the address in the running build. A new HD build is added by running
// native/tools/hd_port.py on the previous build's HD_HOTA.dll and the new one, and copying what it
// finds into a Move table here.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hp_build {
struct File {
    const wchar_t* module; // the loaded module's file name
    const wchar_t* path;   // the file in the HotA folder
    const char* sha256;
};
// HotA's own files. HD loads HotA.dll from its _181fix copy.
inline constexpr File Game[] = {
    {L"h3hota HD.exe", L"h3hota HD.exe", "5aaab925f06cccf23bb09814767590a95b84a557eb33d244800520be4f1f18de"},
    {L"HotA.dll", L"_HD3_Data\\HotA.dll_181fix", "1fa9ad209e756d55e9578b290c4fe6c9ba030d3b52af2085e530416a61ac10c2"},
};

// [from, from + size) in R22's HD_HOTA.dll is at [to, to + size) in another build.
struct Move { std::uint32_t from, to, size; };

struct Hd {
    const wchar_t* name;
    File files[4];      // HD_HOTA.dll first: its base is the one Rva() addresses are relative to
    const Move* moves;  // nullptr: R22 itself
    std::size_t move_count;
    // Entry bytes of HD's operator new and sized delete, which the patch calls (relocation-free).
    const std::uint8_t* allocate; std::size_t allocate_size;
    const std::uint8_t* release; std::size_t release_size;
    // The word HD's allocator puts before a memory file's data; 0 where it puts none of its own.
    std::uint32_t payload_tag;
};

inline constexpr std::uint8_t R22Allocate[] = {0x55,0x8b,0xec,0xeb,0x0d,0xff,0x75,0x08};
inline constexpr std::uint8_t R22Release[] = {0x55,0x8b,0xec,0xff,0x75,0x08,0xe8,0x43,0x06,0x00,0x00,0x59,0x5d,0xc3};
// R24 allocates through pointers HD sets at start: jmp [malloc]; and push ebp ... call [free].
inline constexpr std::uint8_t R24Allocate[] = {0x55,0x8b,0xec,0x5d,0xff,0x25};
inline constexpr std::uint8_t R24Release[] = {0x55,0x8b,0xec,0x6a,0xff,0x68};

// From native/tools/hd_port.py, R22 -> R24; every entry was matched uniquely, or by all its callers.
inline constexpr Move R24Moves[] = {
    // code
    {0x002880, 0x002d60, 1},  // solo transport: activate peer
    {0x005540, 0x005a80, 1},  // memory file vtable entry
    {0x046760, 0x046d00, 1},  // memory file constructor
    {0x0467c0, 0x046d60, 1},  // memory file vtable entry
    {0x046830, 0x046dd0, 1},  // memory file write
    {0x046970, 0x046f10, 1},  // memory file clear
    {0x09ef00, 0x09f2a0, 1},  // world reload inner loader
    {0x11ea90, 0x11eea0, 1},  // simultaneous-turn merge frame
    {0x1753d0, 0x1757e0, 1},  // adventure controls refresh
    {0x17af2d, 0x17b28d, 16}, // completion site, secondary
    {0x17afc4, 0x17b324, 16}, // completion site, head
    {0x184550, 0x1848b0, 1},  // PvP options dialog
    {0x186ee0, 0x187240, 1},  // scenario options procedure
    {0x189ee0, 0x18a240, 1},  // scenario options
    {0x189f84, 0x18a2e4, 4},  // scenario options return
    {0x1952c0, 0x195620, 1},  // hero assignment vector grow
    {0x1c5227, 0x001db0, 1},  // operator new
    {0x1c55ba, 0x001e10, 1},  // sized operator delete
    // data
    {0x202b14, 0x202d8c, 12},    // memory file vtable
    {0x20e068, 0x20e068, 4},     // security cookie
    {0x20ec60, 0x20ec84, 4},     // file vtable
    {0x20fffa, 0x21001e, 0xa0},  // simultaneous-turn block, laid out the same
    {0x2123b4, 0x2123b0, 4},     // online lobby
    {0x2170d0, 0x2170b0, 0x3e0}, // peer table, 8 x 0x7c
    {0x217508, 0x217504, 4},     // exit pending
    {0x2176ec, 0x2176e8, 4},     // protected exit
    {0x21872c, 0x218724, 12},    // memory file registry
    {0x11b8c84, 0x11b8c7c, 4},   // hero roster pointer
};

inline constexpr Hd HdBuilds[] = {
    {L"HD 5.8 R22",
     {{L"HD_HOTA.dll", L"HD_HOTA.dll", "0a2c193d0c510d77080b669a710694ef282761b0ec6f6ac5b86f3166b8038550"},
      {L"HW_HOTA.dll", L"HW_HOTA.dll", "927a087c6d05d40c83de270516f48fdbd1360614259d30689179c1f7300f08db"},
      {L"patcher_x86.dll", L"patcher_x86.dll", "6134c353e5ac2a056e52bf548cdea853c4424fa5f955df1f5a1f073d784bab40"},
      {L"_HD3_.dll", L"_HD3_.dll", "3ed1852e08bae7be2461b31bd922531ccb0c46d01e5b0ecf46d0e7d0dbeeab49"}},
     nullptr, 0, R22Allocate, sizeof R22Allocate, R22Release, sizeof R22Release, 0xf00dbeef},
    {L"HD 5.8 R24",
     {{L"HD_HOTA.dll", L"HD_HOTA.dll", "50825b7d5dd53b899c0c749258ae8b1299fc993bdde4d4044c2916a660704aca"},
      {L"HW_HOTA.dll", L"HW_HOTA.dll", "903b3b5bb4e704a50de4cc9581d7cf585bb34ea833150f50bd7edd5835db2154"},
      {L"patcher_x86.dll", L"patcher_x86.dll", "9de3c571de02430045f332b1a129b8ba31298f9ae7529f554c679da8a571ee22"},
      {L"_HD3_.dll", L"_HD3_.dll", "8e03493c16cd242cef66336721e84cd8e71049674f72d1d69dd7a02723a285d8"}},
     R24Moves, sizeof R24Moves / sizeof R24Moves[0], R24Allocate, sizeof R24Allocate, R24Release, sizeof R24Release, 0},
};
inline constexpr const wchar_t* Name = L"HotA 1.8.1";

// The HD build the game runs with, chosen once its modules check out; R22 until then (and in tests).
inline std::atomic<const Hd*> current{&HdBuilds[0]};
inline const Hd& Current() noexcept { return *current.load(); }
inline const Hd* FindHd(const char* hd_hota_sha256) noexcept {
    for (const auto& b : HdBuilds) if (std::strcmp(b.files[0].sha256, hd_hota_sha256) == 0) return &b;
    return nullptr;
}

// An address in R22's HD_HOTA.dll (relative to its base) in the running build. One this build does
// not have comes out far beyond any module, so reads there fail instead of reading the wrong thing.
inline std::uint32_t Rva(std::uint32_t r22) noexcept {
    const auto& b = Current();
    if (!b.moves) return r22;
    for (std::size_t i = 0; i < b.move_count; ++i) {
        const auto& m = b.moves[i];
        if (r22 >= m.from && r22 - m.from < m.size) return m.to + (r22 - m.from);
    }
    return 0x7fff0000u;
}
}
