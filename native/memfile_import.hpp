#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hota_memfile {

// Experimental existing-stream primitive, NOT a world/turn recovery API.
// Intentionally excludes session-dependent incoming/outgoing names.
enum class Role { BaselineDayOne, BaselineLater, LocalSnapshot };
struct Replacement { Role role; std::vector<std::uint8_t> bytes; };
enum class Error {
    None, NotInitialized, WrongThread, NotQuiescent, Reentrant, UnsupportedBuild,
    InvalidRequest, LimitExceeded, InvalidRegistry, MissingStream, Busy,
    InvalidObject, StagingFailed, StateChanged
};
struct Result {
    Error error = Error::None;
    explicit operator bool() const noexcept { return error == Error::None; }
};
struct Limits {
    std::size_t max_registry_entries = 256;
    std::size_t max_stream_bytes = 64u * 1024u * 1024u;
    std::size_t max_total_bytes = 192u * 1024u * 1024u;
};
// Required caller-owned admission: true only at an exclusively held game-thread
// boundary outside save/load/merge/receive/battle transitions. Closed is not enough.
using Admission = bool (*)(void*) noexcept;

namespace testing {
// Trusted test-only seams. map must return stable memory for the entire call;
// create/write/destroy must implement native ownership and leave valid objects
// after a C++ exception. destroy must never throw. No callbacks run during commit.
struct Backend {
    void* context = nullptr;
    bool (*fingerprint)(void*, std::uint32_t) noexcept = nullptr;
    std::uint32_t (*thread_id)(void*) noexcept = nullptr;
    std::uint8_t* (*map)(void*, std::uint32_t, std::size_t, bool writable) noexcept = nullptr;
    std::uint32_t (*create)(void*, std::uint32_t hd_base, const char* name) = nullptr;
    int (*write)(void*, std::uint32_t hd_base, std::uint32_t object,
                 const void* bytes, int count) = nullptr;
    void (*destroy)(void*, std::uint32_t hd_base, std::uint32_t object) noexcept = nullptr;
    // Native installed engine file constructor/close, or synthetic equivalent.
    // May create an empty registered stream; never edits an existing payload.
    bool (*ensure_registered)(void*,std::uint32_t hd_base,const char* name) noexcept = nullptr;
};
}

class Importer {
public:
    Importer() = default;
    Importer(const Importer&) = delete;
    Importer& operator=(const Importer&) = delete;
    // Verifies loaded module identity, pinned disk SHA256 and native entry bytes.
    // Must be called on the same explicitly supplied game thread as Replace.
    Result InitializeNative(std::uint32_t hd_base, std::uint32_t game_thread_id,
                            Admission admission, void* admission_context = nullptr) noexcept;
    Result InitializeForTesting(std::uint32_t hd_base, std::uint32_t game_thread_id,
                                testing::Backend backend, Admission admission,
                                void* admission_context = nullptr) noexcept;
    // Existing registered streams only. No registry edits, load, merge or metadata
    // restore. After publishing the whole set, only native allocation cleanup runs.
    Result Replace(const std::vector<Replacement>& replacements, Limits limits = {}) noexcept;
    // Quiescent bootstrap only. Creates MISSING known streams through the native
    // installed file constructor+close. Existing streams remain untouched.
    // Creation is not transactional: on later failure earlier empty streams may
    // remain registered. Caller must keep gameplay held on any failure.
    Result EnsureRoles(const std::vector<Role>& roles) noexcept;
private:
    testing::Backend backend_{};
    std::uint32_t hd_base_ = 0, game_thread_ = 0;
    Admission admission_ = nullptr;
    void* admission_context_ = nullptr;
    bool active_ = false;
};
const char* ErrorName(Error error) noexcept;
} // namespace hota_memfile
