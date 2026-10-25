#include "common.hpp"
#include "capture.hpp"
#include "completion_observer.hpp"
#include "menu_dispatch.hpp"
#include "session_bootstrap.hpp"
#include "directplay_port.hpp"
#include "scenario_start.hpp"
#include "session_request.hpp"
#include "adventure_actions.hpp"
#include "departure_pause.hpp"
#include "map_intro.hpp"
#include "completed_leave.hpp"
#include "world_resume.hpp"
#include "resume_session.hpp"
#include "solo_transport.hpp"
#include "dplay_relay.hpp"
#include "vendor/patcher_x86.hpp"
#include <psapi.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>
#include <sstream>
#include <filesystem>
#include <stdexcept>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

static_assert(sizeof(void*) == 4, "Native adapter must be built for x86");
namespace {
HMODULE moduleHandle = nullptr;
std::atomic<bool> started{false}, fingerprintAccepted{false}, captureRequested{false};
std::atomic<bool> completionCaptureRequested{false};
std::atomic<bool> completionSnapshotRequested{false};
std::atomic<unsigned> completionMarkers{0};
std::atomic<unsigned> bootstrapSequence{0};
std::atomic<unsigned> saveCalls{0}, saveReturns{0}, loadCalls{0}, loadReturns{0}, turnCalls{0}, turnReturns{0}, turnDepth{0};
std::atomic<unsigned> observerErrors{0};
std::atomic<bool> currentCaptureRequested{false};
std::atomic<ULONGLONG> leaseDeadline{0};
std::atomic<bool> leaseHeld{false};
thread_local unsigned mergeDepth=0,receiveDepth=0;
bool frameHooksInstalled=false;
hota_resume::Audit pendingCaptureAudit;
bool auditedCapturePending=false;
thread_local unsigned saveDepth = 0;
std::atomic<int> telemetryState{0}; // 0 disabled, 1 pending game thread, 2 active, -1 failed
std::atomic<DWORD> gameThreadId{0};
std::atomic<std::uintptr_t> hdBase{0};
HHOOK bootstrapHook = nullptr;
constexpr UINT bootstrapMessage = WM_APP + 0x617;
std::mutex stateMutex, captureMutex;
std::string fingerprintJson = "[]", fingerprintReason = "not_ready", hookError;
std::string captureStatus = "idle", captureError, lastCaptureManifest;
std::string lastSaveName;
std::string relayError;
std::string preparedMap;
unsigned completedWorld=0, completedToken=0;
int completedLocal=-1;
unsigned short completedDate[3]{};
bool completedArtifactWritten=false;
std::string lastSaveNameHex;
unsigned lastSaveFlags[4]{}, lastSaveResult = 0;
bool lastSaveNameComplete = false;
std::wstring captureRoot;
std::unique_ptr<hota_capture::CaptureEnvelope> detachedCapture;
HANDLE captureEvent = nullptr;

void BeginSession(const hota_menu::Dispatch& dispatch, void*) noexcept {
    hota_session::Begin(dispatch.token);
}

void CaptureCurrentAtBoundary() noexcept;
// Session facts a resume needs besides the world: the relay position and the scenario file.
void AddSessionState(hota_capture::CaptureEnvelope& envelope) {
    if (hp_relay::Installed()) { envelope.relay_consumed = hp_relay::ConsumedCount(); envelope.relay_sent = hp_relay::SentCount(); }
    std::uint32_t world = 0; char name[252]{};
    if (hp::Read(0x699538, world) && world && hp::Read(world + 0x1f6d9, name)) {
        name[251] = 0; const std::string file(name);
        if (std::all_of(file.begin(), file.end(), [](unsigned char c) { return c >= 32 && c < 127 && c != '/' && c != '\\' && c != ':'; }))
            envelope.map_file = file;
    }
}
bool LeaseBlocksGameplay() noexcept {
    // A relay rejoin owns its seat through the relay, which admits one live
    // connection per seat; the lobby branch lease does not apply.
    if(hp_relay::Rejoining())return false;
    const auto deadline=leaseDeadline.load();
    if(deadline&&GetTickCount64()>=deadline)leaseHeld.store(true);
    return leaseHeld.load();
}
bool DepartureBlocksGameplay(void*) noexcept { return LeaseBlocksGameplay() || hota_departure::IsGameplayBlocked() || hota_leave::IsGameplayBlocked() || resume_session::IsGameplayBlocked(); }
bool HoldDepartureAtBoundary(void*) noexcept {
    if(resume_session::TryAtAdventureBoundary())return true;
    if(hota_session::IsScaffoldRestorePending())return true;
    if(hota_leave::TryAtAdventureBoundary() || hota_departure::TryHoldAtAdventureBoundary())return true;
    CaptureCurrentAtBoundary(); return false;
}
bool ReadResumeFrames(void*,hota_resume::Frames& frames) noexcept {
    if(GetCurrentThreadId()!=gameThreadId.load())return false;
    frames.save_depth=saveDepth;frames.turn_depth=turnDepth.load();frames.merge_depth=mergeDepth;frames.receive_depth=receiveDepth;
    frames.at_outer_idle_boundary=hota_adventure::IsAtIdleBoundary();
    frames.input_gate_installed=frameHooksInstalled;frames.receive_gate_installed=frameHooksInstalled;
    return frameHooksInstalled;
}
void AddRecoveryAudit(hota_capture::CaptureEnvelope& envelope,const hota_resume::Audit& before) {
    hota_resume::Audit after;std::string error;
    if(!hota_resume::ReadAudit(after,&error)||!hota_resume::SameWorldContent(before,after))
        throw std::runtime_error("world_changed_during_checkpoint_serialization:"+error);
    envelope.native_world_audit=hota_resume::EncodeAudit(after);
    unsigned roster=0;
    if(!hp::Read(hdBase.load()+0x11b8c84,roster)||!roster)throw std::runtime_error("native_roster_unavailable");
    envelope.hd_roster.resize(187);SIZE_T got=0;
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(roster),envelope.hd_roster.data(),187,&got)||got!=187)
        throw std::runtime_error("native_roster_unreadable");
    unsigned assignments=0,count=0,capacity=0;
    if(!hp::Read(hdBase.load()+0x21007a,assignments)||!hp::Read(hdBase.load()+0x21007e,count)||
       !hp::Read(hdBase.load()+0x210082,capacity)||count<1||count>4096||capacity<count||capacity>4096||!assignments)
        throw std::runtime_error("native_hero_assignment_layout_mismatch");
    envelope.hero_assignments.resize(count);
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(assignments),envelope.hero_assignments.data(),count,&got)||got!=count)
        throw std::runtime_error("native_hero_assignments_unreadable");
}
bool CanLeaveCompleted(const hota_leave::Request& request, void*) noexcept {
    if(GetCurrentThreadId()!=gameThreadId.load() || !hota_adventure::IsAtIdleBoundary() ||
       saveDepth || loadCalls.load()!=loadReturns.load() || turnDepth.load()) return false;
    std::lock_guard<std::mutex> lock(stateMutex);
    if(!completedArtifactWritten || captureStatus!="written" || completedWorld!=request.world_address ||
       completedLocal!=request.local_color || completedToken!=request.checkpoint_token ||
       completedToken!=completionMarkers.load()) return false;
    for(unsigned i=0;i<3;++i)if(completedDate[i]!=request.date_words[i])return false;
    return true;
}
bool CanHoldDeparture(void*) noexcept {
    return GetCurrentThreadId() == gameThreadId.load() && hota_adventure::IsAtIdleBoundary() &&
        saveDepth == 0 && loadCalls.load() == loadReturns.load() && turnDepth.load() == 0;
}
void DepartureLatched(const hota_departure::Snapshot&, void*) noexcept {
    // The module publishes its immutable observation and gate before this call.
    // Never serialize/load or perform network I/O from the native receive stack.
}

void CaptureCompletion(const hota_completion::Snapshot& snapshot, void*) noexcept {
    ++completionMarkers;
    resume_session::ObserveCompletion(snapshot);
    if (!completionCaptureRequested.exchange(false)) return;
    try {
        { std::lock_guard<std::mutex> lock(stateMutex); captureStatus = "copying"; captureError.clear(); }
        if (GetCurrentThreadId() != gameThreadId.load() || !snapshot.marker_confirmed || !snapshot.metadata_stable)
            throw std::runtime_error("native_completion_marker_not_confirmed");
        const bool serializeWorld = completionSnapshotRequested.exchange(false);
        hota_resume::Audit sourceAudit;std::string auditError;
        if(serializeWorld&&!hota_resume::ReadAudit(sourceAudit,&auditError))throw std::runtime_error(auditError);
        unsigned serializedResult = 0;
        if (serializeWorld) {
            // Explicit development experiment only. Use the original chained entry,
            // not a worker call or an old process's trampoline. Native Save preserves
            // HotA extensions and HD's MemFile interception for this known basename.
            if (saveDepth != 0 || loadCalls.load() != loadReturns.load() || turnDepth.load() != 1 ||
                snapshot.current_player != snapshot.local_player || snapshot.merge_failed != 0 || snapshot.stop_pending != 0)
                throw std::runtime_error("completion_snapshot_requires_uncontended_local_turn");
            const auto loadsBefore = loadCalls.load(), turnsBefore = turnCalls.load();
            using SaveFn = unsigned(__thiscall*)(void*, const char*, unsigned, unsigned, unsigned, unsigned);
            serializedResult = reinterpret_cast<SaveFn>(0x4BEB60)(reinterpret_cast<void*>(snapshot.world_address), "RMTGEXBS.BIN", 0, 0, 0, 1);
            if ((serializedResult & 255u) == 0) throw std::runtime_error("native_completion_save_failed");
            auto after = hota_completion::ReadSnapshot(snapshot.source_site, hdBase.load(), snapshot.local_player * 3);
            if (loadCalls.load() != loadsBefore || turnCalls.load() != turnsBefore ||
                !after.marker_confirmed || after.world_address != snapshot.world_address || after.local_player != snapshot.local_player ||
                after.current_player != snapshot.current_player || after.st_day != snapshot.st_day ||
                after.receive_active != snapshot.receive_active || after.merge_failed != snapshot.merge_failed ||
                after.merge_succeeded != snapshot.merge_succeeded || after.st_enabled != snapshot.st_enabled ||
                after.first_participant != snapshot.first_participant || after.barrier_pending != snapshot.barrier_pending ||
                after.ack_pending != snapshot.ack_pending || after.ui_phase != snapshot.ui_phase || after.stop_pending != snapshot.stop_pending ||
                after.world_date_words[0] != snapshot.world_date_words[0] || after.world_date_words[1] != snapshot.world_date_words[1] ||
                after.world_date_words[2] != snapshot.world_date_words[2])
                throw std::runtime_error("native_phase_changed_during_completion_save");
            for (unsigned i = 0; i < 8; ++i) {
                const auto& a = snapshot.participants[i]; const auto& b = after.participants[i];
                if (a.ack != b.ack || a.done != b.done || a.role != b.role || a.participates != b.participates ||
                    a.predecessor != b.predecessor || a.successor != b.successor)
                    throw std::runtime_error("native_participants_changed_during_completion_save");
            }
        }
        hota_capture::CaptureRequest request{};
        request.hd_base = hdBase.load(); request.engine_base = 0x400000;
        request.limits.max_total_bytes = 64u * 1024 * 1024;
        request.limits.max_stream_bytes = 32u * 1024 * 1024;
        request.limits.max_deferred_bytes = 4u * 1024 * 1024;
        auto result = std::make_unique<hota_capture::CaptureEnvelope>(hota_capture::CaptureExistingBranch(request));
        AddSessionState(*result);
        if (result->status != "captured" || !result->metadata_valid || result->local_player != snapshot.local_player || result->current_player != snapshot.current_player ||
            result->st_day != snapshot.st_day || !result->world_date_valid ||
            result->world_date_words[0] != snapshot.world_date_words[0] || result->world_date_words[1] != snapshot.world_date_words[1] ||
            result->world_date_words[2] != snapshot.world_date_words[2]) throw std::runtime_error("completion_capture_phase_mismatch");
        result->native_completion_observed = true;
        result->completion_marker_confirmed = snapshot.marker_confirmed;
        result->completion_source_site = static_cast<std::uint32_t>(snapshot.source_site);
        result->completion_player = snapshot.local_player;
        result->completion_fresh_world_serialized = serializeWorld;
        if (serializeWorld) {
            bool captured = false;
            AddRecoveryAudit(*result,sourceAudit);
            for (const auto& stream : result->streams)
                if (_stricmp(stream.name.c_str(), "RMTGEXBS.BIN") == 0 && stream.status == "present" && !stream.bytes.empty()) captured = true;
            if (!result->metadata_valid || !captured) throw std::runtime_error("fresh_completion_world_not_captured");
            result->native_save_observed = true; result->native_save_name_complete = true;
            result->native_save_name = "RMTGEXBS.BIN";
            result->native_save_name_bytes_hex = "524d5447455842532e42494e";
            result->native_save_flags[3] = 1; result->native_save_result = serializedResult;
            result->warnings.push_back("Experimental game-thread serialization at the completed-turn marker. Fresh world bytes do not prove full cold multiplayer recovery.");
        } else result->warnings.push_back("Captured at the native completed-turn marker before send; no fresh world serialization was requested. Existing streams may predate this turn.");
        { std::lock_guard<std::mutex> lock(captureMutex); detachedCapture = std::move(result); }
        { std::lock_guard<std::mutex> lock(stateMutex);
          if(serializeWorld) {
              completedWorld=static_cast<unsigned>(snapshot.world_address); completedLocal=snapshot.local_player;
              completedToken=completionMarkers.load();
              for(unsigned i=0;i<3;++i)completedDate[i]=snapshot.world_date_words[i];
          }
          captureStatus = "writing";
        }
        SetEvent(captureEvent);
    } catch (const std::exception& e) {
        ++observerErrors;
        try { std::lock_guard<std::mutex> lock(stateMutex); captureStatus = "failed"; captureError = e.what(); } catch (...) {}
    } catch (...) {
        ++observerErrors;
        try { std::lock_guard<std::mutex> lock(stateMutex); captureStatus = "failed"; captureError = "completion_capture_exception"; } catch (...) {}
    }
}

struct Build { const wchar_t* name; const char* hash; };
const Build builds[] = {
    {L"h3hota HD.exe", "5aaab925f06cccf23bb09814767590a95b84a557eb33d244800520be4f1f18de"},
    {L"HD_HOTA.dll", "0a2c193d0c510d77080b669a710694ef282761b0ec6f6ac5b86f3166b8038550"},
    {L"HW_HOTA.dll", "927a087c6d05d40c83de270516f48fdbd1360614259d30689179c1f7300f08db"},
    {L"patcher_x86.dll", "6134c353e5ac2a056e52bf548cdea853c4424fa5f955df1f5a1f073d784bab40"},
    {L"_HD3_.dll", "3ed1852e08bae7be2461b31bd922531ccb0c46d01e5b0ecf46d0e7d0dbeeab49"},
    {L"HotA.dll", "1fa9ad209e756d55e9578b290c4fe6c9ba030d3b52af2085e530416a61ac10c2"}
};

void CheckBuild() {
    if (fingerprintAccepted.load()) return;
    HMODULE modules[256]{}; DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &bytes) || bytes > sizeof modules) return;
    std::wstring root = hp::Canonical(hp::Parent(hp::ModulePath(nullptr)), true);
    bool accepted = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) == 0x400000;
    std::string reason = accepted ? "not_ready" : "unsupported_engine_base";
    std::ostringstream out; out << '[';
    for (size_t r = 0; r < _countof(builds); ++r) {
        HMODULE found = nullptr; std::wstring path;
        for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
            auto p = hp::ModulePath(modules[i]); auto basename = p.substr(p.find_last_of(L"\\/") + 1);
            if (_wcsicmp(basename.c_str(), builds[r].name) == 0 ||
                (r == 5 && _wcsicmp(basename.c_str(), L"HotA.dll_181fix") == 0)) { found = modules[i]; path = hp::Canonical(p); break; }
        }
        std::string hash = found ? hp::Sha256(path) : "";
        bool match = found && hp::Within(path, root) && hash == builds[r].hash;
        if (!match) { accepted = false; reason = found ? "fingerprint_mismatch" : "modules_not_ready"; }
        if (r) out << ',';
        out << "{\"name\":" << hp::Json(hp::Utf8(builds[r].name)) << ",\"path\":" << hp::Json(hp::Utf8(path))
            << ",\"sha256\":" << hp::Json(hash) << ",\"expectedSha256\":" << hp::Json(builds[r].hash)
            << ",\"matched\":" << (match ? "true" : "false") << '}';
        if (r == 1 && match) hdBase.store(reinterpret_cast<std::uintptr_t>(found));
    }
    out << ']';
    // An isolated-copy marker is also required for in-process hooks.
    hp::Handle marker(CreateFileW((root + L"\\.heroespost-owned-copy").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, 0, nullptr));
    char markerText[64]{}; DWORD count = 0;
    bool marked = marker && ReadFile(marker.value, markerText, sizeof markerText, &count, nullptr);
    std::string markerValue(markerText, count);
    while (!markerValue.empty() && (markerValue.back() == '\n' || markerValue.back() == '\r')) markerValue.pop_back();
    if (!marked || markerValue != "HeroesPost.Native.v1") { accepted = false; reason = "isolated_copy_marker_missing"; }
    std::lock_guard<std::mutex> lock(stateMutex);
    fingerprintJson = out.str(); fingerprintReason = accepted ? "accepted" : reason;
    fingerprintAccepted.store(accepted);
}

void CaptureAfterSave() noexcept {
    if (GetCurrentThreadId() != gameThreadId.load()) { ++observerErrors; return; }
    if (!captureRequested.exchange(false)) return;
    try {
        { std::lock_guard<std::mutex> lock(stateMutex); captureStatus = "copying"; captureError.clear(); }
        hota_capture::CaptureRequest request{};
        request.hd_base = hdBase.load(); request.engine_base = 0x400000;
        request.limits.max_total_bytes = 64u * 1024 * 1024;
        request.limits.max_stream_bytes = 32u * 1024 * 1024;
        request.limits.max_deferred_bytes = 4u * 1024 * 1024;
        auto result = std::make_unique<hota_capture::CaptureEnvelope>(hota_capture::CaptureExistingBranch(request));
        AddSessionState(*result);
        if(auditedCapturePending)AddRecoveryAudit(*result,pendingCaptureAudit);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            result->native_save_observed = true; result->native_save_name = lastSaveName;
            result->native_save_name_bytes_hex = lastSaveNameHex;
            result->native_save_name_complete = lastSaveNameComplete;
            result->native_save_result = lastSaveResult;
            for (unsigned i = 0; i < 4; ++i) result->native_save_flags[i] = lastSaveFlags[i];
        }
        { std::lock_guard<std::mutex> lock(captureMutex); detachedCapture = std::move(result); }
        { std::lock_guard<std::mutex> lock(stateMutex); captureStatus = "writing"; }
        SetEvent(captureEvent);
    } catch (const std::exception& error) {
        ++observerErrors;
        try { std::lock_guard<std::mutex> lock(stateMutex); captureStatus = "failed"; captureError = error.what(); } catch (...) {}
    }
}

void CaptureCurrentAtBoundary() noexcept {
    if(!currentCaptureRequested.load())return;
    try {
        if(saveDepth||turnDepth.load()||mergeDepth||receiveDepth||loadCalls.load()!=loadReturns.load())return;
        unsigned modal=0; if(!hp::Read(0x6aad88,modal)||modal)return;
        std::string error;hota_resume::Audit audit;
        if(!hota_resume::ReadAudit(audit,&error))throw std::runtime_error(error);
        if(audit.local!=audit.current)return;
        unsigned char row[6]{};
        if(!hp::Read(hdBase.load()+0x21004a+audit.local*6,row)||!row[3]||row[1])
            throw std::runtime_error("checkpoint_requires_unfinished_local_simultaneous_turn");
        currentCaptureRequested.store(false);pendingCaptureAudit=std::move(audit);auditedCapturePending=true;
        captureRequested.store(true);
        using Save=unsigned(__thiscall*)(void*,const char*,unsigned,unsigned,unsigned,unsigned);
        const auto result=reinterpret_cast<Save>(0x4beb60)(reinterpret_cast<void*>(pendingCaptureAudit.world),"RMTGEXBS.BIN",0,0,0,1);
        auditedCapturePending=false;
        if(!(result&255))throw std::runtime_error("native_current_branch_save_failed");
    }catch(const std::exception& error){
        currentCaptureRequested.store(false);captureRequested.store(false);auditedCapturePending=false;
        try{std::lock_guard<std::mutex> lock(stateMutex);captureStatus="failed";captureError=error.what();}catch(...){}
    }
}

// ABI verified against base instructions: Save ret14h; Load ret0Ch; Turn ret.
// The patcher EXTENDED_/THISCALL_ bridge supplies HiHook* then original ECX.
unsigned __stdcall SaveHook(HiHook* hook, void* self, const char* name, unsigned a, unsigned b, unsigned c, unsigned d) {
    ++saveCalls; ++saveDepth;
    char observedName[261]{}; unsigned observedLength = 0; bool nameComplete = false;
    for (unsigned i = 0; i < 260; ++i) {
        char ch = 0;
        if (!hp::Read(reinterpret_cast<std::uintptr_t>(name) + i, ch)) break;
        if (!ch) { nameComplete = true; break; }
        observedName[observedLength++] = ch;
    }
    using Fn = unsigned(__thiscall*)(void*, const char*, unsigned, unsigned, unsigned, unsigned);
    unsigned result = reinterpret_cast<Fn>(hook->GetDefaultFunc())(self, name, a, b, c, d);
    ++saveReturns; --saveDepth;
    bool observed = false;
    try {
        wchar_t wideName[261]{};
        int chars = MultiByteToWideChar(CP_ACP, 0, observedName, observedLength, wideName, 260);
        std::string text = hp::Utf8(std::wstring(wideName, chars));
        const char* hex = "0123456789abcdef"; std::string bytes;
        for (unsigned i = 0; i < observedLength; ++i) { auto byte = static_cast<unsigned char>(observedName[i]); bytes += hex[byte >> 4]; bytes += hex[byte & 15]; }
        std::lock_guard<std::mutex> lock(stateMutex);
        lastSaveName = std::move(text); lastSaveNameHex = std::move(bytes); lastSaveNameComplete = nameComplete;
        lastSaveFlags[0] = a; lastSaveFlags[1] = b; lastSaveFlags[2] = c; lastSaveFlags[3] = d;
        lastSaveResult = result;
        observed = true;
    } catch (...) { ++observerErrors; }
    // Base success/error paths write AL only; upper EAX bits are not a bool.
    if (observed && saveDepth == 0 && (result & 255u) != 0) CaptureAfterSave();
    return result;
}
unsigned __stdcall LoadHook(HiHook* hook, void* self, const char* name, unsigned a, unsigned b) {
    ++loadCalls;
    hota_resume::ObserveLoadEnter(reinterpret_cast<unsigned>(self),name,a,b);
    using Fn = unsigned(__thiscall*)(void*, const char*, unsigned, unsigned);
    unsigned result = reinterpret_cast<Fn>(hook->GetDefaultFunc())(self, name, a, b);
    ++loadReturns; hota_resume::ObserveLoadReturn(result);return result;
}
void __stdcall MergeFrameHook(HiHook* hook) {
    ++mergeDepth;reinterpret_cast<void(__cdecl*)()>(hook->GetDefaultFunc())();--mergeDepth;
}
unsigned __stdcall ReceiveFrameHook(HiHook* hook,unsigned a,unsigned b,int c,unsigned d,unsigned e) {
    ++receiveDepth;
    const auto result=reinterpret_cast<unsigned(__stdcall*)(unsigned,unsigned,int,unsigned,unsigned)>(hook->GetDefaultFunc())(a,b,c,d,e);
    --receiveDepth;return result;
}
void* __stdcall ResumePollHook(HiHook* hook,void* self,unsigned char remove,unsigned char* decompressed) {
    // The solo transport has no peers, so nothing may be delivered. A relay
    // rejoin's packets come from the live game and are dispatched normally.
    const bool soloPeers=hota_solo_transport::GetSnapshot().active&&!hp_relay::Rejoining();
    if(LeaseBlocksGameplay()||hota_resume::IsGameplayBlocked()||resume_session::IsGameplayBlocked()||soloPeers){
        if(decompressed){const unsigned char zero=0;SIZE_T wrote=0;WriteProcessMemory(GetCurrentProcess(),decompressed,&zero,1,&wrote);}
        return nullptr;
    }
    return reinterpret_cast<void*(__thiscall*)(void*,unsigned char,unsigned char*)>(hook->GetDefaultFunc())(self,remove,decompressed);
}
void __stdcall TurnHook(HiHook* hook, void* self) {
    ++turnCalls; ++turnDepth;
    using Fn = void(__thiscall*)(void*);
    reinterpret_cast<Fn>(hook->GetDefaultFunc())(self);
    --turnDepth; ++turnReturns;
    if(!turnDepth.load())resume_session::ObserveTurnReturned();
}
bool MenuHasNoWorld() {
    unsigned main = 0, map = 0;
    if (!hp::Read(0x699538, main)) return false;
    return main == 0 || (hp::Read(main + 0x1fc40, map) && map == 0);
}

LRESULT CALLBACK InstallOnGameThread(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && wparam == PM_REMOVE) {
        auto message = reinterpret_cast<MSG*>(lparam);
        if (message->message == bootstrapMessage && message->wParam == reinterpret_cast<WPARAM>(moduleHandle)) {
            message->message = WM_NULL;
            if (telemetryState.load() == 1 && fingerprintAccepted.load() && MenuHasNoWorld()) {
                Patcher* patcher = GetPatcher();
                PatcherInstance* instance = patcher ? patcher->CreateInstance("HeroesPost.Native.v1") : nullptr;
                HiHook* hooks[6]{};
                if (instance) {
                    hooks[0] = instance->WriteHiHook(0x4BEB60, SPLICE_, EXTENDED_, THISCALL_, reinterpret_cast<void*>(SaveHook));
                    hooks[1] = instance->WriteHiHook(0x4BEFF0, SPLICE_, EXTENDED_, THISCALL_, reinterpret_cast<void*>(LoadHook));
                    hooks[2] = instance->WriteHiHook(0x4C6CA0, SPLICE_, EXTENDED_, THISCALL_, reinterpret_cast<void*>(TurnHook));
                    hooks[3] = instance->WriteHiHook(hdBase.load()+0x11ea90, SPLICE_, EXTENDED_, CDECL_, reinterpret_cast<void*>(MergeFrameHook));
                    hooks[4] = instance->WriteHiHook(0x4CBA00, SPLICE_, EXTENDED_, STDCALL_, reinterpret_cast<void*>(ReceiveFrameHook));
                }
                bool good = true; for (unsigned i=0;i<5;++i) good = good && hooks[i] && hooks[i]->IsApplied();
                std::string completionError;
                if (good) good = hota_completion::Install(instance, hdBase.load(), CaptureCompletion, nullptr, &completionError);
                if (good) good = hota_session::Install(instance, &completionError);
                if (good) good = hota_port::Install(instance, &completionError);
                if (good) good = hota_start::Install(instance, hdBase.load(), &completionError);
                if (good) good = hota_adventure::Install(instance, hdBase.load(), &completionError);
                if (good) good = hota_adventure::ConfigureInputGate(DepartureBlocksGameplay, nullptr, &completionError);
                if (good) good = hota_departure::Install(instance, hdBase.load(), CanHoldDeparture, DepartureLatched, nullptr, &completionError);
                if (good) { hooks[5]=instance->WriteHiHook(0x553440,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(ResumePollHook));good=hooks[5]&&hooks[5]->IsApplied(); }
                if (good) good = hota_adventure::ConfigureIdleBoundary(HoldDepartureAtBoundary, nullptr, &completionError);
                if (good) good = hota_intro::Install(instance, &completionError);
                if (good) good = hota_leave::Install(hdBase.load(), CanLeaveCompleted, nullptr, &completionError);
                frameHooksInstalled=good;
                if (good) good = hota_resume::Initialize(hdBase.load(),gameThreadId.load(),{nullptr,ReadResumeFrames},&completionError);
                if (good) good = resume_session::Initialize(instance,hdBase.load(),gameThreadId.load(),&completionError);
                if (good) good = hota_menu::Install(instance, BeginSession, nullptr, &completionError);
                if (!good) {
                    hota_menu::Uninstall();
                    hota_intro::Uninstall();
                    hota_session::Uninstall();
                    hota_port::Uninstall();
                    hota_start::Uninstall();
                    hota_departure::Uninstall();
                    hota_adventure::Uninstall();
                    hota_completion::Uninstall();
                    for (auto hook : hooks) if (hook) hook->Destroy();
                    std::lock_guard<std::mutex> lock(stateMutex); hookError = completionError.empty() ? "patcher_hook_install_failed" : completionError;
                }
                telemetryState.store(good ? 2 : -1);
            } else {
                std::lock_guard<std::mutex> lock(stateMutex);
                hookError = "installation_requires_menu_without_loaded_world"; telemetryState.store(-1);
            }
            HHOOK old = bootstrapHook; bootstrapHook = nullptr; if (old) UnhookWindowsHookEx(old);
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
BOOL CALLBACK FindGameWindow(HWND window, LPARAM) {
    DWORD pid = 0; DWORD tid = GetWindowThreadProcessId(window, &pid);
    wchar_t cls[128]{}; GetClassNameW(window, cls, _countof(cls));
    if (pid == GetCurrentProcessId() && _wcsicmp(cls, L"Heroes III") == 0) {
        gameThreadId.store(tid); return FALSE;
    }
    return TRUE;
}
std::string EnableTelemetry() {
    CheckBuild();
    if (!fingerprintAccepted.load()) return hp::Error("unsupported_build", "Pinned loaded-module fingerprints and isolated-copy marker are required.");
    if (telemetryState.load() == 2) return "{\"protocol\":1,\"ok\":true,\"status\":\"active\"}";
    if (telemetryState.load() == 1) return "{\"protocol\":1,\"ok\":true,\"status\":\"pending_game_thread\"}";
    if (telemetryState.load() == -1) return hp::Error("hook_install_failed", "Telemetry failed; restart the owned client before retrying.");
    if (!MenuHasNoWorld()) return hp::Error("loaded_game", "Install telemetry from the initial menu before loading or creating a game.");
    EnumWindows(FindGameWindow, 0);
    if (!gameThreadId.load()) return hp::Error("not_ready", "The native game window is not ready.");
    telemetryState.store(1);
    bootstrapHook = SetWindowsHookExW(WH_GETMESSAGE, InstallOnGameThread, moduleHandle, gameThreadId.load());
    if (!bootstrapHook || !PostThreadMessageW(gameThreadId.load(), bootstrapMessage, reinterpret_cast<WPARAM>(moduleHandle), 0)) {
        if (bootstrapHook) UnhookWindowsHookEx(bootstrapHook); bootstrapHook = nullptr;
        telemetryState.store(-1); return hp::Error("bootstrap_failed", "Could not queue initialization on the game thread.");
    }
    return "{\"protocol\":1,\"ok\":true,\"status\":\"pending_game_thread\"}";
}

std::string Probe() {
    CheckBuild();
    unsigned main = 0; int current = -1, local = -1, network = -1; unsigned short date[3]{};
    bool supported = fingerprintAccepted.load();
    bool mainRead = supported && hp::Read(0x699538, main) && main != 0;
    bool dateRead = mainRead && hp::Read(main + 0x1f63e, date) &&
        date[0] >= 1 && date[0] <= 7 && date[1] >= 1 && date[1] <= 4 && date[2] >= 1;
    if (supported) { hp::Read(0x69ccf4, current); hp::Read(0x6995a4, local); hp::Read(0x69959c, network); }
    const auto session = hota_session::GetSnapshot();
    const auto port = hota_port::GetSnapshot();
    const auto start = hota_start::GetSnapshot();
    const auto action = hota_adventure::GetSnapshot();
    const auto departure = hota_departure::GetSnapshot();
    const auto intro = hota_intro::GetSnapshot();
    const auto leave = hota_leave::GetSnapshot();
    const auto restored = hota_resume::GetSnapshot();
    unsigned modalDepth=0, moveReminder=0, exitPending=0, protectedExit=0;
    unsigned char stRows[8][6]{};
    if(supported) {
        hp::Read(0x6aad88,modalDepth); hp::Read(0x6987c8,moveReminder);
        hp::Read(hdBase.load()+0x21004a,stRows);
        hp::Read(hdBase.load()+0x217508,exitPending); hp::Read(hdBase.load()+0x2176ec,protectedExit);
    }
    unsigned map = 0, scenario = 1, simultaneous = 0;
    const bool mapPresent = mainRead && hp::Read(main + 0x1fc40, map) && map != 0;
    if (supported) { hp::Read(0x69fc44, scenario); if (hdBase.load()) hp::Read(hdBase.load() + 0x210042, simultaneous); }
    // The combat manager's state is 1 from the start of a battle until its result dialog closes.
    unsigned combatManager = 0, combatState = 0;
    const bool combat = supported && hp::Read(0x699420, combatManager) && combatManager && hp::Read(combatManager + 0x34, combatState) && combatState == 1;
    std::lock_guard<std::mutex> lock(stateMutex);
    const char* status = telemetryState.load() == 2 ? "active" : telemetryState.load() == 1 ? "pending_game_thread" : telemetryState.load() == -1 ? "failed" : "disabled";
    std::ostringstream o;
    o << "{\"protocol\":1,\"ok\":true,\"pid\":" << GetCurrentProcessId()
      << ",\"createdFiletime\":" << hp::Json(std::to_string(hp::CreationTime(GetCurrentProcess())))
      << ",\"relay\":" << hp_relay::StatusJson() << ",\"relayError\":" << hp::Json(relayError)
      << ",\"fingerprintAccepted\":" << (supported ? "true" : "false")
      << ",\"fingerprintReason\":" << hp::Json(fingerprintReason) << ",\"modules\":" << fingerprintJson
      << ",\"telemetry\":{\"status\":" << hp::Json(status) << ",\"error\":" << hp::Json(hookError)
      << ",\"gameThreadId\":" << gameThreadId.load() << ",\"saveCalls\":" << saveCalls.load() << ",\"saveReturns\":" << saveReturns.load()
      << ",\"observerErrors\":" << observerErrors.load()
      << ",\"completionMarkers\":" << completionMarkers.load()
      << ",\"loadCalls\":" << loadCalls.load() << ",\"loadReturns\":" << loadReturns.load()
      << ",\"turnCalls\":" << turnCalls.load() << ",\"turnReturns\":" << turnReturns.load() << ",\"turnDepth\":" << turnDepth.load() << '}'
      << ",\"game\":{\"mainPresent\":" << (mainRead ? "true" : "false") << ",\"currentColor\":" << current << ",\"localColor\":" << local
      << ",\"network\":" << network << ",\"dateValid\":" << (dateRead ? "true" : "false") << ",\"date\":{\"day\":" << date[0] << ",\"week\":" << date[1] << ",\"month\":" << date[2] << "}"
      << ",\"adventureMapPresent\":" << (mapPresent ? "true" : "false") << ",\"scenarioOpen\":" << (scenario ? "true" : "false")
      << ",\"simultaneousTurns\":" << (simultaneous == 1 ? "true" : "false")
      << ",\"combatActive\":" << (combat ? "true" : "false") << '}'
      << ",\"nativePhase\":{\"modalDepth\":" << modalDepth << ",\"moveReminder\":" << moveReminder
      << ",\"exitPending\":" << exitPending << ",\"protectedExit\":" << protectedExit << ",\"participants\":[";
    for(unsigned color=0;color<8;++color) {
        if(color)o<<',';o<<'[';
        for(unsigned field=0;field<6;++field){if(field)o<<',';o<<unsigned(stRows[color][field]);}
        o<<']';
    }
    o << "]}"
      << ",\"soloResume\":" << resume_session::SerializeSnapshot()
      << ",\"worldRestore\":{\"status\":" << hp::Json(hota_resume::StatusName(restored.status))
      << ",\"error\":" << hp::Json(restored.error) << ",\"loadInvoked\":" << (restored.load_invoked?"true":"false")
      << ",\"loadSuccess\":" << (restored.load_success?"true":"false")
      << ",\"sourceAuditMatched\":" << (restored.source_audit_matched?"true":"false")
      << ",\"beforeLocal\":" << restored.before.local << ",\"afterLocal\":" << restored.after.local
      << ",\"afterCurrent\":" << restored.after.current
      << ",\"afterAudit\":" << (restored.audit_read?hota_resume::SerializeAudit(restored.after):"null") << '}'
      << ",\"leaseHeld\":" << (LeaseBlocksGameplay()?"true":"false")
      << ",\"captureRoot\":" << hp::Json(hp::Utf8(captureRoot)) << ",\"captureStatus\":" << hp::Json(captureStatus)
      << ",\"completionArmed\":" << (completionCaptureRequested.load() ? "true" : "false")
      << ",\"captureError\":" << hp::Json(captureError) << ",\"lastCaptureManifest\":" << hp::Json(lastCaptureManifest)
      << ",\"sessionBootstrap\":{\"status\":" << hp::Json(hota_session::StatusName(session.status))
      << ",\"token\":" << session.token << ",\"localPlayerId\":" << session.local_player_id
      << ",\"nativeHost\":" << (session.native_host ? "true" : "false")
      << ",\"mapApplied\":" << (session.map_applied ? "true" : "false")
      << ",\"gameStarted\":" << (session.game_started ? "true" : "false")
      << ",\"error\":" << hp::Json(session.error)
      << ",\"nativeOperation\":" << hp::Json(session.native_operation)
      << ",\"nativeCallResult\":" << session.native_call_result
      << ",\"nativeHresult\":" << session.native_hresult
      << ",\"nativeHresultValid\":" << (session.native_hresult_valid ? "true" : "false")
      << ",\"menuStatus\":" << static_cast<unsigned>(hota_menu::GetStatus()) << '}'
      << ",\"nativeMenuReady\":" << (hota_menu::IsReady() ? "true" : "false")
      << ",\"nativePort\":{\"value\":" << port.port << ",\"queryApplied\":" << port.query_applied
      << ",\"fillApplied\":" << port.fill_applied << ",\"error\":" << hp::Json(hota_port::ErrorName(port.error)) << '}'
      << ",\"nativeStart\":{\"status\":" << hp::Json(hota_start::StatusName(start.status)) << ",\"token\":" << start.token
      << ",\"detail\":" << hp::Json(start.detail) << ",\"assignedHumans\":" << start.assigned_humans
      << ",\"pendingTransfers\":" << start.pending_transfers << ",\"optionsMode\":" << start.options_mode
      << ",\"checkbox\":" << start.checkbox << ",\"dateMWD\":" << start.date_mwd << ",\"nativeResult\":" << start.native_result
      << ",\"setupReady\":" << (start.setup_ready ? "true" : "false")
      << ",\"startEventSent\":" << (start.start_event_sent ? "true" : "false")
      << ",\"optionsAccepted\":" << (start.options_accepted ? "true" : "false")
      << ",\"gameplayVerified\":" << (start.gameplay_verified ? "true" : "false") << '}'
      << ",\"adventureAction\":{\"status\":" << hp::Json(hota_adventure::StatusName(action.status))
      << ",\"error\":" << hp::Json(hota_adventure::ErrorName(action.error)) << ",\"token\":" << action.token
      << ",\"nativeResult\":" << action.native_result << '}'
      << ",\"departurePause\":{\"status\":" << hp::Json(hota_departure::StatusName(departure.status))
      << ",\"detail\":" << hp::Json(departure.detail) << ",\"token\":" << departure.token
      << ",\"queueCount\":" << departure.queue_count << ",\"queueCursor\":" << departure.queue_cursor
      << ",\"packetAddress\":" << departure.packet_address
      << ",\"gameplayBlocked\":" << (departure.gameplay_blocked ? "true" : "false")
      << ",\"preMutationVerified\":" << (departure.pre_mutation_verified ? "true" : "false")
      << ",\"safeLogout\":false,\"resumeSupported\":false}"
      << ",\"mapIntro\":{\"status\":" << hp::Json(hota_intro::StatusName(intro.status))
      << ",\"error\":" << hp::Json(intro.error) << ",\"callbackCalls\":" << intro.callback_calls
      << ",\"nativeOkConfirmed\":" << (intro.native_ok_confirmed ? "true" : "false") << '}'
      << ",\"completedLeave\":{\"status\":" << hp::Json(hota_leave::StatusName(leave.status))
      << ",\"detail\":" << hp::Json(leave.detail)
      << ",\"nativeCallReturned\":" << (leave.native_call_returned ? "true" : "false")
      << ",\"transportNull\":" << (leave.transport_null ? "true" : "false")
      << ",\"networkModeZero\":" << (leave.network_mode_zero ? "true" : "false")
      << ",\"gameplayBlocked\":" << (leave.gameplay_blocked ? "true" : "false")
      << ",\"serverReceiptVerified\":false,\"resumeSupported\":false}"
      << ",\"lastNativeSave\":{\"name\":" << hp::Json(lastSaveName) << ",\"nameComplete\":" << (lastSaveNameComplete ? "true" : "false")
      << ",\"nameBytesHex\":" << hp::Json(lastSaveNameHex)
      << ",\"flags\":[" << lastSaveFlags[0] << ',' << lastSaveFlags[1] << ',' << lastSaveFlags[2] << ',' << lastSaveFlags[3]
      << "],\"rawResult\":" << lastSaveResult << ",\"returnByte\":" << (lastSaveResult & 255u) << ",\"worldArtifactIncluded\":false}"
      << ",\"capabilities\":{\"telemetry\":true,\"captureAfterNativeSave\":true,\"captureAtNativeCompletion\":true,\"freshCompletionWorld\":true,\"captureCurrentBranch\":true,\"directSave\":false,\"directLoad\":false,\"advanceTurn\":false,\"resumeBranch\":"
      << (resume_session::GetSnapshot().initialized?"true":"false") << ",\"mergeBranches\":false,\"completeRestore\":false}}";
    return o.str();
}
std::string Command(const std::string& input) {
    std::string command;
    if (!hp::ParseCommand(input, command)) return hp::Error("invalid_request", "Expected exactly one command string field.");
    if (command == "probe") return Probe();
    if (command == "enable_telemetry") return EnableTelemetry();
    if(command=="hold_session") {
        if(telemetryState.load()!=2)return hp::Error("not_ready","The native gameplay gate is not installed.");
        leaseHeld.store(true);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"held\"}";
    }
    if(command=="renew_lease_fence") {
        if(telemetryState.load()!=2||LeaseBlocksGameplay())return hp::Error("lease_expired","An expired or held native client cannot be reactivated.");
        leaseDeadline.store(GetTickCount64()+60000);
        if(LeaseBlocksGameplay())return hp::Error("lease_expired","The native lease expired during renewal.");
        return "{\"protocol\":1,\"ok\":true,\"expiresInSeconds\":60}";
    }
    if (command == "capture_current_branch") {
        if(telemetryState.load()!=2||hota_session::IsScaffoldRestorePending())return hp::Error("not_ready","Native gameplay is not initialized.");
        std::lock_guard<std::mutex> lock(stateMutex);
        if(captureStatus=="copying"||captureStatus=="writing"||currentCaptureRequested.load())return hp::Error("capture_busy","A native checkpoint is in progress.");
        // A pending End Turn capture stays armed: it is taken at the completion marker, this one at an idle moment.
        captureRequested.store(false);
        captureStatus="armed";captureError.clear();currentCaptureRequested.store(true);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"queued_current_branch\"}";
    }
    if (command == "resume_session") {
        if(telemetryState.load()!=2||!MenuHasNoWorld())return hp::Error("not_ready","Resume requires a fresh isolated client at its main menu.");
        const auto path=hp::Parent(hp::ModulePath(nullptr))+L"\\HeroesPost.resume.bin";
        if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return hp::Error("missing_resume","No recovery bundle is staged.");
        {std::lock_guard<std::mutex> lock(stateMutex);
         if(captureStatus=="copying"||captureStatus=="writing")return hp::Error("capture_busy","A native capture is still being written.");
         captureRequested.store(false);currentCaptureRequested.store(false);completionCaptureRequested.store(false);completionSnapshotRequested.store(false);
         captureStatus="idle";captureError.clear();}
        return "{\"protocol\":1,\"ok\":true,\"status\":\"awaiting_solo_bootstrap\"}";
    }
    if (command == "experiment_leave_completed_session") {
        if (!fingerprintAccepted.load() || telemetryState.load() != 2) return hp::Error("not_ready", "The pinned adapter must be active.");
        const auto start=hota_start::GetSnapshot();const auto session=hota_session::GetSnapshot();
        if(session.native_host || !session.local_player_id || !start.options_accepted)
            return hp::Error("not_ready","Requires the started experimental non-host client.");
        hota_leave::Request request;request.token=++bootstrapSequence;request.local_dpid=session.local_player_id;
        { std::lock_guard<std::mutex> lock(stateMutex);
          if(!completedArtifactWritten || captureStatus!="written")return hp::Error("not_ready","A fresh completed-turn artifact must have finished writing.");
          request.world_address=completedWorld;request.local_color=completedLocal;request.checkpoint_token=completedToken;
          for(unsigned i=0;i<3;++i)request.date_words[i]=completedDate[i];
        }
        for(unsigned i=0;i<2;++i)if(start.peer_ids[i]!=request.local_dpid){request.remote_dpid=start.peer_ids[i];request.remote_color=start.colors[i];}
        std::string error;if(!hota_leave::Arm(request,&error))return hp::Error("leave_rejected",error);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"armed_native_teardown\",\"serverReceiptVerified\":false,\"resumeSupported\":false}";
    }
    if (command == "experiment_acknowledge_known_intro") {
        if (!fingerprintAccepted.load() || telemetryState.load() != 2) return hp::Error("not_ready", "The pinned adapter must be active.");
        hota_intro::Request request; request.token=++bootstrapSequence;
        { std::lock_guard<std::mutex> lock(stateMutex); request.map_basename=preparedMap; }
        unsigned short date[3]{};
        if((!hota_start::GetSnapshot().options_accepted&&!hota_session::IsScaffoldRestorePending()) || !hp::Read(0x699538,request.expected_world) || !request.expected_world ||
           !hp::Read(0x6995a4,request.expected_local) || !hp::Read(request.expected_world+0x1f63e,date))
            return hp::Error("not_ready","Requires the initialized experimental map.");
        request.day=date[0];request.week=date[1];request.month=date[2];std::string error;
        if(!hota_intro::QueueKnownIntro(request,&error))return hp::Error("intro_rejected",error);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"queued_known_intro\"}";
    }
    if (command == "experiment_hold_completed_departure") {
        if (!fingerprintAccepted.load() || telemetryState.load() != 2) return hp::Error("not_ready", "The pinned adapter must be active.");
        const auto start = hota_start::GetSnapshot();
        const auto session = hota_session::GetSnapshot();
        hota_departure::Request request;
        request.token=++bootstrapSequence;
        // Development-only correlation, NOT a durable checkpoint receipt.
        request.checkpoint_token=request.token;
        if(!session.native_host || !session.local_player_id || !start.options_accepted ||
           !hp::Read(0x699538,request.world_address) || !request.world_address ||
           !hp::Read(request.world_address+0x1f63e,request.date_words) ||
           !hp::Read(0x6995a4,request.local_color)) return hp::Error("not_ready","Requires the started experimental host.");
        request.local_dpid=session.local_player_id;
        for(unsigned i=0;i<2;++i) if(start.peer_ids[i]!=request.local_dpid) {
            request.remote_dpid=start.peer_ids[i];request.remote_color=start.colors[i];
        }
        std::string error;
        if(!hota_departure::Arm(request,&error))return hp::Error("departure_rejected",error);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"armed\",\"durableCheckpointVerified\":false,\"safeLogout\":false,\"resumeSupported\":false}";
    }
    if (command == "experiment_end_turn") {
        if (!fingerprintAccepted.load() || telemetryState.load() != 2) return hp::Error("not_ready", "The pinned adapter must be active.");
        unsigned world=0; int local=-1; unsigned short date[3]{};
        if(!hp::Read(0x699538,world)||!world||!hp::Read(world+0x1f63e,date)||!hp::Read(0x6995a4,local))
            return hp::Error("not_ready","The native world/owner/date is unreadable.");
        hota_adventure::Request request;request.token=++bootstrapSequence;request.expected_local=local;
        request.day=date[0];request.week=date[1];request.month=date[2];std::string error;
        if(!hota_adventure::QueueEndTurn(request,&error))return hp::Error("action_rejected",error);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"queued\",\"completionVerified\":false}";
    }
    if (command.rfind("experiment_click_tile_",0)==0) {
        if (!fingerprintAccepted.load() || telemetryState.load() != 2) return hp::Error("not_ready", "The pinned adapter must be active.");
        unsigned world=0; int local=-1; unsigned short date[3]{}; int x=-1,y=-1,z=-1;
        if(sscanf_s(command.c_str()+22,"%d_%d_%d",&x,&y,&z)!=3)return hp::Error("invalid_request","Expected experiment_click_tile_X_Y_Z.");
        if(!hp::Read(0x699538,world)||!world||!hp::Read(world+0x1f63e,date)||!hp::Read(0x6995a4,local))
            return hp::Error("not_ready","The native world/owner/date is unreadable.");
        hota_adventure::Request request;request.token=++bootstrapSequence;request.expected_local=local;
        request.day=date[0];request.week=date[1];request.month=date[2];request.kind=hota_adventure::Kind::MapPress;
        request.x=static_cast<std::int16_t>(x);request.y=static_cast<std::int16_t>(y);request.z=static_cast<std::uint8_t>(z);std::string error;
        if(!hota_adventure::QueueMapPress(request,&error))return hp::Error("action_rejected",error);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"queued\"}";
    }
    if (command == "start_session") {
        if (!fingerprintAccepted.load() || telemetryState.load() != 2)
            return hp::Error("not_ready", "Starting requires the pinned active adapter and native scenario setup.");
        hota_request::StartSettings settings; std::string error;
        if (!hota_request::ReadStart(hp::Parent(hp::ModulePath(nullptr)) + L"\\HeroesPost.start.ini", settings, &error))
            return hp::Error("invalid_start_request", error);
        hota_start::Request request; request.token=++bootstrapSequence;
        request.action=settings.action=="HostStart"?hota_start::Action::HostStart:hota_start::Action::PeerAccept;
        request.peer_ids[0]=settings.peer_ids[0];request.peer_ids[1]=settings.peer_ids[1];
        request.map_basename=settings.map_basename;request.date_mwd=settings.date_mwd;
        if(!hota_start::Queue(request,&error))return hp::Error("start_rejected",error);
        return "{\"protocol\":1,\"ok\":true,\"status\":\"queued\",\"gameStarted\":false}";
    }
    if(command=="cancel_start")return hota_start::CancelWaiting()
        ? "{\"protocol\":1,\"ok\":true,\"status\":\"cancellation_requested\"}"
        : hp::Error("start_in_progress","Only a waiting start request can be cancelled.");
    if (command == "bootstrap_session") {
        if (!fingerprintAccepted.load() || telemetryState.load() != 2 || !MenuHasNoWorld())
            return hp::Error("not_ready", "Bootstrap requires the pinned isolated client at the initial menu.");
        hota_request::Settings settings; std::string error;
        if (!hota_request::Read(hp::Parent(hp::ModulePath(nullptr)) + L"\\HeroesPost.session.ini", settings, &error))
            return hp::Error("invalid_bootstrap_request", error);
        hota_session::Request request;
        request.token = ++bootstrapSequence;
        request.role = settings.action == "Host" ? hota_session::Role::Host : settings.action=="SoloScaffold"?hota_session::Role::SoloScaffold:hota_session::Role::Join;
        request.player_name = settings.player_name; request.session_name = settings.session_name;
        request.address = settings.address; request.map_basename = settings.map_basename;
        request.port = settings.port;
        if(request.role==hota_session::Role::SoloScaffold && !resume_session::QueueFile(
            hp::Parent(hp::ModulePath(nullptr))+L"\\HeroesPost.resume.bin",request.token,&error))return hp::Error("resume_rejected",error);
        if (!hota_session::Prepare(request, &error)) return hp::Error("bootstrap_rejected", error);
        if (!hota_menu::Queue(request.token)) {
            hota_session::CancelPrepared(request.token);
            return hp::Error("native_menu_not_ready", "The native main menu must be active without another dialog.");
        }
        { std::lock_guard<std::mutex> lock(stateMutex); preparedMap=settings.map_basename; }
        return "{\"protocol\":1,\"ok\":true,\"status\":\"queued\",\"gameStarted\":false}";
    }
    if (command == "cancel_bootstrap") {
        hota_menu::CancelQueued();
        const bool cancelled = hota_session::CancelQueued();
        return cancelled ? "{\"protocol\":1,\"ok\":true,\"status\":\"cancelled\"}" : hp::Error("bootstrap_in_progress", "Only an unclaimed bootstrap request can be cancelled.");
    }
    if (command == "capture_next_save" || command == "capture_next_completion" || command == "experiment_snapshot_next_completion") {
        if (telemetryState.load() != 2) return hp::Error("not_ready", "Telemetry must be active; perform a native save after arming capture.");
        std::lock_guard<std::mutex> lock(stateMutex);
        if (captureStatus == "armed" || captureStatus == "copying" || captureStatus == "writing") return hp::Error("capture_busy", "A capture is already pending.");
        captureStatus = "armed"; captureError.clear();
        completedArtifactWritten=false;
        captureRequested.store(command == "capture_next_save");
        completionSnapshotRequested.store(command == "experiment_snapshot_next_completion");
        completionCaptureRequested.store(command != "capture_next_save");
        return "{\"protocol\":1,\"ok\":true,\"status\":\"armed\",\"completeRestore\":false}";
    }
    return hp::Error("unsupported_command", "Unsupported native adapter command.");
}

DWORD WINAPI CaptureWriter(void*) {
    unsigned sequence = 0;
    for (;;) {
        WaitForSingleObject(captureEvent, INFINITE);
        std::unique_ptr<hota_capture::CaptureEnvelope> envelope;
        { std::lock_guard<std::mutex> lock(captureMutex); envelope = std::move(detachedCapture); }
        if (!envelope) continue;
        auto directory = captureRoot + L"\\" + std::to_wstring(++sequence);
        std::string error;
        bool ok = false;
        try {
            std::filesystem::create_directories(captureRoot);
            ok = hota_capture::WriteEnvelope(*envelope, directory, &error);
        } catch (const std::exception& e) { error = e.what(); }
          catch (...) { error = "capture_root_creation_failed"; }
        std::lock_guard<std::mutex> lock(stateMutex);
        captureStatus = ok ? "written" : "failed"; captureError = error;
        completedArtifactWritten=ok && envelope->native_completion_observed && envelope->completion_marker_confirmed && envelope->completion_fresh_world_serialized;
        if (ok) lastCaptureManifest = hp::Utf8(directory + L"\\manifest.json");
    }
}

PSECURITY_DESCRIPTOR UserSecurity() {
    hp::Handle token; HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return nullptr;
    token.value = raw;
    DWORD length = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> data(length);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), length, &length)) return nullptr;
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) return nullptr;
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
    return descriptor;
}
bool FinishIo(HANDLE pipe, OVERLAPPED& ov, BOOL immediate, DWORD timeout, DWORD& count) {
    if (!immediate && GetLastError() != ERROR_IO_PENDING) return false;
    if (!immediate && WaitForSingleObject(ov.hEvent, timeout) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &count, TRUE); return false;
    }
    return GetOverlappedResult(pipe, &ov, &count, FALSE) != FALSE;
}
DWORD WINAPI PipeServer(void*) {
    auto security = UserSecurity(); if (!security) return 1;
    SECURITY_ATTRIBUTES sa{sizeof sa, security, FALSE};
    for (;;) {
        hp::Handle pipe(CreateNamedPipeW(hp::PipeName(GetCurrentProcessId()).c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 16384, 4096, 0, &sa));
        if (!pipe) { LocalFree(security); return 2; }
        hp::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{}; ov.hEvent = event.value; DWORD count = 0;
        BOOL connected = ConnectNamedPipe(pipe.value, &ov);
        bool ready = !connected && GetLastError() == ERROR_PIPE_CONNECTED;
        if (!ready) ready = FinishIo(pipe.value, ov, connected, INFINITE, count);
        std::string input; bool valid = ready;
        while (valid && input.size() < 4096 && input.find('\n') == input.npos) {
            char buffer[512]; ResetEvent(event.value); ov = {}; ov.hEvent = event.value;
            BOOL read = ReadFile(pipe.value, buffer, sizeof buffer, &count, &ov);
            valid = FinishIo(pipe.value, ov, read, 5000, count) && count != 0;
            if (valid) input.append(buffer, count);
        }
        std::string response;
        try {
            response = valid && input.size() <= 4096 && input.find('\n') != input.npos
                ? Command(input) : hp::Error("invalid_request", "Request must end in newline and fit 4096 bytes.");
        } catch (...) { response = hp::Error("internal_error", "Native command processing failed."); }
        response += '\n'; ResetEvent(event.value); ov = {}; ov.hEvent = event.value;
        BOOL written = WriteFile(pipe.value, response.data(), static_cast<DWORD>(response.size()), &count, &ov);
        FinishIo(pipe.value, ov, written, 5000, count);
        // Disconnect only after the caller has had an opportunity to consume bytes;
        // no FlushFileBuffers (which lets a local client block the server forever).
        ResetEvent(event.value); ov = {}; ov.hEvent = event.value;
        char eof = 0; BOOL read = ReadFile(pipe.value, &eof, 1, &count, &ov);
        FinishIo(pipe.value, ov, read, 1000, count);
        DisconnectNamedPipe(pipe.value);
    }
}
}

extern "C" __declspec(dllexport) DWORD WINAPI HeroesPostInitialize(void*) {
    if (started.exchange(true)) return 0;
    // Called explicitly after LoadLibrary has completed, never under DllMain.
    captureRoot = hp::Parent(hp::ModulePath(moduleHandle)) + L"\\captures\\" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(hp::CreationTime(GetCurrentProcess()));
    captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent) return 1;
    // The relay replaces DirectPlay only when the owned copy carries a relay
    // configuration. It must be installed before the engine first creates its
    // network object, which happens when a multiplayer screen opens.
    {
        const auto root = hp::Parent(hp::ModulePath(nullptr));
        hp_relay::Config relay; std::string error;
        if (hp_relay::ReadConfig(root + L"\\HeroesPost.relay.ini", relay, &error)) {
            relay.log_path = root + L"\\HeroesPost.relay.log";
            if (!hp_relay::Install(relay, &error)) relayError = error;
        } else relayError = error;
    }
    hp::Handle writer(CreateThread(nullptr, 0, CaptureWriter, nullptr, 0, nullptr));
    hp::Handle server(CreateThread(nullptr, 0, PipeServer, nullptr, 0, nullptr));
    return writer && server ? 0 : 2;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) { moduleHandle = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
