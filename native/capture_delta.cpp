#include "capture_delta.hpp"
#include "common.hpp"
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace hp_delta {
namespace {
constexpr std::uint32_t Version = 1, Block = 32, Prime = 0x01000193u;
enum : unsigned char { Copy = 0, Literal = 1 };

void Put32(std::string& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF); }
bool Get32(const std::string& in, size_t& at, std::uint32_t& v) {
    if (in.size() < 4 || at > in.size() - 4) return false;
    v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[at + i])) << (8 * i);
    at += 4; return true;
}
bool Digest(const std::string& data, unsigned char out[32]) { return hp::Sha256(data.data(), data.size(), out); }

// Polynomial hash of a Block-byte window, rolled one byte at a time.
std::uint32_t Hash(const unsigned char* p) { std::uint32_t h = 0; for (std::uint32_t i = 0; i < Block; ++i) h = h * Prime + p[i]; return h; }
}

std::string Encode(const std::string& base, const std::string& target) {
    unsigned char baseSum[32]{}, targetSum[32]{};
    if (!Digest(base, baseSum) || !Digest(target, targetSum)) return {};
    std::string out = "HPD1";
    Put32(out, Version);
    out.append(reinterpret_cast<const char*>(baseSum), 32); Put32(out, static_cast<std::uint32_t>(base.size()));
    out.append(reinterpret_cast<const char*>(targetSum), 32); Put32(out, static_cast<std::uint32_t>(target.size()));

    const auto* b = reinterpret_cast<const unsigned char*>(base.data());
    const auto* t = reinterpret_cast<const unsigned char*>(target.data());
    const size_t nb = base.size(), nt = target.size();
    // The base's blocks at multiples of Block; a matching window in the target is extended both ways.
    std::unordered_map<std::uint32_t, std::uint32_t> blocks;
    blocks.reserve(nb / Block + 1);
    for (size_t at = 0; at + Block <= nb; at += Block) blocks.emplace(Hash(b + at), static_cast<std::uint32_t>(at));
    std::uint32_t top = 1; for (std::uint32_t i = 1; i < Block; ++i) top *= Prime;

    const auto literal = [&](size_t from, size_t to) {
        if (to <= from) return;
        out += static_cast<char>(Literal); Put32(out, static_cast<std::uint32_t>(to - from));
        out.append(reinterpret_cast<const char*>(t + from), to - from);
    };
    size_t i = 0, pending = 0;
    std::uint32_t h = nt >= Block ? Hash(t) : 0;
    while (i + Block <= nt) {
        const auto found = blocks.find(h);
        if (found != blocks.end() && std::memcmp(b + found->second, t + i, Block) == 0) {
            size_t s = i, o = found->second;
            while (s > pending && o > 0 && b[o - 1] == t[s - 1]) { --s; --o; }
            size_t e = i + Block, oe = found->second + Block;
            while (e < nt && oe < nb && t[e] == b[oe]) { ++e; ++oe; }
            literal(pending, s);
            out += static_cast<char>(Copy); Put32(out, static_cast<std::uint32_t>(o)); Put32(out, static_cast<std::uint32_t>(e - s));
            i = pending = e;
            if (i + Block <= nt) h = Hash(t + i);
            continue;
        }
        if (i + Block >= nt) break;
        h = (h - t[i] * top) * Prime + t[i + Block];
        ++i;
    }
    literal(pending, nt);
    return out;
}

bool Decode(const std::string& base, const std::string& frame, std::string& target) {
    size_t at = 4; std::uint32_t version = 0, baseSize = 0, size = 0;
    if (frame.compare(0, 4, "HPD1") != 0 || !Get32(frame, at, version) || version != Version || frame.size() < at + 32) return false;
    unsigned char sum[32]{};
    if (!Digest(base, sum) || std::memcmp(sum, frame.data() + at, 32) != 0) return false;
    at += 32;
    if (!Get32(frame, at, baseSize) || baseSize != base.size() || frame.size() < at + 32) return false;
    const std::string expected = frame.substr(at, 32); at += 32;
    if (!Get32(frame, at, size)) return false;
    target.clear(); target.reserve(size);
    while (at < frame.size()) {
        const auto kind = static_cast<unsigned char>(frame[at++]);
        std::uint32_t a = 0, n = 0;
        if (kind == Copy) {
            if (!Get32(frame, at, a) || !Get32(frame, at, n) || a > base.size() || n > base.size() - a) return false;
            if (n > size - target.size()) return false;
            target.append(base, a, n);
        } else if (kind == Literal) {
            if (!Get32(frame, at, n) || n > frame.size() - at || n > size - target.size()) return false;
            target.append(frame, at, n); at += n;
        } else return false;
    }
    return target.size() == size && Digest(target, sum) && std::memcmp(sum, expected.data(), 32) == 0;
}
}
