#include "completed_leave.hpp"
#include "supported_build.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
static_assert(sizeof(void*)==4,"Completed native leave requires x86");
namespace hota_leave {
namespace {
std::atomic<bool> installed{false},blocked{false};
std::uintptr_t hd=0;
DWORD game_thread=0;
GuardCallback guard=nullptr;
void* context=nullptr;
SRWLOCK lock=SRWLOCK_INIT;
Request request;
Snapshot published;
std::uint64_t revision=0;
thread_local bool inside=false;
void Error(std::string* out,const char* text) noexcept {if(out)try{*out=text;}catch(...) {}}
void Detail(Snapshot& s,const char* text) noexcept {strncpy_s(s.detail,text,_TRUNCATE);}
bool Read(std::uintptr_t at,void* out,std::size_t size) noexcept {
    if(!at||size>UINT32_MAX-at)return false;
    SIZE_T n=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),out,size,&n)&&n==size;
}
template<class T>bool Read(std::uintptr_t at,T& out) noexcept{return Read(at,&out,sizeof out);}
std::uintptr_t HD(std::uint32_t at) noexcept{return hd+hp_build::Rva(at-0x1000000);}
bool Bytes(std::uintptr_t at,const unsigned char* expected,std::size_t size) noexcept {
    unsigned char actual[32]{};return size<=sizeof actual&&Read(at,actual,size)&&!std::memcmp(actual,expected,size);
}
bool Pin() noexcept {
    const unsigned char entry[]={0x55,0x8b,0xec,0x83,0xec,0x20,0x53,0x33,0xdb};
    const unsigned char destroy[]={0xa1,0x58,0xd6,0x69,0,0x3b,0xc3,0x74,0x0c,0x8b,0x11,0x50,0xff,0x52,0x20};
    const unsigned char close[]={0x8b,0x01,0xff,0x50,0x18,0x8b,0x0d,0x58,0xd8,0x69,0,0x3b,0xcb,0x74,6,0x8b,0x11,0x6a,1,0xff,0x12};
    const unsigned char tail[]={0x5b,0x8b,0xe5,0x5d,0xc3};
    return Bytes(0x5548a0,entry,sizeof entry)&&Bytes(0x5548ec,destroy,sizeof destroy)&&
        Bytes(0x554901,close,sizeof close)&&Bytes(0x554945,tail,sizeof tail);
}
bool Valid(const Request& r) noexcept {
    return r.token&&r.checkpoint_token&&r.world_address&&r.world_address<=UINT32_MAX-0x22000&&
        r.local_dpid&&r.remote_dpid&&r.local_dpid!=r.remote_dpid&&r.local_color>=0&&r.local_color<8&&
        r.remote_color>=0&&r.remote_color<8&&r.local_color!=r.remote_color&&
        r.date_words[0]>=1&&r.date_words[0]<=7&&r.date_words[1]>=1&&r.date_words[1]<=4&&r.date_words[2];
}
bool NativeState(const Request& r,Snapshot& s) noexcept {
    std::uint32_t lobby_pointer=0,lobby=1,enabled=0,exit=0,protected_exit=0;
    std::uint32_t modal=0,scenario=0,closing=0,combat=0,combat_state=0,vt=0,iface=0;
    std::uint32_t destroy=0,close=0,dtor=0,local_player=0,remote_player=0;
    unsigned char host=1,local_human=0,remote_human=0;
    if(!Read(0x699538,s.world)||s.world!=r.world_address||!Read(0x69d858,s.transport_before)||!s.transport_before||
       s.transport_before>UINT32_MAX-0x100||!Read(0x69959c,s.network_before)||s.network_before!=1||
       !Read(0x6995a4,s.local_color)||s.local_color!=r.local_color||!Read(0x69ccf4,s.current_color)||s.current_color!=r.remote_color||
       !Read(0x69d658,s.local_dpid_before)||s.local_dpid_before!=r.local_dpid||
       !Read(s.transport_before+0x56,host)||host||!Read(s.transport_before+0x2c,iface)||!iface||
       !Read(s.transport_before,vt)||vt>UINT32_MAX-0x24||!Read(vt,dtor)||dtor!=0x5526f0||
       !Read(vt+0x18,close)||close!=0x496e10||!Read(vt+0x20,destroy)||destroy!=0x496bc0||
       !Read(HD(0x12123b4),lobby_pointer)||!Read(lobby_pointer,lobby)||lobby||
       !Read(HD(0x1210042),enabled)||enabled!=1||!Read(HD(0x1217508),exit)||exit||
       !Read(HD(0x12176ec),protected_exit)||protected_exit||!Read(s.world+0x1f63e,s.date_words)||
       std::memcmp(s.date_words,r.date_words,sizeof r.date_words)||!Read(HD(0x121004a),s.participants)||
       !Read(0x6aad88,modal)||modal||!Read(0x69fc44,scenario)||scenario||!Read(0x697308,closing)||closing||
       !Read(0x699420,combat)||(combat&&(!Read(combat+0x34,combat_state)||combat_state==1))||
       !Read(s.world+0x20b98+r.local_color*0x168,local_player)||local_player!=r.local_dpid||
       !Read(s.world+0x20b98+r.remote_color*0x168,remote_player)||remote_player!=r.remote_dpid||
       !Read(s.world+0x20bb2+r.local_color*0x168,local_human)||!local_human||
       !Read(s.world+0x20bb2+r.remote_color*0x168,remote_human)||!remote_human)return false;
    return s.participants[r.local_color][3]&&s.participants[r.local_color][1]==1&&
        s.participants[r.remote_color][3]&&!s.participants[r.remote_color][1];
}
bool PublishOwned(Snapshot& s,std::uint64_t expected,bool claim) noexcept {
    AcquireSRWLockExclusive(&lock);
    const bool ok=published.status==Status::Armed&&revision==expected&&published.token==s.token;
    if(ok){if(claim)blocked.store(true);published=s;}
    ReleaseSRWLockExclusive(&lock);return ok;
}
}
bool Install(std::uintptr_t base,GuardCallback g,void* c,std::string* error) noexcept {
    if(installed.load()||!base||base>UINT32_MAX-0x2176f0||!g){Error(error,"invalid_or_existing_completed_leave_install");return false;}
    if(!Pin()){Error(error,"completed_leave_native_entry_or_body_mismatch");return false;}
    hd=base;game_thread=GetCurrentThreadId();guard=g;context=c;installed.store(true);Error(error,"");return true;
}
bool Arm(const Request& r,std::string* error) noexcept {
    if(!installed.load()||!Valid(r)){Error(error,"invalid_or_uninstalled_completed_leave_request");return false;}
    AcquireSRWLockExclusive(&lock);
    if(blocked.load()||published.status==Status::Armed){ReleaseSRWLockExclusive(&lock);Error(error,"completed_leave_busy");return false;}
    ++revision;request=r;published=Snapshot{};published.token=r.token;published.checkpoint_token=r.checkpoint_token;
    published.status=Status::Armed;Detail(published,"armed_requires_verified_completed_capture_binding");
    ReleaseSRWLockExclusive(&lock);Error(error,"");return true;
}
bool TryAtAdventureBoundary() noexcept {
    if(!installed.load()||GetCurrentThreadId()!=game_thread)return false;
    if(blocked.load())return true;
    if(inside)return false;
    inside=true;__declspec(align(16)) unsigned char fp[512];_fxsave(fp);DWORD saved=GetLastError();
    Request r;Status status;std::uint64_t expected=0;
    AcquireSRWLockShared(&lock);r=request;status=published.status;expected=revision;ReleaseSRWLockShared(&lock);
    bool claimed=false;Snapshot s;
    if(status==Status::Armed){
        s.token=r.token;s.checkpoint_token=r.checkpoint_token;s.thread_id=game_thread;s.remote_dpid=r.remote_dpid;
        if(Pin()&&NativeState(r,s)&&guard(r,context)){
            s.status=Status::Closing;s.native_call_started=true;s.gameplay_blocked=true;
            Detail(s,"starting_native_transport_teardown_permanent_local_gate");claimed=PublishOwned(s,expected,true);
        }else{s.status=Status::Refused;Detail(s,"completed_leave_boundary_identity_or_checkpoint_guard_failed");PublishOwned(s,expected,false);}
    }
    SetLastError(saved);_fxrstor(fp);
    if(claimed){
        // Calls the real entry, never a bypass trampoline or individual COM method.
        // Native helper destroys transport and clears multiplayer state. The
        // permanent gate was published before this call can reenter anything.
        reinterpret_cast<void(__cdecl*)()>(0x5548a0)();
        _fxsave(fp);saved=GetLastError();s.native_call_returned=true;
        s.post_state_readable=Read(0x69d858,s.transport_after)&&Read(0x69959c,s.network_after)&&Read(0x69d658,s.local_dpid_after);
        s.transport_null=s.post_state_readable&&!s.transport_after;s.network_mode_zero=s.post_state_readable&&!s.network_after;
        s.status=s.transport_null&&s.network_mode_zero?Status::Closed:Status::TeardownIncomplete;
        Detail(s,s.status==Status::Closed?"native_transport_closed_local_client_frozen":"native_teardown_returned_postconditions_unverified_client_frozen");
        AcquireSRWLockExclusive(&lock);published=s;ReleaseSRWLockExclusive(&lock);SetLastError(saved);_fxrstor(fp);
    }
    inside=false;return claimed;
}
bool IsGameplayBlocked() noexcept{return blocked.load();}
bool CancelArmed() noexcept {
    AcquireSRWLockExclusive(&lock);const bool ok=!blocked.load()&&published.status==Status::Armed;
    if(ok){published.status=Status::Cancelled;Detail(published,"cancelled_before_native_teardown");}
    ReleaseSRWLockExclusive(&lock);return ok;
}
Snapshot GetSnapshot() noexcept {AcquireSRWLockShared(&lock);Snapshot s=published;ReleaseSRWLockShared(&lock);return s;}
const char* StatusName(Status s) noexcept {
    switch(s){case Status::Armed:return "armed";case Status::Refused:return "refused";case Status::Closing:return "closing";
    case Status::Closed:return "closed_local_client_frozen";case Status::TeardownIncomplete:return "teardown_incomplete_client_frozen";
    case Status::Cancelled:return "cancelled";default:return "idle";}
}
}
