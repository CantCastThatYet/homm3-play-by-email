#include "seat_keeper.hpp"
#include "common.hpp"
#include "dplay_relay.hpp"
#include "json_lite.hpp"
#include "lobby_client.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace hp_keeper {
namespace {
Command command;
std::wstring folder;
std::mutex mutex;
std::deque<std::string> notices;   // guarded by mutex
std::atomic<bool> continuing{false}, ready{false};

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

hp_json::Value Run(const char* name) {
    hp_json::Value v;
    try { hp_json::Parse(command(std::string("{\"command\":\"") + name + "\"}"), v); } catch (...) {}
    return v;
}
bool Ok(const char* name) { return Run(name)["ok"].truthy(); }

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
    const auto temporary = path + L".pending";
    {
        hp::Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) return false;
        DWORD wrote = 0;
        if (size && (!WriteFile(file.value, data, static_cast<DWORD>(size), &wrote, nullptr) || wrote != size)) return false;
        FlushFileBuffers(file.value);
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// ------------------------------------------------------------------- zip
std::uint32_t Crc32(const unsigned char* p, size_t n) {
    static std::uint32_t table[256];
    static const bool built = [] {
        for (std::uint32_t i = 0; i < 256; ++i) { std::uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
        return true;
    }();
    (void)built;
    std::uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
void Put16(std::string& out, unsigned v) { out += static_cast<char>(v & 0xFF); out += static_cast<char>((v >> 8) & 0xFF); }
void Put32(std::string& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF); }

// ----------------------------------------------------------- seat saving
// Mirrors the desktop app's seat keeper: End Turn is captured by the game itself at its completion
// marker (armed in advance); a capture of the turn in progress is taken once each new day and,
// at an idle moment, a little after the player has acted.
class Saver {
public:
    void Step(const hp_json::Value& p) {
        const auto now = GetTickCount64();
        const auto& relay = p["relay"]; const auto& game = p["game"];
        const std::string instance = relay["instance"].str();
        if (!relay["active"].truthy() || !relay["sessionOpen"].truthy() || instance.empty() ||
            !game["adventureMapPresent"].truthy() || game["network"].integer() != 1) { sequentialSince = 0; return; }
        // HD switches simultaneous turns on a moment after the map appears.
        if (!game["simultaneousTurns"].truthy()) {
            if (!sequentialSince) sequentialSince = now;
            if (!warnedSequential && now - sequentialSince > 15000) {
                Notice("Simultaneous turns are off in this game: your seat can no longer be saved for leaving and returning.");
                warnedSequential = true;
            }
            return;
        }
        sequentialSince = 0;
        const std::string status = p["captureStatus"].str(), manifest = p["lastCaptureManifest"].str();
        if (status == "written" && !manifest.empty() && manifest != lastManifest) {
            lastManifest = manifest; capturing = false;
            Upload(instance, manifest);
        }
        if (status == "failed" && capturing) {
            capturing = false;
            if (now - lastFailureNotice > 300000) { Notice("Your seat could not be saved just now: " + p["captureError"].str()); lastFailureNotice = now; }
        }
        const auto& date = game["date"];
        const long long day = date["day"].integer() | date["week"].integer() << 8 | date["month"].integer() << 16;
        const auto local = game["localColor"].integer();
        const auto& phase = p["nativePhase"];
        const bool done = local >= 0 && local < 8 && phase["participants"][static_cast<size_t>(local)][1].integer() == 1;
        const bool idle = phase["modalDepth"].integer() == 0 && !game["combatActive"].truthy();
        const bool writing = status == "copying" || status == "writing";
        const bool busy = capturing || status == "armed" || writing;
        if (!done && !busy && !p["completionArmed"].truthy()) Ok("experiment_snapshot_next_completion");
        const long long sent = relay["sent"].integer();
        if (sent != lastSent) { lastSent = sent; lastSentChange = now; }
        // After acting: wait for a pause of two seconds, and at most one capture every ten.
        const bool acted = sent != sentAtCapture && now - lastCapture >= 10000 && now - lastSentChange >= 2000;
        if (!done && idle && !capturing && !writing && (capturedDay != day || acted) && Ok("capture_current_branch")) {
            capturing = true; capturedDay = day; sentAtCapture = sent; lastCapture = now;
        }
    }

private:
    std::string lastManifest;
    long long capturedDay = -1, sentAtCapture = -1, lastSent = -1;
    ULONGLONG lastCapture = 0, lastSentChange = 0, sequentialSince = 0, lastFailureNotice = 0;
    bool capturing = false, warnedSequential = false;

    void Upload(const std::string& instance, const std::string& manifest) {
        const auto path = Wide(manifest, CP_UTF8);
        const auto directory = hp::Parent(path);
        std::string zip, error;
        if (!ZipDirectory(directory, zip, error)) { Notice("Your seat could not be packed for the server: " + error); return; }
        bool completion = false;
        {
            std::ifstream in(path, std::ios::binary); std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            hp_json::Value m; completion = hp_json::Parse(text, m) && m["native_completion"]["observed"].truthy();
        }
        const auto reply = hp_lobby_client::Call("POST", "/api/relay/games/" + instance + "/captures", zip, L"application/octet-stream", 64 * 1024, 120);
        if (reply.status != 200) { Notice("Your seat could not be saved on the server: " + hp_lobby_client::ProblemText(reply)); return; }
        Log("Saved " + std::string(completion ? "End Turn" : "current") + " capture (" + std::to_string(zip.size()) + " bytes) of game " + instance);
        if (completion) Notice("Turn ended and saved on the server. You may close HotA: your seat is kept.");
        Prune(directory);
    }

    // Older captures of this process are on the server now; keep the folder small.
    static void Prune(const std::wstring& kept) {
        const auto parent = hp::Parent(kept);
        const auto name = kept.substr(parent.size() + 1);
        const auto keptNumber = _wtoi(name.c_str());
        WIN32_FIND_DATAW found{};
        const HANDLE search = FindFirstFileW((parent + L"\\*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> old;
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
            const int number = _wtoi(found.cFileName);
            if (number > 0 && number < keptNumber) old.push_back(parent + L"\\" + found.cFileName);
        } while (FindNextFileW(search, &found));
        FindClose(search);
        for (const auto& directory : old) {
            WIN32_FIND_DATAW file{};
            const HANDLE files = FindFirstFileW((directory + L"\\*").c_str(), &file);
            if (files != INVALID_HANDLE_VALUE) {
                do { if (!(file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((directory + L"\\" + file.cFileName).c_str()); }
                while (FindNextFileW(files, &file));
                FindClose(files);
            }
            RemoveDirectoryW(directory.c_str());
        }
    }
};

// ----------------------------------------------------------- the game's players
// Turns the server's seat states and the relay connection into notices for the adventure screen.
class Watcher {
public:
    void Step(const hp_json::Value& p) {
        const auto& relay = p["relay"];
        const std::string instance = relay["instance"].str();
        if (!relay["active"].truthy() || !relay["sessionOpen"].truthy() || instance.empty()) { states.clear(); reconnecting = false; return; }
        hp_lobby_client::Connect("Player");
        const bool now = hp_relay::Reconnecting();
        if (now != reconnecting) {
            reconnecting = now;
            Notice(now ? "Lost the connection to the server. Keep playing: your moves are sent when it is back."
                       : "Connected to the server again.");
        }
        const auto snapshot = hp_lobby_client::Get();
        if (!snapshot.online) return;
        for (const auto& game : snapshot.mine) {
            if (_stricmp(game.id.c_str(), instance.c_str()) != 0 || !game.started) continue;
            for (const auto& seat : game.seats) {
                if (seat.name == game.seat) continue;
                auto& known = states[seat.name];
                if (!known.empty() && known != seat.state) {
                    if (seat.state == "Online") Notice("{" + seat.name + "} is back in the game.");
                    else if (seat.state == "Away") Notice("{" + seat.name + "} left the game. Their seat is kept until they return.");
                    else if (seat.state == "StandIn") Notice("The server is playing {" + seat.name + "}'s turn.");
                }
                known = seat.state;
            }
        }
    }

private:
    std::map<std::string, std::string> states;
    bool reconnecting = false;
};

// ------------------------------------------------------------ Continue
std::uint32_t U32(const std::string& b, size_t& at, bool& ok) {
    if (!ok || b.size() - at < 4 || at > b.size()) { ok = false; return 0; }
    std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + i])) << (8 * i);
    at += 4; return v;
}
std::string Blob(const std::string& b, size_t& at, bool& ok) {
    const auto n = U32(b, at, ok);
    if (!ok || b.size() - at < n) { ok = false; return {}; }
    std::string v = b.substr(at, n); at += n; return v;
}
bool Plain(const std::string& value) { return value.find_first_of("\r\n") == std::string::npos; }

void Prepare(const std::string& id) {
    const auto fail = [](const std::string& text) { hp_lobby_client::SetStatus(text); continuing = false; };
    const auto snapshot = hp_lobby_client::Get();
    std::string gameName;
    for (const auto& g : snapshot.mine) if (g.id == id) gameName = g.name;
    if (!snapshot.online || snapshot.relay_host.empty()) { fail("Cannot continue: not connected to the server."); return; }
    // Claiming may wait for the server's stand-in to save the seat first.
    const auto reply = hp_lobby_client::Call("POST", "/api/relay/games/" + id + "/resume", "", L"application/json", 256u * 1024 * 1024, 180);
    if (reply.status != 200) { fail("Cannot continue: " + hp_lobby_client::ProblemText(reply)); return; }
    // ResumePackage.cs: "HPK1", color, session, seat, from, sent, map, resume image, deferred, deferred count.
    const auto& b = reply.body; size_t at = 0; bool ok = true;
    const auto magic = U32(b, at, ok), color = U32(b, at, ok);
    const auto session = Blob(b, at, ok), seat = Blob(b, at, ok);
    const auto from = U32(b, at, ok), sent = U32(b, at, ok);
    const auto map = hp_lobby_client::GameText(Blob(b, at, ok));
    const auto image = Blob(b, at, ok), deferred = Blob(b, at, ok);
    const auto deferredCount = U32(b, at, ok);
    const auto seatId = U32(b, at, ok);
    if (!ok || magic != 0x314B5048 || color > 7 || image.empty() || !Plain(session) || !Plain(seat) || map.find_first_of("\\/:\r\n") != std::string::npos) {
        fail("Cannot continue: the server sent an unreadable seat."); return;
    }
    const auto mapPath = folder + L"\\Maps\\" + Wide(map, CP_ACP);
    if (GetFileAttributesW(mapPath.c_str()) == INVALID_FILE_ATTRIBUTES) { fail("Cannot continue: this game is played on the map " + map + ", which is not in your Maps folder."); return; }
    if (!WriteFileBytes(folder + L"\\HeroesPost.resume.bin", image.data(), image.size())) { fail("Cannot continue: HeroesPost.resume.bin could not be written."); return; }
    const auto deferredPath = folder + L"\\HeroesPost.deferred.bin";
    DeleteFileW(deferredPath.c_str());
    if (deferredCount && !WriteFileBytes(deferredPath, deferred.data(), deferred.size())) { fail("Cannot continue: HeroesPost.deferred.bin could not be written."); return; }
    // The restarted HotA reads this as its relay configuration (hp_relay::ReadConfig) plus map, color and game.
    std::string ini = "host=" + snapshot.relay_host + "\r\nport=" + std::to_string(snapshot.relay_port) + "\r\nticket=" + snapshot.ticket +
        "\r\nname=" + snapshot.player + "\r\nrejoin=" + session + "\r\nseat=" + seat + "\r\nfrom=" + std::to_string(from) + "\r\n";
    char seatText[16]; std::snprintf(seatText, sizeof seatText, "%08x", seatId);
    ini += std::string("seatid=") + seatText + "\r\n";
    if (sent != 0xFFFFFFFFu) ini += "sent=" + std::to_string(sent) + "\r\n";
    if (deferredCount) ini += "deferred=HeroesPost.deferred.bin\r\n";
    ini += "map=" + map + "\r\ncolor=" + std::to_string(color) + "\r\ngame=" + gameName + "\r\n";
    if (!Plain(snapshot.ticket) || !Plain(snapshot.player) || !WriteFileBytes(folder + L"\\HeroesPost.continue.ini", ini.data(), ini.size())) {
        fail("Cannot continue: HeroesPost.continue.ini could not be written."); return;
    }
    hp_lobby_client::SetStatus("Restarting HotA into " + gameName + "...");
    ready = true;
}

void Fail(const std::string& text) {
    Log("Continue failed: " + text);
    hp_relay::Deactivate();
    MessageBoxW(nullptr, Wide("Heroes Post could not return you to your game.\n\n" + text, CP_ACP).c_str(), L"Heroes Post", MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

// In the HotA that Restart started: resume the prepared seat once the main menu is up, as the
// desktop app's NativeHost resume does through the same adapter commands.
void ResumeAfterRestart() {
    const auto path = folder + L"\\HeroesPost.continue.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    hp_relay::Config config; std::string error;
    const bool read = hp_relay::ReadConfig(path, config, &error);
    std::string map, game; int color = -1;
    {
        std::ifstream in(path, std::ios::binary); std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("map=", 0) == 0) map = line.substr(4);
            else if (line.rfind("color=", 0) == 0) color = std::atoi(line.c_str() + 6);
            else if (line.rfind("game=", 0) == 0) game = line.substr(5);
        }
    }
    // One attempt per Continue: a failure must not trap HotA in a loop of resumes.
    MoveFileExW(path.c_str(), (path + L".done").c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!read || config.rejoin_seat.empty() || map.empty() || color < 0 || color > 7) { Fail("The prepared seat is incomplete (" + error + ")."); return; }
    config.log_path = folder + L"\\HeroesPost.relay.log";

    const auto deadline = [](ULONGLONG ms) { return GetTickCount64() + ms; };
    auto until = deadline(60000);
    while (!(Run("probe")["nativeMenuReady"].truthy())) {
        if (GetTickCount64() > until) { Fail("HotA's main menu did not appear."); return; }
        Sleep(250);
    }
    hp_relay::Activate(config);
    const auto accepted = Run("resume_session");
    if (!accepted["ok"].truthy()) { Fail("HotA refused to resume: " + accepted["error"]["message"].str()); return; }
    // The adapter starts a one-player scaffold on the game's map and loads the seat into it.
    // HotA remembers the scaffold's player name as its multiplayer name: give it the seat's own.
    std::string player = config.rejoin_seat;
    const bool plain = !player.empty() && player.size() <= 20 && player.front() != ' ' && player.back() != ' ' &&
        std::all_of(player.begin(), player.end(), [](unsigned char c) { return c >= 32 && c <= 126 && c != '=' && c != ';' && c != '#' && c != '"'; });
    if (!plain) player = "HeroesPost";
    const std::wstring session = L"[Session]\r\nVersion=1\r\nAction=SoloScaffold\r\nPlayerName=" + Wide(player, CP_ACP) + L"\r\nSessionName=HeroesPostResume\r\n"
        L"Address=127.0.0.1\r\nMapFile=" + Wide(map, CP_ACP) + L"\r\nPort=26777\r\n";
    std::wstring bom = L"\xFEFF" + session;
    if (!WriteFileBytes(folder + L"\\HeroesPost.session.ini", bom.data(), bom.size() * sizeof(wchar_t))) { Fail("HeroesPost.session.ini could not be written."); return; }
    until = deadline(30000);
    while (!Ok("bootstrap_session")) {
        if (GetTickCount64() > until) { Fail("HotA could not start the game from its main menu."); return; }
        Sleep(500);
    }
    until = deadline(120000);
    bool introQueued = false;
    ULONGLONG verifiedSince = 0, lastEnter = 0;
    while (GetTickCount64() < until) {
        Sleep(200);
        const auto state = Run("probe");
        if (state["sessionBootstrap"]["status"].str() == "failed") { Fail("Starting the game failed: " + state["sessionBootstrap"]["error"].str()); return; }
        const auto start = state["nativeStart"]["status"].str();
        if (start == "refused" || start == "cancelled") { Fail("Starting the game failed: " + state["nativeStart"]["detail"].str()); return; }
        if (!introQueued) introQueued = Ok("experiment_acknowledge_known_intro");
        // Other maps' intro messages and day-one events: the scaffold's world is about to be
        // replaced, so its dialogs are accepted as the player would.
        if (!introQueued && state["nativePhase"]["modalDepth"].integer() > 0 && state["game"]["adventureMapPresent"].truthy() &&
            GetTickCount64() - lastEnter > 1500) {
            HWND window = nullptr;
            if (hp::Read(0x699650, window) && window) {
                PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0x001C0001);
                PostMessageW(window, WM_KEYUP, VK_RETURN, static_cast<LPARAM>(0xC01C0001u));
            }
            lastEnter = GetTickCount64();
        }
        const auto& resume = state["soloResume"];
        if (resume.kind != hp_json::Value::Object) continue;
        if (!resume["error"].str().empty()) { Fail("Loading your seat failed: " + resume["error"].str()); return; }
        if (resume["transportFault"].truthy() || !resume["journalError"].str().empty()) { Fail("The game connection failed: " + resume["transportError"].str() + resume["journalError"].str()); return; }
        const auto& g = state["game"];
        const bool valid = resume["verified"].truthy() && (resume["playable"].truthy() || resume["completedInputHeld"].truthy()) &&
            g["adventureMapPresent"].truthy() && !g["scenarioOpen"].truthy() && g["network"].integer() == 1 &&
            g["simultaneousTurns"].truthy() && g["localColor"].integer() == color;
        if (!valid) { verifiedSince = 0; continue; }
        if (!verifiedSince) verifiedSince = GetTickCount64();
        if (GetTickCount64() - verifiedSince >= 1000) {
            Notice("Welcome back" + (game.empty() ? std::string() : " to {" + game + "}") + ". Press F7 to see who is in the game.");
            return;
        }
    }
    Fail("Your seat did not finish loading in time.");
}

void Loop() {
    // The hooks are in once telemetry is active.
    for (int i = 0; i < 2400 && Run("probe")["telemetry"]["status"].str() != "active"; ++i) Sleep(250);
    ResumeAfterRestart();
    Saver saver; Watcher watcher;
    for (;;) {
        Sleep(1000);
        const auto probe = Run("probe");
        if (!probe["ok"].truthy()) continue;
        try { saver.Step(probe); watcher.Step(probe); } catch (...) {}
    }
}
}

void Start(Command handler, const std::wstring& game_folder) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    command = std::move(handler); folder = game_folder;
    std::thread(Loop).detach();
}

void Continue(const std::string& game_id) {
    if (continuing.exchange(true)) return;
    ready = false;
    hp_lobby_client::SetStatus("Continuing: asking the server for your seat...");
    std::thread([game_id] { try { Prepare(game_id); } catch (...) { hp_lobby_client::SetStatus("Cannot continue: an unexpected error."); continuing = false; } }).detach();
}
bool Continuing() noexcept { return continuing.load(); }
bool ReadyToRestart() noexcept { return ready.load(); }

void Restart() {
    wchar_t exe[MAX_PATH]{}, directory[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetCurrentDirectoryW(MAX_PATH, directory);
    std::wstring line = GetCommandLineW();
    // The new process's version.dll waits for this one to be gone before the game starts.
    SetEnvironmentVariableW(L"HEROESPOST_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    STARTUPINFOW startup{}; startup.cb = sizeof startup; PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, directory, &startup, &process)) {
        SetEnvironmentVariableW(L"HEROESPOST_WAIT_PID", nullptr);
        ready = false; continuing = false;
        hp_lobby_client::SetStatus("Cannot continue: HotA could not be started again.");
        return;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    TerminateProcess(GetCurrentProcess(), 0);
}

void Log(const std::string& text) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    if (folder.empty()) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char stamp[32]; std::snprintf(stamp, sizeof stamp, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    std::ofstream(folder + L"\\HeroesPost.keeper.log", std::ios::app | std::ios::binary) << stamp << text << "\r\n";
}

void Notice(const std::string& text) {
    Log("Notice: " + text);
    std::lock_guard<std::mutex> lock(mutex);
    notices.push_back(text);
    while (notices.size() > 20) notices.pop_front();
}
std::vector<std::string> TakeNotices() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> out(notices.begin(), notices.end());
    notices.clear();
    return out;
}

bool ZipDirectory(const std::wstring& directory, std::string& zip, std::string& error) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) { error = "the capture folder is missing"; return false; }
    do { if (!(found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) names.push_back(found.cFileName); }
    while (FindNextFileW(search, &found));
    FindClose(search);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        if ((a == L"manifest.json") != (b == L"manifest.json")) return a == L"manifest.json";
        return a < b;
    });
    if (names.empty() || names[0] != L"manifest.json") { error = "the capture has no manifest"; return false; }
    zip.clear();
    std::string central;
    for (const auto& name : names) {
        std::ifstream in(directory + L"\\" + name, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof()) { error = "a capture file is unreadable"; return false; }
        const auto narrow = Narrow(name, CP_UTF8);
        const auto crc = Crc32(reinterpret_cast<const unsigned char*>(data.data()), data.size());
        const auto offset = static_cast<std::uint32_t>(zip.size());
        // Stored entries dated 1980-01-01, as the desktop app's bundles.
        Put32(zip, 0x04034b50); Put16(zip, 20); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0); Put16(zip, 0x21);
        Put32(zip, crc); Put32(zip, static_cast<std::uint32_t>(data.size())); Put32(zip, static_cast<std::uint32_t>(data.size()));
        Put16(zip, static_cast<unsigned>(narrow.size())); Put16(zip, 0); zip += narrow; zip += data;
        Put32(central, 0x02014b50); Put16(central, 20); Put16(central, 20); Put16(central, 0); Put16(central, 0); Put16(central, 0); Put16(central, 0x21);
        Put32(central, crc); Put32(central, static_cast<std::uint32_t>(data.size())); Put32(central, static_cast<std::uint32_t>(data.size()));
        Put16(central, static_cast<unsigned>(narrow.size())); Put16(central, 0); Put16(central, 0); Put16(central, 0); Put16(central, 0);
        Put32(central, 0); Put32(central, offset); central += narrow;
    }
    const auto start = static_cast<std::uint32_t>(zip.size());
    zip += central;
    Put32(zip, 0x06054b50); Put16(zip, 0); Put16(zip, 0); Put16(zip, static_cast<unsigned>(names.size())); Put16(zip, static_cast<unsigned>(names.size()));
    Put32(zip, static_cast<std::uint32_t>(central.size())); Put32(zip, start); Put16(zip, 0);
    return true;
}
}
