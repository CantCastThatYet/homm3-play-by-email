#include "memfile_import.hpp"
#include "common.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#if !defined(_M_IX86)
#error MemFile import requires the verified Windows x86 ABI.
#endif

namespace hota_memfile {
namespace {
constexpr std::uint32_t kRegistryRva = 0x21872c, kVtableRva = 0x202b14;
constexpr std::size_t kHardStream = 64u * 1024u * 1024u;
constexpr char kHash[] = "0a2c193d0c510d77080b669a710694ef282761b0ec6f6ac5b86f3166b8038550";
#pragma pack(push, 1)
struct Object {
    std::uint32_t vtable;
    char name[0x104];
    std::uint32_t cursor, payload, length, capacity, open;
    std::uint8_t status;
};
#pragma pack(pop)
static_assert(sizeof(Object) == 0x11d && offsetof(Object, cursor) == 0x108,
              "Pinned native MemFile layout");

std::uint8_t* NativeMap(void*, std::uint32_t address, std::size_t size, bool writable) noexcept {
    if (!address || !size || size > UINT32_MAX - address) return nullptr;
    std::uintptr_t at = address, end = at + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return nullptr;
        const auto protection = info.Protect & 0xff;
        const bool can_write = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool can_read = can_write || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
        if (!can_read || (writable && !can_write)) return nullptr;
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= at) return nullptr;
        at = next;
    }
    return reinterpret_cast<std::uint8_t*>(address);
}
bool Signature(std::uint32_t address, const std::uint8_t* bytes, std::size_t length) {
    const auto* p = NativeMap(nullptr, address, length, false);
    MEMORY_BASIC_INFORMATION info{};
    if (!p || !VirtualQuery(p, &info, sizeof info)) return false;
    const auto protection = info.Protect & 0xff;
    return (protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY) && std::memcmp(p, bytes, length) == 0;
}
bool NativeFingerprint(void*, std::uint32_t base) noexcept {
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(base), &module) ||
            reinterpret_cast<std::uintptr_t>(module) != base ||
            hp::Sha256(hp::ModulePath(module)) != kHash) return false;
        // Relocation-free entry prefixes from re/memfile-import-static.txt.
        constexpr std::uint8_t ctor[] = {0x55,0x8b,0xec,0x8b,0x45,0x08,0x56,0x8b,0xf1};
        constexpr std::uint8_t write[] = {0x55,0x8b,0xec,0x83,0xec,0x08,0x53,0x8b,0x5d,0x0c};
        constexpr std::uint8_t clear[] = {0x56,0x8b,0xf1,0x8b,0x8e,0x0c,0x01,0x00,0x00};
        constexpr std::uint8_t alloc[] = {0x55,0x8b,0xec,0xeb,0x0d,0xff,0x75,0x08};
        constexpr std::uint8_t sized_delete[] = {0x55,0x8b,0xec,0xff,0x75,0x08,0xe8,0x43,0x06,0x00,0x00,0x59,0x5d,0xc3};
        if (!Signature(base + 0x46760, ctor, sizeof ctor) ||
            !Signature(base + 0x46830, write, sizeof write) ||
            !Signature(base + 0x46970, clear, sizeof clear) ||
            !Signature(base + 0x1c5227, alloc, sizeof alloc) ||
            !Signature(base + 0x1c55ba, sized_delete, sizeof sized_delete)) return false;
        const auto* table = NativeMap(nullptr, base + kVtableRva, 12, false);
        std::uint32_t expected[3] = {base + 0x5540, base + 0x467c0, base + 0x46830};
        return table && std::memcmp(table, expected, sizeof expected) == 0;
    } catch (...) { return false; }
}
std::uint32_t NativeThread(void*) noexcept { return GetCurrentThreadId(); }
std::uint32_t NativeCreate(void*, std::uint32_t base, const char* name) {
    using Allocate = void* (__cdecl*)(std::size_t);
    using Construct = void* (__thiscall*)(void*, const char*);
    using Delete = void (__cdecl*)(void*, std::size_t);
    void* object = reinterpret_cast<Allocate>(base + 0x1c5227)(sizeof(Object));
    if (!object) return 0;
    try { reinterpret_cast<Construct>(base + 0x46760)(object, name); }
    catch (...) { reinterpret_cast<Delete>(base + 0x1c55ba)(object, sizeof(Object)); throw; }
    return reinterpret_cast<std::uint32_t>(object);
}
int NativeWrite(void*, std::uint32_t base, std::uint32_t object, const void* bytes, int count) {
    using Write = int (__thiscall*)(void*, const void*, int);
    return reinterpret_cast<Write>(base + 0x46830)(reinterpret_cast<void*>(object), bytes, count);
}
void NativeDestroy(void*, std::uint32_t base, std::uint32_t object) noexcept {
    using Clear = void (__thiscall*)(void*);
    using Delete = void (__cdecl*)(void*, std::size_t);
    reinterpret_cast<Clear>(base + 0x46970)(reinterpret_cast<void*>(object));
    reinterpret_cast<Delete>(base + 0x1c55ba)(reinterpret_cast<void*>(object), sizeof(Object));
}
bool NativePreparationFingerprint() noexcept {
    try {
        const auto module=GetModuleHandleW(nullptr);
        if(reinterpret_cast<std::uintptr_t>(module)!=0x400000||hp::Sha256(hp::ModulePath(module))!=
           "5aaab925f06cccf23bb09814767590a95b84a557eb33d244800520be4f1f18de")return false;
        constexpr std::uint8_t open_tail[]={0x8b,0xc6,0x5e,0x64,0x89,0x0d,0,0,0,0,0x8b,0xe5,0x5d,0xc2,8,0};
        constexpr std::uint8_t close_tail[]={0xc7,6,0xc0,0xda,0x63,0,0x5e,0xc3};
        return Signature(0x4d6f16,open_tail,sizeof open_tail)&&Signature(0x4d6fd1,close_tail,sizeof close_tail);
    }catch(...) {return false;}
}
bool NativeEnsure(void*,std::uint32_t base,const char* name) noexcept {
    // Call installed engine entries, retaining HD's allocation/registration and
    // close chain. rb cannot truncate an existing file if interception fails.
    // Verified native constructor ECX=8-byte object, (name,mode), RET8; close
    // ECX=object, no stack args, RET. HD's wrapper vtable is RVA20EC60.
    if(!NativePreparationFingerprint())return false;
    std::uint32_t file[2]{};
    using Open=void*(__thiscall*)(void*,const char*,const char*);
    using Close=void(__thiscall*)(void*);
    __try {
        reinterpret_cast<Open>(0x4d6eb0)(file,name,"rb");
        const bool native=file[0]==base+0x20ec60&&file[1]!=0;
        reinterpret_cast<Close>(0x4d6fc0)(file);
        return native;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Do not run a second uncertain destructor after a native exception.
        return false;
    }
}
const char* Name(Role role) noexcept {
    switch (role) {
    case Role::BaselineDayOne: return "orig0.bin";
    case Role::BaselineLater: return "orig.dat";
    case Role::LocalSnapshot: return "RMTGEXBS.BIN";
    default: return nullptr;
    }
}
bool SameName(const char* a, const char* b) noexcept {
    for (;;) {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
        if (lower(*a) != lower(*b)) return false;
        if (!*a) return true;
        ++a; ++b;
    }
}
std::uint8_t* Map(const testing::Backend& b, std::uint32_t address, std::size_t size, bool write = false) {
    if (!address || !size || size > UINT32_MAX - address) return nullptr;
    return b.map(b.context, address, size, write);
}
bool ValidObject(const testing::Backend& b, const Object& o, std::uint32_t base) {
    if (o.vtable != base + kVtableRva || !std::memchr(o.name, 0, sizeof o.name) || !o.name[0] ||
        o.length > o.capacity || o.cursor > o.length || o.capacity > kHardStream || o.open > 1 ||
        ((o.capacity == 0) != (o.payload == 0))) return false;
    if (!o.payload) return true;
    if (o.payload < 4) return false;
    auto* bytes = Map(b, o.payload - 4, static_cast<std::size_t>(o.capacity) + 4);
    std::uint32_t tag = 0;
    if (!bytes) return false;
    std::memcpy(&tag, bytes, 4);
    return tag == 0xf00dbeef;
}
bool Overlaps(std::uint32_t a, std::size_t a_size, std::uint32_t b, std::size_t b_size) noexcept {
    return a_size && b_size && static_cast<std::uint64_t>(a) + a_size > b &&
        static_cast<std::uint64_t>(b) + b_size > a;
}
struct Entry { std::uint32_t address; Object before; Object* live; };
struct Stage {
    testing::Backend backend{};
    std::uint32_t base = 0, address = 0;
    Object* live = nullptr;
    ~Stage() { if (address) backend.destroy(backend.context, base, address); }
};
}

Result Importer::InitializeNative(std::uint32_t base, std::uint32_t thread,
                                 Admission admission, void* context) noexcept {
    return InitializeForTesting(base, thread,
        {nullptr, NativeFingerprint, NativeThread, NativeMap, NativeCreate, NativeWrite, NativeDestroy,NativeEnsure},
        admission, context);
}
Result Importer::EnsureRoles(const std::vector<Role>& roles) noexcept {
    if(!hd_base_)return {Error::NotInitialized};
    if(backend_.thread_id(backend_.context)!=game_thread_)return {Error::WrongThread};
    if(active_)return {Error::Reentrant};
    active_=true;struct Reset{bool& value;~Reset(){value=false;}} reset{active_};
    try {
        if(!admission_(admission_context_))return {Error::NotQuiescent};
        if(!backend_.fingerprint(backend_.context,hd_base_))return {Error::UnsupportedBuild};
        if(roles.empty()||roles.size()>3||!backend_.ensure_registered)return {Error::InvalidRequest};
        for(std::size_t i=0;i<roles.size();++i){if(!Name(roles[i]))return {Error::InvalidRequest};
            for(std::size_t j=0;j<i;++j)if(roles[i]==roles[j])return {Error::InvalidRequest};}
        auto inspect=[&](const char* wanted,bool& found)->Error {
            found=false;std::uint32_t r[3]{};auto* p=Map(backend_,hd_base_+kRegistryRva,sizeof r);
            if(!p)return Error::InvalidRegistry;std::memcpy(r,p,sizeof r);
            if(r[1]>256||r[2]>4096*4||r[2]%32||r[1]*4>r[2]||((r[2]==0)!=(r[0]==0))||
               Overlaps(r[0],r[2],hd_base_+kRegistryRva,12))return Error::InvalidRegistry;
            auto* pointers=r[2]?Map(backend_,r[0],r[2]):nullptr;if(r[2]&&!pointers)return Error::InvalidRegistry;
            std::vector<Entry> seen;seen.reserve(r[1]);
            for(std::uint32_t i=0;i<r[1];++i) {
                std::uint32_t address=0;std::memcpy(&address,pointers+i*4,4);auto* bytes=Map(backend_,address,sizeof(Object));
                if(!bytes)return Error::InvalidRegistry;Entry e{address,{},nullptr};std::memcpy(&e.before,bytes,sizeof(Object));
                if(!ValidObject(backend_,e.before,hd_base_))return Error::InvalidObject;
                if(e.before.open)return Error::Busy;
                const auto payload=e.before.payload?e.before.payload-4:0;
                const std::size_t size=e.before.payload?e.before.capacity+4u:0;
                if(Overlaps(address,sizeof(Object),r[0],r[2])||Overlaps(payload,size,r[0],r[2])||
                   Overlaps(address,sizeof(Object),hd_base_+kRegistryRva,12)||Overlaps(payload,size,hd_base_+kRegistryRva,12)||
                   Overlaps(address,sizeof(Object),payload,size))return Error::InvalidRegistry;
                for(const auto& old:seen) {
                    const auto old_payload=old.before.payload?old.before.payload-4:0;
                    const std::size_t old_size=old.before.payload?old.before.capacity+4u:0;
                    if(SameName(e.before.name,old.before.name)||Overlaps(address,sizeof(Object),old.address,sizeof(Object))||
                       Overlaps(payload,size,old_payload,old_size)||Overlaps(address,sizeof(Object),old_payload,old_size)||
                       Overlaps(payload,size,old.address,sizeof(Object)))return Error::InvalidRegistry;
                }
                if(SameName(e.before.name,wanted))found=true;seen.push_back(e);
            }
            return Error::None;
        };
        // Validate complete registry before invoking even the first constructor.
        bool found=false;auto error=inspect(Name(roles.front()),found);if(error!=Error::None)return {error};
        for(auto role:roles) {
            error=inspect(Name(role),found);if(error!=Error::None)return {error};if(found)continue;
            std::uint32_t count=0;auto* count_ptr=Map(backend_,hd_base_+kRegistryRva+4,4);
            if(!count_ptr)return {Error::InvalidRegistry};std::memcpy(&count,count_ptr,4);
            if(count>=256)return {Error::LimitExceeded};
            if(!admission_(admission_context_))return {Error::NotQuiescent};
            if(!backend_.ensure_registered(backend_.context,hd_base_,Name(role)))return {Error::StagingFailed};
            error=inspect(Name(role),found);if(error!=Error::None)return {error};if(!found)return {Error::MissingStream};
        }
        return admission_(admission_context_)?Result{}:Result{Error::NotQuiescent};
    }catch(...) {return {Error::StagingFailed};}
}
Result Importer::InitializeForTesting(std::uint32_t base, std::uint32_t thread,
                                    testing::Backend b, Admission admission, void* context) noexcept {
    if (active_) return {Error::Reentrant};
    hd_base_ = 0;
    if (!base || base > UINT32_MAX - 0x220000 || !thread || !admission ||
        !b.fingerprint || !b.thread_id || !b.map || !b.create || !b.write || !b.destroy)
        return {Error::InvalidRequest};
    if (b.thread_id(b.context) != thread) return {Error::WrongThread};
    if (!b.fingerprint(b.context, base)) return {Error::UnsupportedBuild};
    backend_ = b; hd_base_ = base; game_thread_ = thread;
    admission_ = admission; admission_context_ = context;
    return {};
}
Result Importer::Replace(const std::vector<Replacement>& requests, Limits limits) noexcept {
    if (!hd_base_) return {Error::NotInitialized};
    if (backend_.thread_id(backend_.context) != game_thread_) return {Error::WrongThread};
    if (active_) return {Error::Reentrant};
    active_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{active_};
    try {
        if (!admission_(admission_context_)) return {Error::NotQuiescent};
        if (!backend_.fingerprint(backend_.context, hd_base_)) return {Error::UnsupportedBuild};
        if (requests.empty() || requests.size() > 3 || !limits.max_registry_entries ||
            limits.max_registry_entries > 4096 || limits.max_stream_bytes > kHardStream ||
            limits.max_total_bytes > kHardStream * 3) return {Error::InvalidRequest};
        std::size_t total = 0;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (!Name(requests[i].role)) return {Error::InvalidRequest};
            for (std::size_t j = 0; j < i; ++j)
                if (requests[i].role == requests[j].role) return {Error::InvalidRequest};
            const auto size = requests[i].bytes.size();
            if (size > limits.max_stream_bytes || size > limits.max_total_bytes - total)
                return {Error::LimitExceeded};
            total += size;
        }
        std::uint32_t registry[3]{};
        auto* registry_live = Map(backend_, hd_base_ + kRegistryRva, sizeof registry);
        if (!registry_live) return {Error::InvalidRegistry};
        std::memcpy(registry, registry_live, sizeof registry);
        if (registry[1] > limits.max_registry_entries || registry[2] > 4096u * 4u || registry[2] % 32u ||
            registry[1] * 4u > registry[2] || ((registry[2] == 0) != (registry[0] == 0)))
            return {Error::InvalidRegistry};
        if (Overlaps(registry[0], registry[2], hd_base_ + kRegistryRva, sizeof registry))
            return {Error::InvalidRegistry};
        std::vector<std::uint32_t> addresses(registry[1]);
        auto* registry_array = registry[2] ? Map(backend_, registry[0], registry[2]) : nullptr;
        if (registry[2] && !registry_array) return {Error::InvalidRegistry};
        if (!addresses.empty()) std::memcpy(addresses.data(), registry_array, addresses.size() * 4);
        std::vector<Entry> entries;
        entries.reserve(addresses.size());
        for (auto address : addresses) {
            auto* bytes = Map(backend_, address, sizeof(Object));
            if (!bytes) return {Error::InvalidRegistry};
            Entry e{address, {}, nullptr};
            std::memcpy(&e.before, bytes, sizeof(Object));
            if (!ValidObject(backend_, e.before, hd_base_)) return {Error::InvalidObject};
            if (e.before.open) return {Error::Busy};
            const auto payload_start = e.before.payload ? e.before.payload - 4 : 0;
            const std::size_t payload_size = e.before.payload ? static_cast<std::size_t>(e.before.capacity) + 4 : 0;
            if (Overlaps(address, sizeof(Object), payload_start, payload_size) ||
                Overlaps(address, sizeof(Object), registry[0], registry[2]) ||
                Overlaps(address, sizeof(Object), hd_base_ + kRegistryRva, sizeof registry) ||
                Overlaps(payload_start, payload_size, registry[0], registry[2]) ||
                Overlaps(payload_start, payload_size, hd_base_ + kRegistryRva, sizeof registry))
                return {Error::InvalidRegistry};
            for (const auto& other : entries)
                if (SameName(e.before.name, other.before.name) ||
                    Overlaps(address, sizeof(Object), other.address, sizeof(Object)) ||
                    Overlaps(payload_start, payload_size, other.address, sizeof(Object)) ||
                    Overlaps(address, sizeof(Object), other.before.payload ? other.before.payload - 4 : 0,
                        other.before.payload ? static_cast<std::size_t>(other.before.capacity) + 4 : 0) ||
                    Overlaps(payload_start, payload_size, other.before.payload ? other.before.payload - 4 : 0,
                        other.before.payload ? static_cast<std::size_t>(other.before.capacity) + 4 : 0))
                    return {Error::InvalidRegistry};
            entries.push_back(e);
        }
        std::array<Entry*, 3> targets{};
        for (std::size_t i = 0; i < requests.size(); ++i) {
            for (auto& entry : entries)
                if (SameName(entry.before.name, Name(requests[i].role))) targets[i] = &entry;
            if (!targets[i]) return {Error::MissingStream};
            if (targets[i]->before.open) return {Error::Busy};
            targets[i]->live = reinterpret_cast<Object*>(Map(backend_, targets[i]->address, sizeof(Object), true));
            if (!targets[i]->live) return {Error::InvalidObject};
        }
        std::array<Stage, 3> staged{};
        for (std::size_t i = 0; i < requests.size(); ++i) {
            auto& s = staged[i]; s.backend = backend_; s.base = hd_base_;
            s.address = backend_.create(backend_.context, hd_base_, targets[i]->before.name);
            if (!s.address) return {Error::StagingFailed};
            s.live = reinterpret_cast<Object*>(Map(backend_, s.address, sizeof(Object), true));
            if (!s.live || !ValidObject(backend_, *s.live, hd_base_) || s.live->payload || s.live->open ||
                !SameName(s.live->name, targets[i]->before.name)) return {Error::StagingFailed};
            const auto& bytes = requests[i].bytes;
            if (!bytes.empty() && backend_.write(backend_.context, hd_base_, s.address,
                    bytes.data(), static_cast<int>(bytes.size())) != static_cast<int>(bytes.size()))
                return {Error::StagingFailed};
            if (!ValidObject(backend_, *s.live, hd_base_) || s.live->length != bytes.size() ||
                s.live->cursor != bytes.size() || s.live->open) return {Error::StagingFailed};
            if (!bytes.empty()) {
                auto* payload = Map(backend_, s.live->payload, bytes.size());
                if (!payload || std::memcmp(payload, bytes.data(), bytes.size())) return {Error::StagingFailed};
            }
            s.live->cursor = 0;
        }
        // Revalidate everything before commit. No allocation, callback, fallible
        // operation or engine re-entry occurs within the following commit loop.
        if (!admission_(admission_context_)) return {Error::NotQuiescent};
        if (backend_.thread_id(backend_.context) != game_thread_) return {Error::WrongThread};
        if (std::memcmp(registry_live, registry, sizeof registry) ||
            (!addresses.empty() && std::memcmp(registry_array, addresses.data(), addresses.size() * 4)))
            return {Error::StateChanged};
        for (const auto& entry : entries) {
            auto* now = Map(backend_, entry.address, sizeof(Object));
            if (!now || std::memcmp(now, &entry.before, sizeof(Object))) return {Error::StateChanged};
        }
        for (std::size_t i = 0; i < requests.size(); ++i) {
            // Copies only ownership/scalar tail; preserves destination name/vtable.
            // Both open fields were validated zero. Retired bytes live in staging.
            std::uint8_t old[sizeof(Object) - offsetof(Object, cursor)];
            std::memcpy(old, &targets[i]->live->cursor, sizeof old);
            std::memcpy(&targets[i]->live->cursor, &staged[i].live->cursor, sizeof old);
            std::memcpy(&staged[i].live->cursor, old, sizeof old);
        }
        return {}; // Staging destructors release all retired payloads after commit.
    } catch (...) { return {Error::StagingFailed}; }
}
const char* ErrorName(Error error) noexcept {
    switch (error) {
#define CASE(x) case Error::x: return #x
    CASE(None); CASE(NotInitialized); CASE(WrongThread); CASE(NotQuiescent); CASE(Reentrant);
    CASE(UnsupportedBuild); CASE(InvalidRequest); CASE(LimitExceeded); CASE(InvalidRegistry);
    CASE(MissingStream); CASE(Busy); CASE(InvalidObject); CASE(StagingFailed); CASE(StateChanged);
#undef CASE
    default: return "Unknown";
    }
}
} // namespace hota_memfile
