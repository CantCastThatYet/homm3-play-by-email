#include "common.hpp"
#include <tlhelp32.h>
#include <iostream>
#include <map>
#include <limits>

static_assert(sizeof(void*) == 4, "Injector must be x86");
namespace {
bool Decimal(const std::wstring& text, std::uint64_t& value) {
    value = 0; if (text.empty()) return false;
    for (auto c : text) {
        if (c < L'0' || c > L'9' || value > (std::numeric_limits<std::uint64_t>::max() - (c - L'0')) / 10) return false;
        value = value * 10 + (c - L'0');
    }
    return true;
}
DWORD ParentPid(DWORD pid) {
    hp::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof entry;
    if (!snapshot || !Process32FirstW(snapshot.value, &entry)) return 0;
    do { if (entry.th32ProcessID == pid) return entry.th32ParentProcessID; } while (Process32NextW(snapshot.value, &entry));
    return 0;
}
std::uintptr_t RemoteModule(DWORD pid, const std::wstring& nameOrPath, bool exactPath) {
    HANDLE raw=INVALID_HANDLE_VALUE;
    for(unsigned attempt=0;attempt<50;++attempt) {
        raw=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,pid);
        if(raw!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_BAD_LENGTH)break;
        Sleep(10);
    }
    hp::Handle snapshot(raw);
    MODULEENTRY32W entry{}; entry.dwSize = sizeof entry;
    if (!snapshot || !Module32FirstW(snapshot.value, &entry)) return 0;
    do {
        auto path = exactPath ? hp::Canonical(entry.szExePath) : std::wstring(entry.szModule);
        if (_wcsicmp(path.c_str(), nameOrPath.c_str()) == 0) return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot.value, &entry));
    return 0;
}
std::uintptr_t RemoteLoadLibrary(DWORD pid) {
    auto function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    MEMORY_BASIC_INFORMATION info{};
    if (!function || !VirtualQuery(reinterpret_cast<void*>(function), &info, sizeof info)) return 0;
    auto module = reinterpret_cast<HMODULE>(info.AllocationBase);
    auto path = hp::ModulePath(module); auto name = path.substr(path.find_last_of(L"\\/") + 1);
    auto remote = RemoteModule(pid, name, false);
    return remote ? remote + reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(module) : 0;
}
int Fail(const char* code, const std::string& message) { std::cout << hp::Error(code, message) << std::endl; return 1; }
}
int wmain(int argc, wchar_t** argv) {
    std::map<std::wstring, std::wstring> args;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc || args.count(argv[i])) return Fail("arguments", "Expected unique named argument/value pairs.");
        args[argv[i]] = argv[i + 1];
    }
    for (auto key : {L"--pid", L"--expected-exe", L"--dll", L"--created-filetime", L"--isolated-root"})
        if (!args.count(key)) return Fail("arguments", "Required: --pid --expected-exe --dll --created-filetime --isolated-root.");
    if (args.size() != 5) return Fail("arguments", "Unknown argument.");
    std::uint64_t pid64 = 0, created = 0;
    if (!Decimal(args[L"--pid"], pid64) || !pid64 || pid64 > MAXDWORD || !Decimal(args[L"--created-filetime"], created) || !created)
        return Fail("arguments", "PID and created-filetime must be exact positive decimal integers.");
    DWORD pid = static_cast<DWORD>(pid64);
    auto root = hp::Canonical(args[L"--isolated-root"], true);
    auto expected = hp::Canonical(args[L"--expected-exe"]);
    auto dll = hp::Canonical(args[L"--dll"]);
    if (root.empty() || expected.empty() || dll.empty() || !hp::Within(expected, root))
        return Fail("path_validation", "Expected executable must resolve inside the existing isolated root; DLL must exist.");
    if (_wcsicmp(expected.substr(expected.find_last_of(L"\\/") + 1).c_str(), L"h3hota HD.exe") != 0)
        return Fail("path_validation", "Only the isolated h3hota HD.exe is supported.");
    if (hp::Within(dll, root)) return Fail("path_validation", "Adapter DLL must stay outside the game copy.");
    hp::Handle marker(CreateFileW((root + L"\\.heroespost-owned-copy").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, 0, nullptr));
    char markerBytes[64]{}; DWORD read = 0;
    if (!marker || !ReadFile(marker.value, markerBytes, sizeof markerBytes, &read, nullptr)) return Fail("ownership", "Isolated-copy marker is missing.");
    std::string markerText(markerBytes, read);
    while (!markerText.empty() && (markerText.back() == '\r' || markerText.back() == '\n')) markerText.pop_back();
    if (markerText != "HeroesPost.Native.v1") return Fail("ownership", "Isolated-copy marker does not match.");
    DWORD creator = ParentPid(GetCurrentProcessId());
    if (!creator || ParentPid(pid) != creator) return Fail("ownership", "Target and injector must have the same live launcher parent.");
    hp::Handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, creator));
    if (!parent || WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT) return Fail("ownership", "Launcher parent is no longer live.");
    hp::Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                   PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid));
    if (!process || hp::CreationTime(process.value) != created) return Fail("ownership", "Target creation time does not match or target is inaccessible.");
    std::wstring actual(32768, L'\0'); DWORD length = static_cast<DWORD>(actual.size());
    if (!QueryFullProcessImageNameW(process.value, 0, actual.data(), &length)) return Fail("ownership", "Cannot read target image path.");
    actual.resize(length); actual = hp::Canonical(actual);
    if (_wcsicmp(actual.c_str(), expected.c_str()) != 0 || !hp::Within(actual, root)) return Fail("ownership", "Actual target executable path does not match.");
    if (hp::Sha256(actual) != "5aaab925f06cccf23bb09814767590a95b84a557eb33d244800520be4f1f18de")
        return Fail("unsupported_build", "Target executable fingerprint is unsupported.");
    auto remoteBase = RemoteModule(pid, dll, true);
    if (!remoteBase) {
        auto loadAddress = RemoteLoadLibrary(pid);
        if (!loadAddress) return Fail("loader", "Could not resolve the target LoadLibraryW address.");
        SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t), written = 0;
        void* path = VirtualAllocEx(process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!path) return Fail("loader", "Could not allocate DLL path in target.");
        if (!WriteProcessMemory(process.value, path, dll.c_str(), bytes, &written) || written != bytes) {
            VirtualFreeEx(process.value, path, 0, MEM_RELEASE); return Fail("loader", "Could not write DLL path.");
        }
        hp::Handle thread(CreateRemoteThread(process.value, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadAddress), path, 0, nullptr));
        if (!thread) { VirtualFreeEx(process.value, path, 0, MEM_RELEASE); return Fail("loader", "Could not start explicit adapter loader."); }
        if (WaitForSingleObject(thread.value, 30000) != WAIT_OBJECT_0)
            return Fail("loader_timeout", "LoadLibrary is still pending; target was not terminated and its path allocation remains valid.");
        VirtualFreeEx(process.value, path, 0, MEM_RELEASE);
        DWORD loaded=0;GetExitCodeThread(thread.value,&loaded);
        for(unsigned attempt=0;attempt<20;++attempt) {
            remoteBase=RemoteModule(pid,dll,true);
            if(remoteBase||!loaded)break;
            Sleep(25);
        }
        if (!remoteBase) return Fail("loader", "Adapter did not appear in the target module list; LoadLibrary result="+std::to_string(loaded));
    }
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return Fail("loader", "Could not inspect adapter export table.");
    auto init = GetProcAddress(local, "HeroesPostInitialize");
    auto offset = reinterpret_cast<std::uintptr_t>(init) - reinterpret_cast<std::uintptr_t>(local);
    FreeLibrary(local);
    if (!init) return Fail("loader", "Adapter has no HeroesPostInitialize export.");
    hp::Handle initThread(CreateRemoteThread(process.value, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + offset), nullptr, 0, nullptr));
    if (!initThread || WaitForSingleObject(initThread.value, 10000) != WAIT_OBJECT_0) return Fail("initialize", "Adapter initialization did not finish; process was not terminated.");
    DWORD result = 1; GetExitCodeThread(initThread.value, &result);
    if (result != 0) return Fail("initialize", "Adapter initialization returned " + std::to_string(result));
    std::cout << "{\"protocol\":1,\"ok\":true,\"pid\":" << pid << ",\"pipe\":" << hp::Json(hp::Utf8(hp::PipeName(pid)))
              << ",\"status\":\"adapter_loaded_probe_required\"}" << std::endl;
    return 0;
}
