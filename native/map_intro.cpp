#include "map_intro.hpp"
#include "common.hpp"
#include "vendor/patcher_x86.hpp"
#include <atomic>
#include <cstring>
#include <intrin.h>
static_assert(sizeof(void*)==4,"Map intro action requires x86");
namespace hota_intro {
namespace {
// Exact bytes from REA ev_698f883604734c7504bc9c37db5f233884daa839c48cc6f0ac67a00d542520b7.
constexpr char known_hex[]="c3e4e52df2ee20e2ee20f2fcece520efe5f9e5f020e820efeee4e7e5ecedfbf520f5eee4eee220efeeeaeee8f2f1ff20ece5f720eff0e0e2eef1f3e4e8ff2e2020d2f0e820e2e5ebe8eae8f520eff0e0e2e8f2e5ebff20efeee4e7e5ece5ebe8e920e2eee7ede0ece5f0e8ebe8f1fc20ede0e9f2e820e5e3ee2c20eaeeecf320eff0e8ede0e4ebe5e6e8f220ece5f72c20f2eef220eaeeedf2f0eeebe8f0f3e5f220e2f1e52e";
static_assert(sizeof(known_hex)==333,"Pinned166-byte intro text");
struct Message { std::uint32_t command,subtype,item,flags,x,y,parameter,parent; };
HiHook* hook=nullptr;DWORD game_thread=0;
std::atomic<bool> installed{false},busy{false},confirmed{false};
std::atomic_flag lock=ATOMIC_FLAG_INIT;
std::atomic<Status> status{Status::Idle};
std::atomic<std::uint32_t> token{0},calls{0},native_result{0};
std::atomic<const char*> reason{""};
thread_local bool inside=false;
std::uint32_t expected_world=0;int expected_local=-1;WORD expected_date[3]{};ULONGLONG queued_at=0;
void Error(std::string* out,const char* value) noexcept { if(out)try{*out=value;}catch(...){} }
bool Read(std::uintptr_t address,void* buffer,SIZE_T size) noexcept {
    SIZE_T got=0;return address&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),buffer,size,&got)&&got==size;
}
bool Bytes(std::uintptr_t address,const unsigned char* expected,SIZE_T count) noexcept {
    unsigned char actual[32]{};return count<=sizeof actual&&Read(address,actual,count)&&!std::memcmp(actual,expected,count);
}
unsigned Nibble(char c) noexcept { return c<='9'?c-'0':c-'a'+10; }
bool KnownText(std::uintptr_t address,unsigned length) noexcept {
    unsigned char actual[167]{};
    if(length!=166||!Read(address,actual,sizeof actual)||actual[166])return false;
    for(unsigned i=0;i<166;++i)if(actual[i]!=((Nibble(known_hex[i*2])<<4)|Nibble(known_hex[i*2+1])))return false;
    return true;
}
const char* Guard(std::uint32_t& dialog) noexcept {
    std::uint32_t world=0,wm=0,depth=0,native_dialog=0,vt=0,kind=0,scenario=0,combat=0,combat_state=0;
    int local=-1;WORD date[3]{};
    if(!hp::Read(0x699538,world)||world!=expected_world||!world||!hp::Read(0x6995a4,local)||local!=expected_local||
       !hp::Read(world+0x1f63e,date)||std::memcmp(date,expected_date,sizeof date))return "intro_world_owner_or_date_changed";
    if(!hp::Read(0x6aad88,depth)||depth!=1||!hp::Read(0x69fc44,scenario)||scenario||
       !hp::Read(0x6992d0,wm)||!wm||!hp::Read(wm+0x54,dialog)||!dialog||
       !hp::Read(0x6995e0,native_dialog)||dialog!=native_dialog||!hp::Read(dialog,vt)||vt!=0x63db40||
       !hp::Read(0x699570,kind)||kind!=1)return "not_single_native_ok_message";
    if(!hp::Read(0x699420,combat)||(combat&&(!hp::Read(combat+0x34,combat_state)||combat_state==1)))return "intro_combat_active";
    std::uint32_t item=0;unsigned text_count=0,ok_count=0,count=0;
    if(!hp::Read(dialog+0x2c,item))return "intro_items_unreadable";
    while(item&&count++<512) {
        WORD id=0,flags=0;std::uint32_t parent=0,next=0,item_vt=0;
        if(!hp::Read(item+0x10,id)||!hp::Read(item+4,parent)||parent!=dialog||!hp::Read(item+8,next)||!hp::Read(item,item_vt))return "intro_items_unreadable";
        if(id==1) {
            std::uint32_t text=0,length=0;
            if(item_vt!=0x642dc0||!hp::Read(item+0x34,text)||!hp::Read(item+0x38,length)||!KnownText(text,length))return "intro_exact_text_mismatch";
            ++text_count;
        }else if(id==0x7802) {
            if(!hp::Read(item+0x16,flags)||(flags&6)!=6||(flags&0x20))return "intro_ok_button_inactive";
            ++ok_count;
        }else if(id!=0x7866&&(id<200||id>213))return "intro_unexpected_control";
        item=next;
    }
    return item||text_count!=1||ok_count!=1?"intro_control_set_mismatch":nullptr;
}
unsigned __stdcall MessageHook(HiHook* chain,Message* message) {
    using Native=unsigned(__thiscall*)(Message*);
    bool dispatched=false;
    __declspec(align(16)) unsigned char fp[512];_fxsave(fp);const DWORD saved=GetLastError();
    ++calls;
    if(installed.load()&&!inside&&GetCurrentThreadId()==game_thread&&status.load()==Status::Queued&&!lock.test_and_set()) {
        Message current{};
        if(GetTickCount64()-queued_at>10000) { reason.store("intro_request_expired");status.store(Status::Expired);busy.store(false); }
        else if(message&&hp::Read(reinterpret_cast<std::uintptr_t>(message),current)&&current.command==0) {
            std::uint32_t dialog=0;const char* failure=Guard(dialog);
            if(failure) { reason.store(failure);status.store(Status::Refused);busy.store(false); }
            else {
                const Message action{0x200,0xd,0x7802,0,current.x,current.y,0,dialog};SIZE_T written=0;
                if(!WriteProcessMemory(GetCurrentProcess(),message,&action,sizeof action,&written)||written!=sizeof action) {
                    reason.store("intro_event_write_failed");status.store(Status::Refused);busy.store(false);
                }else { inside=true;dispatched=true;status.store(Status::Dispatching); }
            }
        }
        lock.clear();
    }
    SetLastError(saved);_fxrstor(fp);
    const unsigned result=reinterpret_cast<Native>(chain->GetDefaultFunc())(message);
    if(dispatched) {
        _fxsave(fp);const DWORD after=GetLastError();std::uint32_t wm=0,choice=0;Message output{};
        const bool ok=result==2&&hp::Read(0x6992d0,wm)&&wm&&hp::Read(wm+0x38,choice)&&choice==0x7802&&
            hp::Read(reinterpret_cast<std::uintptr_t>(message),output)&&output.command==0x200&&output.subtype==10&&output.item==10;
        confirmed.store(ok);native_result.store(result);reason.store(ok?"":"native_intro_ok_not_confirmed");
        status.store(Status::Returned);inside=false;busy.store(false);SetLastError(after);_fxrstor(fp);
    }
    return result;
}
}
const char* StatusName(Status s) noexcept {
    switch(s) {case Status::Idle:return "idle";case Status::Queued:return "queued";case Status::Dispatching:return "dispatching_known_intro_ok";
    case Status::Returned:return "native_intro_callback_returned";case Status::Refused:return "refused_known_intro";default:return "expired";}
}
Snapshot GetSnapshot() noexcept {return {installed.load(),status.load(),token.load(),calls.load(),native_result.load(),confirmed.load(),reason.load()};}
bool QueueKnownIntro(const Request& r,std::string* error) noexcept {
    if(!installed.load()) { Error(error,"intro_hook_not_installed");return false; }
    if(!r.token||!r.expected_world||r.expected_local<0||r.expected_local>7||!r.day||r.day>7||!r.week||r.week>4||!r.month||r.map_basename!="Judgement Day.h3m") {
        Error(error,"intro_requires_owned_judgement_day_world_owner_date");return false;
    }
    if(lock.test_and_set()) { Error(error,"intro_request_busy");return false; }
    if(busy.load()) {lock.clear();Error(error,"intro_request_busy");return false;}
    expected_world=r.expected_world;expected_local=r.expected_local;expected_date[0]=r.day;expected_date[1]=r.week;expected_date[2]=r.month;
    token.store(r.token);confirmed.store(false);native_result.store(0);reason.store("");queued_at=GetTickCount64();busy.store(true);status.store(Status::Queued);lock.clear();return true;
}
bool CancelQueued() noexcept {
    if(lock.test_and_set())return false;const bool result=status.load()==Status::Queued;
    if(result){status.store(Status::Idle);busy.store(false);}lock.clear();return result;
}
bool Install(PatcherInstance* instance,std::string* error) noexcept {
    if(!instance||hook){Error(error,"invalid_or_owned_intro_hook");return false;}
    const unsigned char dispatch[]={0x81,0x3e,0,2,0,0,0x0f,0x85,0xad,0,0,0,0x83,0x7e,4,0x0d};
    const unsigned char close[]={0x8b,0x0d,0xd0,0x92,0x69,0,0x89,0x41,0x38,0x89,0x7e,8,0x89,0x7e,4};
    if(!Bytes(0x4f1684,dispatch,sizeof dispatch)||!Bytes(0x4f17d0,close,sizeof close)){Error(error,"native_intro_callback_bytes_mismatch");return false;}
    game_thread=GetCurrentThreadId();
    try {
        hook=instance->CreateHiHook(0x4f1650,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(MessageHook));
        if(!hook||hook->Apply()<0||!hook->IsApplied()){Uninstall();Error(error,"intro_hook_install_failed");return false;}
        installed.store(true);return true;
    }catch(...){Uninstall();Error(error,"intro_hook_install_exception");return false;}
}
bool Uninstall(std::string* error) noexcept {
    if(inside||busy.load()||(game_thread&&GetCurrentThreadId()!=game_thread)){Error(error,"intro_uninstall_requires_idle_game_thread");return false;}
    try {if(hook&&!hook->Destroy()){Error(error,"intro_hook_rollback_failed");return false;}hook=nullptr;installed.store(false);return true;}
    catch(...){Error(error,"intro_hook_rollback_exception");return false;}
}
}
