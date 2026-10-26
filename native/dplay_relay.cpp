#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include "dplay_relay.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

static_assert(sizeof(void*) == 4, "The DirectPlay replacement must match the x86 game ABI");

namespace hp_relay {
namespace {
typedef DWORD DPID;
constexpr DPID DPID_SYSMSG = 0, DPID_ALLPLAYERS = 0, DPID_SERVERPLAYER = 1;
constexpr HRESULT DP_OK = 0;
constexpr HRESULT DPERR(unsigned code) { return static_cast<HRESULT>(0x88770000u | code); }
const HRESULT DPERR_ALREADYINITIALIZED = DPERR(5), DPERR_BUFFERTOOSMALL = DPERR(30),
    DPERR_CANTCREATEPLAYER = DPERR(70), DPERR_INVALIDPLAYER = DPERR(150), DPERR_INVALIDGROUP = DPERR(155),
    DPERR_NOCONNECTION = DPERR(160), DPERR_NOMESSAGES = DPERR(190), DPERR_NOSESSIONS = DPERR(200),
    DPERR_INVALIDPASSWORD = DPERR(330), DPERR_CONNECTIONLOST = DPERR(500), DPERR_NOTLOBBIED = DPERR(1070);
const HRESULT DPERR_INVALIDPARAMS = E_INVALIDARG, DPERR_UNSUPPORTED = E_NOTIMPL, DPERR_GENERIC = E_FAIL;

constexpr DWORD DPPLAYERTYPE_GROUP = 0, DPPLAYERTYPE_PLAYER = 1;
constexpr DWORD DPSYS_CREATEPLAYERORGROUP = 3, DPSYS_DESTROYPLAYERORGROUP = 5, DPSYS_ADDPLAYERTOGROUP = 7,
    DPSYS_DELETEPLAYERFROMGROUP = 0x21, DPSYS_SESSIONLOST = 0x31, DPSYS_HOST = 0x101,
    DPSYS_SETPLAYERORGROUPDATA = 0x102, DPSYS_SETPLAYERORGROUPNAME = 0x103, DPSYS_SETSESSIONDESC = 0x104,
    DPSYS_ADDGROUPTOGROUP = 0x105, DPSYS_DELETEGROUPFROMGROUP = 0x106, DPSYS_CHAT = 0x109;
constexpr DWORD DPOPEN_JOIN = 1, DPOPEN_CREATE = 2;
constexpr DWORD DPRECEIVE_TOPLAYER = 2, DPRECEIVE_FROMPLAYER = 4, DPRECEIVE_PEEK = 8;
constexpr DWORD DPENUMPLAYERS_LOCAL = 8, DPENUMPLAYERS_REMOTE = 0x10, DPENUMPLAYERS_GROUP = 0x20,
    DPENUMPLAYERS_SESSION = 0x80, DPENUMPLAYERS_SERVERPLAYER = 0x100, DPENUMPLAYERS_SPECTATOR = 0x200;
constexpr DWORD DPPLAYER_SERVERPLAYER = 0x100, DPPLAYER_SPECTATOR = 0x200, DPPLAYER_LOCAL = 0x1000;
constexpr DWORD DPGET_LOCAL = 1, DPSET_LOCAL = 1;
constexpr DWORD DPESC_TIMEDOUT = 1;
constexpr DWORD DPCAPS_ISHOST = 2, DPCAPS_GUARANTEEDOPTIMIZED = 0x20, DPCAPS_GUARANTEEDSUPPORTED = 0x40;
constexpr DWORD DPMESSAGEQUEUE_SEND = 1, DPMESSAGEQUEUE_RECEIVE = 2;

const GUID CLSID_DirectPlay{0xd1eb6d20,0x8923,0x11d0,{0x9d,0x97,0x00,0xa0,0xc9,0x0a,0x43,0xcb}};
const GUID CLSID_DirectPlayLobby{0x2fe8f810,0xb2a5,0x11d0,{0xa7,0x87,0x00,0x00,0xf8,0x03,0xab,0xfc}};
const GUID IID_IDirectPlay2A{0x9d460b00,0x3d36,0x11d0,{0x9c,0x4f,0x00,0xa0,0xc9,0x05,0x42,0x5e}};
const GUID IID_IDirectPlay3A{0x2b74f7c0,0x9154,0x11cf,{0xa9,0xcd,0x00,0xaa,0x00,0x68,0x86,0xe3}};
const GUID IID_IDirectPlay4A{0x0ab1c531,0x4745,0x11d1,{0xa7,0xa1,0x00,0x00,0xf8,0x03,0xab,0xfc}};
const GUID IID_IDirectPlayLobby2A{0x1bb4af80,0xa303,0x11d0,{0x9c,0x4f,0x00,0xa0,0xc9,0x05,0x42,0x5e}};
const GUID IID_IDirectPlayLobby3A{0x2db72491,0x652c,0x11d1,{0xa7,0xa8,0x00,0x00,0xf8,0x03,0xab,0xfc}};
const GUID DPSPGUID_TCPIP{0x36e95ee0,0x8577,0x11cf,{0x96,0x0c,0x00,0x80,0xc7,0x53,0x4e,0x82}};
const GUID DPAID_TotalSize{0x1318f560,0x912c,0x11d0,{0x9d,0xaa,0x00,0xa0,0xc9,0x0a,0x43,0xcb}};
const GUID DPAID_ServiceProvider{0x07d916c0,0xe0af,0x11cf,{0x9c,0x4e,0x00,0xa0,0xc9,0x05,0x42,0x5e}};
const GUID DPAID_INet{0xc4a54da0,0xe0af,0x11cf,{0x9c,0x4e,0x00,0xa0,0xc9,0x05,0x42,0x5e}};

#pragma pack(push, 4)
struct DPNAME { DWORD dwSize, dwFlags; char* lpszShortNameA; char* lpszLongNameA; };
struct DPSESSIONDESC2 {
    DWORD dwSize, dwFlags; GUID guidInstance, guidApplication; DWORD dwMaxPlayers, dwCurrentPlayers;
    char* lpszSessionNameA; char* lpszPasswordA; DWORD dwReserved1, dwReserved2, dwUser1, dwUser2, dwUser3, dwUser4;
};
struct DPCAPS { DWORD dwSize, dwFlags, dwMaxBufferSize, dwMaxQueueSize, dwMaxPlayers, dwHundredBaud, dwLatency, dwMaxLocalPlayers, dwHeaderLength, dwTimeout; };
struct DPCHAT { DWORD dwSize, dwFlags; char* lpszMessageA; };
struct DPCOMPOUNDADDRESSELEMENT { GUID guidDataType; DWORD dwDataSize; void* lpData; };
#pragma pack(pop)
static_assert(sizeof(DPSESSIONDESC2) == 0x50, "DPSESSIONDESC2 layout");
static_assert(sizeof(DPCAPS) == 0x28, "DPCAPS layout");

typedef BOOL(WINAPI* EnumSessionsCallback)(const DPSESSIONDESC2*, DWORD*, DWORD, void*);
typedef BOOL(WINAPI* EnumPlayersCallback)(DPID, DWORD, const DPNAME*, DWORD, void*);
typedef BOOL(WINAPI* EnumConnectionsCallback)(const GUID*, void*, DWORD, const DPNAME*, DWORD, void*);
typedef BOOL(WINAPI* EnumAddressCallback)(REFGUID, DWORD, const void*, void*);

// ---------------------------------------------------------------- wire format
// Every frame: u32 length of the remainder, u16 type, then fields. Integers are
// little-endian; strings and blobs are u32 length + bytes. See
// src/HeroesLobby.Server/Relay/RelayProtocol.cs for the matching server side.
enum : std::uint16_t {
    C_HELLO = 1, C_ENUM = 2, C_OPEN = 3, C_CREATE_PLAYER = 4, C_DESTROY_PLAYER = 5, C_SEND = 6,
    C_SET_NAME = 7, C_SET_DATA = 8, C_SET_SESSION = 9, C_CREATE_GROUP = 10, C_DESTROY_GROUP = 11,
    C_GROUP_MEMBER = 12, C_CHAT = 13, C_CLOSE = 14, C_GROUP_IN_GROUP = 15, C_REJOIN = 16, C_RESUME = 20, C_CAUGHT_UP = 21,
    S_REPLY = 100, S_DELIVER = 101, S_CREATED = 102, S_DESTROYED = 103, S_NAME = 104, S_DATA = 105,
    S_SESSION = 106, S_MEMBER = 107, S_HOST = 108, S_CHAT = 109, S_SESSION_LOST = 110, S_GROUP_IN_GROUP = 111
};
constexpr std::uint32_t ProtocolVersion = 1;
constexpr std::uint32_t MaxFrame = 4u * 1024 * 1024;

struct Writer {
    std::vector<std::uint8_t> b;
    explicit Writer(std::uint16_t type) { u32(0); b.push_back(static_cast<std::uint8_t>(type)); b.push_back(static_cast<std::uint8_t>(type >> 8)); }
    Writer& u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); return *this; }
    Writer& raw(const void* p, size_t n) { auto c = static_cast<const std::uint8_t*>(p); b.insert(b.end(), c, c + n); return *this; }
    Writer& guid(const GUID& g) { return raw(&g, sizeof g); }
    Writer& blob(const void* p, size_t n) { u32(static_cast<std::uint32_t>(n)); return n ? raw(p, n) : *this; }
    Writer& str(const std::string& s) { return blob(s.data(), s.size()); }
    std::vector<std::uint8_t> Finish() {
        const auto n = static_cast<std::uint32_t>(b.size() - 4);
        for (int i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(n >> (8 * i));
        return std::move(b);
    }
};
struct Reader {
    const std::uint8_t* p; size_t n, i = 0; bool ok = true;
    Reader(const std::vector<std::uint8_t>& v, size_t start) : p(v.data()), n(v.size()), i(start) {}
    bool Need(size_t k) { if (!ok || n - i < k || i > n) ok = false; return ok; }
    std::uint32_t u32() { if (!Need(4)) return 0; std::uint32_t v = 0; for (int k = 0; k < 4; ++k) v |= std::uint32_t(p[i + k]) << (8 * k); i += 4; return v; }
    GUID guid() { GUID g{}; if (Need(sizeof g)) { std::memcpy(&g, p + i, sizeof g); i += sizeof g; } return g; }
    std::vector<std::uint8_t> blob() { auto k = u32(); if (!Need(k)) return {}; std::vector<std::uint8_t> v(p + i, p + i + k); i += k; return v; }
    std::string str() { auto v = blob(); return std::string(v.begin(), v.end()); }
};

// ------------------------------------------------------------- local mirror
struct SessionInfo {
    GUID instance{}, application{}; DWORD flags = 0, max_players = 0, current_players = 0;
    std::string name, password; DWORD user[4]{};
};
struct Entity {
    bool group = false; DWORD flags = 0; DPID parent = 0; std::string short_name, long_name;
    std::vector<std::uint8_t> remote_data, local_data; bool local = false; HANDLE event = nullptr;
    std::vector<DPID> members, subgroups;
};
void WriteSession(Writer& w, const SessionInfo& s) {
    w.guid(s.instance).guid(s.application).u32(s.flags).u32(s.max_players).u32(s.current_players).str(s.name).str(s.password);
    for (auto u : s.user) w.u32(u);
}
SessionInfo ReadSession(Reader& r) {
    SessionInfo s; s.instance = r.guid(); s.application = r.guid(); s.flags = r.u32(); s.max_players = r.u32();
    s.current_players = r.u32(); s.name = r.str(); s.password = r.str(); for (auto& u : s.user) u = r.u32(); return s;
}
SessionInfo FromDesc(const DPSESSIONDESC2& d) {
    SessionInfo s; s.instance = d.guidInstance; s.application = d.guidApplication; s.flags = d.dwFlags;
    s.max_players = d.dwMaxPlayers; s.current_players = d.dwCurrentPlayers;
    if (d.lpszSessionNameA) s.name = d.lpszSessionNameA;
    if (d.lpszPasswordA) s.password = d.lpszPasswordA;
    s.user[0] = d.dwUser1; s.user[1] = d.dwUser2; s.user[2] = d.dwUser3; s.user[3] = d.dwUser4; return s;
}

// A byte image whose pointer fields hold offsets until copied into a caller buffer.
struct Image {
    std::vector<std::uint8_t> bytes; std::vector<size_t> pointers;
    size_t Reserve(size_t n) { const auto at = bytes.size(); bytes.resize(at + n); return at; }
    void Put32(size_t at, DWORD v) { std::memcpy(bytes.data() + at, &v, 4); }
    void Pointer(size_t field, const void* data, size_t n, bool terminate) {
        if (!data && !terminate) { Put32(field, 0); return; }
        const auto at = bytes.size();
        if (n) bytes.insert(bytes.end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + n);
        if (terminate) bytes.push_back(0);
        while (bytes.size() % 4) bytes.push_back(0);
        Put32(field, static_cast<DWORD>(at)); pointers.push_back(field);
    }
    void String(size_t field, const std::string& s) { Pointer(field, s.data(), s.size(), true); }
    void Name(size_t at, const std::string& shortName, const std::string& longName) {
        Put32(at, sizeof(DPNAME)); Put32(at + 4, 0); String(at + 8, shortName); String(at + 12, longName);
    }
    void Session(size_t at, const SessionInfo& s) {
        DPSESSIONDESC2 d{}; d.dwSize = sizeof d; d.dwFlags = s.flags; d.guidInstance = s.instance; d.guidApplication = s.application;
        d.dwMaxPlayers = s.max_players; d.dwCurrentPlayers = s.current_players;
        d.dwUser1 = s.user[0]; d.dwUser2 = s.user[1]; d.dwUser3 = s.user[2]; d.dwUser4 = s.user[3];
        std::memcpy(bytes.data() + at, &d, sizeof d);
        String(at + offsetof(DPSESSIONDESC2, lpszSessionNameA), s.name);
        // Passwords never leave the relay; sessions report only whether one is required.
        Put32(at + offsetof(DPSESSIONDESC2, lpszPasswordA), 0);
    }
    HRESULT CopyTo(void* out, DWORD* size) const {
        if (!size) return DPERR_INVALIDPARAMS;
        const auto needed = static_cast<DWORD>(bytes.size());
        if (!out || *size < needed) { *size = needed; return DPERR_BUFFERTOOSMALL; }
        auto* dst = static_cast<std::uint8_t*>(out);
        std::memcpy(dst, bytes.data(), needed);
        for (auto field : pointers) {
            DWORD offset = 0; std::memcpy(&offset, dst + field, 4);
            const auto absolute = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(dst) + offset);
            std::memcpy(dst + field, &absolute, 4);
        }
        *size = needed; return DP_OK;
    }
};
struct Pending { DPID from = 0, to = 0; Image payload; bool injected = false; };

// ------------------------------------------------------------------ logging
std::mutex logMutex;
std::wstring logPath;
size_t logBytes = 0;
void Log(const std::string& line) {
    if (logPath.empty()) return;
    try {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logBytes > 16u * 1024 * 1024) return;
        SYSTEMTIME t; GetLocalTime(&t);
        char stamp[32]; std::snprintf(stamp, sizeof stamp, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        const std::string text = stamp + line + "\r\n";
        hp::Handle file(CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr));
        DWORD wrote = 0; if (file) WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
        logBytes += text.size();
    } catch (...) {}
}
std::string GuidText(const GUID& g) {
    char b[40]; std::snprintf(b, sizeof b, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]); return b;
}
std::string Hex(DWORD v) { char b[16]; std::snprintf(b, sizeof b, "%08lx", static_cast<unsigned long>(v)); return b; }

// -------------------------------------------------------------- statistics
std::atomic<std::uint32_t> statSent{0};
std::atomic<unsigned> statDelivered{0}, statSystem{0}, statObjects{0};
std::atomic<bool> statConnected{false}, statOpen{false};
std::atomic<std::uint32_t> statConsumed{0};
std::atomic<bool> statReconnecting{false};
std::atomic<unsigned> statReconnects{0};
std::atomic<std::uint32_t> statGeneration{0};
std::atomic<bool> holdGameMessages{false};
std::atomic<bool> statCatchingUp{false};
std::map<std::uint32_t, std::string> rejoinNames; // guarded by statusMutex
std::mutex statusMutex;
std::string statusSession, statusError, statusInstance;
std::uint32_t statusSeat = 0; // local player id in the open session
Config installedConfig; // guarded by configMutex once installed
std::mutex configMutex;
std::atomic<bool> installed{false};
// The game patch installs the hook at startup but leaves DirectPlay alone until a Heroes Post game is entered.
std::atomic<bool> active{false};
Config CurrentConfig() { std::lock_guard<std::mutex> lock(configMutex); return installedConfig; }
bool SameInstance(const GUID& g, const std::string& text) { return _stricmp(GuidText(g).c_str(), text.c_str()) == 0; }

// ------------------------------------------------------------- DirectPlay
class DirectPlay {
public:
    explicit DirectPlay(Config c) : config(std::move(c)) { ++statObjects; Log("DirectPlay object created"); }
    virtual HRESULT WINAPI QueryInterface(REFIID iid, void** out) {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IDirectPlay4A) || IsEqualGUID(iid, IID_IDirectPlay3A) || IsEqualGUID(iid, IID_IDirectPlay2A)) {
            *out = this; AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }
    virtual ULONG WINAPI AddRef() { return ++refs; }
    virtual ULONG WINAPI Release() { const ULONG left = --refs; if (!left) { Shutdown(); delete this; } return left; }
    virtual HRESULT WINAPI AddPlayerToGroup(DPID group, DPID player) { return Member(group, player, true); }
    virtual HRESULT WINAPI Close() {
        Log("Close");
        if (open) { Writer w(C_CLOSE); w.u32(0); Request(std::move(w)); }
        { std::lock_guard<std::mutex> lock(mutex); open = false; queue.clear(); early.clear(); }
        if (wake) SetEvent(wake);
        statOpen = false; return DP_OK;
    }
    virtual HRESULT WINAPI CreateGroup(DPID* id, DPNAME* name, void* data, DWORD size, DWORD flags) { return MakeGroup(0, id, name, data, size, flags); }
    virtual HRESULT WINAPI CreatePlayer(DPID* id, DPNAME* name, HANDLE event, void* data, DWORD size, DWORD flags) {
        if (!id) return DPERR_INVALIDPARAMS;
        if (!open) return DPERR_NOCONNECTION;
        Writer w(C_CREATE_PLAYER); w.u32(0).u32(flags).str(Short(name)).str(Long(name)).blob(data, data ? size : 0);
        std::vector<std::uint8_t> reply; const HRESULT hr = Request(std::move(w), &reply);
        if (FAILED(hr)) { Log("CreatePlayer failed " + Hex(hr)); return hr; }
        Reader r(reply, 0); const DPID created = r.u32();
        if (!r.ok || !created) return DPERR_CANTCREATEPLAYER;
        // The seat's generation rises each time a new process claims it; older servers send none.
        // A claimed seat also learns how many frames the relay replays to it, and how many of those
        // the other players heard the seat answer (older servers: all of them).
        const std::uint32_t claimedGeneration = reply.size() >= 8 ? r.u32() : 0;
        const std::uint32_t replayed = reply.size() >= 12 ? r.u32() : 0;
        const std::uint32_t heard = reply.size() >= 16 ? (std::min)(r.u32(), replayed) : replayed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!localSeat) { localSeat = created; generation = claimedGeneration; statGeneration = claimedGeneration; }
            if (!config.rejoin_seat.empty() && reply.size() >= 12) {
                heardEnd = config.rejoin_from + heard; caughtUp = false; replayDoneAt = 0; statCatchingUp = true;
                Log("The relay replays " + std::to_string(replayed) + " messages to the claimed seat; " + std::to_string(replayed - heard) + " are new to it");
            }
            const bool claimed = entities.count(created) != 0;
            auto& e = entities[created]; e.group = false; e.flags = flags | DPPLAYER_LOCAL; e.short_name = Short(name); e.long_name = Long(name);
            e.remote_data.assign(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + (data ? size : 0));
            e.local = true; e.event = event;
            if (!claimed) ++session.current_players;
            if (!config.rejoin_seat.empty()) seatId = created;
        }
        { std::lock_guard<std::mutex> lock(statusMutex); statusSeat = created; }
        *id = created; Log("CreatePlayer " + Short(name) + " -> " + Hex(created) + " flags " + Hex(flags)); return DP_OK;
    }
    virtual HRESULT WINAPI DeletePlayerFromGroup(DPID group, DPID player) { return Member(group, player, false); }
    virtual HRESULT WINAPI DestroyGroup(DPID id) {
        Writer w(C_DESTROY_GROUP); w.u32(0).u32(id); const HRESULT hr = Request(std::move(w));
        if (SUCCEEDED(hr)) { std::lock_guard<std::mutex> lock(mutex); RemoveEntity(id); }
        return hr;
    }
    virtual HRESULT WINAPI DestroyPlayer(DPID id) {
        { std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id); if (it == entities.end() || it->second.group || !it->second.local) return DPERR_INVALIDPLAYER; }
        Writer w(C_DESTROY_PLAYER); w.u32(0).u32(id); const HRESULT hr = Request(std::move(w));
        if (SUCCEEDED(hr)) { std::lock_guard<std::mutex> lock(mutex); RemoveEntity(id); if (session.current_players) --session.current_players; }
        Log("DestroyPlayer " + Hex(id) + " " + Hex(hr)); return hr;
    }
    virtual HRESULT WINAPI EnumGroupPlayers(DPID group, GUID*, EnumPlayersCallback callback, void* context, DWORD flags) {
        if (!callback) return DPERR_INVALIDPARAMS;
        std::vector<DPID> ids;
        { std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(group); if (it == entities.end() || !it->second.group) return DPERR_INVALIDGROUP; ids = it->second.members; }
        return EnumIds(ids, callback, context, flags, false);
    }
    virtual HRESULT WINAPI EnumGroups(GUID*, EnumPlayersCallback callback, void* context, DWORD flags) { return EnumGroupChildren(0, callback, context, flags); }
    virtual HRESULT WINAPI EnumPlayers(GUID*, EnumPlayersCallback callback, void* context, DWORD flags) {
        if (!callback) return DPERR_INVALIDPARAMS;
        if (flags & DPENUMPLAYERS_SESSION) return DPERR_UNSUPPORTED;
        std::vector<DPID> ids;
        { std::lock_guard<std::mutex> lock(mutex); for (auto& [id, e] : entities) if (!e.group || ((flags & DPENUMPLAYERS_GROUP) && !e.parent)) ids.push_back(id); }
        return EnumIds(ids, callback, context, flags, true);
    }
    virtual HRESULT WINAPI EnumSessions(DPSESSIONDESC2* filter, DWORD, EnumSessionsCallback callback, void* context, DWORD flags) {
        if (!callback) return DPERR_INVALIDPARAMS;
        Writer w(C_ENUM); w.u32(0).guid(filter ? filter->guidApplication : GUID{});
        std::vector<std::uint8_t> reply; const HRESULT hr = Request(std::move(w), &reply);
        Log("EnumSessions flags " + Hex(flags) + " -> " + Hex(hr));
        if (FAILED(hr)) return hr;
        Reader r(reply, 0); const auto count = r.u32();
        std::vector<SessionInfo> found;
        for (std::uint32_t i = 0; i < count && r.ok && i < 1024; ++i) found.push_back(ReadSession(r));
        if (!r.ok) return DPERR_GENERIC;
        for (auto& s : found) {
            if (!config.only_instance.empty() && !SameInstance(s.instance, config.only_instance)) continue;
            Image image; image.Reserve(sizeof(DPSESSIONDESC2)); image.Session(0, s);
            // Report password protection without disclosing anything.
            std::vector<std::uint8_t> buffer(image.bytes.size()); DWORD size = static_cast<DWORD>(buffer.size());
            image.CopyTo(buffer.data(), &size);
            DWORD timeout = 0;
            if (!callback(reinterpret_cast<DPSESSIONDESC2*>(buffer.data()), &timeout, 0, context)) return DP_OK;
        }
        DWORD timeout = 0; callback(nullptr, &timeout, DPESC_TIMEDOUT, context);
        return DP_OK;
    }
    virtual HRESULT WINAPI GetCaps(DPCAPS* caps, DWORD) {
        if (!caps || caps->dwSize != sizeof(DPCAPS)) return DPERR_INVALIDPARAMS;
        caps->dwFlags = DPCAPS_GUARANTEEDOPTIMIZED | DPCAPS_GUARANTEEDSUPPORTED | (host ? DPCAPS_ISHOST : 0);
        caps->dwMaxBufferSize = 0xffff; caps->dwMaxQueueSize = 0; caps->dwMaxPlayers = 65536; caps->dwHundredBaud = 0;
        caps->dwLatency = 50; caps->dwMaxLocalPlayers = 65536; caps->dwHeaderLength = 0; caps->dwTimeout = 5000;
        return DP_OK;
    }
    virtual HRESULT WINAPI GetGroupData(DPID id, void* data, DWORD* size, DWORD flags) { return Data(id, true, data, size, flags); }
    virtual HRESULT WINAPI GetGroupName(DPID id, void* data, DWORD* size) { return NameOf(id, true, data, size); }
    virtual HRESULT WINAPI GetMessageCount(DPID id, DWORD* count) {
        if (!count) return DPERR_INVALIDPARAMS;
        std::lock_guard<std::mutex> lock(mutex); DWORD n = 0; for (auto& m : queue) if (!id || m.to == id) ++n; *count = n; return DP_OK;
    }
    virtual HRESULT WINAPI GetPlayerAddress(DPID, void* data, DWORD* size) {
        if (!size) return DPERR_INVALIDPARAMS;
        auto address = BuildAddress(config.host);
        if (!data || *size < address.size()) { *size = static_cast<DWORD>(address.size()); return DPERR_BUFFERTOOSMALL; }
        std::memcpy(data, address.data(), address.size()); *size = static_cast<DWORD>(address.size()); return DP_OK;
    }
    virtual HRESULT WINAPI GetPlayerCaps(DPID, DPCAPS* caps, DWORD flags) { return GetCaps(caps, flags); }
    virtual HRESULT WINAPI GetPlayerData(DPID id, void* data, DWORD* size, DWORD flags) { return Data(id, false, data, size, flags); }
    virtual HRESULT WINAPI GetPlayerName(DPID id, void* data, DWORD* size) { return NameOf(id, false, data, size); }
    virtual HRESULT WINAPI GetSessionDesc(void* data, DWORD* size) {
        if (!open) return DPERR_NOCONNECTION;
        Image image; image.Reserve(sizeof(DPSESSIONDESC2));
        { std::lock_guard<std::mutex> lock(mutex); image.Session(0, session); }
        return image.CopyTo(data, size);
    }
    virtual HRESULT WINAPI Initialize(GUID*) { return DPERR_ALREADYINITIALIZED; }
    virtual HRESULT WINAPI Open(DPSESSIONDESC2* desc, DWORD flags) {
        if (!desc || desc->dwSize != sizeof(DPSESSIONDESC2)) return DPERR_INVALIDPARAMS;
        if (open) return DPERR_ALREADYINITIALIZED;
        const DWORD mode = flags & (DPOPEN_JOIN | DPOPEN_CREATE);
        if (mode != DPOPEN_JOIN && mode != DPOPEN_CREATE) return DPERR_INVALIDPARAMS;
        // A rejoining client's own session setup claims its held seat in the
        // running game instead of creating a new session.
        const bool rejoin = mode == DPOPEN_CREATE && !config.rejoin_seat.empty();
        Writer w(rejoin ? C_REJOIN : C_OPEN); w.u32(0);
        if (rejoin) {
            w.str(config.rejoin_session).str(config.rejoin_seat).u32(config.rejoin_from).u32(config.rejoin_sent);
            if (config.rejoin_seat_id) w.u32(config.rejoin_seat_id);
        }
        else { w.u32(mode); WriteSession(w, FromDesc(*desc)); }
        std::vector<std::uint8_t> reply; const HRESULT hr = Request(std::move(w), &reply);
        Log(std::string("Open ") + (rejoin ? "rejoin " + config.rejoin_session + " as " + config.rejoin_seat : std::string(mode == DPOPEN_CREATE ? "create " : "join ") + (desc->lpszSessionNameA ? desc->lpszSessionNameA : "")) + " -> " + Hex(hr));
        if (FAILED(hr)) { std::lock_guard<std::mutex> lock(mutex); early.clear(); return hr; }
        Reader r(reply, 0); auto s = ReadSession(r); const auto count = r.u32();
        std::map<DPID, Entity> snapshot;
        for (std::uint32_t i = 0; i < count && r.ok && i < 4096; ++i) {
            const DPID id = r.u32(); Entity e; e.group = r.u32() != 0; e.flags = r.u32(); e.parent = r.u32();
            e.short_name = r.str(); e.long_name = r.str(); e.remote_data = r.blob();
            const auto members = r.u32(); for (std::uint32_t k = 0; k < members && r.ok && k < 4096; ++k) e.members.push_back(r.u32());
            snapshot[id] = std::move(e);
        }
        if (!r.ok) return DPERR_GENERIC;
        for (auto& [id, e] : snapshot) if (e.group && e.parent && snapshot.count(e.parent)) snapshot[e.parent].subgroups.push_back(id);
        if (rejoin) {
            std::lock_guard<std::mutex> lock(statusMutex);
            for (auto& [id, e] : snapshot) if (!e.group) rejoinNames[id] = e.short_name;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            session = std::move(s); entities = std::move(snapshot); queue.clear();
            host = mode == DPOPEN_CREATE && !rejoin; open = true;
            statConsumed = rejoin ? config.rejoin_from : 0;
            // The relay replays a rejoined seat's log from the captured count.
            received = rejoin ? config.rejoin_from : 0;
            localSeat = 0; generation = 0; abandoned = false;
            {
                std::lock_guard<std::mutex> send(sendMutex);
                statSent = rejoin ? config.rejoin_sent : 0;
                outbox.clear(); outboxBytes = 0; outboxBase = statSent; seatLive = true;
            }
            // Events the relay sent after this reply but before this thread woke.
            auto pending = std::move(early); early.clear();
            for (auto& frame : pending) HandleLocked(frame);
        }
        statOpen = true;
        { std::lock_guard<std::mutex> lock(statusMutex); statusSession = session.name; statusInstance = GuidText(session.instance); }
        return DP_OK;
    }
    virtual HRESULT WINAPI Receive(DPID* from, DPID* to, DWORD flags, void* data, DWORD* size) {
        if (!from || !to || !size) return DPERR_INVALIDPARAMS;
        std::lock_guard<std::mutex> lock(mutex);
        // The engine polls here all the time, so a rejoined seat says it has caught up even
        // while its player does nothing.
        if (CatchUpDueLocked()) { std::lock_guard<std::mutex> send(sendMutex); SayCaughtUpLocked(); }
        const bool hold = holdGameMessages.load();
        if (!hold && !deferredDelivered && seatId) {
            // Ahead of the relay replay, exactly as the captured engine had already received them.
            deferredDelivered = true;
            auto at = queue.begin();
            for (const auto& message : config.rejoin_deferred) {
                if (message.size() < 0x14) continue;
                DPID sender = 0; std::memcpy(&sender, message.data() + 4, 4);
                Pending p; p.from = sender; p.to = seatId; p.payload.bytes = message; p.injected = true;
                at = std::next(queue.insert(at, std::move(p)));
            }
            if (!config.rejoin_deferred.empty()) Log("Re-delivered " + std::to_string(config.rejoin_deferred.size()) + " deferred messages");
        }
        // Until it has caught up, a rejoined seat takes only the frames its answers were heard for.
        std::uint32_t index = statConsumed.load();
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            const bool counted = it->from != DPID_SYSMSG && !it->injected;
            const bool unanswered = counted && !caughtUp && index >= heardEnd;
            if (counted) ++index;
            if (hold && it->from != DPID_SYSMSG) continue;
            if (unanswered) continue;
            if ((flags & DPRECEIVE_TOPLAYER) && it->to != *to) continue;
            if ((flags & DPRECEIVE_FROMPLAYER) && it->from != *from) continue;
            const HRESULT hr = it->payload.CopyTo(data, size);
            if (hr == DPERR_BUFFERTOOSMALL) return hr;
            *from = it->from; *to = it->to;
            if (!(flags & DPRECEIVE_PEEK)) {
                if (it->from != DPID_SYSMSG && !it->injected) ++statConsumed;
                queue.erase(it);
            }
            return hr;
        }
        return DPERR_NOMESSAGES;
    }
    virtual HRESULT WINAPI Send(DPID from, DPID to, DWORD flags, void* data, DWORD size) {
        if (!data && size) return DPERR_INVALIDPARAMS;
        if (!open) return DPERR_NOCONNECTION;
        { std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(from); if (it == entities.end() || !it->second.local) return DPERR_INVALIDPLAYER; }
        Writer w(C_SEND); w.u32(from).u32(to).u32(flags).blob(data, size);
        auto frame = w.Finish();
        // Kept until the relay may have it: after a dropped connection the relay says how many of
        // this seat's messages it has, and the rest are sent again.
        bool announce = false;
        { std::lock_guard<std::mutex> state(mutex); announce = CatchUpDueLocked(); }
        std::lock_guard<std::mutex> lock(sendMutex);
        if (announce) SayCaughtUpLocked();
        outboxBytes += frame.size();
        outbox.push_back(frame);
        while (outbox.size() > 1 && (outbox.size() > 8192 || outboxBytes > 32u * 1024 * 1024)) {
            outboxBytes -= outbox.front().size(); outbox.pop_front(); ++outboxBase;
        }
        ++statSent;
        if (seatLive) SendLocked(frame);
        return DP_OK;
    }
    virtual HRESULT WINAPI SetGroupData(DPID id, void* data, DWORD size, DWORD flags) { return SetData(id, true, data, size, flags); }
    virtual HRESULT WINAPI SetGroupName(DPID id, DPNAME* name, DWORD) { return SetName(id, true, name); }
    virtual HRESULT WINAPI SetPlayerData(DPID id, void* data, DWORD size, DWORD flags) { return SetData(id, false, data, size, flags); }
    virtual HRESULT WINAPI SetPlayerName(DPID id, DPNAME* name, DWORD) { return SetName(id, false, name); }
    virtual HRESULT WINAPI SetSessionDesc(DPSESSIONDESC2* desc, DWORD) {
        if (!desc || desc->dwSize != sizeof(DPSESSIONDESC2)) return DPERR_INVALIDPARAMS;
        if (!open) return DPERR_NOCONNECTION;
        Writer w(C_SET_SESSION); w.u32(0); WriteSession(w, FromDesc(*desc));
        const HRESULT hr = Request(std::move(w));
        if (SUCCEEDED(hr)) { std::lock_guard<std::mutex> lock(mutex); auto s = FromDesc(*desc); s.instance = session.instance; s.current_players = session.current_players; session = s; }
        return hr;
    }
    virtual HRESULT WINAPI AddGroupToGroup(DPID parent, DPID group) { return GroupInGroup(parent, group, true); }
    virtual HRESULT WINAPI CreateGroupInGroup(DPID parent, DPID* id, DPNAME* name, void* data, DWORD size, DWORD flags) { return MakeGroup(parent, id, name, data, size, flags); }
    virtual HRESULT WINAPI DeleteGroupFromGroup(DPID parent, DPID group) { return GroupInGroup(parent, group, false); }
    virtual HRESULT WINAPI EnumConnections(const GUID*, EnumConnectionsCallback callback, void* context, DWORD) {
        if (!callback) return DPERR_INVALIDPARAMS;
        auto address = BuildAddress("");
        char shortName[] = "Heroes Post relay";
        DPNAME name{sizeof(DPNAME), 0, shortName, nullptr};
        callback(&DPSPGUID_TCPIP, address.data(), static_cast<DWORD>(address.size()), &name, 0, context);
        return DP_OK;
    }
    virtual HRESULT WINAPI EnumGroupsInGroup(DPID parent, GUID*, EnumPlayersCallback callback, void* context, DWORD flags) { return EnumGroupChildren(parent, callback, context, flags); }
    virtual HRESULT WINAPI GetGroupConnectionSettings(DWORD, DPID, void*, DWORD*) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI InitializeConnection(void*, DWORD) {
        if (initialized) return DPERR_ALREADYINITIALIZED;
        initialized = true; Log("InitializeConnection"); return DP_OK;
    }
    virtual HRESULT WINAPI SecureOpen(const DPSESSIONDESC2* desc, DWORD flags, const void*, const void*) { return Open(const_cast<DPSESSIONDESC2*>(desc), flags); }
    virtual HRESULT WINAPI SendChatMessage(DPID from, DPID to, DWORD flags, DPCHAT* chat) {
        if (!chat || !chat->lpszMessageA) return DPERR_INVALIDPARAMS;
        if (!open) return DPERR_NOCONNECTION;
        Writer w(C_CHAT); w.u32(from).u32(to).u32(flags).str(chat->lpszMessageA);
        return Post(std::move(w)) ? DP_OK : DPERR_CONNECTIONLOST;
    }
    virtual HRESULT WINAPI SetGroupConnectionSettings(DWORD, DPID, void*) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI StartSession(DWORD, DPID) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI GetGroupFlags(DPID id, DWORD* flags) {
        if (!flags) return DPERR_INVALIDPARAMS;
        std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id); if (it == entities.end() || !it->second.group) return DPERR_INVALIDGROUP;
        *flags = it->second.flags; return DP_OK;
    }
    virtual HRESULT WINAPI GetGroupParent(DPID id, DPID* parent) {
        if (!parent) return DPERR_INVALIDPARAMS;
        std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id); if (it == entities.end() || !it->second.group) return DPERR_INVALIDGROUP;
        *parent = it->second.parent; return DP_OK;
    }
    virtual HRESULT WINAPI GetPlayerAccount(DPID, DWORD, void*, DWORD*) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI GetPlayerFlags(DPID id, DWORD* flags) {
        if (!flags) return DPERR_INVALIDPARAMS;
        std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id); if (it == entities.end() || it->second.group) return DPERR_INVALIDPLAYER;
        *flags = it->second.flags & ~DPPLAYER_LOCAL; if (it->second.local) *flags |= DPPLAYER_LOCAL; return DP_OK;
    }
    virtual HRESULT WINAPI GetGroupOwner(DPID, DPID*) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI SetGroupOwner(DPID, DPID) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI SendEx(DPID from, DPID to, DWORD flags, void* data, DWORD size, DWORD, DWORD, void*, DWORD* id) {
        if (id) *id = 0; return Send(from, to, flags, data, size);
    }
    virtual HRESULT WINAPI GetMessageQueue(DPID, DPID to, DWORD flags, DWORD* count, DWORD* bytes) {
        DWORD n = 0, total = 0;
        if (flags & DPMESSAGEQUEUE_RECEIVE) {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& m : queue) if (!to || m.to == to) { ++n; total += static_cast<DWORD>(m.payload.bytes.size()); }
        } else if (!(flags & DPMESSAGEQUEUE_SEND)) return DPERR_INVALIDPARAMS;
        if (count) *count = n; if (bytes) *bytes = total; return DP_OK;
    }
    void TestDrop() { std::lock_guard<std::mutex> lock(sendMutex); if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH); }
    virtual HRESULT WINAPI CancelMessage(DWORD, DWORD) { return DP_OK; }
    virtual HRESULT WINAPI CancelPriority(DWORD, DWORD, DWORD) { return DP_OK; }

private:
    Config config;
    std::atomic<ULONG> refs{1};
    std::mutex mutex, sendMutex;
    std::condition_variable replies;
    std::map<std::uint32_t, std::pair<HRESULT, std::vector<std::uint8_t>>> answered;
    std::uint32_t nextRequest = 1;
    SOCKET socket_ = INVALID_SOCKET;
    std::thread reader;
    bool connected = false, lost = false, initialized = false, open = false, host = false;
    SessionInfo session;
    std::map<DPID, Entity> entities;
    std::deque<Pending> queue;
    DPID seatId = 0; bool deferredDelivered = false;
    std::vector<std::vector<std::uint8_t>> early;
    // Resuming the seat after the connection drops (guarded by mutex): the local player, its
    // generation, the frames received for it, the pending Resume request.
    DPID localSeat = 0; std::uint32_t generation = 0, received = 0, resumeRequest = 0;
    // A rejoined seat: the consumed count up to which the other players heard the seat answer
    // the relay's replay, and when the restored engine had those. The relay ignores the seat until
    // CaughtUp, sent before message caughtUpAt (both guarded by sendMutex).
    std::atomic<std::uint32_t> heardEnd{0}; ULONGLONG replayDoneAt = 0; bool caughtUp = true;
    bool caughtUpSaid = false; std::uint32_t caughtUpAt = 0;
    bool closing = false, abandoned = false;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool wsa = false;
    // Sent game messages not yet known to be on the relay (guarded by sendMutex): outbox[i] is
    // message outboxBase + i. While seatLive is false new messages only queue.
    std::deque<std::vector<std::uint8_t>> outbox; std::uint32_t outboxBase = 0; size_t outboxBytes = 0; bool seatLive = true;

    static std::string Short(const DPNAME* n) { return n && n->lpszShortNameA ? n->lpszShortNameA : ""; }
    static std::string Long(const DPNAME* n) { return n && n->lpszLongNameA ? n->lpszLongNameA : ""; }

    static std::vector<std::uint8_t> BuildAddress(const std::string& inet) {
        Writer w(0); w.b.clear();
        auto element = [&](const GUID& g, const void* p, DWORD n) { w.guid(g).u32(n).raw(p, n); };
        const DWORD total = 0; element(DPAID_TotalSize, &total, 4);
        element(DPAID_ServiceProvider, &DPSPGUID_TCPIP, sizeof(GUID));
        if (!inet.empty()) element(DPAID_INet, inet.c_str(), static_cast<DWORD>(inet.size() + 1));
        const DWORD size = static_cast<DWORD>(w.b.size()); std::memcpy(w.b.data() + 20, &size, 4);
        return std::move(w.b);
    }

    // A rejoined seat has caught up once its restored engine has been given the game's messages,
    // has consumed the replayed frames the seat had already answered, and has had five seconds to
    // send what they set off: the relay takes those for the repeats they are. The timer starts at
    // the release when there is no such frame. Frames the seat never answered (sent while it was
    // away, or arriving now) wait until after CaughtUp, so its answers to them reach the others:
    // the ST head starts the day change when the last player's End Turn arrives. Until then the
    // player's commands are held (CatchingUp), so none of theirs is mistaken for a repeat. Caller
    // holds mutex; true once, when CaughtUp is due.
    bool CatchUpDueLocked() {
        if (caughtUp) return false;
        const ULONGLONG now = GetTickCount64();
        if (!replayDoneAt) {
            if (holdGameMessages.load() || statConsumed.load() < heardEnd.load()) return false;
            replayDoneAt = now; Log("The restored seat has the frames it had answered; catching up for five seconds");
            return false;
        }
        if (now - replayDoneAt < 5000) return false;
        caughtUp = true; return true;
    }
    // Carries where the seat started holding frames back, which tells the relay it did.
    std::vector<std::uint8_t> CaughtUpFrame() { Writer w(C_CAUGHT_UP); w.u32(heardEnd.load()); return w.Finish(); }
    // Caller holds sendMutex.
    void SayCaughtUpLocked() {
        caughtUpSaid = true; caughtUpAt = statSent.load();
        if (seatLive) SendLocked(CaughtUpFrame());
        statCatchingUp = false;
        Log("Caught up with the relay's replay");
    }
    // ---- connection
    SOCKET Dial() {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        addrinfo* found = nullptr; const std::string port = std::to_string(config.port);
        if (getaddrinfo(config.host.c_str(), port.c_str(), &hints, &found) || !found) { SetError("relay_host_unresolved"); return INVALID_SOCKET; }
        SOCKET s = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        const bool ok = s != INVALID_SOCKET && ::connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen)) == 0;
        freeaddrinfo(found);
        if (!ok) { if (s != INVALID_SOCKET) closesocket(s); SetError("relay_unreachable"); return INVALID_SOCKET; }
        BOOL yes = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
        // Players wait at the day barrier for a long time without traffic: probe so a dead route is noticed.
        tcp_keepalive alive{1, 15000, 3000}; DWORD unused = 0;
        WSAIoctl(s, SIO_KEEPALIVE_VALS, &alive, sizeof alive, nullptr, 0, &unused, nullptr, nullptr);
        return s;
    }
    bool Connect() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (connected && !lost) return true;
            // While the reader redials for the seat, nothing else may open a connection.
            if (statReconnecting.load()) return false;
        }
        if (reader.joinable()) reader.join();
        if (!wsa) { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) return false; wsa = true; }
        const SOCKET s = Dial();
        if (s == INVALID_SOCKET) return false;
        { std::lock_guard<std::mutex> lock(sendMutex); socket_ = s; }
        { std::lock_guard<std::mutex> lock(mutex); connected = true; lost = false; }
        statConnected = true;
        reader = std::thread([this] { ReadLoop(); });
        Writer hello(C_HELLO); hello.u32(ProtocolVersion).str(config.ticket).str(config.name);
        Log("Connected to relay " + config.host + ":" + std::to_string(config.port));
        return Post(std::move(hello));
    }
    void Shutdown() {
        { std::lock_guard<std::mutex> lock(mutex); closing = true; }
        if (wake) SetEvent(wake);
        { std::lock_guard<std::mutex> lock(sendMutex); if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH); }
        if (reader.joinable()) reader.join();
        { std::lock_guard<std::mutex> lock(sendMutex); if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; } }
        if (wsa) { WSACleanup(); wsa = false; }
        if (wake) { CloseHandle(wake); wake = nullptr; }
        connected = false; statConnected = false; statOpen = false; statReconnecting = false; statCatchingUp = false; --statObjects;
    }
    static void SetError(const char* text) { std::lock_guard<std::mutex> lock(statusMutex); statusError = text; }
    // Caller holds sendMutex.
    bool SendLocked(const std::vector<std::uint8_t>& frame) {
        if (socket_ == INVALID_SOCKET) return false;
        size_t done = 0;
        while (done < frame.size()) {
            const int n = ::send(socket_, reinterpret_cast<const char*>(frame.data() + done), static_cast<int>(frame.size() - done), 0);
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
        }
        return true;
    }
    bool Post(Writer&& w) {
        auto frame = w.Finish();
        std::lock_guard<std::mutex> lock(sendMutex);
        return SendLocked(frame);
    }
    // Requests carry an id in the first u32 after the type.
    HRESULT Request(Writer&& w, std::vector<std::uint8_t>* reply = nullptr) {
        if (!Connect()) return DPERR_NOCONNECTION;
        std::uint32_t id;
        { std::lock_guard<std::mutex> lock(mutex); id = nextRequest++; }
        for (int i = 0; i < 4; ++i) w.b[6 + i] = static_cast<std::uint8_t>(id >> (8 * i));
        if (!Post(std::move(w))) return DPERR_CONNECTIONLOST;
        std::unique_lock<std::mutex> lock(mutex);
        if (!replies.wait_for(lock, std::chrono::seconds(15), [&] { return answered.count(id) || lost; })) return DPERR_GENERIC;
        auto it = answered.find(id); if (it == answered.end()) return DPERR_CONNECTIONLOST;
        const HRESULT hr = it->second.first; if (reply) *reply = std::move(it->second.second); answered.erase(it);
        return hr;
    }
    bool ReadExact(std::uint8_t* p, size_t n) {
        while (n) { const int got = ::recv(socket_, reinterpret_cast<char*>(p), static_cast<int>(n), 0); if (got <= 0) return false; p += got; n -= static_cast<size_t>(got); }
        return true;
    }
    void ReadFrames() {
        for (;;) {
            std::uint8_t head[4];
            if (!ReadExact(head, 4)) break;
            const std::uint32_t length = head[0] | head[1] << 8 | head[2] << 16 | std::uint32_t(head[3]) << 24;
            if (length < 2 || length > MaxFrame) break;
            std::vector<std::uint8_t> frame(length);
            if (!ReadExact(frame.data(), length)) break;
            try { Handle(frame); } catch (...) { Log("relay frame handling failed"); }
        }
    }
    // One thread per connection run: when the connection drops during a game the engine is not
    // told; this thread redials and asks the relay for the seat back (Resume).
    void ReadLoop() {
        for (;;) {
            ReadFrames();
            bool resume = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                lost = true; statConnected = false;
                resume = open && localSeat && !closing && !abandoned;
                statReconnecting = resume;
                if (open && !resume && !closing) Enqueue(System(DPSYS_SESSIONLOST, 4));
                replies.notify_all();
            }
            {
                std::lock_guard<std::mutex> lock(sendMutex);
                seatLive = false;
                if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
            }
            Log(resume ? "Relay connection lost; reconnecting to keep the seat" : "Relay connection closed");
            if (!resume || !Redial()) { statReconnecting = false; return; }
        }
    }
    bool Redial() {
        statReconnecting = true;
        DWORD delay = 1000;
        for (;;) {
            if (WaitForSingleObject(wake, delay) == WAIT_OBJECT_0) {
                std::lock_guard<std::mutex> lock(mutex);
                if (closing || !open) return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (closing || !open || abandoned) return false;
            }
            const SOCKET s = Dial();
            if (s == INVALID_SOCKET) { delay = delay * 2 > 10000 ? 10000 : delay * 2; continue; }
            Writer resume(C_RESUME);
            {
                std::lock_guard<std::mutex> lock(mutex);
                resumeRequest = nextRequest++;
                resume.u32(resumeRequest).guid(session.instance).u32(localSeat).u32(generation).u32(received);
            }
            {
                std::lock_guard<std::mutex> lock(sendMutex);
                socket_ = s;
                resume.u32(statSent.load());
                Writer hello(C_HELLO); hello.u32(ProtocolVersion).str(config.ticket).str(config.name);
                SendLocked(hello.Finish()); SendLocked(resume.Finish());
            }
            { std::lock_guard<std::mutex> lock(mutex); lost = false; }
            statConnected = true; ++statReconnects;
            Log("Reconnected to relay " + config.host + ":" + std::to_string(config.port) + "; asking for seat " + Hex(localSeat) + " back");
            return true;
        }
    }
    // The relay's answer to Resume (reader thread, under mutex).
    void Resumed(HRESULT hr, Reader& r) {
        resumeRequest = 0; statReconnecting = false;
        if (FAILED(hr)) {
            abandoned = true;
            Log("The relay refused to give the seat back: " + Hex(static_cast<DWORD>(hr)));
            Enqueue(System(DPSYS_SESSIONLOST, 4));
            std::lock_guard<std::mutex> lock(sendMutex);
            if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH);
            return;
        }
        const std::uint32_t held = r.u32();
        std::lock_guard<std::mutex> lock(sendMutex);
        const std::uint32_t end = outboxBase + static_cast<std::uint32_t>(outbox.size());
        if (held < outboxBase) Log("The relay is missing messages " + std::to_string(held) + ".." + std::to_string(outboxBase) + " of this seat; they are no longer kept");
        std::uint32_t resent = 0;
        // CaughtUp may have been lost with the connection: it is said again in its place among
        // the seat's messages. A relay that already had it has no seat catching up left.
        bool told = !caughtUpSaid;
        for (std::uint32_t i = held > outboxBase ? held : outboxBase; i < end; ++i, ++resent) {
            if (!told && i >= caughtUpAt) { SendLocked(CaughtUpFrame()); told = true; }
            SendLocked(outbox[i - outboxBase]);
        }
        if (!told) SendLocked(CaughtUpFrame());
        seatLive = true;
        Log("Seat " + Hex(localSeat) + " resumed; the relay had " + std::to_string(held) + " of its messages, resent " + std::to_string(resent));
    }
    // ---- inbound
    static Image System(DWORD type, size_t size) { Image i; i.Reserve(size); i.Put32(0, type); return i; }
    void Enqueue(Image&& image, DPID from = DPID_SYSMSG, DPID to = 0) {
        if (!to) for (auto& [id, e] : entities) if (e.local && !e.group) { to = id; break; }
        HANDLE event = nullptr;
        auto it = entities.find(to); if (it != entities.end()) event = it->second.event;
        Pending p; p.from = from; p.to = to; p.payload = std::move(image); queue.push_back(std::move(p));
        if (from == DPID_SYSMSG) ++statSystem; else ++statDelivered;
        if (event) SetEvent(event);
    }
    void RemoveEntity(DPID id) {
        for (auto& [other, e] : entities) {
            e.members.erase(std::remove(e.members.begin(), e.members.end(), id), e.members.end());
            e.subgroups.erase(std::remove(e.subgroups.begin(), e.subgroups.end(), id), e.subgroups.end());
        }
        entities.erase(id);
    }
    void Handle(const std::vector<std::uint8_t>& f) { std::lock_guard<std::mutex> lock(mutex); HandleLocked(f); }
    void HandleLocked(const std::vector<std::uint8_t>& f) {
        const std::uint16_t type = static_cast<std::uint16_t>(f[0] | f[1] << 8);
        Reader r(f, 2);
        if (type != S_REPLY && !open) { if (early.size() < 65536) early.push_back(f); return; }
        switch (type) {
        case S_REPLY: {
            const auto id = r.u32(); const auto hr = static_cast<HRESULT>(r.u32());
            if (id && id == resumeRequest) { Resumed(hr, r); return; }
            answered[id] = {hr, std::vector<std::uint8_t>(f.begin() + static_cast<std::ptrdiff_t>(r.i), f.end())};
            replies.notify_all(); return;
        }
        case S_DELIVER: {
            const DPID from = r.u32(), to = r.u32(); auto data = r.blob(); if (!r.ok) return;
            ++received;
            Image image; image.bytes = std::move(data); Enqueue(std::move(image), from, to); return;
        }
        case S_CREATED: {
            const DPID id = r.u32(); Entity e; e.group = r.u32() != 0; e.flags = r.u32(); e.parent = r.u32();
            e.short_name = r.str(); e.long_name = r.str(); e.remote_data = r.blob(); const DWORD current = r.u32();
            if (!r.ok || !open || entities.count(id)) return;
            if (e.group && e.parent) { auto p = entities.find(e.parent); if (p != entities.end()) p->second.subgroups.push_back(id); }
            if (!e.group) session.current_players = current;
            Image m = System(DPSYS_CREATEPLAYERORGROUP, 48);
            m.Put32(4, e.group ? DPPLAYERTYPE_GROUP : DPPLAYERTYPE_PLAYER); m.Put32(8, id); m.Put32(12, current);
            m.Pointer(16, e.remote_data.empty() ? nullptr : e.remote_data.data(), e.remote_data.size(), false); m.Put32(20, static_cast<DWORD>(e.remote_data.size()));
            m.Name(24, e.short_name, e.long_name); m.Put32(40, e.parent); m.Put32(44, e.flags);
            entities[id] = std::move(e); Enqueue(std::move(m)); return;
        }
        case S_DESTROYED: {
            const DPID id = r.u32(); const DWORD current = r.u32(); if (!r.ok) return;
            auto it = entities.find(id); if (it == entities.end()) return;
            const Entity e = it->second;
            Image m = System(DPSYS_DESTROYPLAYERORGROUP, 52);
            m.Put32(4, e.group ? DPPLAYERTYPE_GROUP : DPPLAYERTYPE_PLAYER); m.Put32(8, id);
            m.Pointer(12, e.local_data.empty() ? nullptr : e.local_data.data(), e.local_data.size(), false); m.Put32(16, static_cast<DWORD>(e.local_data.size()));
            m.Pointer(20, e.remote_data.empty() ? nullptr : e.remote_data.data(), e.remote_data.size(), false); m.Put32(24, static_cast<DWORD>(e.remote_data.size()));
            m.Name(28, e.short_name, e.long_name); m.Put32(44, e.parent); m.Put32(48, e.flags);
            RemoveEntity(id); if (!e.group) session.current_players = current;
            Enqueue(std::move(m)); return;
        }
        case S_NAME: {
            const DPID id = r.u32(); auto s = r.str(); auto l = r.str(); if (!r.ok) return;
            auto it = entities.find(id); if (it == entities.end()) return;
            it->second.short_name = s; it->second.long_name = l;
            Image m = System(DPSYS_SETPLAYERORGROUPNAME, 28); m.Put32(4, it->second.group ? DPPLAYERTYPE_GROUP : DPPLAYERTYPE_PLAYER); m.Put32(8, id); m.Name(12, s, l);
            Enqueue(std::move(m)); return;
        }
        case S_DATA: {
            const DPID id = r.u32(); auto data = r.blob(); if (!r.ok) return;
            auto it = entities.find(id); if (it == entities.end()) return;
            it->second.remote_data = data;
            Image m = System(DPSYS_SETPLAYERORGROUPDATA, 20); m.Put32(4, it->second.group ? DPPLAYERTYPE_GROUP : DPPLAYERTYPE_PLAYER); m.Put32(8, id);
            m.Pointer(12, data.empty() ? nullptr : data.data(), data.size(), false); m.Put32(16, static_cast<DWORD>(data.size()));
            Enqueue(std::move(m)); return;
        }
        case S_SESSION: {
            auto s = ReadSession(r); if (!r.ok) return;
            s.current_players = session.current_players; session = s;
            Image m = System(DPSYS_SETSESSIONDESC, 4 + sizeof(DPSESSIONDESC2)); m.Session(4, session); Enqueue(std::move(m)); return;
        }
        case S_MEMBER: {
            const DPID group = r.u32(), player = r.u32(); const bool add = r.u32() != 0; if (!r.ok) return;
            auto it = entities.find(group); if (it == entities.end()) return;
            auto& v = it->second.members;
            if (add) { if (std::find(v.begin(), v.end(), player) == v.end()) v.push_back(player); }
            else v.erase(std::remove(v.begin(), v.end(), player), v.end());
            Image m = System(add ? DPSYS_ADDPLAYERTOGROUP : DPSYS_DELETEPLAYERFROMGROUP, 12); m.Put32(4, group); m.Put32(8, player);
            Enqueue(std::move(m)); return;
        }
        case S_GROUP_IN_GROUP: {
            const DPID parent = r.u32(), group = r.u32(); const bool add = r.u32() != 0; if (!r.ok) return;
            auto it = entities.find(parent); if (it == entities.end()) return;
            auto& v = it->second.subgroups;
            if (add) { if (std::find(v.begin(), v.end(), group) == v.end()) v.push_back(group); }
            else v.erase(std::remove(v.begin(), v.end(), group), v.end());
            Image m = System(add ? DPSYS_ADDGROUPTOGROUP : DPSYS_DELETEGROUPFROMGROUP, 12); m.Put32(4, parent); m.Put32(8, group);
            Enqueue(std::move(m)); return;
        }
        case S_HOST: { host = true; Enqueue(System(DPSYS_HOST, 4)); Log("Became session host"); return; }
        case S_CHAT: {
            const DPID from = r.u32(), to = r.u32(), group = r.u32(); const DWORD flags = r.u32(); auto text = r.str(); if (!r.ok) return;
            // DPMSG_CHAT is followed by its DPCHAT; lpChat and the text point into the same buffer.
            Image m = System(DPSYS_CHAT, 24 + sizeof(DPCHAT)); m.Put32(4, flags); m.Put32(8, from); m.Put32(12, to); m.Put32(16, group);
            m.Put32(20, 24); m.pointers.push_back(20); m.Put32(24, sizeof(DPCHAT)); m.Put32(28, 0); m.String(32, text);
            Enqueue(std::move(m), DPID_SYSMSG, to); return;
        }
        case S_SESSION_LOST: { Enqueue(System(DPSYS_SESSIONLOST, 4)); return; }
        default: Log("Unknown relay frame " + std::to_string(type));
        }
    }
    // ---- helpers
    HRESULT EnumIds(const std::vector<DPID>& ids, EnumPlayersCallback callback, void* context, DWORD flags, bool players) {
        struct Item { DPID id; bool group; DWORD flags; std::string s, l; bool local; };
        std::vector<Item> items;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto id : ids) {
                auto it = entities.find(id); if (it == entities.end()) continue; const auto& e = it->second;
                if (!e.group && players) {
                    if ((flags & DPENUMPLAYERS_LOCAL) && !e.local) continue;
                    if ((flags & DPENUMPLAYERS_REMOTE) && e.local) continue;
                    if ((flags & DPENUMPLAYERS_SERVERPLAYER) && !(e.flags & DPPLAYER_SERVERPLAYER)) continue;
                    if ((flags & DPENUMPLAYERS_SPECTATOR) && !(e.flags & DPPLAYER_SPECTATOR)) continue;
                }
                items.push_back({id, e.group, e.flags, e.short_name, e.long_name, e.local});
            }
        }
        for (auto& i : items) {
            DPNAME name{sizeof(DPNAME), 0, i.s.data(), i.l.data()};
            DWORD reported = (i.local ? DPENUMPLAYERS_LOCAL : DPENUMPLAYERS_REMOTE) |
                (i.flags & DPPLAYER_SERVERPLAYER ? DPENUMPLAYERS_SERVERPLAYER : 0) | (i.flags & DPPLAYER_SPECTATOR ? DPENUMPLAYERS_SPECTATOR : 0);
            if (!callback(i.id, i.group ? DPPLAYERTYPE_GROUP : DPPLAYERTYPE_PLAYER, &name, reported, context)) break;
        }
        return DP_OK;
    }
    HRESULT EnumGroupChildren(DPID parent, EnumPlayersCallback callback, void* context, DWORD flags) {
        if (!callback) return DPERR_INVALIDPARAMS;
        std::vector<DPID> ids;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (parent) { auto it = entities.find(parent); if (it == entities.end() || !it->second.group) return DPERR_INVALIDGROUP; ids = it->second.subgroups; }
            else for (auto& [id, e] : entities) if (e.group && !e.parent) ids.push_back(id);
        }
        return EnumIds(ids, callback, context, flags, false);
    }
    HRESULT MakeGroup(DPID parent, DPID* id, DPNAME* name, void* data, DWORD size, DWORD flags) {
        if (!id) return DPERR_INVALIDPARAMS;
        if (!open) return DPERR_NOCONNECTION;
        Writer w(C_CREATE_GROUP); w.u32(0).u32(parent).u32(flags).str(Short(name)).str(Long(name)).blob(data, data ? size : 0);
        std::vector<std::uint8_t> reply; const HRESULT hr = Request(std::move(w), &reply);
        if (FAILED(hr)) return hr;
        Reader r(reply, 0); const DPID created = r.u32(); if (!r.ok || !created) return DPERR_GENERIC;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto& e = entities[created]; e.group = true; e.flags = flags; e.parent = parent; e.short_name = Short(name); e.long_name = Long(name);
            e.remote_data.assign(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + (data ? size : 0)); e.local = true;
            if (parent) { auto p = entities.find(parent); if (p != entities.end()) p->second.subgroups.push_back(created); }
        }
        *id = created; Log("CreateGroup " + Short(name) + " -> " + Hex(created)); return DP_OK;
    }
    HRESULT Member(DPID group, DPID player, bool add) {
        Writer w(C_GROUP_MEMBER); w.u32(0).u32(group).u32(player).u32(add ? 1 : 0);
        const HRESULT hr = Request(std::move(w));
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(group);
            if (it != entities.end()) { auto& v = it->second.members; if (add) { if (std::find(v.begin(), v.end(), player) == v.end()) v.push_back(player); } else v.erase(std::remove(v.begin(), v.end(), player), v.end()); }
        }
        return hr;
    }
    HRESULT GroupInGroup(DPID parent, DPID group, bool add) {
        Writer w(C_GROUP_IN_GROUP); w.u32(0).u32(parent).u32(group).u32(add ? 1 : 0);
        const HRESULT hr = Request(std::move(w));
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(parent);
            if (it != entities.end()) { auto& v = it->second.subgroups; if (add) v.push_back(group); else v.erase(std::remove(v.begin(), v.end(), group), v.end()); }
        }
        return hr;
    }
    HRESULT Data(DPID id, bool group, void* data, DWORD* size, DWORD flags) {
        if (!size) return DPERR_INVALIDPARAMS;
        std::vector<std::uint8_t> copy;
        {
            std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id);
            if (it == entities.end() || it->second.group != group) return group ? DPERR_INVALIDGROUP : DPERR_INVALIDPLAYER;
            copy = (flags & DPGET_LOCAL) ? it->second.local_data : it->second.remote_data;
        }
        if (!data || *size < copy.size()) { *size = static_cast<DWORD>(copy.size()); return copy.empty() && data ? DP_OK : DPERR_BUFFERTOOSMALL; }
        if (!copy.empty()) std::memcpy(data, copy.data(), copy.size()); *size = static_cast<DWORD>(copy.size()); return DP_OK;
    }
    HRESULT NameOf(DPID id, bool group, void* data, DWORD* size) {
        Image image; image.Reserve(sizeof(DPNAME));
        {
            std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id);
            if (it == entities.end() || it->second.group != group) return group ? DPERR_INVALIDGROUP : DPERR_INVALIDPLAYER;
            image.Name(0, it->second.short_name, it->second.long_name);
        }
        return image.CopyTo(data, size);
    }
    HRESULT SetData(DPID id, bool group, void* data, DWORD size, DWORD flags) {
        if (!data && size) return DPERR_INVALIDPARAMS;
        std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + (data ? size : 0));
        {
            std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id);
            if (it == entities.end() || it->second.group != group) return group ? DPERR_INVALIDGROUP : DPERR_INVALIDPLAYER;
            if (flags & DPSET_LOCAL) { it->second.local_data = bytes; return DP_OK; }
            it->second.remote_data = bytes;
        }
        Writer w(C_SET_DATA); w.u32(0).u32(id).u32(group ? 1 : 0).blob(bytes.data(), bytes.size());
        return Request(std::move(w));
    }
    HRESULT SetName(DPID id, bool group, DPNAME* name) {
        {
            std::lock_guard<std::mutex> lock(mutex); auto it = entities.find(id);
            if (it == entities.end() || it->second.group != group) return group ? DPERR_INVALIDGROUP : DPERR_INVALIDPLAYER;
            it->second.short_name = Short(name); it->second.long_name = Long(name);
        }
        Writer w(C_SET_NAME); w.u32(0).u32(id).u32(group ? 1 : 0).str(Short(name)).str(Long(name));
        return Request(std::move(w));
    }
};

// ------------------------------------------------------------ DirectPlayLobby
class Lobby {
public:
    Lobby() = default;
    virtual HRESULT WINAPI QueryInterface(REFIID iid, void** out) {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IDirectPlayLobby3A) || IsEqualGUID(iid, IID_IDirectPlayLobby2A)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    virtual ULONG WINAPI AddRef() { return ++refs; }
    virtual ULONG WINAPI Release() { const ULONG left = --refs; if (!left) delete this; return left; }
    virtual HRESULT WINAPI Connect(DWORD, void**, IUnknown*) { return DPERR_NOTLOBBIED; }
    virtual HRESULT WINAPI CreateAddress(REFGUID provider, REFGUID type, const void* data, DWORD size, void* address, DWORD* addressSize) {
        DPCOMPOUNDADDRESSELEMENT e[2]{{DPAID_ServiceProvider, sizeof(GUID), const_cast<GUID*>(&provider)}, {type, size, const_cast<void*>(data)}};
        return CreateCompoundAddress(e, 2, address, addressSize);
    }
    virtual HRESULT WINAPI EnumAddress(EnumAddressCallback callback, const void* address, DWORD size, void* context) {
        if (!callback || (!address && size)) return DPERR_INVALIDPARAMS;
        auto p = static_cast<const std::uint8_t*>(address); DWORD i = 0;
        while (i + sizeof(GUID) + 4 <= size) {
            GUID g; std::memcpy(&g, p + i, sizeof g); DWORD n; std::memcpy(&n, p + i + sizeof g, 4);
            i += sizeof(GUID) + 4; if (n > size - i) return DPERR_INVALIDPARAMS;
            if (!callback(g, n, p + i, context)) break;
            i += n;
        }
        return DP_OK;
    }
    virtual HRESULT WINAPI EnumAddressTypes(void*, REFGUID, void*, DWORD) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI EnumLocalApplications(void*, void*, DWORD) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI GetConnectionSettings(DWORD, void*, DWORD*) { return DPERR_NOTLOBBIED; }
    virtual HRESULT WINAPI ReceiveLobbyMessage(DWORD, DWORD, DWORD*, void*, DWORD*) { return DPERR_NOTLOBBIED; }
    virtual HRESULT WINAPI RunApplication(DWORD, DWORD*, void*, HANDLE) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI SendLobbyMessage(DWORD, DWORD, void*, DWORD) { return DPERR_NOTLOBBIED; }
    virtual HRESULT WINAPI SetConnectionSettings(DWORD, DWORD, void*) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI SetLobbyMessageEvent(DWORD, DWORD, HANDLE) { return DPERR_NOTLOBBIED; }
    virtual HRESULT WINAPI CreateCompoundAddress(const DPCOMPOUNDADDRESSELEMENT* elements, DWORD count, void* address, DWORD* size) {
        if (!size || (!elements && count)) return DPERR_INVALIDPARAMS;
        std::vector<std::uint8_t> out;
        auto put = [&](const GUID& g, const void* p, DWORD n) {
            auto a = reinterpret_cast<const std::uint8_t*>(&g); out.insert(out.end(), a, a + sizeof g);
            for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(n >> (8 * k)));
            if (n) out.insert(out.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
        };
        const DWORD placeholder = 0; put(DPAID_TotalSize, &placeholder, 4);
        for (DWORD i = 0; i < count; ++i) {
            if (elements[i].dwDataSize && !elements[i].lpData) return DPERR_INVALIDPARAMS;
            put(elements[i].guidDataType, elements[i].lpData, elements[i].dwDataSize);
        }
        const DWORD total = static_cast<DWORD>(out.size()); std::memcpy(out.data() + 20, &total, 4);
        if (!address || *size < total) { *size = total; return DPERR_BUFFERTOOSMALL; }
        std::memcpy(address, out.data(), total); *size = total; return DP_OK;
    }
    virtual HRESULT WINAPI ConnectEx(DWORD, REFIID, void**, IUnknown*) { return DPERR_NOTLOBBIED; }
    virtual HRESULT WINAPI RegisterApplication(DWORD, void*) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI UnregisterApplication(DWORD, REFGUID) { return DPERR_UNSUPPORTED; }
    virtual HRESULT WINAPI WaitForConnectionSettings(DWORD) { return DPERR_UNSUPPORTED; }
private:
    std::atomic<ULONG> refs{1};
};

// --------------------------------------------------------------- installation
typedef HRESULT(WINAPI* CoCreateInstanceFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
CoCreateInstanceFn originalCoCreateInstance = nullptr;
HRESULT WINAPI CoCreateInstanceHook(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out) {
    if (!active.load()) return originalCoCreateInstance(clsid, outer, context, iid, out);
    if (IsEqualGUID(clsid, CLSID_DirectPlay)) {
        Log("CoCreateInstance(CLSID_DirectPlay)");
        if (!out) return E_POINTER;
        void* object = nullptr; const HRESULT hr = CreateDirectPlay(CurrentConfig(), &object);
        if (FAILED(hr)) return hr;
        auto* dp = static_cast<DirectPlay*>(object); const HRESULT q = dp->QueryInterface(iid, out); dp->Release(); return q;
    }
    if (IsEqualGUID(clsid, CLSID_DirectPlayLobby)) {
        Log("CoCreateInstance(CLSID_DirectPlayLobby)");
        if (!out) return E_POINTER;
        void* object = nullptr; const HRESULT hr = CreateLobby(&object);
        if (FAILED(hr)) return hr;
        auto* lobby = static_cast<Lobby*>(object); const HRESULT q = lobby->QueryInterface(iid, out); lobby->Release(); return q;
    }
    return originalCoCreateInstance(clsid, outer, context, iid, out);
}

bool PatchImport(HMODULE module, const char* dll, const char* function, void* replacement, void** original) {
    auto base = reinterpret_cast<std::uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&slots->u1.Function), static_cast<LONG>(reinterpret_cast<std::uintptr_t>(replacement)));
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}
std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; return s.substr(i);
}
} // namespace

bool ReadDeferred(const std::wstring& path, std::vector<std::vector<std::uint8_t>>& out);
bool ReadConfig(const std::wstring& path, Config& config, std::string* error) {
    if (error) error->clear();
    hp::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) return false;
    char buffer[4096]; DWORD got = 0;
    if (!ReadFile(file.value, buffer, sizeof buffer - 1, &got, nullptr)) { if (error) *error = "relay_config_unreadable"; return false; }
    std::istringstream in(std::string(buffer, got)); std::string line; Config c;
    while (std::getline(in, line)) {
        line = Trim(line); if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('='); if (eq == std::string::npos) continue;
        const auto key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "host") c.host = value;
        else if (key == "port") { try { const auto p = std::stoul(value); if (p && p < 65536) c.port = static_cast<std::uint16_t>(p); } catch (...) {} }
        else if (key == "ticket") c.ticket = value;
        else if (key == "name") c.name = value;
        else if (key == "rejoin") c.rejoin_session = value;
        else if (key == "seat") c.rejoin_seat = value;
        else if (key == "seatid") { try { c.rejoin_seat_id = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16)); } catch (...) {} }
        else if (key == "from") { try { c.rejoin_from = static_cast<std::uint32_t>(std::stoul(value)); } catch (...) {} }
        else if (key == "only") c.only_instance = value;
        else if (key == "sent") { try { c.rejoin_sent = static_cast<std::uint32_t>(std::stoul(value)); } catch (...) {} }
        else if (key == "deferred" && !ReadDeferred(hp::Parent(path) + L"\\" + std::wstring(value.begin(), value.end()), c.rejoin_deferred)) {
            if (error) *error = "relay_deferred_file_invalid"; return false;
        }
    }
    if (c.host.empty() || !c.port) { if (error) *error = "relay_config_requires_host_and_port"; return false; }
    config = c; return true;
}

bool ReadDeferred(const std::wstring& path, std::vector<std::vector<std::uint8_t>>& out) {
    hp::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) return false;
    LARGE_INTEGER size{}; if (!GetFileSizeEx(file.value, &size) || size.QuadPart > 16 * 1024 * 1024) return false;
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size.QuadPart)); DWORD got = 0;
    if (!bytes.empty() && (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size())) return false;
    for (size_t at = 0; at < bytes.size();) {
        if (bytes.size() - at < 4) return false;
        std::uint32_t n = 0; std::memcpy(&n, bytes.data() + at, 4); at += 4;
        if (n < 0x14 || n > bytes.size() - at || out.size() >= 4096) return false;
        out.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + n)); at += n;
    }
    return true;
}

HRESULT CreateDirectPlay(const Config& config, void** out) {
    if (!out) return E_POINTER;
    try { *out = new DirectPlay(config); return S_OK; } catch (...) { *out = nullptr; return E_OUTOFMEMORY; }
}
void TestDropConnection(void* directplay) { if (directplay) static_cast<DirectPlay*>(directplay)->TestDrop(); }
HRESULT CreateLobby(void** out) {
    if (!out) return E_POINTER;
    try { *out = new Lobby(); return S_OK; } catch (...) { *out = nullptr; return E_OUTOFMEMORY; }
}

bool InstallPassive(const std::wstring& log_path, std::string* error) {
    if (installed.load()) return true;
    if (!log_path.empty()) logPath = log_path;
    void* original = nullptr;
    if (!PatchImport(GetModuleHandleW(nullptr), "ole32.dll", "CoCreateInstance", reinterpret_cast<void*>(CoCreateInstanceHook), &original) || !original) {
        if (error) *error = "cocreateinstance_import_not_found";
        return false;
    }
    originalCoCreateInstance = reinterpret_cast<CoCreateInstanceFn>(original);
    installed.store(true);
    return true;
}
void Activate(const Config& config) {
    { std::lock_guard<std::mutex> lock(configMutex); installedConfig = config; }
    holdGameMessages = !config.rejoin_seat.empty();
    active.store(true);
    Log("Relay active for " + config.host + ":" + std::to_string(config.port) + (config.only_instance.empty() ? "" : " showing only " + config.only_instance));
}
void Deactivate() {
    if (!active.exchange(false)) return;
    Log("Relay inactive; DirectPlay is HotA's own again");
}
bool Install(const Config& config, std::string* error) {
    if (installed.load()) return true;
    if (!InstallPassive(config.log_path, error)) return false;
    Activate(config);
    return true;
}
bool Installed() noexcept { return active.load(); }
bool Reconnecting() noexcept { return statReconnecting.load(); }
bool InGame() noexcept { return active.load() && statOpen.load(); }
std::string CurrentInstance() { if (!InGame()) return {}; std::lock_guard<std::mutex> lock(statusMutex); return statusInstance; }
unsigned ReconnectCount() noexcept { return statReconnects.load(); }
std::uint32_t ConsumedCount() noexcept { return statConsumed.load(); }
std::uint32_t SentCount() noexcept { return statSent.load(); }
void ReleaseGameMessages() noexcept { holdGameMessages = false; }
bool CatchingUp() noexcept { return statCatchingUp.load(); }
bool RejoinPlayerName(std::uint32_t id, std::string& name) {
    std::lock_guard<std::mutex> lock(statusMutex);
    const auto it = rejoinNames.find(id); if (it == rejoinNames.end()) return false;
    name = it->second; return true;
}

bool Rejoining() noexcept {
    if (!active.load()) return false;
    std::lock_guard<std::mutex> lock(configMutex); return !installedConfig.rejoin_seat.empty();
}
std::uint32_t RejoinDeferredCount() noexcept {
    if (!active.load()) return 0;
    std::lock_guard<std::mutex> lock(configMutex); return static_cast<std::uint32_t>(installedConfig.rejoin_deferred.size());
}

std::string StatusJson() {
    std::ostringstream o; std::string session, error, instance; std::uint32_t seat = 0; const Config config = CurrentConfig();
    { std::lock_guard<std::mutex> lock(statusMutex); session = statusSession; error = statusError; instance = statusInstance; seat = statusSeat; }
    o << "{\"installed\":" << (installed.load() ? "true" : "false") << ",\"active\":" << (active.load() ? "true" : "false")
      << ",\"host\":" << hp::Json(config.host) << ",\"port\":" << config.port
      << ",\"objects\":" << statObjects.load() << ",\"connected\":" << (statConnected.load() ? "true" : "false")
      << ",\"sessionOpen\":" << (statOpen.load() ? "true" : "false") << ",\"session\":" << hp::Json(session) << ",\"instance\":" << hp::Json(instance) << ",\"seat\":" << seat
      << ",\"rejoinSeat\":" << hp::Json(config.rejoin_seat) << ",\"consumed\":" << statConsumed.load()
      << ",\"sent\":" << statSent.load() << ",\"delivered\":" << statDelivered.load() << ",\"system\":" << statSystem.load()
      << ",\"reconnecting\":" << (statReconnecting.load() ? "true" : "false") << ",\"reconnects\":" << statReconnects.load()
      << ",\"generation\":" << statGeneration.load()
      << ",\"error\":" << hp::Json(error) << '}';
    return o.str();
}
}
