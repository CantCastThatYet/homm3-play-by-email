#include "departure_pause.hpp"
#include "supported_build.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include "vendor/patcher_x86.hpp"
static_assert(sizeof(void*)==4,"Departure pause requires x86");
namespace hota_departure {
namespace {
HiHook* hook=nullptr;
std::uintptr_t hd=0;
DWORD game_thread=0;
GuardCallback guard=nullptr;
LatchedCallback latched=nullptr;
void* context=nullptr;
std::atomic<bool> installed{false},blocked{false};
SRWLOCK lock=SRWLOCK_INIT;
Request request;
Snapshot published;
std::uint64_t request_revision=0;
thread_local unsigned depth=0;
std::uint32_t held_transport=0;
void Error(std::string* out,const char* text) noexcept {if(out)try{*out=text;}catch(...) {}}
void Detail(Snapshot& s,const char* text) noexcept {strncpy_s(s.detail,text,_TRUNCATE);}
bool Read(std::uintptr_t at,void* out,std::size_t size) noexcept {
    if(!at||size>UINT32_MAX-at)return false;
#ifdef HEROESPOST_DEPARTURE_TEST_READER
    // Defined only by the standalone synthetic test translation unit.
    return HEROESPOST_DEPARTURE_TEST_READER(at,out,size);
#else
    SIZE_T n=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),out,size,&n)&&n==size;
#endif
}
template<class T> bool Read(std::uintptr_t at,T& value) noexcept {return Read(at,&value,sizeof value);}
std::uintptr_t HD(std::uint32_t address) noexcept {return hd+hp_build::Rva(address-0x1000000);}
bool Bytes(std::uintptr_t at,const unsigned char* expected,std::size_t size) noexcept {
    unsigned char actual[32]{};return size<=sizeof actual&&Read(at,actual,size)&&!std::memcmp(actual,expected,size);
}
bool Pin() noexcept {
    const unsigned char entry[]={0x55,0x8b,0xec,0x83,0xec,0x18,0x8b,0x45,0x0c,0x53,0x56,0x57,0x85,0xc0,0x8b,0xf1};
    const unsigned char body[]={0x8b,0x86,0x94,0,0,0,0x85,0xc0,0x75,0x0b,0x5f,0x5e,0x33,0xc0,0x5b,0x8b,0xe5,0x5d,0xc2,8,0};
    const unsigned char tail[]={0x8b,0xc7,0x5f,0x5e,0x5b,0x8b,0xe5,0x5d,0xc2,8,0};
    const unsigned char host[]={0x8a,0x41,0x56,0xc3};
    return Bytes(0x553440,entry,sizeof entry)&&Bytes(0x553469,body,sizeof body)&&
        Bytes(0x553606,tail,sizeof tail)&&Bytes(0x496830,host,sizeof host);
}
bool Valid(const Request& r) noexcept {
    return r.token&&r.checkpoint_token&&r.world_address&&r.world_address<=UINT32_MAX-0x22000&&
        r.local_dpid&&r.remote_dpid&&r.local_dpid!=r.remote_dpid&&
        r.local_color>=0&&r.local_color<8&&r.remote_color>=0&&r.remote_color<8&&r.local_color!=r.remote_color&&
        r.date_words[0]>=1&&r.date_words[0]<=7&&r.date_words[1]>=1&&r.date_words[1]<=4&&r.date_words[2]>=1;
}
bool PublishOwned(const Snapshot& s,bool latch,std::uint64_t revision) noexcept {
    AcquireSRWLockExclusive(&lock);
    const bool owned=published.status==Status::Armed&&published.token==s.token&&request_revision==revision;
    if(owned){if(latch){held_transport=s.transport;blocked.store(true);}published=s;}
    ReleaseSRWLockExclusive(&lock);return owned;
}
bool Front(std::uint32_t transport,Snapshot& s) noexcept {
    std::uint32_t begin=0,end=0;
    if(transport>UINT32_MAX-0x98||!Read(transport+0x94,s.queue_count)||!s.queue_count||s.queue_count>65536||
       !Read(transport+0x6c,begin)||!Read(transport+0x70,end)||!Read(transport+0x74,s.queue_cursor)||
       !begin||end<=begin||end-begin!=0x1000||(begin&3)||(s.queue_cursor&3)||
       s.queue_cursor<begin||s.queue_cursor>end-4||!Read(s.queue_cursor,s.packet_address))return false;
    return Read(s.packet_address,s.packet_words);
}
bool NativeState(const Request& r,Snapshot& s) noexcept {
    std::uint32_t native_transport=0,local_dpid=0,lobby_pointer=0,lobby=1,roster=0;
    std::uint32_t combat=0,combat_state=0,modal=0,scenario=0,closing=0;
    unsigned char host=0;
    if(!Read(0x699538,s.world)||s.world!=r.world_address||!Read(0x69d858,native_transport)||native_transport!=s.transport||
       !Read(0x69959c,s.network)||s.network!=1||!Read(0x6995a4,s.local_color)||s.local_color!=r.local_color||
       !Read(0x69ccf4,s.current_color)||s.current_color!=r.local_color||!Read(0x69d658,local_dpid)||local_dpid!=r.local_dpid||
       !Read(s.transport+0x56,host)||host!=1||!Read(HD(0x12123b4),lobby_pointer)||!Read(lobby_pointer,lobby)||lobby||
       !Read(HD(0x1210042),s.st_enabled)||s.st_enabled!=1||!Read(HD(0x1217508),s.exit_pending)||s.exit_pending||
       !Read(HD(0x12176ec),s.protected_exit)||s.protected_exit||!Read(s.world+0x1f63e,s.date_words)||
       std::memcmp(r.date_words,s.date_words,sizeof s.date_words)||!Read(HD(0x121004a),s.participants)||
       !Read(0x6aad88,modal)||modal||!Read(0x69fc44,scenario)||scenario||!Read(0x697308,closing)||closing||
       !Read(0x699420,combat)||(combat&&(!Read(combat+0x34,combat_state)||combat_state==1)))return false;
    // First experiment freezes an unfinished host while the remote completed
    // branch leaves. Do not enter an already-completed host's merge/day barrier.
    if(!s.participants[r.local_color][3]||s.participants[r.local_color][1]||
       !s.participants[r.remote_color][3]||s.participants[r.remote_color][1]!=1)return false;
    const int colors[]={r.local_color,r.remote_color};
    const std::uint32_t ids[]={r.local_dpid,r.remote_dpid};
    for(unsigned i=0;i<2;++i){
        if(!Read(s.world+0x20b98+colors[i]*0x168,s.player_records[i]))return false;
        std::uint32_t id=0;std::memcpy(&id,s.player_records[i],4);
        // Native4CE600 tests the player human/controller flag at20BB2.
        if(id!=ids[i]||!s.player_records[i][0x1a])return false;
    }
    if(!Read(HD(0x21b8c84),roster)||!Read(roster,s.hd_roster)||s.hd_roster[0]!=2||s.hd_roster[2])return false;
    unsigned local_found=0,remote_found=0;
    for(unsigned i=0;i<8;++i){std::uint32_t id=0;std::memcpy(&id,s.hd_roster+3+i*0x17,4);
        if(id==r.local_dpid&&s.hd_roster[7+i*0x17]==r.local_color)++local_found;
        if(id==r.remote_dpid&&s.hd_roster[7+i*0x17]==r.remote_color)++remote_found;}
    return local_found==1&&remote_found==1;
}
bool WriteZero(unsigned char* out) noexcept {
    if(!out)return true;
    const unsigned char zero=0;SIZE_T n=0;
    return WriteProcessMemory(GetCurrentProcess(),out,&zero,1,&n)&&n==1;
}
void* __stdcall PollHook(HiHook* previous,void* transport,unsigned char remove,unsigned char* decompressed) {
    using Native=void* (__thiscall*)(void*,unsigned char,unsigned char*);
    ++depth;
    // First latch is forbidden here: the caller would still execute the rest
    // of its native/HD dispatcher after receiving null. Only enforce a hold
    // established before entering the complete adventure dispatch chain.
    if(installed.load()&&blocked.load()&&
       static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(transport))==held_transport){
        const DWORD saved=GetLastError();WriteZero(decompressed);SetLastError(saved);
        --depth;return nullptr;
    }
    void* result=reinterpret_cast<Native>(previous->GetDefaultFunc())(transport,remove,decompressed);
    --depth;return result;
}
}
bool TryHoldAtAdventureBoundary() noexcept {
    if(!installed.load()||GetCurrentThreadId()!=game_thread)return false;
    if(blocked.load())return true;
    if(depth)return false;
    __declspec(align(16)) unsigned char fp[512];_fxsave(fp);const DWORD saved=GetLastError();
    ++depth;
    Request r;Status status;std::uint64_t revision=0;
    AcquireSRWLockShared(&lock);r=request;status=published.status;revision=request_revision;ReleaseSRWLockShared(&lock);
    bool hold=false;
    if(status==Status::Armed){
        Snapshot s;s.token=r.token;s.checkpoint_token=r.checkpoint_token;s.thread_id=game_thread;
        s.local_dpid=r.local_dpid;s.remote_dpid=r.remote_dpid;
        if(Read(0x69d858,s.transport)&&Front(s.transport,s)&&
           s.packet_words[2]==0x3f6&&s.packet_words[1]==r.remote_dpid){
            const bool exact=s.packet_words[0]==UINT32_MAX&&s.packet_words[3]==24&&
                s.packet_words[4]==0&&s.packet_words[5]==r.remote_dpid;
            if(exact&&NativeState(r,s)&&guard(context)){
                s.status=Status::Held;s.gameplay_blocked=true;s.pre_mutation_verified=true;
                Detail(s,"departure_held_before_adventure_dispatch_no_resume");
                hold=PublishOwned(s,true,revision);if(hold)latched(s,context);
            }else {s.status=Status::Refused;Detail(s,"departure_boundary_identity_or_packet_guard_failed");PublishOwned(s,false,revision);}
        }
    }
    --depth;SetLastError(saved);_fxrstor(fp);return hold;
}

bool Install(PatcherInstance* patcher,std::uintptr_t hd_base,GuardCallback g,LatchedCallback l,void* c,std::string* error) noexcept {
    if(!patcher||!g||!l||hook||!hd_base||hd_base>UINT32_MAX-0x11b8c88){Error(error,"invalid_departure_install");return false;}
    if(!Pin()){Error(error,"departure_poll_pinned_bytes_mismatch");return false;}
    hd=hd_base;game_thread=GetCurrentThreadId();guard=g;latched=l;context=c;
    try{
        hook=patcher->CreateHiHook(0x553440,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(PollHook));
        if(!hook||hook->Apply()<0||!hook->IsApplied()){
            bool clean=!hook;if(hook&&hook->Destroy()){hook=nullptr;clean=true;}
            Error(error,clean?"departure_hook_apply_failed":"departure_hook_rollback_incomplete");return false;
        }
        installed.store(true);Error(error,"");return true;
    }catch(...){if(hook)try{if(hook->Destroy())hook=nullptr;}catch(...){}Error(error,hook?"departure_hook_rollback_incomplete":"departure_install_exception");return false;}
}
bool Arm(const Request& r,std::string* error) noexcept {
    if(!installed.load()||!Valid(r)){Error(error,"invalid_or_uninstalled_departure_request");return false;}
    AcquireSRWLockExclusive(&lock);
    if(!installed.load()||blocked.load()||published.status==Status::Armed){ReleaseSRWLockExclusive(&lock);Error(error,"departure_request_busy");return false;}
    ++request_revision;request=r;published=Snapshot{};published.status=Status::Armed;published.token=r.token;published.checkpoint_token=r.checkpoint_token;
    Detail(published,"armed_requires_matching_completed_remote_departure");ReleaseSRWLockExclusive(&lock);Error(error,"");return true;
}
bool CancelArmed() noexcept {
    AcquireSRWLockExclusive(&lock);const bool ok=!blocked.load()&&published.status==Status::Armed;
    if(ok){published.status=Status::Cancelled;Detail(published,"cancelled_before_departure");}
    ReleaseSRWLockExclusive(&lock);return ok;
}
Snapshot GetSnapshot() noexcept {AcquireSRWLockShared(&lock);Snapshot s=published;ReleaseSRWLockShared(&lock);return s;}
bool IsGameplayBlocked() noexcept{return blocked.load();}
bool HasOwnedHooks() noexcept{return hook!=nullptr;}
bool Uninstall(std::string* error) noexcept {
    if(blocked.load()){Error(error,"held_departure_has_no_release_or_resume_operation");return false;}
    if(hook&&(GetCurrentThreadId()!=game_thread||depth)){Error(error,"departure_uninstall_requires_idle_game_thread");return false;}
    installed.store(false);
    try{if(hook&&!hook->Destroy()){Error(error,"departure_hook_destroy_failed");return false;}hook=nullptr;}
    catch(...){Error(error,"departure_hook_destroy_exception");return false;}
    AcquireSRWLockExclusive(&lock);published=Snapshot{};request=Request{};ReleaseSRWLockExclusive(&lock);Error(error,"");return true;
}
const char* StatusName(Status s) noexcept {
    switch(s){case Status::Armed:return "armed";case Status::Held:return "held";case Status::Refused:return "refused";
    case Status::Cancelled:return "cancelled";default:return "idle";}
}
}
