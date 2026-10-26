#include "common.hpp"
#include <bcrypt.h>
#include <sstream>
#include <iomanip>
#include <cwctype>
#pragma comment(lib, "bcrypt.lib")

namespace hp {
std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::string Json(const std::string& s) {
    std::ostringstream o; o << '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if (c < 32) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
            else o << c;
        }
    }
    o << '"'; return o.str();
}
std::wstring ModulePath(HMODULE m) {
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(m, p.data(), static_cast<DWORD>(p.size()));
    if (!n || n == p.size()) return {}; p.resize(n); return p;
}
std::wstring Canonical(const std::wstring& p, bool directory) {
    Handle f(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr));
    if (!f) return {};
    std::wstring out(32768, L'\0');
    DWORD n = GetFinalPathNameByHandleW(f.value, out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED);
    if (!n || n >= out.size()) return {};
    out.resize(n);
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) out = L"\\\\" + out.substr(8);
    else if (out.rfind(L"\\\\?\\", 0) == 0) out.erase(0, 4);
    return out;
}
std::wstring Parent(const std::wstring& p) { auto n = p.find_last_of(L"\\/"); return n == p.npos ? L"" : p.substr(0, n); }
std::wstring ContinueTag() {
    wchar_t value[16]{};
    const DWORD n = GetEnvironmentVariableW(L"HEROESPOST_CONTINUE", value, 16);
    std::wstring tag;
    if (n && n < 16) for (const wchar_t* c = value; *c; ++c) if (*c >= L'0' && *c <= L'9') tag += *c;
    return tag;
}
std::wstring HandoffPath(const std::wstring& folder, const std::wstring& tag, const wchar_t* name) {
    return folder + L"\\HeroesPost." + (tag.empty() ? L"" : tag + L".") + name;
}
bool Within(const std::wstring& child, const std::wstring& dir) {
    std::wstring prefix = dir;
    if (prefix.empty()) return false;
    if (prefix.back() != L'\\') prefix += L'\\';
    return child.size() > prefix.size() && _wcsnicmp(child.c_str(), prefix.c_str(), prefix.size()) == 0;
}
std::string Sha256(const std::wstring& p) {
    Handle f(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(alg, 0); return {}; }
    unsigned char buffer[65536], digest[32]; DWORD n = 0; bool ok = true;
    for (;;) {
        if (!ReadFile(f.value, buffer, sizeof buffer, &n, nullptr)) { ok = false; break; }
        if (!n) break;
        if (BCryptHashData(hash, buffer, n, 0) < 0) { ok = false; break; }
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    std::ostringstream o; for (auto c : digest) o << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return o.str();
}
std::uint64_t CreationTime(HANDLE p) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(p, &c, &e, &k, &u)) return 0;
    return (std::uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}
std::wstring PipeName(DWORD pid) { return L"\\\\.\\pipe\\HeroesPost.Native.v1." + std::to_wstring(pid); }
std::string Error(const std::string& code, const std::string& message) {
    return "{\"protocol\":1,\"ok\":false,\"error\":{\"code\":" + Json(code) + ",\"message\":" + Json(message) + "}}";
}
bool ParseCommand(const std::string& line, std::string& command) {
    // Deliberately narrow protocol: one string field, no duplicates or extra values.
    size_t i = 0;
    auto ws = [&]() { while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) ++i; };
    auto take = [&](char c) { ws(); return i < line.size() && line[i++] == c; };
    auto str = [&](std::string& out) {
        if (!take('"')) return false;
        out.clear();
        while (i < line.size() && line[i] != '"') {
            char c = line[i++]; if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') return false; out += c;
        }
        return i < line.size() && line[i++] == '"';
    };
    std::string key;
    if (!take('{') || !str(key) || key != "command" || !take(':') || !str(command) || !take('}')) return false;
    ws(); return i == line.size();
}
}
