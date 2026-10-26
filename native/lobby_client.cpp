#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include "lobby_client.hpp"
#include "json_lite.hpp"
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

namespace hp_lobby_client {
namespace {
std::mutex mutex;
std::condition_variable wake;
Snapshot state;                 // guarded by mutex
bool started = false, open = false, wanted = false, kick = false;
std::string name;               // the name to register with, game charset
std::wstring iniPath;
HWND notifyWindow = nullptr;

std::wstring Wide(const std::string& s, UINT page) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(page, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(page, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w, UINT page) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(page, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(page, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string JsonString(const std::string& utf8) {
    std::string out = "\"";
    for (const unsigned char c : utf8) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

// ------------------------------------------------------------------ settings
// HeroesPost.ini in the HotA folder names the server. Who plays belongs to the Windows user: their
// sign-ins live in %LOCALAPPDATA%\HeroesPost\accounts.ini, one token per server, so any HotA they
// start plays as them, and a copied or shared HotA folder carries no one's identity.
struct Settings { std::string server, token; };
std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
std::string Unslashed(std::string server) { while (!server.empty() && server.back() == '/') server.pop_back(); return server; }

std::mutex storeMutex;
// HEROES_POST_PROFILE names another directory for it, as it does for the desktop app: two players on one
// Windows user, for trying a game alone.
std::wstring StorePath() {
    wchar_t base[MAX_PATH]{};
    std::wstring dir;
    if (GetEnvironmentVariableW(L"HEROES_POST_PROFILE", base, MAX_PATH)) dir = base;
    else if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) dir = std::wstring(base) + L"\\HeroesPost";
    else return {};
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\accounts.ini";
}
// Lines "server<TAB>token<TAB>account": the account name stays after signing out, for the next sign-in.
struct Stored { std::string server, token, account; };
std::vector<Stored> ReadStore() {
    std::vector<Stored> entries;
    std::ifstream in(StorePath()); std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        const auto tab = line.find('\t');
        if (line.empty() || line[0] == ';' || tab == std::string::npos) continue;
        const auto rest = line.substr(tab + 1);
        const auto second = rest.find('\t');
        entries.push_back({Unslashed(line.substr(0, tab)), Trim(rest.substr(0, second)), second == std::string::npos ? "" : Trim(rest.substr(second + 1))});
    }
    return entries;
}
Stored StoredFor(const std::string& server) {
    std::lock_guard<std::mutex> lock(storeMutex);
    for (const auto& e : ReadStore()) if (e.server == server) return e;
    return {server, "", ""};
}
std::string StoredToken(const std::string& server) { return StoredFor(server).token; }
// An empty token signs out of the server; an empty account keeps the one remembered.
void StoreToken(const std::string& server, const std::string& token, const std::string& account = {}) {
    std::lock_guard<std::mutex> lock(storeMutex);
    auto entries = ReadStore();
    Stored entry{server, token, account};
    for (const auto& e : entries) if (e.server == server && entry.account.empty()) entry.account = e.account;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Stored& e) { return e.server == server; }), entries.end());
    if (!entry.token.empty() || !entry.account.empty()) entries.push_back(entry);
    std::ostringstream out; out << "; Heroes Post sign-ins of this Windows user: server, token (empty when signed out), account name; tab-separated.\r\n";
    for (const auto& e : entries) out << e.server << '\t' << e.token << '\t' << e.account << "\r\n";
    std::ofstream(StorePath(), std::ios::binary | std::ios::trunc) << out.str();
}

Settings ReadSettings() {
    Settings s; std::string token, tokenServer;
    {
        std::ifstream in(iniPath); std::string line;
        while (std::getline(in, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            const auto eq = line.find('='); if (eq == std::string::npos) continue;
            const auto key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
            if (key == "server") s.server = Unslashed(value);
            else if (key == "token") token = value;
            else if (key == "token_server") tokenServer = Unslashed(value);
        }
    }
    // Before accounts the identity was written into HeroesPost.ini: it moves to the Windows user's store.
    if (!token.empty()) {
        if (!tokenServer.empty() && StoredToken(tokenServer).empty()) StoreToken(tokenServer, token);
        std::ifstream in(iniPath); std::string line; std::ostringstream out;
        while (std::getline(in, line)) {
            const auto key = Trim(line.substr(0, line.find('=')));
            if (key != "token" && key != "token_server") out << Trim(line) << "\r\n";
        }
        in.close();
        std::ofstream(iniPath, std::ios::binary | std::ios::trunc) << out.str();
    }
    if (!s.server.empty()) s.token = StoredToken(s.server);
    return s;
}
void WriteTemplate() {
    if (GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::ofstream(iniPath, std::ios::binary) << "; Heroes Post: the server this HotA plays Heroes Post games on.\r\n"
        "; Example: server=http://heroes.example.org:8080\r\nserver=\r\n";
}

// ---------------------------------------------------------------------- http
// Signing up, finding the relay port, and bulk transfers (seat captures and Continue); the lobby
// itself comes over the relay.
struct Url { std::wstring host, path; INTERNET_PORT port = 0; bool secure = false; };
bool Crack(const std::string& server, Url& url) {
    const auto wide = Wide(server, CP_UTF8);
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof parts;
    wchar_t host[256]{}, path[1024]{};
    parts.lpszHostName = host; parts.dwHostNameLength = 256; parts.lpszUrlPath = path; parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return false;
    url.host = host; url.path = path; url.port = parts.nPort; url.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return !url.host.empty();
}
using Response = Reply;
Response Request(const std::string& server, const char* method, const std::string& path, const std::string& body,
    const std::string& token = {}, const wchar_t* type = L"application/json", size_t limit = 1024 * 1024, int timeout = 5) {
    Response r; Url url;
    if (!Crack(server, url)) { r.error = "the server address is not a valid http:// URL"; return r; }
    HINTERNET session = WinHttpOpen(L"HeroesPost/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { r.error = "network unavailable"; return r; }
    WinHttpSetTimeouts(session, 4000, 4000, timeout * 1000, timeout * 1000);
    HINTERNET connection = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, Wide(method, CP_UTF8).c_str(), Wide(path, CP_UTF8).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    std::wstring headers = std::wstring(L"Content-Type: ") + type + L"\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + Wide(token, CP_UTF8) + L"\r\n";
    if (request && WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
        r.status = static_cast<int>(status);
        for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available && r.body.size() < limit;) {
            std::string chunk(available, '\0'); DWORD got = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &got) || !got) break;
            r.body.append(chunk.data(), got);
        }
    } else r.error = "cannot reach the server";
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return r;
}
// The server's problem details carry a human-readable "detail".
std::string Problem(const Response& r) {
    if (!r.error.empty()) return r.error;
    hp_json::Value v;
    if (hp_json::Parse(r.body, v) && !v["detail"].str().empty()) return v["detail"].str();
    return "the server answered " + std::to_string(r.status);
}

// ------------------------------------------------------------- relay frames
// The relay's framing (RelayProtocol.cs): u32 length of the rest, u16 type, little-endian fields.
constexpr std::uint32_t ProtocolVersion = 1;
constexpr std::uint16_t C_HELLO = 1, C_LOBBY_WATCH = 17, C_LOBBY_CHAT = 18, C_LOBBY_TURN_LIMIT = 19;
constexpr std::uint16_t S_LOBBY_STATE = 112, S_LOBBY_CHAT = 113, S_LOBBY_NOTICE = 114;
constexpr std::uint32_t NOTICE_SIGN_IN = 1;
constexpr std::uint32_t MaxFrame = 4 * 1024 * 1024;

struct Frame {
    std::vector<std::uint8_t> b;
    explicit Frame(std::uint16_t type) : b(6, 0) { b[4] = static_cast<std::uint8_t>(type); b[5] = static_cast<std::uint8_t>(type >> 8); }
    Frame& u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); return *this; }
    Frame& bytes(const void* p, size_t n) { const auto* c = static_cast<const std::uint8_t*>(p); b.insert(b.end(), c, c + n); return *this; }
    Frame& blob(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); return bytes(s.data(), s.size()); }
    std::vector<std::uint8_t> Finish() {
        const auto n = static_cast<std::uint32_t>(b.size() - 4);
        for (int i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(n >> (8 * i));
        return std::move(b);
    }
};
struct Reader {
    const std::vector<std::uint8_t>& f; size_t at; bool ok = true;
    std::uint32_t u32() {
        if (f.size() - at < 4) { ok = false; return 0; }
        std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(f[at + i]) << (8 * i);
        at += 4; return v;
    }
    std::string blob() {
        const auto n = u32();
        if (!ok || f.size() - at < n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(f.data() + at), n); at += n; return s;
    }
};

// The connection is the worker's; other threads only send on it or shut it down.
std::mutex sendMutex;
SOCKET relay = INVALID_SOCKET;  // guarded by sendMutex

bool Post(Frame& frame) {
    const auto bytes = frame.Finish();
    std::lock_guard<std::mutex> lock(sendMutex);
    if (relay == INVALID_SOCKET) return false;
    for (size_t done = 0; done < bytes.size();) {
        const int n = ::send(relay, reinterpret_cast<const char*>(bytes.data() + done), static_cast<int>(bytes.size() - done), 0);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}
void Drop() { std::lock_guard<std::mutex> lock(sendMutex); if (relay != INVALID_SOCKET) shutdown(relay, SD_BOTH); }
bool ReadExact(SOCKET s, std::uint8_t* p, size_t n) {
    while (n) { const int got = ::recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0); if (got <= 0) return false; p += got; n -= static_cast<size_t>(got); }
    return true;
}
SOCKET Dial(const std::string& host, int port, std::string& error) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) || !found) { error = "cannot find " + host; return INVALID_SOCKET; }
    SOCKET s = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    const bool ok = s != INVALID_SOCKET && ::connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen)) == 0;
    freeaddrinfo(found);
    if (!ok) { if (s != INVALID_SOCKET) closesocket(s); error = "cannot reach the game relay on port " + std::to_string(port); return INVALID_SOCKET; }
    BOOL yes = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
    // The lobby can stay quiet for hours; probe so a dead route is noticed and redialled.
    tcp_keepalive alive{1, 30000, 5000}; DWORD unused = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &alive, sizeof alive, nullptr, 0, &unused, nullptr, nullptr);
    return s;
}

// --------------------------------------------------------------- the worker
void Notify() {
    if (!notifyWindow) return;
    { std::lock_guard<std::mutex> lock(mutex); if (!open) return; }
    POINT p{}; GetCursorPos(&p); ScreenToClient(notifyWindow, &p);
    PostMessageW(notifyWindow, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
}
void Publish(const std::function<void(Snapshot&)>& change) {
    { std::lock_guard<std::mutex> lock(mutex); change(state); ++state.version; }
    Notify();
}
void Offline(const std::string& status) { Publish([&](Snapshot& s) { s.online = false; s.status = status; }); }

Settings settings;              // the worker's copy
std::atomic<unsigned> identity{0}; // changes whenever this HotA signs in or out

void SignedOut(const std::string& status) {
    Publish([&](Snapshot& s) {
        s.online = false; s.signed_in = false; s.needs_account = false; s.account.clear(); s.player.clear(); s.ticket.clear();
        s.mine.clear(); s.games.clear(); s.chat.clear(); s.status = status;
    });
}

// "2026-10-25T12:34:56.789+00:00" (the server's UTC timestamps) as Unix seconds; 0 if absent.
long long UnixSeconds(const std::string& iso) {
    SYSTEMTIME t{};
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf_s(iso.c_str(), "%4u-%2u-%2uT%2u:%2u:%2u", &year, &month, &day, &hour, &minute, &second) != 6) return 0;
    t.wYear = static_cast<WORD>(year); t.wMonth = static_cast<WORD>(month); t.wDay = static_cast<WORD>(day);
    t.wHour = static_cast<WORD>(hour); t.wMinute = static_cast<WORD>(minute); t.wSecond = static_cast<WORD>(second);
    FILETIME f{}; if (!SystemTimeToFileTime(&t, &f)) return 0;
    const auto ticks = (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    return static_cast<long long>(ticks / 10000000ULL) - 11644473600LL;
}

void ApplyState(const hp_json::Value& v) {
    Publish([&](Snapshot& s) {
        s.online = true;
        s.player = GameText(v["player"].str());
        s.status = "Connected to " + settings.server + " as {" + s.player + "}";
        s.default_turn_seconds = static_cast<int>(v["defaultTurnSeconds"].integer());
        s.mine.clear();
        for (const auto& g : v["mine"].items) {
            // HotA names a session "<game>\xFA<host>"; the open-games list already has them apart.
            MyGame m; m.id = g["id"].str(); m.name = BytesText(g["name"].str()); m.seat = BytesText(g["seat"].str());
            m.name = m.name.substr(0, m.name.find('\xFA'));
            m.state = g["state"].str(); m.date = g["captureDate"].str(); m.started = g["started"].truthy();
            m.turn_seconds = static_cast<int>(g["turnSeconds"].integer());
            for (const auto& p : g["seats"].items) {
                Seat seat; seat.name = BytesText(p["name"].str()); seat.state = p["state"].str();
                seat.away_since = UnixSeconds(p["since"].str());
                m.seats.push_back(seat);
            }
            s.mine.push_back(m);
        }
        s.games.clear();
        for (const auto& g : v["games"].items) {
            OpenGame o; o.id = g["id"].str(); o.name = BytesText(g["name"].str()); o.host = BytesText(g["host"].str());
            o.started = g["started"].truthy(); o.password = g["password"].truthy();
            o.players = static_cast<int>(g["players"].integer()); o.max_players = static_cast<int>(g["maxPlayers"].integer());
            s.games.push_back(o);
        }
    });
}
// The first batch after watching is the recent history and replaces what we had.
void ApplyChat(const hp_json::Value& v, bool replace) {
    Publish([&](Snapshot& s) {
        if (replace) s.chat.clear();
        for (const auto& c : v.items) {
            ChatLine line; line.id = c["id"].integer(); line.name = GameText(c["name"].str()); line.text = GameText(c["text"].str());
            s.chat.push_back(line);
        }
        if (s.chat.size() > 100) s.chat.erase(s.chat.begin(), s.chat.end() - 100);
    });
}

enum class Ended { Retry, Now, Wait };

// One connection: sign in, watch the lobby, apply what the server pushes until it ends.
Ended Session() {
    if (settings.server.empty()) {
        Offline("No server set: press Server to choose the one you play on.");
        return Ended::Wait;
    }
    const unsigned signedAs = identity.load();
    if (settings.token.empty()) { SignedOut("Sign in to " + settings.server + " to play."); return Ended::Wait; }
    hp_json::Value v;
    const auto account = Request(settings.server, "GET", "/api/account", "", settings.token);
    if (account.status == 401) {
        // The server no longer knows this sign-in: signed out elsewhere, or a new database.
        if (identity.load() == signedAs) StoreToken(settings.server, "");
        SignedOut("Your sign-in to " + settings.server + " has ended. Sign in again.");
        return Ended::Wait;
    }
    if (account.status != 200 || !hp_json::Parse(account.body, v)) {
        Offline("Cannot reach " + settings.server + ": " + GameText(Problem(account)));
        return Ended::Retry;
    }
    Publish([&](Snapshot& s) {
        s.signed_in = true; s.needs_account = v["name"].str().empty();
        s.account = GameText(v["name"].str()); s.player = GameText(v["player"]["displayName"].str());
    });
    const auto info = Request(settings.server, "GET", "/api/relay", "");
    if (info.status != 200 || !hp_json::Parse(info.body, v) || v["port"].integer() <= 0) {
        Offline("Cannot reach " + settings.server + ": " + GameText(info.status == 200 ? "the server has no game relay" : Problem(info)));
        return Ended::Retry;
    }
    Url url; Crack(settings.server, url);
    const auto host = Narrow(url.host, CP_UTF8); const int port = static_cast<int>(v["port"].integer());
    std::string error;
    const SOCKET s = Dial(host, port, error);
    if (s == INVALID_SOCKET) { Offline("Cannot reach " + settings.server + ": " + error); return Ended::Retry; }
    { std::lock_guard<std::mutex> lock(sendMutex); relay = s; }
    // Signed in or out while this connection was being made: start over as the new player.
    if (identity.load() != signedAs) { std::lock_guard<std::mutex> lock(sendMutex); closesocket(relay); relay = INVALID_SOCKET; return Ended::Now; }
    std::string player; { std::lock_guard<std::mutex> lock(mutex); player = name; }
    Frame hello(C_HELLO); hello.u32(ProtocolVersion).blob(settings.token).blob(player);
    Frame watch(C_LOBBY_WATCH);
    Publish([&](Snapshot& snap) { snap.server = settings.server; snap.relay_host = host; snap.relay_port = port; snap.ticket = settings.token; });

    Ended ended = Ended::Retry; bool firstChat = true, refused = false;
    if (Post(hello) && Post(watch)) {
        std::vector<std::uint8_t> frame;
        for (;;) {
            std::uint8_t header[4];
            if (!ReadExact(s, header, 4)) break;
            const std::uint32_t length = header[0] | header[1] << 8 | header[2] << 16 | static_cast<std::uint32_t>(header[3]) << 24;
            if (length < 2 || length > MaxFrame) break;
            frame.resize(length);
            if (!ReadExact(s, frame.data(), length)) break;
            const std::uint16_t type = static_cast<std::uint16_t>(frame[0] | frame[1] << 8);
            Reader r{frame, 2};
            if (type == S_LOBBY_STATE || type == S_LOBBY_CHAT) {
                hp_json::Value json;
                if (!hp_json::Parse(r.blob(), json)) continue;
                if (type == S_LOBBY_STATE) ApplyState(json);
                else { ApplyChat(json, firstChat); firstChat = false; }
            } else if (type == S_LOBBY_NOTICE) {
                const auto code = r.u32(); const auto text = GameText(r.blob());
                if (code == NOTICE_SIGN_IN) {
                    // The relay refused this sign-in: the account check of the next try signs out if it is gone.
                    refused = true;
                    break;
                }
                Publish([&](Snapshot& snap) { snap.status = text; });
            }
        }
    }
    { std::lock_guard<std::mutex> lock(sendMutex); closesocket(relay); relay = INVALID_SOCKET; }
    if (identity.load() != signedAs) return Ended::Now;
    if (refused) { Offline("The server refused this sign-in. Trying again..."); return Ended::Retry; }
    if (ended == Ended::Retry) Offline("Lost the connection to " + settings.server + ". Reconnecting...");
    return ended;
}

void Kick() {
    { std::lock_guard<std::mutex> lock(mutex); wanted = true; kick = true; }
    wake.notify_all();
}

void Worker() {
    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa);
    int delay = 0;
    for (;;) {
        {
            // Wait until the lobby has been opened once, then for the retry delay or a kick.
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [] { return wanted; });
            if (delay) wake.wait_for(lock, std::chrono::seconds(delay), [] { return kick; });
            kick = false;
        }
        const auto fresh = ReadSettings();
        if (fresh.server != settings.server) Publish([](Snapshot& s) { s.chat.clear(); s.mine.clear(); s.games.clear(); });
        settings = fresh;
        const auto began = std::chrono::steady_clock::now();
        const Ended ended = Session();
        if (ended == Ended::Wait) {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [] { return kick; });
            delay = 0;
        } else if (ended == Ended::Now) delay = 0;
        // A connection that lasted a while starts the back-off over.
        else delay = std::chrono::steady_clock::now() - began > std::chrono::minutes(1) ? 2 : (delay ? (delay * 2 > 30 ? 30 : delay * 2) : 2);
    }
}
}

std::string GameText(const std::string& utf8) { return Narrow(Wide(utf8, CP_UTF8), CP_ACP); }
std::string Utf8Text(const std::string& game) { return Narrow(Wide(game, CP_ACP), CP_UTF8); }
std::string BytesText(const std::string& utf8) {
    std::string out;
    for (const wchar_t c : Wide(utf8, CP_UTF8)) out += c < 256 ? static_cast<char>(c) : '?';
    return out;
}

void Start(const std::wstring& folder, HWND window) {
    std::lock_guard<std::mutex> lock(mutex);
    if (started) return;
    started = true; notifyWindow = window; iniPath = folder + L"\\HeroesPost.ini";
    WriteTemplate();
    state.server = ReadSettings().server;
    std::thread(Worker).detach();
}
void Open(const std::string& player_name) {
    const auto server = ReadSettings().server;
    bool redial = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true; name = player_name;
        // The player may have edited HeroesPost.ini since: a new server means a new connection.
        redial = wanted && server != state.server;
        if (!wanted || redial || !state.online) { wanted = true; kick = true; }
        if (!state.online) state.status = "Connecting...";
        state.server = server;
        // Known at once, so the lobby can ask for a sign-in before any connection.
        const auto stored = StoredFor(server);
        if (!state.online) state.signed_in = !server.empty() && !stored.token.empty();
        state.remembered_account = GameText(stored.account);
        ++state.version;
    }
    if (redial) Drop();
    wake.notify_all();
}
void Close() { std::lock_guard<std::mutex> lock(mutex); open = false; }
void Connect(const std::string& player_name) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!started || wanted) return;
        if (name.empty()) name = player_name;
        wanted = true; kick = true;
    }
    wake.notify_all();
}
void SetStatus(const std::string& text) { Publish([&](Snapshot& s) { s.status = text; }); }

Reply Call(const char* method, const std::string& path, const std::string& body, const wchar_t* type, size_t limit, int timeout_seconds) {
    const auto ini = ReadSettings();
    if (ini.server.empty() || ini.token.empty()) { Reply r; r.error = "not signed in to the server"; return r; }
    return Request(ini.server, method, path, body, ini.token, type, limit, timeout_seconds);
}

void SignIn(const std::string& account, const std::string& password, bool create) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!started || state.signing_in) return;
        state.signing_in = true; state.sign_in_error.clear(); ++state.version;
    }
    Notify();
    std::thread([account, password, create] {
        const auto ini = ReadSettings();
        std::string error;
        if (ini.server.empty()) error = "No server set: press Server to choose the one you play on.";
        else {
            // Registering as a player from before accounts keeps that player, and with it their games.
            bool claim = false; { std::lock_guard<std::mutex> lock(mutex); claim = create && state.needs_account; }
            const auto body = "{\"name\":" + JsonString(Utf8Text(account)) + ",\"password\":" + JsonString(Utf8Text(password)) + "}";
            const auto r = Request(ini.server, "POST", create ? "/api/account" : "/api/account/sign-in", body, claim ? ini.token : std::string());
            hp_json::Value v;
            if (r.status == 200 && hp_json::Parse(r.body, v) && !v["token"].str().empty()) {
                StoreToken(ini.server, v["token"].str(), v["player"]["displayName"].str());
                ++identity;
                // Published before the old connection is dropped: what the new one reports comes
                // after it and is not overwritten.
                Publish([](Snapshot& s) {
                    s.signing_in = false; s.sign_in_error.clear();
                    s.signed_in = true; s.needs_account = false; s.online = false; s.status = "Signed in. Connecting...";
                    s.mine.clear(); s.games.clear(); s.chat.clear();
                });
                Drop(); Kick();
                return;
            }
            error = GameText(Problem(r));
        }
        Publish([&](Snapshot& s) { s.signing_in = false; s.sign_in_error = error; });
    }).detach();
}
void SignOut() {
    const auto ini = ReadSettings();
    if (ini.server.empty()) return;
    StoreToken(ini.server, "");
    ++identity;
    Drop();
    SignedOut("Signed out of " + ini.server + ".");
    // The server forgets the token too; if that fails, an unused token stays behind on it.
    if (!ini.token.empty()) std::thread([ini] { Request(ini.server, "POST", "/api/account/sign-out", "", ini.token); }).detach();
    Kick();
}
std::string NormalServer(const std::string& text) {
    const auto s = Unslashed(Trim(text));
    std::string lower = s;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) return "";
    if (s.find_first_of(" \t\"") != std::string::npos) return "";
    Url url;
    return Crack(s, url) && !url.host.empty() ? s : "";
}
void SetServer(const std::string& server) {
    // HeroesPost.ini keeps its other lines.
    std::ifstream in(iniPath); std::string line; std::ostringstream out; bool written = false;
    while (std::getline(in, line)) {
        line = Trim(line);
        const auto eq = line.find('=');
        const bool setting = !line.empty() && line[0] != ';' && line[0] != '#' && eq != std::string::npos;
        if (setting && Trim(line.substr(0, eq)) == "server") { if (!written) out << "server=" << server << "\r\n"; written = true; }
        else out << line << "\r\n";
    }
    in.close();
    if (!written) out << "server=" << server << "\r\n";
    std::ofstream(iniPath, std::ios::binary | std::ios::trunc) << out.str();
    // Each server has its own sign-in: the lobby starts over with this server's.
    const auto stored = StoredFor(server);
    ++identity;
    Publish([&](Snapshot& s) {
        s.server = server; s.online = false; s.signed_in = !stored.token.empty(); s.needs_account = false;
        s.account.clear(); s.player.clear(); s.ticket.clear(); s.remembered_account = GameText(stored.account);
        s.mine.clear(); s.games.clear(); s.chat.clear(); s.status = "Connecting to " + server + "...";
    });
    Drop();
    Kick();
}
std::string ProblemText(const Reply& reply) { return GameText(Problem(reply)); }
Snapshot Get() { std::lock_guard<std::mutex> lock(mutex); return state; }

void SendChat(const std::string& text) {
    Frame frame(C_LOBBY_CHAT); frame.blob(Utf8Text(text));
    if (!Post(frame)) Publish([](Snapshot& s) { s.status = "Message not sent: not connected to the server."; });
}
void SetTurnLimit(const std::string& game_id, int seconds) {
    // Relay game ids are .NET Guids; IIDFromString gives the same byte order as Guid.ToByteArray.
    GUID id{};
    if (FAILED(IIDFromString(Wide("{" + game_id + "}", CP_UTF8).c_str(), &id))) return;
    Frame frame(C_LOBBY_TURN_LIMIT); frame.bytes(&id, sizeof id).u32(static_cast<std::uint32_t>(seconds));
    if (!Post(frame)) Publish([](Snapshot& s) { s.status = "Turn limit not changed: not connected to the server."; });
}
}
