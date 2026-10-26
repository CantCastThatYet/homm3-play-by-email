// HeroesPost-Setup.exe: installs the Heroes Post game patch into a HotA folder (version.dll and
// HeroesPost.Native.dll, carried as resources). It checks the HotA build first: the patch stays off
// on any other build, so installing it there would only add a button that never appears. The
// server is chosen in the game, the first time the player presses Heroes Post. Uninstall removes
// the patch; the account stays on the server.
//
// Command line, for scripts and for the elevated copy of itself:
//   /S  silent        /D=<HotA folder>      /uninstall
//   /lang=en|ru       /log=<file>  (the result, one line)
// Exit codes: 0 done, 1 failed, 2 not a supported HotA folder, 3 HotA is running.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <cwctype>
#include <string>
#include "supported_build.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
// Resources (installer.rc, written by package.ps1).
constexpr int ResLoader = 101, ResAdapter = 102, StrVersion = 2;
const wchar_t* const Loader = L"version.dll";
const wchar_t* const Adapter = L"HeroesPost.Native.dll";
const wchar_t* const SetupCopy = L"HeroesPost.Setup.exe";
const wchar_t* const Project = L"https://github.com/CantCastThatYet/homm3-play-by-email";

// ------------------------------------------------------------------------------------- text
enum Text {
    Title, Heading, Intro, FolderLabel, Browse, InstallButton, UpdateButton, UninstallButton, CloseButton,
    NoFolder, NoGame, Unsupported, UnsupportedTail, Foreign, Ready, Installed, Running, Done, Removed, Failed, Refused, PickFolder,
    RemoveQuestion, TextCount
};
const wchar_t* const English[TextCount] = {
    L"Heroes Post Setup",
    L"Heroes Post for HotA",
    L"Heroes Post adds a button to HotA's multiplayer screen: online games through a server, which you "
    L"can leave and return to at any time.",
    L"HotA folder:",
    L"Browse...",
    L"Install",
    L"Update",
    L"Uninstall",
    L"Close",
    L"Choose the folder HotA is installed in (the one with h3hota HD.exe).",
    L"There is no h3hota HD.exe in this folder.",
    L"This HotA or HD Mod version is not supported. Heroes Post works with the latest HotA 1.8.1 and HD Mod (",
    L"). Start HotA with its launcher and let it update HotA and HD Mod, then run this setup again. If both are up "
    L"to date, Heroes Post itself needs an update. Different files: ",
    L"This folder already has a version.dll from another mod. Heroes Post needs that file name, so it cannot "
    L"be installed here.",
    L"HotA 1.8.1 found. Heroes Post is not installed yet.",
    L"HotA 1.8.1 found. Heroes Post is installed.",
    L"HotA is running from this folder. Close it and try again.",
    L"Heroes Post is installed.\n\nStart HotA, open its multiplayer screen and press Heroes Post, below the "
    L"connection types. The first time, it asks for the address of your server; then sign in or register.",
    L"Heroes Post is removed from this HotA. Your account stays on the server.",
    L"Something went wrong: ",
    L"Windows did not allow the change: administrator rights were not granted.",
    L"Choose the HotA folder",
    L"Remove Heroes Post from this HotA folder?\n\n",
};
const wchar_t* const Russian[TextCount] = {
    L"Установка Heroes Post",
    L"Heroes Post для HotA",
    L"Heroes Post добавляет кнопку на экран сетевой игры HotA: онлайн-игры через сервер, из которых можно "
    L"выйти и вернуться в любой момент.",
    L"Папка HotA:",
    L"Обзор...",
    L"Установить",
    L"Обновить",
    L"Удалить",
    L"Закрыть",
    L"Выберите папку, в которую установлена HotA (где лежит h3hota HD.exe).",
    L"В этой папке нет h3hota HD.exe.",
    L"Эта версия HotA или HD Mod не поддерживается. Heroes Post работает с последними HotA 1.8.1 и HD Mod (",
    L"). Запустите HotA через её лаунчер и дайте ему обновить HotA и HD Mod, затем снова запустите эту установку. "
    L"Если всё уже обновлено, обновление нужно самому Heroes Post. Отличаются файлы: ",
    L"В этой папке уже есть version.dll от другого мода. Heroes Post нужен этот файл, поэтому сюда его "
    L"установить нельзя.",
    L"Найдена HotA 1.8.1. Heroes Post ещё не установлен.",
    L"Найдена HotA 1.8.1. Heroes Post установлен.",
    L"HotA запущена из этой папки. Закройте её и попробуйте снова.",
    L"Heroes Post установлен.\n\nЗапустите HotA, откройте экран сетевой игры и нажмите Heroes Post под типами "
    L"подключения. В первый раз он спросит адрес вашего сервера; затем войдите или зарегистрируйтесь.",
    L"Heroes Post удалён из этой HotA. Ваша учётная запись остаётся на сервере.",
    L"Что-то пошло не так: ",
    L"Windows не разрешила изменение: права администратора не получены.",
    L"Выберите папку HotA",
    L"Удалить Heroes Post из этой папки HotA?\n\n",
};
const wchar_t* const* strings = English;
const wchar_t* T(Text t) { return strings[t]; }
const wchar_t* Language() { return strings == Russian ? L"ru" : L"en"; }
void ChooseLanguage(const std::wstring& forced) {
    if (forced == L"ru") { strings = Russian; return; }
    if (forced == L"en") { strings = English; return; }
    const auto language = PRIMARYLANGID(GetUserDefaultUILanguage());
    strings = language == LANG_RUSSIAN || language == LANG_UKRAINIAN || language == LANG_BELARUSIAN ? Russian : English;
}

// ------------------------------------------------------------------------------- utilities
std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }
std::wstring Trim(const std::wstring& s) {
    const auto first = s.find_first_not_of(L" \t\r\n"); if (first == std::wstring::npos) return L"";
    return s.substr(first, s.find_last_not_of(L" \t\r\n") - first + 1);
}
bool Same(const std::wstring& a, const std::wstring& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }
bool Exists(const std::wstring& path) { const DWORD a = GetFileAttributesW(path.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring Parent(const std::wstring& path) { const auto slash = path.find_last_of(L"\\/"); return slash == std::wstring::npos ? L"" : path.substr(0, slash); }
std::wstring Normal(const std::wstring& folder) {
    std::wstring s = Trim(folder);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    if (s.empty()) return s;
    wchar_t full[MAX_PATH * 2]{};
    if (GetFullPathNameW(s.c_str(), _countof(full), full, nullptr)) s = full;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}
std::wstring Join(const std::wstring& folder, const std::wstring& name) { return folder + L"\\" + name; }
std::wstring SelfPath() { wchar_t p[MAX_PATH * 2]{}; GetModuleFileNameW(nullptr, p, _countof(p)); return p; }
std::wstring Resource(int id) { wchar_t b[512]{}; LoadStringW(nullptr, id, b, _countof(b)); return b; }

std::wstring SystemText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = n && buffer ? Trim(buffer) : L"error " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    return text;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    std::string out(WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    return out;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), static_cast<int>(out.size()));
    return out;
}

bool ReadAll(const std::wstring& path, std::string& data, size_t limit) {
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; bool ok = GetFileSizeEx(h, &size) && static_cast<unsigned long long>(size.QuadPart) <= limit;
    if (ok) {
        data.resize(static_cast<size_t>(size.QuadPart)); DWORD got = 0;
        ok = data.empty() || (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size());
    }
    CloseHandle(h); return ok;
}
// Written beside the target and moved over it, so a failure never leaves half a file.
DWORD WriteAll(const std::wstring& path, const void* data, size_t size) {
    const auto temp = path + L".heroespost-new";
    const HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wrote = 0; const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size && FlushFileBuffers(h);
    const DWORD error = ok ? 0 : GetLastError(); CloseHandle(h);
    if (!ok) { DeleteFileW(temp.c_str()); return error; }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError(); DeleteFileW(temp.c_str()); return e;
    }
    return 0;
}

std::string Sha256(const std::wstring& path) {
    std::string data; if (!ReadAll(path, data, 256u << 20)) return {};
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; unsigned char digest[32]{};
    const bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                    BCryptHashData(hash, reinterpret_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0 &&
                    BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    static const char hex[] = "0123456789abcdef"; std::string out;
    for (const auto b : digest) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}

// ------------------------------------------------------------------------------ the folder
bool HasGame(const std::wstring& folder) { return !folder.empty() && Exists(Join(folder, L"h3hota HD.exe")); }

// Our loader names the adapter it loads; another mod's version.dll does not.
bool OurLoader(const std::wstring& path) {
    std::string data; if (!ReadAll(path, data, 8u << 20)) return false;
    const std::wstring marker = L"\\HeroesPost.Native.dll";
    const std::string bytes(reinterpret_cast<const char*>(marker.data()), marker.size() * sizeof(wchar_t));
    return data.find(bytes) != std::string::npos;
}

struct Inspection {
    enum Kind { NoFolder, NoGame, Unsupported, Foreign, Ready, Installed } kind = NoFolder;
    std::wstring different;
    bool usable() const { return kind == Ready || kind == Installed; }
};
Inspection Inspect(const std::wstring& folder) {
    Inspection r;
    if (folder.empty() || GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) return r;
    if (!HasGame(folder)) { r.kind = Inspection::NoGame; return r; }
    // HotA's own files, then those of the HD Mod build its HD_HOTA.dll belongs to.
    auto check = [&](const hp_build::File& file) {
        if (Sha256(Join(folder, file.path)) != file.sha256) r.different += (r.different.empty() ? L"" : L", ") + std::wstring(file.path);
    };
    for (const auto& file : hp_build::Game) check(file);
    const auto* hd = hp_build::FindHd(Sha256(Join(folder, L"HD_HOTA.dll")).c_str());
    for (const auto& file : (hd ? *hd : hp_build::HdBuilds[_countof(hp_build::HdBuilds) - 1]).files) check(file);
    if (!r.different.empty()) { r.kind = Inspection::Unsupported; return r; }
    const auto loader = Join(folder, Loader);
    if (Exists(loader) && !OurLoader(loader)) { r.kind = Inspection::Foreign; return r; }
    r.kind = Exists(loader) && Exists(Join(folder, Adapter)) ? Inspection::Installed : Inspection::Ready;
    return r;
}

// Where HotA is: the HotA installer's uninstall entry (or ours), a running HotA, the usual folders.
std::wstring FindHota() {
    const struct { HKEY root; const wchar_t* path; } places[] = {
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    };
    for (const auto& place : places) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(place.root, place.path, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD n = _countof(name);
            if (RegEnumKeyExW(key, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t display[512]{}, location[MAX_PATH * 2]{}; DWORD ds = sizeof display, ls = sizeof location;
            if (RegGetValueW(key, name, L"DisplayName", RRF_RT_REG_SZ, nullptr, display, &ds) != ERROR_SUCCESS) continue;
            const std::wstring shown = display;
            if (shown.find(L"Horn of the Abyss") == std::wstring::npos && shown.rfind(L"Heroes Post", 0) != 0) continue;
            if (RegGetValueW(key, name, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location, &ls) != ERROR_SUCCESS) continue;
            const auto folder = Normal(location);
            if (HasGame(folder)) { RegCloseKey(key); return folder; }
        }
        RegCloseKey(key);
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W e{}; e.dwSize = sizeof e;
        for (BOOL more = Process32FirstW(snapshot, &e); more; more = Process32NextW(snapshot, &e)) {
            if (Lower(e.szExeFile).rfind(L"h3hota", 0) != 0) continue;
            const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID); if (!p) continue;
            wchar_t image[MAX_PATH * 2]{}; DWORD n = _countof(image);
            const bool ok = QueryFullProcessImageNameW(p, 0, image, &n); CloseHandle(p);
            if (ok && HasGame(Parent(image))) { CloseHandle(snapshot); return Parent(image); }
        }
        CloseHandle(snapshot);
    }
    const wchar_t* const usual[] = {
        L"C:\\Games\\Heroes3_HotA", L"C:\\Games\\HotA", L"C:\\Games\\Heroes 3",
        L"%ProgramFiles(x86)%\\Heroes3_HotA", L"%ProgramFiles(x86)%\\HotA",
        L"C:\\GOG Games\\Heroes of Might and Magic 3 Complete",
        L"%ProgramFiles(x86)%\\GOG Galaxy\\Games\\Heroes of Might and Magic 3 Complete",
        L"%ProgramFiles(x86)%\\3DO\\Heroes 3", L"%ProgramFiles(x86)%\\Ubisoft\\Heroes of Might and Magic III",
    };
    for (const auto* candidate : usual) {
        wchar_t path[MAX_PATH * 2]{}; ExpandEnvironmentStringsW(candidate, path, _countof(path));
        if (HasGame(path)) return Normal(path);
    }
    return L"";
}

bool RunningFrom(const std::wstring& folder) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool running = false; const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W e{}; e.dwSize = sizeof e;
    for (BOOL more = Process32FirstW(snapshot, &e); more && !running; more = Process32NextW(snapshot, &e)) {
        if (e.th32ProcessID == self) continue;
        const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID); if (!p) continue;
        wchar_t image[MAX_PATH * 2]{}; DWORD n = _countof(image);
        if (QueryFullProcessImageNameW(p, 0, image, &n) && Same(Parent(image), folder)) running = true;
        CloseHandle(p);
    }
    CloseHandle(snapshot);
    return running;
}

// --------------------------------------------------------------------- install, uninstall
struct Outcome { bool ok = true; DWORD error = 0; std::wstring text; };
Outcome Fail(DWORD error) { return {false, error, SystemText(error)}; }

std::wstring UninstallKey(const std::wstring& folder) {
    unsigned hash = 2166136261u; for (const wchar_t c : Lower(folder)) { hash ^= c; hash *= 16777619u; }
    wchar_t name[32]; swprintf_s(name, L"HeroesPost-%08x", hash);
    return std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + name;
}

DWORD Extract(int id, const std::wstring& target) {
    const HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    const HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* p = g ? LockResource(g) : nullptr;
    if (!p) return ERROR_RESOURCE_DATA_NOT_FOUND;
    return WriteAll(target, p, SizeofResource(nullptr, r));
}

// Add or Remove Programs, for this Windows user.
void Register(const std::wstring& folder) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey(folder).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const auto setup = Join(folder, SetupCopy);
    auto text = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    auto number = [&](const wchar_t* name, DWORD value) { RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value); };
    text(L"DisplayName", L"Heroes Post for HotA");
    text(L"DisplayVersion", Resource(StrVersion));
    text(L"Publisher", L"Heroes Post");
    text(L"URLInfoAbout", Project);
    text(L"InstallLocation", folder);
    text(L"DisplayIcon", Join(folder, L"h3hota HD.exe") + L",0");
    text(L"UninstallString", L"\"" + setup + L"\" /uninstall \"/D=" + folder + L"\"");
    text(L"QuietUninstallString", L"\"" + setup + L"\" /S /uninstall \"/D=" + folder + L"\"");
    number(L"NoModify", 1); number(L"NoRepair", 1);
    WIN32_FILE_ATTRIBUTE_DATA a{}; DWORD kb = 0;
    for (const auto* f : {Loader, Adapter, SetupCopy})
        if (GetFileAttributesExW(Join(folder, f).c_str(), GetFileExInfoStandard, &a)) kb += (a.nFileSizeLow + 1023) / 1024;
    number(L"EstimatedSize", kb);
    RegCloseKey(key);
}

Outcome Install(const std::wstring& folder) {
    if (const DWORD e = Extract(ResAdapter, Join(folder, Adapter))) return Fail(e);
    if (const DWORD e = Extract(ResLoader, Join(folder, Loader))) return Fail(e);
    // A copy of this program in the folder uninstalls the patch from Add or Remove Programs.
    const auto self = SelfPath(), copy = Join(folder, SetupCopy);
    if (!Same(self, copy) && !CopyFileW(self.c_str(), copy.c_str(), FALSE)) return Fail(GetLastError());
    Register(folder);
    return {};
}

bool removeSelf = false;
Outcome Uninstall(const std::wstring& folder) {
    if (!HasGame(folder)) return {false, ERROR_PATH_NOT_FOUND, T(NoGame)};
    const auto loader = Join(folder, Loader);
    if (Exists(loader) && OurLoader(loader) && !DeleteFileW(loader.c_str())) return Fail(GetLastError());
    // The patch's own files all start with "HeroesPost.": the adapter, settings, logs and Continue's handoff files.
    WIN32_FIND_DATAW f{}; const auto self = SelfPath();
    const HANDLE find = FindFirstFileW(Join(folder, L"HeroesPost.*").c_str(), &f);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const auto path = Join(folder, f.cFileName);
            if (Same(path, self)) { removeSelf = true; continue; }
            if (!DeleteFileW(path.c_str())) { const DWORD e = GetLastError(); FindClose(find); return Fail(e); }
        } while (FindNextFileW(find, &f));
        FindClose(find);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKey(folder).c_str());
    return {};
}

// The running copy in the HotA folder goes once this process has ended.
void RemoveSelfLater() {
    if (!removeSelf) return;
    std::wstring command = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + SelfPath() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

bool Elevated() {
    HANDLE token = nullptr; TOKEN_ELEVATION e{}; DWORD n = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && GetTokenInformation(token, TokenElevation, &e, sizeof e, &n);
    if (token) CloseHandle(token);
    return ok && e.TokenIsElevated;
}

// --------------------------------------------------------------------------- command line
struct Options { bool silent = false, uninstall = false; std::wstring folder, lang, log; };
Options Parse() {
    Options o; int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i], l = Lower(a);
        if (l == L"/s") o.silent = true;
        else if (l == L"/uninstall") o.uninstall = true;
        else if (l.rfind(L"/d=", 0) == 0) o.folder = a.substr(3);
        else if (l.rfind(L"/lang=", 0) == 0) o.lang = l.substr(6);
        else if (l.rfind(L"/log=", 0) == 0) o.log = a.substr(5);
    }
    if (argv) LocalFree(argv);
    o.folder = Normal(o.folder);
    return o;
}

void Log(const Options& o, const std::wstring& line) {
    if (o.log.empty()) return;
    const HANDLE h = CreateFileW(o.log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const auto text = Narrow(line + L"\r\n"); DWORD n = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr); CloseHandle(h);
}

std::wstring StatusText(const Inspection& r) {
    switch (r.kind) {
    case Inspection::NoFolder: return T(NoFolder);
    case Inspection::NoGame: return T(NoGame);
    case Inspection::Unsupported: {
        std::wstring builds;
        for (const auto& b : hp_build::HdBuilds) builds += (builds.empty() ? L"" : L", ") + std::wstring(b.name + 3); // without "HD "
        return T(Unsupported) + builds + T(UnsupportedTail) + r.different + L".";
    }
    case Inspection::Foreign: return T(Foreign);
    case Inspection::Ready: return T(Ready);
    default: return T(Installed);
    }
}

int Silent(const Options& o) {
    const auto folder = o.folder.empty() ? FindHota() : o.folder;
    if (RunningFrom(folder)) { Log(o, L"ERROR " + std::wstring(T(Running))); return 3; }
    if (o.uninstall) {
        const auto r = Uninstall(folder);
        Log(o, r.ok ? L"OK " + std::wstring(T(Removed)) : L"ERROR " + std::wstring(T(Failed)) + r.text);
        RemoveSelfLater();
        return r.ok ? 0 : 1;
    }
    const auto check = Inspect(folder);
    if (!check.usable()) { Log(o, L"ERROR " + StatusText(check)); return 2; }
    const auto r = Install(folder);
    Log(o, r.ok ? L"OK " + std::wstring(T(Done)) : L"ERROR " + std::wstring(T(Failed)) + r.text);
    return r.ok ? 0 : 1;
}

// Runs this program elevated with /S and returns what it logged.
Outcome AsAdministrator(const std::wstring& arguments) {
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto log = std::wstring(temp) + L"HeroesPost.setup." + std::to_wstring(GetCurrentProcessId()) + L".log";
    DeleteFileW(log.c_str());
    const auto parameters = L"/S " + arguments + L" /lang=" + Language() + L" \"/log=" + log + L"\"";
    const auto self = SelfPath();
    SHELLEXECUTEINFOW e{}; e.cbSize = sizeof e; e.fMask = SEE_MASK_NOCLOSEPROCESS; e.lpVerb = L"runas";
    e.lpFile = self.c_str(); e.lpParameters = parameters.c_str(); e.nShow = SW_HIDE;
    if (!ShellExecuteExW(&e) || !e.hProcess) return {false, ERROR_CANCELLED, T(Refused)};
    WaitForSingleObject(e.hProcess, INFINITE); CloseHandle(e.hProcess);
    std::string data; ReadAll(log, data, 1u << 20); DeleteFileW(log.c_str());
    const auto text = Trim(Wide(data));
    if (text.rfind(L"OK", 0) == 0) return {};
    return {false, 0, text.rfind(L"ERROR ", 0) == 0 ? text.substr(6) : text};
}

// Install or uninstall here, or as administrator when Windows refuses the files.
Outcome Change(const std::wstring& folder, bool uninstall) {
    auto r = uninstall ? Uninstall(folder) : Install(folder);
    if (!r.ok && r.error == ERROR_ACCESS_DENIED && !Elevated())
        r = AsAdministrator(std::wstring(uninstall ? L"/uninstall " : L"") + L"\"/D=" + folder + L"\"");
    return r;
}

void Report(HWND owner, const Outcome& r, Text success) {
    const auto text = r.ok ? std::wstring(T(success)) : r.error == ERROR_CANCELLED ? r.text : T(Failed) + r.text;
    MessageBoxW(owner, text.c_str(), T(Title), MB_OK | (r.ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

// --------------------------------------------------------------------------------- window
enum Id { IdFolder = 100, IdBrowse, IdStatus, IdInstall, IdUninstall, IdClose };
struct Ui {
    HWND window = nullptr, folder = nullptr, status = nullptr, install = nullptr, uninstall = nullptr;
    HFONT font = nullptr, bold = nullptr; int dpi = 96;
    Inspection check; std::wstring checked;
    Options options;
} ui;
int S(int v) { return MulDiv(v, ui.dpi, 96); }
std::wstring Get(HWND h) { std::wstring s(GetWindowTextLengthW(h), L'\0'); GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1); return s; }

void Refresh() {
    ui.checked = Normal(Get(ui.folder));
    ui.check = Inspect(ui.checked);
    SetWindowTextW(ui.status, StatusText(ui.check).c_str());
    EnableWindow(ui.install, ui.check.usable());
    EnableWindow(ui.uninstall, ui.check.kind == Inspection::Installed);
    SetWindowTextW(ui.install, T(ui.check.kind == Inspection::Installed ? UpdateButton : InstallButton));
    InvalidateRect(ui.status, nullptr, TRUE);
}

void OnBrowse() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD flags = 0; dialog->GetOptions(&flags); dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(T(PickFolder));
    IShellItem* item = nullptr;
    if (!ui.checked.empty() && SUCCEEDED(SHCreateItemFromParsingName(ui.checked.c_str(), nullptr, IID_PPV_ARGS(&item)))) { dialog->SetFolder(item); item->Release(); item = nullptr; }
    if (SUCCEEDED(dialog->Show(ui.window)) && SUCCEEDED(dialog->GetResult(&item))) {
        wchar_t* path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { SetWindowTextW(ui.folder, path); CoTaskMemFree(path); Refresh(); }
        item->Release();
    }
    dialog->Release();
}

void OnChange(bool uninstall) {
    Refresh();
    if (uninstall ? ui.check.kind != Inspection::Installed : !ui.check.usable()) return;
    if (RunningFrom(ui.checked)) { MessageBoxW(ui.window, T(Running), T(Title), MB_OK | MB_ICONWARNING); return; }
    const auto r = Change(ui.checked, uninstall);
    Refresh();
    Report(ui.window, r, uninstall ? Removed : Done);
}

HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    const HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), ui.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
    return c;
}

constexpr int Width = 520, Height = 296, Margin = 16;
void Build() {
    const HWND heading = Child(L"STATIC", T(Heading), SS_LEFT, Margin, 14, Width - 2 * Margin, 26, 0);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(ui.bold), TRUE);
    Child(L"STATIC", T(Intro), SS_LEFT, Margin, 44, Width - 2 * Margin, 36, 0);
    Child(L"STATIC", T(FolderLabel), SS_LEFT, Margin, 92, Width - 2 * Margin, 18, 0);
    ui.folder = Child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Margin, 112, Width - 2 * Margin - 104, 24, IdFolder, WS_EX_CLIENTEDGE);
    Child(L"BUTTON", T(Browse), WS_TABSTOP | BS_PUSHBUTTON, Width - Margin - 96, 111, 96, 26, IdBrowse);
    ui.status = Child(L"STATIC", L"", SS_LEFT, Margin, 146, Width - 2 * Margin, 100, IdStatus);
    ui.install = Child(L"BUTTON", T(InstallButton), WS_TABSTOP | BS_DEFPUSHBUTTON, Width - Margin - 3 * 104 + 8, 256, 96, 28, IdInstall);
    ui.uninstall = Child(L"BUTTON", T(UninstallButton), WS_TABSTOP | BS_PUSHBUTTON, Width - Margin - 2 * 104 + 8, 256, 96, 28, IdUninstall);
    Child(L"BUTTON", T(CloseButton), WS_TABSTOP | BS_PUSHBUTTON, Width - Margin - 104 + 8, 256, 96, 28, IdClose);
    const auto version = Resource(StrVersion);
    if (!version.empty()) Child(L"STATIC", (L"v" + version).c_str(), SS_LEFT, Margin, 262, 120, 18, 0);
    SetWindowTextW(ui.folder, (ui.options.folder.empty() ? FindHota() : ui.options.folder).c_str());
    Refresh();
    SetFocus(IsWindowEnabled(ui.install) ? ui.install : ui.folder);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: ui.window = h; Build(); return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IdFolder: if (HIWORD(w) == EN_CHANGE) SetTimer(h, 1, 400, nullptr); return 0;
        case IdBrowse: OnBrowse(); return 0;
        case IdInstall: case IDOK: OnChange(false); return 0;
        case IdUninstall: OnChange(true); return 0;
        case IdClose: case IDCANCEL: DestroyWindow(h); return 0;
        }
        break;
    case WM_TIMER: KillTimer(h, 1); Refresh(); return 0;
    case WM_CTLCOLORSTATIC: {
        const HDC dc = reinterpret_cast<HDC>(w);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if (reinterpret_cast<HWND>(l) == ui.status) SetTextColor(dc, ui.check.usable() ? RGB(0, 110, 40) : RGB(170, 30, 30));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int Window(HINSTANCE instance) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    const HDC screen = GetDC(nullptr); ui.dpi = GetDeviceCaps(screen, LOGPIXELSY); ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof metrics;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    ui.font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW big = metrics.lfMessageFont; big.lfWeight = FW_BOLD; big.lfHeight = MulDiv(big.lfHeight, 4, 3);
    ui.bold = CreateFontIndirectW(&big);
    WNDCLASSEXW wc{sizeof wc}; wc.lpfnWndProc = Proc; wc.hInstance = instance; wc.lpszClassName = L"HeroesPostSetup";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, S(Width), S(Height)}; AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, T(Title), style,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    DeleteObject(ui.font); DeleteObject(ui.bold);
    return 0;
}

// Uninstall from Add or Remove Programs: a question, then the result; no setup window.
int AskAndUninstall(const Options& o) {
    const auto folder = o.folder.empty() ? FindHota() : o.folder;
    if (MessageBoxW(nullptr, (T(RemoveQuestion) + folder).c_str(), T(Title), MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
    if (RunningFrom(folder)) { MessageBoxW(nullptr, T(Running), T(Title), MB_OK | MB_ICONWARNING); return 3; }
    const auto r = Change(folder, true);
    Report(nullptr, r, Removed);
    RemoveSelfLater();
    return r.ok ? 0 : 1;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const Options options = Parse();
    ChooseLanguage(options.lang);
    if (options.silent) return Silent(options);
    if (options.uninstall) return AskAndUninstall(options);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ui.options = options;
    const int code = Window(instance);
    RemoveSelfLater();
    CoUninitialize();
    return code;
}
