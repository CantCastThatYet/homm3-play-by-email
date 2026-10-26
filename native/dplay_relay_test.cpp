// Integration test: two replacement DirectPlay objects talk through a live relay.
// The interface below is declared independently from dplay_relay.cpp, in the
// documented IDirectPlay4A slot order, so the test also checks the vtable ABI
// that the game's compiled calls depend on.
// Usage: Native.RelayTest.exe <host> <port>
#include "dplay_relay.hpp"
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
typedef DWORD DPID;
#pragma pack(push, 4)
struct DPNAME { DWORD dwSize, dwFlags; char* shortName; char* longName; };
struct DPSESSIONDESC2 { DWORD dwSize, dwFlags; GUID instance, application; DWORD maxPlayers, currentPlayers; char* name; char* password; DWORD r1, r2, u1, u2, u3, u4; };
struct DPCHAT { DWORD dwSize, dwFlags; char* text; };
struct DPCOMPOUNDADDRESSELEMENT { GUID type; DWORD size; void* data; };
#pragma pack(pop)
typedef BOOL(WINAPI* EnumSessionsCb)(const DPSESSIONDESC2*, DWORD*, DWORD, void*);
typedef BOOL(WINAPI* EnumPlayersCb)(DPID, DWORD, const DPNAME*, DWORD, void*);

struct IDP4 {
    virtual HRESULT WINAPI QueryInterface(REFIID, void**) = 0;
    virtual ULONG WINAPI AddRef() = 0;
    virtual ULONG WINAPI Release() = 0;
    virtual HRESULT WINAPI AddPlayerToGroup(DPID, DPID) = 0;
    virtual HRESULT WINAPI Close() = 0;
    virtual HRESULT WINAPI CreateGroup(DPID*, DPNAME*, void*, DWORD, DWORD) = 0;
    virtual HRESULT WINAPI CreatePlayer(DPID*, DPNAME*, HANDLE, void*, DWORD, DWORD) = 0;
    virtual HRESULT WINAPI DeletePlayerFromGroup(DPID, DPID) = 0;
    virtual HRESULT WINAPI DestroyGroup(DPID) = 0;
    virtual HRESULT WINAPI DestroyPlayer(DPID) = 0;
    virtual HRESULT WINAPI EnumGroupPlayers(DPID, GUID*, EnumPlayersCb, void*, DWORD) = 0;
    virtual HRESULT WINAPI EnumGroups(GUID*, EnumPlayersCb, void*, DWORD) = 0;
    virtual HRESULT WINAPI EnumPlayers(GUID*, EnumPlayersCb, void*, DWORD) = 0;
    virtual HRESULT WINAPI EnumSessions(DPSESSIONDESC2*, DWORD, EnumSessionsCb, void*, DWORD) = 0;
    virtual HRESULT WINAPI GetCaps(void*, DWORD) = 0;
    virtual HRESULT WINAPI GetGroupData(DPID, void*, DWORD*, DWORD) = 0;
    virtual HRESULT WINAPI GetGroupName(DPID, void*, DWORD*) = 0;
    virtual HRESULT WINAPI GetMessageCount(DPID, DWORD*) = 0;
    virtual HRESULT WINAPI GetPlayerAddress(DPID, void*, DWORD*) = 0;
    virtual HRESULT WINAPI GetPlayerCaps(DPID, void*, DWORD) = 0;
    virtual HRESULT WINAPI GetPlayerData(DPID, void*, DWORD*, DWORD) = 0;
    virtual HRESULT WINAPI GetPlayerName(DPID, void*, DWORD*) = 0;
    virtual HRESULT WINAPI GetSessionDesc(void*, DWORD*) = 0;
    virtual HRESULT WINAPI Initialize(GUID*) = 0;
    virtual HRESULT WINAPI Open(DPSESSIONDESC2*, DWORD) = 0;
    virtual HRESULT WINAPI Receive(DPID*, DPID*, DWORD, void*, DWORD*) = 0;
    virtual HRESULT WINAPI Send(DPID, DPID, DWORD, void*, DWORD) = 0;
    virtual HRESULT WINAPI SetGroupData(DPID, void*, DWORD, DWORD) = 0;
    virtual HRESULT WINAPI SetGroupName(DPID, DPNAME*, DWORD) = 0;
    virtual HRESULT WINAPI SetPlayerData(DPID, void*, DWORD, DWORD) = 0;
    virtual HRESULT WINAPI SetPlayerName(DPID, DPNAME*, DWORD) = 0;
    virtual HRESULT WINAPI SetSessionDesc(DPSESSIONDESC2*, DWORD) = 0;
    virtual HRESULT WINAPI AddGroupToGroup(DPID, DPID) = 0;
    virtual HRESULT WINAPI CreateGroupInGroup(DPID, DPID*, DPNAME*, void*, DWORD, DWORD) = 0;
    virtual HRESULT WINAPI DeleteGroupFromGroup(DPID, DPID) = 0;
    virtual HRESULT WINAPI EnumConnections(const GUID*, void*, void*, DWORD) = 0;
    virtual HRESULT WINAPI EnumGroupsInGroup(DPID, GUID*, EnumPlayersCb, void*, DWORD) = 0;
    virtual HRESULT WINAPI GetGroupConnectionSettings(DWORD, DPID, void*, DWORD*) = 0;
    virtual HRESULT WINAPI InitializeConnection(void*, DWORD) = 0;
    virtual HRESULT WINAPI SecureOpen(const DPSESSIONDESC2*, DWORD, const void*, const void*) = 0;
    virtual HRESULT WINAPI SendChatMessage(DPID, DPID, DWORD, DPCHAT*) = 0;
    virtual HRESULT WINAPI SetGroupConnectionSettings(DWORD, DPID, void*) = 0;
    virtual HRESULT WINAPI StartSession(DWORD, DPID) = 0;
    virtual HRESULT WINAPI GetGroupFlags(DPID, DWORD*) = 0;
    virtual HRESULT WINAPI GetGroupParent(DPID, DPID*) = 0;
    virtual HRESULT WINAPI GetPlayerAccount(DPID, DWORD, void*, DWORD*) = 0;
    virtual HRESULT WINAPI GetPlayerFlags(DPID, DWORD*) = 0;
    virtual HRESULT WINAPI GetGroupOwner(DPID, DPID*) = 0;
    virtual HRESULT WINAPI SetGroupOwner(DPID, DPID) = 0;
    virtual HRESULT WINAPI SendEx(DPID, DPID, DWORD, void*, DWORD, DWORD, DWORD, void*, DWORD*) = 0;
    virtual HRESULT WINAPI GetMessageQueue(DPID, DPID, DWORD, DWORD*, DWORD*) = 0;
};
struct ILobby3 {
    virtual HRESULT WINAPI QueryInterface(REFIID, void**) = 0;
    virtual ULONG WINAPI AddRef() = 0;
    virtual ULONG WINAPI Release() = 0;
    virtual HRESULT WINAPI Connect(DWORD, void**, IUnknown*) = 0;
    virtual HRESULT WINAPI CreateAddress(REFGUID, REFGUID, const void*, DWORD, void*, DWORD*) = 0;
    virtual HRESULT WINAPI EnumAddress(BOOL(WINAPI*)(REFGUID, DWORD, const void*, void*), const void*, DWORD, void*) = 0;
    virtual HRESULT WINAPI EnumAddressTypes(void*, REFGUID, void*, DWORD) = 0;
    virtual HRESULT WINAPI EnumLocalApplications(void*, void*, DWORD) = 0;
    virtual HRESULT WINAPI GetConnectionSettings(DWORD, void*, DWORD*) = 0;
    virtual HRESULT WINAPI ReceiveLobbyMessage(DWORD, DWORD, DWORD*, void*, DWORD*) = 0;
    virtual HRESULT WINAPI RunApplication(DWORD, DWORD*, void*, HANDLE) = 0;
    virtual HRESULT WINAPI SendLobbyMessage(DWORD, DWORD, void*, DWORD) = 0;
    virtual HRESULT WINAPI SetConnectionSettings(DWORD, DWORD, void*) = 0;
    virtual HRESULT WINAPI SetLobbyMessageEvent(DWORD, DWORD, HANDLE) = 0;
    virtual HRESULT WINAPI CreateCompoundAddress(const DPCOMPOUNDADDRESSELEMENT*, DWORD, void*, DWORD*) = 0;
};

const GUID App{0x12345678, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};
const GUID SP_TCP{0x36e95ee0,0x8577,0x11cf,{0x96,0x0c,0x00,0x80,0xc7,0x53,0x4e,0x82}};
const GUID AID_SP{0x07d916c0,0xe0af,0x11cf,{0x9c,0x4e,0x00,0xa0,0xc9,0x05,0x42,0x5e}};
const GUID AID_INET{0xc4a54da0,0xe0af,0x11cf,{0x9c,0x4e,0x00,0xa0,0xc9,0x05,0x42,0x5e}};
const HRESULT NoMessages = static_cast<HRESULT>(0x887700BE), TooSmall = static_cast<HRESULT>(0x8877001E);
int failures = 0;
void Check(bool condition, const char* what) { std::printf("%s %s\n", condition ? "PASS" : "FAIL", what); if (!condition) ++failures; }

struct Message { DPID from = 0, to = 0; std::vector<unsigned char> data; HRESULT hr = NoMessages; };
Message Wait(IDP4* dp, DWORD timeoutMs = 5000) {
    const auto deadline = GetTickCount64() + timeoutMs;
    Message m;
    while (GetTickCount64() < deadline) {
        DWORD size = 0; m.from = m.to = 0;
        m.hr = dp->Receive(&m.from, &m.to, 1, nullptr, &size);
        if (m.hr == TooSmall) {
            m.data.resize(size);
            m.hr = dp->Receive(&m.from, &m.to, 1, m.data.data(), &size);
            return m;
        }
        Sleep(10);
    }
    return m;
}
// The next game message, skipping system messages.
Message WaitGame(IDP4* dp, DWORD timeoutMs = 5000) {
    const auto deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const auto now = GetTickCount64(); if (now >= deadline) return {};
        auto m = Wait(dp, static_cast<DWORD>(deadline - now));
        if (m.hr != S_OK || m.from != 0) return m;
    }
}
DWORD At(const Message& m, size_t offset) { DWORD v = 0; if (m.data.size() >= offset + 4) std::memcpy(&v, m.data.data() + offset, 4); return v; }
const char* Ptr(const Message& m, size_t offset) { return reinterpret_cast<const char*>(static_cast<std::uintptr_t>(At(m, offset))); }

struct Found { GUID instance{}; bool found = false; DWORD flags = 0; };
BOOL WINAPI OnSession(const DPSESSIONDESC2* d, DWORD*, DWORD flags, void* context) {
    if (flags & 1) return FALSE;
    auto f = static_cast<Found*>(context);
    if (d && d->name && std::strcmp(d->name, "Relay test") == 0) { f->instance = d->instance; f->found = true; f->flags = d->dwFlags; }
    return TRUE;
}
struct Named { const char* name; GUID instance{}; bool found = false; };
BOOL WINAPI OnNamedSession(const DPSESSIONDESC2* d, DWORD*, DWORD flags, void* context) {
    if (flags & 1) return FALSE;
    auto f = static_cast<Named*>(context);
    if (d && d->name && std::strcmp(d->name, f->name) == 0) { f->instance = d->instance; f->found = true; }
    return TRUE;
}
struct Players { std::vector<std::pair<DPID, DWORD>> list; };
BOOL WINAPI OnPlayer(DPID id, DWORD, const DPNAME*, DWORD flags, void* context) { static_cast<Players*>(context)->list.push_back({id, flags}); return TRUE; }
struct AddressSeen { bool sp = false; std::string inet; };
BOOL WINAPI OnAddress(REFGUID g, DWORD n, const void* data, void* context) {
    auto a = static_cast<AddressSeen*>(context);
    if (IsEqualGUID(g, AID_SP) && n == sizeof(GUID) && IsEqualGUID(*static_cast<const GUID*>(data), SP_TCP)) a->sp = true;
    if (IsEqualGUID(g, AID_INET)) a->inet.assign(static_cast<const char*>(data));
    return TRUE;
}
}

int main(int argc, char** argv) {
    if (argc != 3) { std::printf("usage: Native.RelayTest.exe <host> <port>\n"); return 2; }
    hp_relay::Config config; config.host = argv[1]; config.port = static_cast<std::uint16_t>(std::atoi(argv[2])); config.name = "relay-test";

    void* raw = nullptr;
    hp_relay::CreateLobby(&raw); auto lobby = static_cast<ILobby3*>(raw);
    const char ip[] = "10.0.0.7";
    DPCOMPOUNDADDRESSELEMENT elements[2]{{AID_SP, sizeof(GUID), const_cast<GUID*>(&SP_TCP)}, {AID_INET, sizeof ip, const_cast<char*>(ip)}};
    DWORD addressSize = 0;
    Check(lobby->CreateCompoundAddress(elements, 2, nullptr, &addressSize) == TooSmall && addressSize > 0, "compound address reports its size");
    std::vector<unsigned char> address(addressSize);
    Check(SUCCEEDED(lobby->CreateCompoundAddress(elements, 2, address.data(), &addressSize)), "compound address created");
    AddressSeen seen; lobby->EnumAddress(OnAddress, address.data(), addressSize, &seen);
    Check(seen.sp && seen.inet == ip, "address enumerates provider and IP");

    hp_relay::CreateDirectPlay(config, &raw); auto host = static_cast<IDP4*>(raw);
    hp_relay::CreateDirectPlay(config, &raw); auto peer = static_cast<IDP4*>(raw);
    Check(SUCCEEDED(host->InitializeConnection(address.data(), 0)), "host InitializeConnection");
    Check(SUCCEEDED(peer->InitializeConnection(address.data(), 0)), "peer InitializeConnection");

    DPSESSIONDESC2 desc{}; desc.dwSize = sizeof desc; desc.dwFlags = 0x44; desc.application = App; desc.maxPlayers = 8;
    char sessionName[] = "Relay test"; desc.name = sessionName;
    Check(SUCCEEDED(host->Open(&desc, 2)), "host creates session");
    char hostName[] = "Red"; DPNAME hn{sizeof(DPNAME), 0, hostName, nullptr};
    DPID hostId = 0; Check(SUCCEEDED(host->CreatePlayer(&hostId, &hn, nullptr, nullptr, 0, 0x100)) && hostId == 1, "host server player gets DPID 1");

    Found found; DPSESSIONDESC2 filter{}; filter.dwSize = sizeof filter; filter.application = App;
    Check(SUCCEEDED(peer->EnumSessions(&filter, 0, OnSession, &found, 0x42)) && found.found, "peer enumerates the session");
    DPSESSIONDESC2 join{}; join.dwSize = sizeof join; join.instance = found.instance;
    Check(SUCCEEDED(peer->Open(&join, 1)), "peer joins session");
    char peerName[] = "Blue"; DPNAME pn{sizeof(DPNAME), 0, peerName, nullptr};
    unsigned char peerData[] = {9, 8, 7};
    DPID peerId = 0; Check(SUCCEEDED(peer->CreatePlayer(&peerId, &pn, nullptr, peerData, sizeof peerData, 0)) && peerId > 1, "peer player created");

    auto created = Wait(host);
    Check(created.hr == S_OK && created.from == 0 && At(created, 0) == 3 && At(created, 4) == 1 && At(created, 8) == peerId,
          "host receives DPSYS_CREATEPLAYERORGROUP for peer");
    Check(Ptr(created, 32) && std::strcmp(Ptr(created, 32), "Blue") == 0, "create message name points into caller buffer");
    Check(At(created, 20) == 3 && Ptr(created, 16) && std::memcmp(Ptr(created, 16), peerData, 3) == 0, "create message carries player data");

    Players players; peer->EnumPlayers(nullptr, OnPlayer, &players, 0);
    bool sawHost = false, sawSelf = false;
    for (auto& [id, flags] : players.list) { if (id == 1 && (flags & 0x10) && (flags & 0x100)) sawHost = true; if (id == peerId && (flags & 8)) sawSelf = true; }
    Check(players.list.size() == 2 && sawHost && sawSelf, "peer enumerates remote server player and its local player");

    char hello[] = "hello from red";
    Check(SUCCEEDED(host->Send(hostId, 0, 1, hello, sizeof hello)), "host broadcasts");
    auto got = Wait(peer);
    Check(got.hr == S_OK && got.from == hostId && got.to == peerId && got.data.size() == sizeof hello && std::memcmp(got.data.data(), hello, sizeof hello) == 0,
          "peer receives broadcast with DirectPlay addressing");
    char back[] = "blue answers";
    peer->Send(peerId, hostId, 1, back, sizeof back);
    got = Wait(host);
    Check(got.hr == S_OK && got.from == peerId && got.to == hostId && std::string(reinterpret_cast<char*>(got.data.data())) == back, "host receives direct message");

    for (int i = 0; i < 200; ++i) { unsigned char n[4]; std::memcpy(n, &i, 4); host->Send(hostId, peerId, 1, n, 4); }
    bool ordered = true;
    for (int i = 0; i < 200; ++i) { auto m = Wait(peer); int v = -1; if (m.data.size() == 4) std::memcpy(&v, m.data.data(), 4); if (v != i) { ordered = false; break; } }
    Check(ordered, "200 messages arrive complete and in order");

    std::vector<unsigned char> big(64 * 1024); for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<unsigned char>(i * 31);
    host->Send(hostId, peerId, 1, big.data(), static_cast<DWORD>(big.size()));
    got = Wait(peer); Check(got.data == big, "64 KiB message round-trips intact");

    char text[] = "gg wp"; DPCHAT chat{sizeof(DPCHAT), 0, text};
    peer->SendChatMessage(peerId, 0, 0, &chat);
    got = Wait(host);
    Check(got.hr == S_OK && At(got, 0) == 0x109 && At(got, 8) == peerId && Ptr(got, 20) && std::strcmp(reinterpret_cast<const DPCHAT*>(Ptr(got, 20))->text, "gg wp") == 0,
          "chat arrives as DPSYS_CHAT");

    char groupName[] = "Allies"; DPNAME gn{sizeof(DPNAME), 0, groupName, nullptr};
    DPID group = 0; Check(SUCCEEDED(host->CreateGroup(&group, &gn, nullptr, 0, 0)) && group, "host creates group");
    got = Wait(peer); Check(At(got, 0) == 3 && At(got, 4) == 0 && At(got, 8) == group, "peer learns of group");
    Check(SUCCEEDED(host->AddPlayerToGroup(group, peerId)), "host adds peer to group");
    got = Wait(peer); Check(At(got, 0) == 7 && At(got, 4) == group && At(got, 8) == peerId, "peer receives DPSYS_ADDPLAYERTOGROUP");
    char toGroup[] = "group msg"; host->Send(hostId, group, 1, toGroup, sizeof toGroup);
    got = Wait(peer); Check(got.from == hostId && got.to == peerId && got.data.size() == sizeof toGroup, "group message reaches member");

    unsigned char newData[] = {1, 2, 3, 4, 5};
    peer->SetPlayerData(peerId, newData, sizeof newData, 0);
    got = Wait(host); Check(At(got, 0) == 0x102 && At(got, 8) == peerId && At(got, 16) == 5, "host receives DPSYS_SETPLAYERORGROUPDATA");
    DWORD size = 0; host->GetPlayerData(peerId, nullptr, &size, 0); std::vector<unsigned char> readBack(size);
    Check(SUCCEEDED(host->GetPlayerData(peerId, readBack.data(), &size, 0)) && size == 5 && std::memcmp(readBack.data(), newData, 5) == 0, "host reads updated remote data");
    size = 0; host->GetPlayerName(peerId, nullptr, &size); std::vector<unsigned char> nameBuffer(size);
    Check(SUCCEEDED(host->GetPlayerName(peerId, nameBuffer.data(), &size)) && std::strcmp(reinterpret_cast<DPNAME*>(nameBuffer.data())->shortName, "Blue") == 0, "GetPlayerName");
    size = 0; peer->GetSessionDesc(nullptr, &size); std::vector<unsigned char> sd(size);
    Check(SUCCEEDED(peer->GetSessionDesc(sd.data(), &size)) && std::strcmp(reinterpret_cast<DPSESSIONDESC2*>(sd.data())->name, "Relay test") == 0 &&
          reinterpret_cast<DPSESSIONDESC2*>(sd.data())->currentPlayers == 2, "peer session description");

    DWORD tiny = 1; DPID f = 0, t = 0; host->Send(hostId, peerId, 1, hello, sizeof hello); Sleep(300);
    Check(peer->Receive(&f, &t, 1, &tiny, &tiny) == TooSmall && tiny == sizeof hello, "small buffer reports required size without losing message");
    got = Wait(peer); Check(got.data.size() == sizeof hello, "message still delivered after size query");

    Check(SUCCEEDED(peer->Close()), "peer closes");
    got = Wait(host); Check(At(got, 0) == 5 && At(got, 8) == peerId, "host receives DPSYS_DESTROYPLAYERORGROUP");
    Check(Wait(host, 300).hr == NoMessages, "no stray messages");

    // A started game survives a dropped connection: the client redials, takes its seat back, and
    // nothing is lost or repeated in either direction.
    hp_relay::CreateDirectPlay(config, &raw); auto red = static_cast<IDP4*>(raw);
    hp_relay::CreateDirectPlay(config, &raw); auto blue = static_cast<IDP4*>(raw);
    red->InitializeConnection(address.data(), 0); blue->InitializeConnection(address.data(), 0);
    char resumeName[] = "Reconnect test"; DPSESSIONDESC2 second = desc; second.name = resumeName;
    Check(SUCCEEDED(red->Open(&second, 2)), "second session created");
    DPID redId = 0; red->CreatePlayer(&redId, &hn, nullptr, nullptr, 0, 0x100);
    Named named{"Reconnect test"}; blue->EnumSessions(&filter, 0, OnNamedSession, &named, 0x42);
    DPSESSIONDESC2 rejoin{}; rejoin.dwSize = sizeof rejoin; rejoin.instance = named.instance;
    Check(named.found && SUCCEEDED(blue->Open(&rejoin, 1)), "second peer joins");
    DPID blueId = 0; blue->CreatePlayer(&blueId, &pn, nullptr, nullptr, 0, 0);
    Wait(red);
    unsigned char start[0x18]{}; const DWORD loadGame = 0x3E8; std::memcpy(start + 4, &redId, 4); std::memcpy(start + 8, &loadGame, 4);
    red->Send(redId, 0, 1, start, sizeof start);
    Check(Wait(blue).data.size() == sizeof start, "the game starts");
    const unsigned reconnects = hp_relay::ReconnectCount();
    hp_relay::TestDropConnection(blue);
    Sleep(300);
    for (int i = 0; i < 3; ++i) { unsigned char n[8]{'r', 0, 0, 0, static_cast<unsigned char>(i)}; red->Send(redId, blueId, 1, n, 8); }
    bool accepted = true;
    for (int i = 0; i < 2; ++i) { unsigned char n[8]{'b', 0, 0, 0, static_cast<unsigned char>(i)}; accepted &= SUCCEEDED(blue->Send(blueId, redId, 1, n, 8)); }
    Check(accepted, "sends while disconnected are accepted");
    bool inOrder = true;
    for (int i = 0; i < 3; ++i) { auto m = Wait(blue, 15000); if (m.data.size() != 8 || m.data[0] != 'r' || m.data[4] != i) inOrder = false; }
    Check(inOrder, "messages sent to the client while it was away arrive after it reconnects");
    inOrder = true;
    for (int i = 0; i < 2; ++i) { auto m = Wait(red, 15000); if (m.data.size() != 8 || m.data[0] != 'b' || m.data[4] != i) inOrder = false; }
    Check(inOrder, "messages the client sent while away reach the host");
    Check(hp_relay::ReconnectCount() > reconnects && !hp_relay::Reconnecting(), "the client reconnected by itself");
    Check(Wait(blue, 500).hr == NoMessages && Wait(red, 500).hr == NoMessages, "no session loss and no duplicates");
    red->Release(); blue->Release();

    // A seat claimed back with nothing to replay (Continue where the player left): the relay
    // ignores what the new process sends until it has caught up, which it says by itself once
    // its engine has polled for five seconds, without a message of its own to carry it.
    hp_relay::CreateDirectPlay(config, &raw); auto keeper = static_cast<IDP4*>(raw);
    hp_relay::CreateDirectPlay(config, &raw); auto leaver = static_cast<IDP4*>(raw);
    keeper->InitializeConnection(address.data(), 0); leaver->InitializeConnection(address.data(), 0);
    char catchName[] = "Catch-up test"; DPSESSIONDESC2 third = desc; third.name = catchName;
    Check(SUCCEEDED(keeper->Open(&third, 2)), "third session created");
    DPID keeperId = 0; keeper->CreatePlayer(&keeperId, &hn, nullptr, nullptr, 0, 0x100);
    Named catching{"Catch-up test"}; leaver->EnumSessions(&filter, 0, OnNamedSession, &catching, 0x42);
    DPSESSIONDESC2 joinThird{}; joinThird.dwSize = sizeof joinThird; joinThird.instance = catching.instance;
    Check(catching.found && SUCCEEDED(leaver->Open(&joinThird, 1)), "third peer joins");
    DPID leaverId = 0; leaver->CreatePlayer(&leaverId, &pn, nullptr, nullptr, 0, 0);
    Wait(keeper);
    std::memcpy(start + 4, &keeperId, 4); keeper->Send(keeperId, 0, 1, start, sizeof start);
    Check(WaitGame(leaver).data.size() == sizeof start, "the third game starts");
    leaver->Close(); leaver->Release();
    hp_relay::Config continuing = config; continuing.rejoin_session = "Catch-up test"; continuing.rejoin_seat = "Blue"; continuing.rejoin_from = 1; continuing.rejoin_sent = 0;
    hp_relay::CreateDirectPlay(continuing, &raw); auto returned = static_cast<IDP4*>(raw);
    returned->InitializeConnection(address.data(), 0);
    Check(SUCCEEDED(returned->Open(&third, 2)), "the player continues");
    DPID returnedId = 0;
    Check(SUCCEEDED(returned->CreatePlayer(&returnedId, &pn, nullptr, nullptr, 0, 0)) && returnedId == leaverId && hp_relay::CatchingUp(),
          "the seat is claimed back and catching up");
    char repeat[] = "repeat"; returned->Send(returnedId, keeperId, 1, repeat, sizeof repeat);
    Check(WaitGame(keeper, 1000).hr == NoMessages, "what it sends while catching up is not delivered");
    Check(Wait(returned, 6000).hr == NoMessages && !hp_relay::CatchingUp(), "it catches up with nothing to replay and nothing to send");
    char fresh[] = "fresh"; returned->Send(returnedId, keeperId, 1, fresh, sizeof fresh);
    got = WaitGame(keeper);
    Check(got.hr == S_OK && got.from == returnedId && got.data.size() == sizeof fresh && std::memcmp(got.data.data(), fresh, sizeof fresh) == 0,
          "its next message reaches the other player");
    keeper->Release(); returned->Release();

    peer->Release(); host->Release(); lobby->Release();
    std::printf(failures ? "%d FAILED\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
