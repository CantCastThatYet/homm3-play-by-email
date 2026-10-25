// version.dll placed in the HotA folder: the game imports VERSION.dll, which is not a
// Windows KnownDLL, so Windows loads this file from the game folder first. It forwards
// every export to the system version.dll and loads the Heroes Post adapter beside it.
// Nothing happens unless HeroesPost.Native.dll is present.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

static_assert(sizeof(void*) == 4, "The proxy must match the x86 game");

namespace {
const char* const Names[] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"};
constexpr int Count = sizeof(Names) / sizeof(Names[0]);
}
extern "C" FARPROC ProxyTargets[Count] = {};

namespace {
HMODULE self = nullptr;

std::wstring Folder(HMODULE module) {
    wchar_t path[MAX_PATH]{}; const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring s(path, n); const auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

// Runs after the loader lock is released: the adapter must not initialize inside DllMain.
DWORD WINAPI LoadAdapter(void*) {
    const auto dll = Folder(self) + L"\\HeroesPost.Native.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    const HMODULE adapter = LoadLibraryW(dll.c_str());
    if (!adapter) return 1;
    using Initialize = DWORD(WINAPI*)(void*);
    const auto init = reinterpret_cast<Initialize>(GetProcAddress(adapter, "HeroesPostInitialize"));
    return init ? init(reinterpret_cast<void*>(1)) : 2;
}
}

#define PROXY(index, name) extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [ProxyTargets + index * 4] } }
PROXY(0, GetFileVersionInfoA) PROXY(1, GetFileVersionInfoByHandle) PROXY(2, GetFileVersionInfoExA) PROXY(3, GetFileVersionInfoExW)
PROXY(4, GetFileVersionInfoSizeA) PROXY(5, GetFileVersionInfoSizeExA) PROXY(6, GetFileVersionInfoSizeExW) PROXY(7, GetFileVersionInfoSizeW)
PROXY(8, GetFileVersionInfoW) PROXY(9, VerFindFileA) PROXY(10, VerFindFileW) PROXY(11, VerInstallFileA) PROXY(12, VerInstallFileW)
PROXY(13, VerLanguageNameA) PROXY(14, VerLanguageNameW) PROXY(15, VerQueryValueA) PROXY(16, VerQueryValueW)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    self = instance; DisableThreadLibraryCalls(instance);
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return FALSE;
    const HMODULE real = LoadLibraryW((std::wstring(system) + L"\\version.dll").c_str());
    if (!real) return FALSE;
    for (int i = 0; i < Count; ++i) ProxyTargets[i] = GetProcAddress(real, Names[i]);
    if (const HANDLE thread = CreateThread(nullptr, 0, LoadAdapter, nullptr, 0, nullptr)) CloseHandle(thread);
    return TRUE;
}
