#include "common.hpp"
#include <iostream>

std::string Exchange(const std::string& request) {
    auto name = hp::PipeName(GetCurrentProcessId());
    hp::Handle pipe;
    for (int i = 0; i < 100; ++i) {
        pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe) break;
        Sleep(25);
    }
    if (!pipe) return {};
    DWORD count = 0;
    if (!WriteFile(pipe.value, request.data(), static_cast<DWORD>(request.size()), &count, nullptr)) return {};
    std::string response;
    while (response.size() < 65536 && response.find('\n') == response.npos) {
        char buffer[1024];
        if (!ReadFile(pipe.value, buffer, sizeof buffer, &count, nullptr) || !count) break;
        response.append(buffer, count);
    }
    return response;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Usage: Native.SmokeTest.exe <adapter.dll>\n"; return 1; }
    auto dll = LoadLibraryW(argv[1]);
    auto init = dll ? reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(dll, "HeroesPostInitialize")) : nullptr;
    if (!init || init(nullptr) != 0) { std::cerr << "Adapter initialization failed\n"; return 2; }
    struct Test { const char* request; const char* expected; } tests[] = {
        {"{\"command\":\"probe\"}\n", "\"fingerprintAccepted\":false"},
        {"{\"command\":\"enable_telemetry\"}\n", "\"code\":\"unsupported_build\""},
        {"{\"command\":\"capture_next_save\"}\n", "\"code\":\"not_ready\""},
        {"{\"command\":\"capture_next_completion\"}\n", "\"code\":\"not_ready\""},
        {"{\"command\":\"bootstrap_session\"}\n", "\"code\":\"not_ready\""},
        {"{\"command\":\"load\"}\n", "\"code\":\"unsupported_command\""},
        {"{\"command\":\"probe\",\"command\":\"load\"}\n", "\"code\":\"invalid_request\""},
        {"{\"command\":\"probe\"}garbage\n", "\"code\":\"invalid_request\""}
    };
    for (const auto& test : tests) {
        auto response = Exchange(test.request);
        if (response.find(test.expected) == response.npos) {
            std::cerr << "FAIL expected " << test.expected << " in " << response << '\n'; return 3;
        }
    }
    std::cout << "PASS: adapter IPC/probe, unsupported build gating, unsupported mutation, duplicate/trailing JSON rejection (no game launched).\n";
    // DLL is process-lifetime: worker/hook teardown is intentionally not exposed.
    return 0;
}
