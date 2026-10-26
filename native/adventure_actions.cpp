#include "adventure_actions.hpp"
#include "common.hpp"
#include "vendor/patcher_x86.hpp"
#include <atomic>
#include <cstring>
#include <intrin.h>
static_assert(sizeof(void*)==4,"Adventure actions require x86");
namespace hota_adventure {
namespace {
HiHook* hook=nullptr;
DWORD game_thread=0;
std::uintptr_t hd=0;
Request request;
ULONGLONG queued_at=0;
std::atomic_flag lock=ATOMIC_FLAG_INIT;
std::atomic<bool> installed{false},busy{false};
std::atomic<Status> status{Status::Idle};
std::atomic<Error> last_error{Error::None};
std::atomic<std::uint32_t> token{0},native_result{0};
thread_local bool inside=false;
thread_local unsigned dispatch_depth=0;
thread_local bool idle_boundary=false,processing_idle=false;
Predicate input_gate=nullptr,idle_callback=nullptr,command_gate=nullptr;
void* input_context=nullptr;
void* command_context=nullptr;
void* idle_context=nullptr;
struct Message { std::uint32_t command,subtype,item,flags,x,y,parameter,parent; };
static_assert(sizeof(Message)==32,"Native H3Msg ABI");
void ErrorText(std::string* out,const char* text) noexcept { if(out)try{*out=text;}catch(...){} }
bool Bytes(std::uintptr_t address,const unsigned char* bytes,size_t count) noexcept {
    unsigned char actual[32]{};SIZE_T got=0;
    return count<=sizeof actual&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),actual,count,&got)&&got==count&&!std::memcmp(actual,bytes,count);
}
Error Guard(std::uint32_t self,std::uint32_t& dialog) noexcept {
    std::uint32_t adventure=0,type=0,state=0,wm=0,top=0,depth=0,scenario=0,combat=0,combat_state=0;
    if(!hp::Read(0x6992b8,adventure)||adventure!=self||!self||
       !hp::Read(self+0xc,type)||type!=0x400||!hp::Read(self+0x34,state)||state!=1||
       !hp::Read(self+0x44,dialog)||!dialog)return Error::Manager;
    if(!hp::Read(0x6aad88,depth)||depth||!hp::Read(0x69fc44,scenario)||scenario||
       !hp::Read(0x6992d0,wm)||!wm||!hp::Read(wm+0x54,top)||top!=dialog)return Error::Modal;
    if(!hp::Read(0x699420,combat)||(combat&&(!hp::Read(combat+0x34,combat_state)||combat_state==1)))return Error::Combat;
    // Same linked-item traversal as native5FF5B0, bounded and read-only. Do not
    // bypass a hidden, disabled or deactivated End Turn control during movement.
    std::uint32_t item=0;bool button=false;
    if(!hp::Read(dialog+0x2c,item))return Error::Button;
    for(unsigned count=0;item&&count<512;++count) {
        WORD id=0,flags=0;std::uint32_t parent=0,next=0;
        if(!hp::Read(item+0x10,id)||!hp::Read(item+4,parent)||parent!=dialog||!hp::Read(item+8,next))return Error::Button;
        if(id==0xc) {
            if(!hp::Read(item+0x16,flags)||(flags&6)!=6||(flags&0x20))return Error::Button;
            button=true;break;
        }
        item=next;
    }
    if(!button)return Error::Button;
    std::uint32_t world=0,map=0,multiplayer=0,closing=0;
    if(!hp::Read(0x699538,world)||!world||!hp::Read(world+0x1fc40,map)||!map||
       !hp::Read(0x69959c,multiplayer)||!multiplayer||!hp::Read(0x697308,closing)||closing)return Error::World;
    int local=-1,current=-1;
    if(!hp::Read(0x6995a4,local)||local!=request.expected_local||!hp::Read(0x69ccf4,current)||current!=local)return Error::Owner;
    WORD date[3]{};
    if(!hp::Read(world+0x1f63e,date)||date[0]!=request.day||date[1]!=request.week||date[2]!=request.month)return Error::Date;
    int enabled=0,barrier=0,ack=0,stop=0;unsigned char participant[6]{};
    if(!hp::Read(hd+0x210042,enabled)||!enabled||!hp::Read(hd+0x21004a+local*6,participant)||
       !participant[3]||participant[1]||
       !hp::Read(hd+0x21001e,barrier)||barrier||!hp::Read(hd+0x210022,ack)||ack||
       !hp::Read(hd+0x21002e,stop)||stop)return Error::SimultaneousState;
    return Error::None;
}
unsigned __stdcall MessageHook(HiHook* chain,std::uint32_t self,Message* message) {
    using Native=unsigned(__thiscall*)(std::uint32_t,Message*);
    bool dispatched=false;
    const bool previous_idle=processing_idle,previous_boundary=idle_boundary;
    ++dispatch_depth;processing_idle=false;idle_boundary=false;
    __declspec(align(16)) unsigned char fp[512];_fxsave(fp);
    const DWORD saved=GetLastError();
    const bool own_thread=installed.load()&&GetCurrentThreadId()==game_thread;
    Message current{};
    const bool real_idle=message&&hp::Read(reinterpret_cast<std::uintptr_t>(message),current)&&current.command==0;
    bool blocked=own_thread&&input_gate&&input_gate(input_context);
    if(own_thread&&!blocked&&!real_idle&&command_gate&&command_gate(command_context))blocked=true;
    if(own_thread&&!blocked&&dispatch_depth==1&&!inside&&real_idle&&idle_callback) {
        // Before any preceding HD/HotA/native chain. In-poll latching is too late:
        // native408710 can continue into AI/end-turn after its network poll.
        idle_boundary=true;blocked=idle_callback(idle_context);idle_boundary=false;
        if(input_gate&&input_gate(input_context))blocked=true;
    }
    if(blocked) {
        if(!lock.test_and_set()) {
            if(status.load()==Status::Queued) {
                last_error.store(Error::GameplayBlocked);status.store(Status::Refused);busy.store(false);
            }
            lock.clear();
        }
        --dispatch_depth;processing_idle=previous_idle;idle_boundary=previous_boundary;
        SetLastError(saved);_fxrstor(fp);
        return 0; // Same no-dispatch return as native disabled manager at408721.
    }
    if(own_thread&&dispatch_depth==1&&!inside&&status.load()==Status::Queued&&!lock.test_and_set()) {
        if(status.load()==Status::Queued&&GetTickCount64()-queued_at>10000) {
            last_error.store(Error::Expired);status.store(Status::Expired);busy.store(false);
        } else if(status.load()==Status::Queued&&real_idle&&!(command_gate&&command_gate(command_context))) {
            std::uint32_t dialog=0;
            const Error failure=Guard(self,dialog);
            if(failure!=Error::None) {
                last_error.store(failure);status.store(Status::Refused);busy.store(false);
            }else {
                // Exactly the native left-button release event for End Turn.
                // Replace an idle event in its original storage, so native close
                // protocol mutations remain visible to the real message pump.
                Message action{0x200,0xd,0xc,0,current.x,current.y,0,dialog};SIZE_T written=0;
                bool hovered=true;
                if(request.kind==Kind::MapPress) {
                    // Hover first, exactly like the cursor: the native mouse-move handler
                    // (40E2C0) derives the hovered tile (+0xE8), path preview and cursor from
                    // screen coordinates. Tiles are 32 px from an 8 px map border, origin at
                    // the packed view position +0xE4 (10-bit signed x, y at bit 16).
                    std::uint32_t view=0;hovered=hp::Read(self+0xe4,view);
                    const int vx=static_cast<std::int16_t>(static_cast<std::uint16_t>(view<<6))>>6;
                    const int vy=static_cast<std::int16_t>(static_cast<std::uint16_t>((view>>16)<<6))>>6;
                    Message move{4,0,0,0,static_cast<std::uint32_t>(8+(request.x-vx)*32+16),static_cast<std::uint32_t>(8+(request.y-vy)*32+16),0,0};
                    if(hovered)reinterpret_cast<Native>(chain->GetDefaultFunc())(self,&move);
                    std::uint32_t tile=0;
                    const std::uint32_t want=(static_cast<std::uint32_t>(request.x)&0x3ff)|((static_cast<std::uint32_t>(request.y)&0x3ff)<<16)|(static_cast<std::uint32_t>(request.z&0xf)<<26);
                    hovered=hovered&&hp::Read(self+0xe8,tile)&&tile==want;
                    action={0x200,0xc,0x25,0,move.x,move.y,0,dialog};
                }
                if(!hovered||!WriteProcessMemory(GetCurrentProcess(),message,&action,sizeof action,&written)||written!=sizeof action) {
                    last_error.store(Error::MessageWrite);status.store(Status::Refused);busy.store(false);
                }else { inside=true;dispatched=true;status.store(Status::Dispatching); }
            }
        }
        lock.clear();
    }
    processing_idle=own_thread&&dispatch_depth==1&&real_idle&&!dispatched;
    SetLastError(saved);_fxrstor(fp);
    // Preserve all pre-existing HotA/HD hooks and native warning/turn paths.
    const unsigned result=reinterpret_cast<Native>(chain->GetDefaultFunc())(self,message);
    --dispatch_depth;processing_idle=previous_idle;idle_boundary=previous_boundary;
    if(dispatched) {
        _fxsave(fp);const DWORD after=GetLastError();
        native_result.store(result);status.store(Status::Returned);inside=false;busy.store(false);
        SetLastError(after);_fxrstor(fp);
    }
    return result;
}
}
const char* StatusName(Status value) noexcept {
    switch(value) {
    case Status::Idle:return "idle";case Status::Queued:return "queued";case Status::Dispatching:return "dispatching_native_event";
    case Status::Returned:return "native_event_returned";case Status::Refused:return "refused_phase";default:return "expired";
    }
}
const char* ErrorName(Error value) noexcept {
    switch(value) {
    case Error::None:return "";case Error::NotInstalled:return "adventure_hook_not_installed";case Error::Busy:return "adventure_action_busy";
    case Error::InvalidRequest:return "invalid_end_turn_owner_date_or_token";case Error::Expired:return "end_turn_request_expired";
    case Error::Manager:return "adventure_manager_not_active";case Error::Modal:return "adventure_not_top_or_modal_open";
    case Error::Combat:return "combat_active_or_unreadable";case Error::World:return "multiplayer_map_not_active";
    case Error::Owner:return "local_current_owner_mismatch";case Error::Date:return "end_turn_date_changed";
    case Error::SimultaneousState:return "st_branch_not_active_unfinished_idle";
    case Error::Button:return "end_turn_button_not_enabled_visible_active";
    case Error::GameplayBlocked:return "gameplay_held_for_departure";default:return "native_event_write_failed";
    }
}
Snapshot GetSnapshot() noexcept { return {installed.load(),status.load(),last_error.load(),token.load(),game_thread,native_result.load()}; }
bool IsAtIdleBoundary() noexcept { return dispatch_depth==1&&idle_boundary; }
bool IsProcessingIdle() noexcept { return dispatch_depth==1&&processing_idle; }
bool ConfigureInputGate(Predicate predicate,void* context,std::string* error) noexcept {
    if(!installed.load()||GetCurrentThreadId()!=game_thread||dispatch_depth) { ErrorText(error,"adventure_config_requires_idle_installed_game_thread");return false; }
    input_gate=predicate;input_context=context;return true;
}
bool ConfigureCommandGate(Predicate predicate,void* context,std::string* error) noexcept {
    if(!installed.load()||GetCurrentThreadId()!=game_thread||dispatch_depth) { ErrorText(error,"adventure_config_requires_idle_installed_game_thread");return false; }
    command_gate=predicate;command_context=context;return true;
}
bool ConfigureIdleBoundary(Predicate predicate,void* context,std::string* error) noexcept {
    if(!installed.load()||GetCurrentThreadId()!=game_thread||dispatch_depth) { ErrorText(error,"adventure_config_requires_idle_installed_game_thread");return false; }
    idle_callback=predicate;idle_context=context;return true;
}
bool QueueEndTurn(const Request& value,std::string* error) noexcept {
    if(!installed.load()) { ErrorText(error,ErrorName(Error::NotInstalled));return false; }
    if(!value.token||value.expected_local<0||value.expected_local>7||value.day<1||value.day>7||value.week<1||value.week>4||!value.month) {
        ErrorText(error,ErrorName(Error::InvalidRequest));return false;
    }
    if(lock.test_and_set()) { ErrorText(error,ErrorName(Error::Busy));return false; }
    if(busy.load()) { lock.clear();ErrorText(error,ErrorName(Error::Busy));return false; }
    request=value;queued_at=GetTickCount64();token.store(value.token);native_result.store(0);last_error.store(Error::None);
    busy.store(true);status.store(Status::Queued);lock.clear();return true;
}
bool QueueMapPress(const Request& value,std::string* error) noexcept {
    if(value.kind!=Kind::MapPress||value.x<0||value.y<0||value.x>255||value.y>255||value.z>1) { ErrorText(error,ErrorName(Error::InvalidRequest));return false; }
    return QueueEndTurn(value,error);
}
bool CancelQueued() noexcept {
    if(lock.test_and_set())return false;
    const bool cancelled=status.load()==Status::Queued;
    if(cancelled) { status.store(Status::Idle);busy.store(false); }
    lock.clear();return cancelled;
}
bool Install(PatcherInstance* patcher,std::uintptr_t hd_base,std::string* error) noexcept {
    if(!patcher||!hd_base||hd_base>UINT32_MAX-0x210098||hook) { ErrorText(error,"invalid_or_owned_adventure_hook");return false; }
    // Entries may already be detoured. Check native dispatch and button path,
    // not an assumed unpatched entry prologue or legacy hero-array layout.
    const unsigned char dispatch[]={0x8b,0x47,0x04,0x83,0xe8,0x0c,0x74,0x65,0x48,0x74,0x3b};
    const unsigned char button[]={0x8d,0x55,0xf8,0x8d,0x45,0xf4,0x52,0x8d,0x4d,0xff,0x50,0x51,0x57,0x8b,0xce,0xe8,0x37,0x10,0,0};
    // HotA/HD rewrites this CALL's rel32 destination at runtime. Pin the native
    // world receiver and CALL opcode, retaining the installed native hook chain.
    const unsigned char end[]={0x8b,0x0d,0x38,0x95,0x69,0,0xe8};
    if(!Bytes(0x408933,dispatch,sizeof dispatch)||!Bytes(0x408985,button,sizeof button)||!Bytes(0x409cd9,end,sizeof end)) {
        ErrorText(error,"native_adventure_dispatch_bytes_mismatch");return false;
    }
    game_thread=GetCurrentThreadId();hd=hd_base;
    try {
        hook=patcher->CreateHiHook(0x408710,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(MessageHook));
        if(!hook||hook->Apply()<0||!hook->IsApplied()) { Uninstall();ErrorText(error,"adventure_hook_install_failed");return false; }
        installed.store(true);return true;
    }catch(...) { Uninstall();ErrorText(error,"adventure_hook_install_exception");return false; }
}
bool Uninstall(std::string* error) noexcept {
    if(dispatch_depth||inside||busy.load()||(game_thread&&GetCurrentThreadId()!=game_thread)) { ErrorText(error,"adventure_uninstall_requires_idle_game_thread");return false; }
    try {
        if(hook&&!hook->Destroy()) { ErrorText(error,"adventure_hook_rollback_failed");return false; }
        hook=nullptr;installed.store(false);input_gate=nullptr;idle_callback=nullptr;command_gate=nullptr;input_context=nullptr;idle_context=nullptr;command_context=nullptr;return true;
    }catch(...) { ErrorText(error,"adventure_hook_rollback_exception");return false; }
}
}
