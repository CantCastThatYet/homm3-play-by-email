#include "lobby_client.hpp"
#include "json_lite.hpp"
#include <winhttp.h>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <cstdio>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#pragma comment(lib, "winhttp.lib")

namespace hp_lobby_client {
namespace {
std::mutex mutex;
std::condition_variable wake;
Snapshot state;                 // guarded by mutex
bool started = false, open = false;
std::string name;               // the name to register with, game charset
std::deque<std::function<void()>> actions;
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
struct Settings { std::string server, token, token_server; };
std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
Settings ReadSettings() {
    Settings s; std::ifstream in(iniPath); std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        const auto eq = line.find('='); if (eq == std::string::npos) continue;
        const auto key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "server") s.server = value;
        else if (key == "token") s.token = value;
        else if (key == "token_server") s.token_server = value;
    }
    while (!s.server.empty() && s.server.back() == '/') s.server.pop_back();
    return s;
}
// Rewrites token= and token_server=, keeping every other line as the player wrote it.
void SaveIdentity(const std::string& token, const std::string& server) {
    std::ifstream in(iniPath); std::string line; std::ostringstream out;
    while (std::getline(in, line)) {
        const auto key = Trim(line.substr(0, line.find('=')));
        if (key == "token" || key == "token_server") continue;
        out << Trim(line) << "\r\n";
    }
    in.close();
    out << "token=" << token << "\r\ntoken_server=" << server << "\r\n";
    std::ofstream(iniPath, std::ios::binary | std::ios::trunc) << out.str();
}
void WriteTemplate() {
    if (GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::ofstream(iniPath, std::ios::binary) << "; Heroes Post: the server this HotA plays Heroes Post games on.\r\n"
        "; Example: server=http://heroes.example.org:8080\r\nserver=\r\n";
}

// ---------------------------------------------------------------------- http
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
struct Response { int status = 0; std::string body, error; };
Response Request(const std::string& server, const char* method, const std::string& path, const std::string& token, const std::string& body) {
    Response r; Url url;
    if (!Crack(server, url)) { r.error = "the server address is not a valid http:// URL"; return r; }
    HINTERNET session = WinHttpOpen(L"HeroesPost/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { r.error = "network unavailable"; return r; }
    WinHttpSetTimeouts(session, 4000, 4000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, Wide(method, CP_UTF8).c_str(), Wide(path, CP_UTF8).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + Wide(token, CP_UTF8) + L"\r\n";
    if (request && WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
        r.status = static_cast<int>(status);
        for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available && r.body.size() < 1024 * 1024;) {
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

// --------------------------------------------------------------- the worker
void Notify() {
    if (!notifyWindow) return;
    POINT p{}; GetCursorPos(&p); ScreenToClient(notifyWindow, &p);
    PostMessageW(notifyWindow, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
}
void Publish(const std::function<void(Snapshot&)>& change) {
    { std::lock_guard<std::mutex> lock(mutex); change(state); ++state.version; }
    Notify();
}

Settings settings;
long long lastChat = 0;
std::string lastBody;

bool EnsureIdentity() {
    if (!settings.token.empty() && settings.token_server == settings.server) return true;
    std::string player; { std::lock_guard<std::mutex> lock(mutex); player = name; }
    if (player.empty()) player = "Player";
    const auto r = Request(settings.server, "POST", "/api/players", "", "{\"displayName\":" + JsonString(Utf8Text(player)) + "}");
    hp_json::Value v;
    if (r.status != 200 || !hp_json::Parse(r.body, v) || v["token"].str().empty()) {
        const auto why = GameText(Problem(r));
        Publish([&](Snapshot& s) { s.online = false; s.status = "Cannot sign in to " + settings.server + ": " + why; });
        return false;
    }
    settings.token = v["token"].str(); settings.token_server = settings.server;
    SaveIdentity(settings.token, settings.server);
    return true;
}

void Poll() {
    if (settings.server.empty()) {
        Publish([](Snapshot& s) { s.online = false; s.status = "No server set: put server=http://host:port in HeroesPost.ini in the HotA folder."; });
        return;
    }
    if (!EnsureIdentity()) return;
    auto r = Request(settings.server, "GET", "/api/relay/lobby?after=" + std::to_string(lastChat), settings.token, "");
    if (r.status == 401) {
        // The server no longer knows this identity (a new database): make a new one once.
        settings.token.clear();
        if (!EnsureIdentity()) return;
        r = Request(settings.server, "GET", "/api/relay/lobby?after=" + std::to_string(lastChat), settings.token, "");
    }
    hp_json::Value v;
    if (r.status != 200 || !hp_json::Parse(r.body, v)) {
        const auto why = GameText(Problem(r));
        bool changed = false;
        { std::lock_guard<std::mutex> lock(mutex); changed = state.online || state.status.find(why) == std::string::npos; }
        if (changed) Publish([&](Snapshot& s) { s.online = false; s.status = "Cannot reach " + settings.server + ": " + why; });
        lastBody.clear();
        return;
    }
    if (r.body == lastBody) return;
    lastBody = r.body;
    Url url; Crack(settings.server, url);
    Publish([&](Snapshot& s) {
        s.online = true; s.server = settings.server; s.ticket = settings.token;
        s.player = GameText(v["player"].str());
        s.status = "Connected to " + settings.server + " as {" + s.player + "}";
        s.relay_host = Narrow(url.host, CP_UTF8); s.relay_port = static_cast<int>(v["relayPort"].integer());
        s.default_turn_seconds = static_cast<int>(v["defaultTurnSeconds"].integer());
        s.mine.clear();
        for (const auto& g : v["mine"].items) {
            // HotA names a session "<game>\xFA<host>"; the open-games list already has them apart.
            MyGame m; m.id = g["id"].str(); m.name = BytesText(g["name"].str()); m.seat = BytesText(g["seat"].str());
            m.name = m.name.substr(0, m.name.find('\xFA'));
            m.state = g["state"].str(); m.date = g["captureDate"].str(); m.started = g["started"].truthy();
            m.turn_seconds = static_cast<int>(g["turnSeconds"].integer());
            s.mine.push_back(m);
        }
        s.games.clear();
        for (const auto& g : v["games"].items) {
            OpenGame o; o.id = g["id"].str(); o.name = BytesText(g["name"].str()); o.host = BytesText(g["host"].str());
            o.started = g["started"].truthy(); o.password = g["password"].truthy();
            o.players = static_cast<int>(g["players"].integer()); o.max_players = static_cast<int>(g["maxPlayers"].integer());
            s.games.push_back(o);
        }
        for (const auto& c : v["chat"].items) {
            ChatLine line; line.id = c["id"].integer(); line.name = GameText(c["name"].str()); line.text = GameText(c["text"].str());
            if (line.id > lastChat) lastChat = line.id;
            s.chat.push_back(line);
        }
        if (s.chat.size() > 100) s.chat.erase(s.chat.begin(), s.chat.end() - 100);
    });
}

void Worker() {
    for (;;) {
        std::function<void()> action; bool polling = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, std::chrono::seconds(open ? 2 : 3600), [] { return !actions.empty(); });
            if (!actions.empty()) { action = std::move(actions.front()); actions.pop_front(); }
            polling = open;
        }
        if (polling) {
            const auto fresh = ReadSettings();
            if (fresh.server != settings.server) { lastBody.clear(); lastChat = 0; Publish([](Snapshot& s) { s.chat.clear(); }); }
            settings = fresh;
        }
        if (action) action();
        if (polling) Poll();
    }
}
void Enqueue(std::function<void()> action) {
    { std::lock_guard<std::mutex> lock(mutex); actions.push_back(std::move(action)); }
    wake.notify_one();
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
    settings = ReadSettings();
    state.server = settings.server;
    std::thread(Worker).detach();
}
void Open(const std::string& player_name) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true; name = player_name;
        if (!state.online) { state.status = "Connecting..."; ++state.version; }
    }
    // Publish the next reply even if it matches the last one: the window is new.
    Enqueue([] { lastBody.clear(); });
}
void Close() { std::lock_guard<std::mutex> lock(mutex); open = false; }
Snapshot Get() { std::lock_guard<std::mutex> lock(mutex); return state; }

void SendChat(const std::string& text) {
    const auto body = "{\"text\":" + JsonString(Utf8Text(text)) + "}";
    Enqueue([body] {
        if (!EnsureIdentity()) return;
        const auto r = Request(settings.server, "POST", "/api/relay/chat", settings.token, body);
        if (r.status != 200) { const auto why = GameText(Problem(r)); Publish([&](Snapshot& s) { s.status = "Message not sent: " + why; }); }
    });
}
void SetTurnLimit(const std::string& game_id, int seconds) {
    Enqueue([game_id, seconds] {
        if (!EnsureIdentity()) return;
        const auto r = Request(settings.server, "PUT", "/api/relay/games/" + game_id + "/turn-limit", settings.token,
            "{\"turnSeconds\":" + std::to_string(seconds) + "}");
        if (r.status != 200) { const auto why = GameText(Problem(r)); Publish([&](Snapshot& s) { s.status = "Turn limit not changed: " + why; }); }
        lastBody.clear();
    });
}
}
