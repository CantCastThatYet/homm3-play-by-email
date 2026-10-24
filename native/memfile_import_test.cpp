// Standalone synthetic tests. Never loads HD or calls an original game function.
#include "memfile_import.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace hota_memfile;
constexpr std::uint32_t base = 0x1000000, reg = base + 0x21872c, array = 0x3000000;
constexpr std::uint32_t first = 0x3100000, second = 0x3101000;
void Require(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
struct Fixture {
    std::map<std::uint32_t, std::vector<std::uint8_t>> memory;
    std::uint32_t next = 0x4000000, thread = 17;
    bool fingerprint = true, admitted = true, fail_second_admission = false;
    int creates = 0, writes = 0, destroys = 0, fail_create = 0, fail_write = 0, admissions = 0,ensures=0,fail_ensure=0;
    bool mutate_during_stage = false, short_write = false, corrupt_write = false;
    std::vector<std::uint32_t> live_stages;
    Importer importer;

    std::uint8_t* Ptr(std::uint32_t address, std::size_t length) noexcept {
        auto found = memory.upper_bound(address);
        if (found == memory.begin()) return nullptr;
        --found;
        const std::size_t offset = address - found->first;
        if (offset > found->second.size() || length > found->second.size() - offset) return nullptr;
        return found->second.data() + offset;
    }
    std::uint32_t Get(std::uint32_t address) {
        std::uint32_t v; auto* p = Ptr(address, 4); Require(p != nullptr, "read fixture memory");
        std::memcpy(&v, p, 4); return v;
    }
    void Put(std::uint32_t address, std::uint32_t value) {
        auto* p = Ptr(address, 4); Require(p != nullptr, "write fixture memory"); std::memcpy(p, &value, 4);
    }
    void Empty(std::uint32_t address, const char* name) {
        memory[address].resize(0x11d); Put(address, base + 0x202b14);
        std::memcpy(Ptr(address + 4, 0x104), name, std::strlen(name) + 1);
    }
    void Fill(std::uint32_t address, const void* data, std::size_t length) {
        const auto payload = next + 4; next += 0x10000;
        memory[payload - 4].resize(length + 4); Put(payload - 4, 0xf00dbeef);
        if (length) std::memcpy(Ptr(payload, length), data, length);
        Put(address + 0x108, static_cast<std::uint32_t>(length));
        Put(address + 0x10c, payload); Put(address + 0x110, static_cast<std::uint32_t>(length));
        Put(address + 0x114, static_cast<std::uint32_t>(length));
    }
    static bool Fingerprint(void* ctx, std::uint32_t hd) noexcept {
        return hd == base && static_cast<Fixture*>(ctx)->fingerprint;
    }
    static std::uint32_t Thread(void* ctx) noexcept { return static_cast<Fixture*>(ctx)->thread; }
    static std::uint8_t* Map(void* ctx, std::uint32_t at, std::size_t count, bool) noexcept {
        return static_cast<Fixture*>(ctx)->Ptr(at, count);
    }
    static std::uint32_t Create(void* ctx, std::uint32_t, const char* name) {
        auto& f = *static_cast<Fixture*>(ctx);
        if (++f.creates == f.fail_create) throw std::bad_alloc();
        const auto object = f.next; f.next += 0x10000; f.Empty(object, name);
        f.live_stages.push_back(object);
        if (f.mutate_during_stage && f.creates == 2) f.Put(first + 0x108, 1);
        return object;
    }
    static int Write(void* ctx, std::uint32_t, std::uint32_t object, const void* data, int count) {
        auto& f = *static_cast<Fixture*>(ctx);
        if (++f.writes == f.fail_write) throw std::bad_alloc();
        f.Fill(object, data, static_cast<std::size_t>(count));
        if (f.corrupt_write) *f.Ptr(f.Get(object + 0x10c), 1) ^= 1;
        return f.short_write ? count - 1 : count;
    }
    static void Destroy(void* ctx, std::uint32_t, std::uint32_t object) noexcept {
        auto& f = *static_cast<Fixture*>(ctx);
        const auto payload = f.Get(object + 0x10c);
        if (payload) f.memory.erase(payload - 4);
        f.memory.erase(object); ++f.destroys;
        for (auto it = f.live_stages.begin(); it != f.live_stages.end(); ++it)
            if (*it == object) { f.live_stages.erase(it); break; }
    }
    static bool Admission(void* ctx) noexcept {
        auto& f = *static_cast<Fixture*>(ctx); ++f.admissions;
        return f.admitted && !(f.fail_second_admission && f.admissions == 2);
    }
    static bool Ensure(void* ctx,std::uint32_t,const char* name) noexcept {
        auto& f=*static_cast<Fixture*>(ctx);
        if(++f.ensures==f.fail_ensure)return false;
        try {const auto object=f.next;f.next+=0x10000;f.Empty(object,name);
            const auto count=f.Get(reg+4);f.Put(array+count*4,object);f.Put(reg+4,count+1);return true;
        }catch(...){return false;}
    }
    testing::Backend Backend() { return {this, Fingerprint, Thread, Map, Create, Write, Destroy,Ensure}; }
    Fixture() {
        memory[reg].resize(12); memory[array].resize(32);
        Put(reg, array); Put(reg + 4, 2); Put(reg + 8, 32);
        Put(array, first); Put(array + 4, second);
        Empty(first, "ORIG0.BIN"); Empty(second, "RMTGEXBS.BIN");
        Fill(first, "abcdefgh", 8); Fill(second, "ijklmnop", 8);
        Put(first + 0x108, 0); Put(second + 0x108, 0);
        Require(static_cast<bool>(importer.InitializeForTesting(base, thread, Backend(), Admission, this)), "initialize fixture");
    }
    std::string Bytes(std::uint32_t address) {
        const auto n = Get(address + 0x110);
        return n ? std::string(reinterpret_cast<char*>(Ptr(Get(address + 0x10c), n)), n) : "";
    }
    std::array<std::vector<std::uint8_t>, 2> Headers() { return {memory[first], memory[second]}; }
    void Unchanged(const std::array<std::vector<std::uint8_t>, 2>& headers) {
        Require(memory[first] == headers[0] && memory[second] == headers[1], "original headers preserved");
        Require(Bytes(first) == "abcdefgh" && Bytes(second) == "ijklmnop", "original payloads preserved");
        Require(live_stages.empty(), "staging allocations cleaned");
    }
};
std::vector<Replacement> Pair() {
    return {{Role::BaselineDayOne, {'x','y'}}, {Role::LocalSnapshot, {'z'}}};
}
void SuccessAndEmpty() {
    Fixture f; const auto before = f.Headers();
    const auto old_a = f.Get(first + 0x10c), old_b = f.Get(second + 0x10c);
    Require(static_cast<bool>(f.importer.Replace(Pair())), "two-stream commit succeeds");
    Require(f.Bytes(first) == "xy" && f.Bytes(second) == "z", "shorter writes truncate exactly");
    Require(f.Get(first + 0x108) == 0 && f.Get(second + 0x108) == 0, "published cursors rewound");
    Require(std::memcmp(f.Ptr(first, 0x108), before[0].data(), 0x108) == 0 &&
        std::memcmp(f.Ptr(second, 0x108), before[1].data(), 0x108) == 0, "names and vtables preserved");
    Require(!f.Ptr(old_a, 1) && !f.Ptr(old_b, 1) && f.destroys == 2 && f.live_stages.empty(), "retired native ownership released");
    Require(static_cast<bool>(f.importer.Replace({{Role::BaselineDayOne, {}}})), "empty replacement succeeds");
    Require(f.Get(first + 0x10c) == 0 && f.Get(first + 0x110) == 0 && f.Get(first + 0x114) == 0, "empty replacement really clears storage");
}
void FailuresPreserveAll() {
    for (int mode = 0; mode < 5; ++mode) {
        Fixture f; const auto before = f.Headers();
        if (mode == 0) f.fail_create = 2;
        if (mode == 1) f.fail_write = 2;
        if (mode == 2) f.short_write = true;
        if (mode == 3) f.corrupt_write = true;
        if (mode == 4) f.fail_second_admission = true;
        const auto result = f.importer.Replace(Pair());
        Require(result.error == (mode == 4 ? Error::NotQuiescent : Error::StagingFailed), "injected failure reported");
        f.Unchanged(before);
    }
    Fixture f; f.fail_write = 2; Require(!f.importer.Replace(Pair()), "first attempt fails");
    f.fail_write = 0; Require(static_cast<bool>(f.importer.Replace(Pair())), "failure clears reentrancy guard");
}
void AdmissionAndMalformed() {
    for (int mode = 0; mode < 13; ++mode) {
        Fixture f; Error expected = Error::InvalidObject;
        if (mode == 0) { f.Put(second + 0x118, 1); expected = Error::Busy; }
        if (mode == 1) f.Put(second + 0x110, 900);
        if (mode == 2) std::memset(f.Ptr(second + 4, 0x104), 'x', 0x104);
        if (mode == 3) f.Put(second, 123);
        if (mode == 4) f.Put(f.Get(second + 0x10c) - 4, 0);
        if (mode == 5) { f.Put(array + 4, first); expected = Error::InvalidRegistry; }
        if (mode == 6) { f.Put(reg + 4, 300); expected = Error::InvalidRegistry; }
        if (mode == 7) { f.thread = 18; expected = Error::WrongThread; }
        if (mode == 8) { f.fingerprint = false; expected = Error::UnsupportedBuild; }
        if (mode == 9) { f.admitted = false; expected = Error::NotQuiescent; }
        if (mode == 10) { f.Put(reg + 4, 1); expected = Error::MissingStream; }
        if (mode == 11) { f.Put(second + 0x10c, f.Get(first + 0x10c)); expected = Error::InvalidRegistry; }
        if (mode == 12) { std::memcpy(f.Ptr(second + 4, 0x104), "orig0.bin", 10); expected = Error::InvalidRegistry; }
        const auto before = f.Headers();
        Require(f.importer.Replace(Pair()).error == expected, "admission/malformed error reported");
        Require(f.Headers() == before && f.creates == 0, "rejected before allocating or mutating targets");
    }
}
void LimitsAndStateChanges() {
    Fixture f; const auto before = f.Headers();
    Require(f.importer.Replace({{Role::BaselineLater, {'x'}}}).error == Error::MissingStream, "never creates missing role");
    Require(f.importer.Replace({{static_cast<Role>(999), {}}}).error == Error::InvalidRequest, "unknown role denied");
    Require(f.importer.Replace({{Role::LocalSnapshot, {}}, {Role::LocalSnapshot, {}}}).error == Error::InvalidRequest, "duplicate requests denied");
    Limits limits; limits.max_stream_bytes = 1;
    Require(f.importer.Replace(Pair(), limits).error == Error::LimitExceeded, "per-stream bound");
    limits.max_stream_bytes = 10; limits.max_total_bytes = 2;
    Require(f.importer.Replace(Pair(), limits).error == Error::LimitExceeded, "aggregate bound");
    f.Unchanged(before);
    f.mutate_during_stage = true;
    Require(f.importer.Replace(Pair()).error == Error::StateChanged, "changed native header aborts before publish");
    Require(f.Bytes(first) == "abcdefgh" && f.Bytes(second) == "ijklmnop" && f.live_stages.empty(), "changed-state abort preserves payload ownership");
    Importer uninitialized;
    Require(uninitialized.Replace(Pair()).error == Error::NotInitialized, "initialization required");
    Require(uninitialized.InitializeForTesting(base, 999, f.Backend(), Fixture::Admission, &f).error == Error::WrongThread,
        "binding only on installing game thread");
    Require(uninitialized.InitializeNative(base, f.thread, Fixture::Admission, &f).error != Error::None,
        "synthetic base cannot pass native fingerprint validation");
}
void EnsureMissingStreams() {
    Fixture f;const auto old=f.Headers();
    Require(bool(f.importer.EnsureRoles({Role::BaselineDayOne,Role::LocalSnapshot})),"existing roles admitted");
    Require(f.ensures==0&&f.Headers()==old,"ensure existing streams never opens or mutates them");
    Require(bool(f.importer.EnsureRoles({Role::BaselineLater})),"missing baseline constructed through seam");
    Require(f.ensures==1&&f.Get(reg+4)==3,"one registered object added");
    const auto added=f.Get(array+8);Require(f.Get(added+0x110)==0&&f.Get(added+0x118)==0,"new object empty and closed");
    Require(bool(f.importer.Replace({{Role::BaselineLater,{'n','e','w'}}})),"new registered role is replaceable");
    Require(f.Bytes(added)=="new"&&f.Bytes(first)=="abcdefgh"&&f.Bytes(second)=="ijklmnop","ensure and replace preserve unrelated payloads");
    Fixture fail;fail.fail_ensure=1;const auto before=fail.Headers();
    Require(fail.importer.EnsureRoles({Role::BaselineLater}).error==Error::StagingFailed,"factory failure surfaced");
    Require(fail.Headers()==before,"factory failure retains prior payloads");
    Fixture busy;busy.Put(second+0x118,1);
    Require(busy.importer.EnsureRoles({Role::BaselineLater}).error==Error::Busy&&busy.ensures==0,"busy registry blocks constructor");
    Fixture malformed;malformed.Put(array+4,first);
    Require(malformed.importer.EnsureRoles({Role::BaselineLater}).error==Error::InvalidRegistry&&malformed.ensures==0,"duplicate registry blocks constructor");
}
}
int main() {
    try {
        SuccessAndEmpty(); FailuresPreserveAll(); AdmissionAndMalformed(); LimitsAndStateChanges(); EnsureMissingStreams();
        std::cout << "MemFile import synthetic tests passed (no game execution).\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MemFile import test failure: " << e.what() << '\n'; return 1;
    }
}
