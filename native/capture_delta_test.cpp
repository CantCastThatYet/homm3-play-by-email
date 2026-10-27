// Capture differences: round trips, sizes for a few scattered edits, and refusals.
#include "capture_delta.hpp"
#include <cstdio>
#include <random>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
std::string Random(size_t n, std::mt19937& rng) { std::string s(n, '\0'); for (auto& c : s) c = static_cast<char>(rng()); return s; }
bool RoundTrip(const std::string& base, const std::string& target, size_t* size = nullptr) {
    const auto frame = hp_delta::Encode(base, target);
    if (size) *size = frame.size();
    std::string back;
    return !frame.empty() && hp_delta::Decode(base, frame, back) && back == target;
}
}

int main() {
    std::mt19937 rng(1234);
    // About the size of a 72x72 capture; a move changed 401 bytes in 11 places and grew the world by 340.
    const auto base = Random(2 * 1024 * 1024, rng);
    std::string moved = base;
    for (int k = 0; k < 11; ++k) {
        const size_t at = (k + 1) * (moved.size() / 13);
        if (k % 2) moved.insert(at, Random(62, rng)); else moved.replace(at, 36, Random(36, rng));
    }
    size_t size = 0;
    Check(RoundTrip(base, moved, &size), "a capture with scattered edits and insertions round-trips");
    std::printf("     difference: %zu bytes for %zu\n", size, moved.size());
    Check(size < 4096, "the difference is a few kilobytes at most");
    Check(RoundTrip(base, base, &size) && size < 200, "an unchanged capture is one copy");
    Check(RoundTrip(base, Random(100000, rng)), "unrelated data round-trips as literals");
    Check(RoundTrip({}, moved.substr(0, 5000)), "an empty base round-trips");
    Check(RoundTrip(base, {}), "an empty target round-trips");
    Check(RoundTrip(base.substr(0, 20), base.substr(0, 10)), "inputs shorter than a block round-trip");
    std::string front = Random(3, rng) + base;
    Check(RoundTrip(base, front, &size) && size < 200, "bytes inserted at the front shift the rest into one copy");

    const auto frame = hp_delta::Encode(base, moved);
    std::string back;
    std::string other = base; other[12345] ^= 1;
    Check(!hp_delta::Decode(other, frame, back), "another base is refused");
    Check(!hp_delta::Decode(base, frame.substr(0, frame.size() - 1), back), "a truncated frame is refused");
    std::string damaged = frame; damaged[damaged.size() - 1] ^= 1;
    Check(!hp_delta::Decode(base, damaged, back), "a damaged literal fails the result's checksum");
    std::string version = frame; version[4] = 2;
    Check(!hp_delta::Decode(base, version, back), "another version is refused");
    std::printf(failures ? "%d FAILED\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
