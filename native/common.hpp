#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace hp {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::string Utf8(const std::wstring& s);
std::string Json(const std::string& s);
std::wstring ModulePath(HMODULE module);
std::wstring Canonical(const std::wstring& path, bool directory = false);
std::wstring Parent(const std::wstring& path);
// Files a Continue hands to the HotA it restarts, in the game folder: HeroesPost.<tag>.<name>, tagged with
// the restarting process's id so that HotAs started from one folder never read each other's. The restarted
// process finds its tag in HEROESPOST_CONTINUE; without one (the desktop app's launches) the file is the
// plain HeroesPost.<name>.
std::wstring ContinueTag();
std::wstring HandoffPath(const std::wstring& folder, const std::wstring& tag, const wchar_t* name);
bool Within(const std::wstring& child, const std::wstring& directory);
std::string Sha256(const std::wstring& path);
std::uint64_t CreationTime(HANDLE process);
std::wstring PipeName(DWORD pid);
std::string Error(const std::string& code, const std::string& message);
bool ParseCommand(const std::string& line, std::string& command);
template<class T> bool Read(std::uintptr_t address, T& result) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &result,
                             sizeof result, &got) && got == sizeof result;
}
}
