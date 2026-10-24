#include "session_bootstrap.hpp"
#include "common.hpp"
#include "scenario_config.hpp"
#include "scenario_start.hpp"
#include "directplay_port.hpp"
#include "vendor/patcher_x86.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

static_assert(sizeof(void*) == 4, "Session bootstrap requires x86");
namespace hota_session {
namespace {
std::mutex mutex;
Request queued, running;
Snapshot snapshot;
std::array<HiHook*,3> hooks{};
std::atomic<bool> installed{false};
std::atomic<bool> busy{false};
std::atomic<bool> scaffold_pending{false};
DWORD game_thread = 0;
bool dispatching = false; // game thread only
bool failure_unwind_pending = false; // consumed once by the next native TypeRun

void Error(std::string* out, const char* s) noexcept { if(out) try { *out=s; } catch (...) {} }
void State(Status value, const char* error = "") noexcept {
    try { std::lock_guard<std::mutex> lock(mutex); snapshot.status=value; snapshot.error=error; } catch (...) {}
}
bool Failed() noexcept { try { std::lock_guard<std::mutex> lock(mutex); return snapshot.status==Status::Failed; } catch (...) { return true; } }
bool Ascii(const std::string& s, size_t max, bool empty = false) {
    if ((!empty && s.empty()) || s.size()>max) return false;
    for (unsigned char c:s) if(c<32 || c>126) return false;
    return true;
}
bool IPv4(const std::string& s) {
    unsigned fields=0,value=0,digits=0;
    for(size_t i=0;i<=s.size();++i) {
        const char c=i==s.size()?'.':s[i];
        if(c=='.') { if(!digits||value>255||++fields>4) return false; value=digits=0; }
        else { if(c<'0'||c>'9'||++digits>3)return false; value=value*10+(c-'0'); }
    }
    return fields==4;
}
bool Text(std::uintptr_t p, char* buffer, size_t cap) {
    for(size_t i=0;i<cap;++i) { if(!hp::Read(p+i,buffer[i]))return false; if(!buffer[i])return true; }
    return false;
}
bool Read32(std::uintptr_t p, std::uint32_t& v) { return hp::Read(p,v); }
void RecordNativeResult(const char* operation, unsigned result) noexcept {
    std::uint32_t network=0,hr=0;
    const bool valid=Read32(0x69d858,network)&&network&&Read32(network+0x40,hr);
    try { std::lock_guard<std::mutex> lock(mutex);snapshot.native_operation=operation;
        snapshot.native_call_result=result;snapshot.native_hresult=hr;snapshot.native_hresult_valid=valid;
    } catch (...) {}
}
bool Write32(std::uintptr_t p, std::uint32_t v) {
    SIZE_T done=0;
    return p && WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),&v,4,&done)&&done==4;
}
bool NativeChoice(unsigned choice) {
    std::uint32_t manager=0;
    return Read32(0x6992d0,manager)&&manager&&Write32(manager+0x38,choice);
}
bool Bytes(std::uintptr_t p,const unsigned char* expected,size_t size) {
    unsigned char actual[32]{}; SIZE_T got=0;
    return size<=sizeof actual&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),actual,size,&got)&&
           got==size&&!std::memcmp(actual,expected,size);
}
bool SetName(void* dialog) {
    // H3BaseDlg::GetDlgItem and H3DlgEdit::vSetText; objects are engine constructed.
    using Get=void*(__thiscall*)(void*,int);
    void* edit=reinterpret_cast<Get>(0x5ff5b0)(dialog,0x7d);
    std::uint32_t table=0, setter=0;
    if(!edit||!Read32(reinterpret_cast<std::uintptr_t>(edit),table)||!Read32(table+0x34,setter)||!setter)return false;
    reinterpret_cast<void(__thiscall*)(void*,const char*)>(setter)(edit,running.player_name.c_str());
    return true;
}
bool Establish(void* dialog) {
    if(!SetName(dialog)) { State(Status::Failed,"native_name_control_unavailable"); return false; }
    if(running.port&&!hota_port::Arm(running.port)) { State(Status::Failed,hota_port::ErrorName(hota_port::GetSnapshot().error));return false; }
    struct PortScope { bool active; ~PortScope() { if(active)hota_port::Disarm(); } } port_scope{running.port!=0};
    unsigned configured=0;
    if(running.role==Role::Host) {
        // Native TCP/IP button path0050F740 calls these in order. Besides control
        // updates,511660 performs native Winsock startup/address query and initial
        // session enumeration. Preserve those provider-side effects before Open.
        reinterpret_cast<void(__thiscall*)(void*)>(0x50f150)(dialog);
        configured=reinterpret_cast<unsigned char(__thiscall*)(void*)>(0x511660)(dialog);
        RecordNativeResult("host_tcp_prelude",configured);
    } else {
        using Configure=unsigned char(__thiscall*)(void*,int,const char*,unsigned);
        configured=reinterpret_cast<Configure>(0x50fe20)(dialog,2,running.address.c_str(),0);
        RecordNativeResult("join_tcp_initialize",configured);
    }
    if(running.port) {
        const auto port=hota_port::Disarm();port_scope.active=false;
        if(!port.Applied()) { State(Status::Failed,port.error==hota_port::Error::None?"native_port_not_applied":hota_port::ErrorName(port.error));return false; }
    }
    if(!configured) {
        State(Status::Failed,"native_tcp_initialization_failed"); return false;
    }
    std::uint32_t network=0,table=0;
    if(!Read32(0x69d858,network)||!network||!Read32(network,table)||!table) {
        State(Status::Failed,"native_network_object_missing"); return false;
    }
    bool success=false;
    if(running.role==Role::Host) {
        // Same role globals as native Host handler00510000 (Base207960).
        if(!Write32(0x6992cc,1)||!Write32(0x699534,1))return false;
        using Host=unsigned char(__thiscall*)(void*,const char*,const char*);
        const unsigned result=reinterpret_cast<Host>(0x50fd10)(dialog,running.session_name.c_str(),nullptr);
        RecordNativeResult("host_session",result);success=result!=0;
    } else {
        // Same role setup as native Join handler005111B0 (Base208755).
        if(!Write32(0x6992cc,2)||!Write32(0x699534,1)||!Write32(0x6992d8,1))return false;
        std::uint32_t list=0,enumerate=0;
        if(!Read32(reinterpret_cast<std::uintptr_t>(dialog)+0x60,list)||!list||!Read32(table+0x68,enumerate)||!enumerate)return false;
        using Enum=unsigned char(__thiscall*)(void*,void*,unsigned,unsigned);
        // Exactly native explicit-IP enumeration flags/timeout005118D0. It owns
        // allocation/clearing; do not retain entries beyond this callback.
        const unsigned enumerated=reinterpret_cast<Enum>(enumerate)(reinterpret_cast<void*>(network),reinterpret_cast<void*>(list),5000,0x42);
        RecordNativeResult("enumerate_sessions",enumerated);
        if(!enumerated) {
            State(Status::Failed,"native_session_enumeration_failed");return false;
        }
        std::uint32_t count=0,list_table=0,get=0;
        if(!Read32(list+0x10,count)||count>256||!Read32(list,list_table)||!Read32(list_table+8,get)||!get)return false;
        std::uint32_t selected=0;
        for(unsigned i=0;i<count;++i) {
            using Get=std::uint32_t(__thiscall*)(void*,unsigned);
            const auto entry=reinterpret_cast<Get>(get)(reinterpret_cast<void*>(list),i);
            char name[256]{};
            if(!entry||!Text(entry+0x2c,name,sizeof name))continue;
            if(char* delimiter=std::strchr(name,static_cast<char>(0xfa))) *delimiter=0;
            if(running.session_name==name) {
                if(selected) { State(Status::Failed,"native_session_name_ambiguous");return false; }
                selected=entry;
            }
        }
        if(!selected) { State(Status::Failed,"native_named_session_not_found");return false; }
        using Join=unsigned char(__thiscall*)(void*,void*,const char*);
        const unsigned result=reinterpret_cast<Join>(0x50fc70)(dialog,reinterpret_cast<void*>(selected),nullptr);
        RecordNativeResult("join_session",result);success=result!=0;
    }
    std::uint32_t id=0,host_fn=0;
    if(!success||!Read32(0x69d658,id)||!id||!Read32(table+0x90,host_fn)||!host_fn) {
        State(Status::Failed,"native_session_creation_or_join_failed");return false;
    }
    const bool host=reinterpret_cast<unsigned char(__thiscall*)(void*)>(host_fn)(reinterpret_cast<void*>(network))!=0;
    if(host!=(running.role==Role::Host)) { State(Status::Failed,"native_role_mismatch");return false; }
    { std::lock_guard<std::mutex> lock(mutex); snapshot.local_player_id=id;snapshot.native_host=host; }
    return NativeChoice(0x7802);
}
unsigned __stdcall TypeHook(HiHook* hook,void* dialog) {
    using Fn=unsigned(__thiscall*)(void*);
    if(failure_unwind_pending&&GetCurrentThreadId()==game_thread) {
        // Network cancel7801 re-enters NewGame's type loop. Its Back selector is
        // 68 (Base183248), unlike the load-game loop's7801 cancellation contract.
        if(NativeChoice(0x68)) { failure_unwind_pending=false;busy.store(false);return 0; }
    }
    if(!dispatching||GetCurrentThreadId()!=game_thread) return reinterpret_cast<Fn>(hook->GetDefaultFunc())(dialog);
    State(Status::SelectingNetwork);
    if(!NativeChoice(running.role==Role::SoloScaffold?0x64:0x66)) { dispatching=false;State(Status::Failed,"native_manager_unavailable"); }
    return 0; // Wrapper reads manager selector; native constructor/destructor run normally.
}
unsigned __stdcall NetworkModalHook(HiHook* hook,void* dialog,unsigned parameter) {
    using Fn=unsigned(__thiscall*)(void*,unsigned);
    if(!dispatching||GetCurrentThreadId()!=game_thread) return reinterpret_cast<Fn>(hook->GetDefaultFunc())(dialog,parameter);
    std::uint32_t current=0;
    if(!Read32(0x69ca78,current)||current!=reinterpret_cast<std::uintptr_t>(dialog)||parameter!=0) {
        dispatching=false;failure_unwind_pending=true;State(Status::Failed,"native_network_dialog_phase_mismatch");NativeChoice(0x7801);return 0;
    }
    State(Status::EstablishingNetwork);
    bool success=false;
    try { success=Establish(dialog); }
    catch(...) { State(Status::Failed,"bootstrap_observer_exception"); }
    if(success) State(Status::NetworkEstablished);
    else {
        if(!Failed())State(Status::Failed,"native_session_setup_failed");
        // Native error/cancel paths use this network teardown helper.
        reinterpret_cast<void(__fastcall*)()>(0x5548a0)();
        NativeChoice(0x7801);dispatching=false;failure_unwind_pending=true;
    }
    return 0;
}
unsigned __stdcall ScenarioHook(HiHook* hook,void* dialog,unsigned parameter) {
    using Fn=unsigned(__thiscall*)(void*,unsigned);
    if(dispatching&&GetCurrentThreadId()==game_thread) {
        dispatching=false;
        bool connected=false;
        try { std::lock_guard<std::mutex> lock(mutex);connected=snapshot.status==Status::NetworkEstablished; } catch (...) {}
        const bool solo=running.role==Role::SoloScaffold;
        if(connected||solo) {
            bool applied=false;
            if((running.role==Role::Host||solo)&&!running.map_basename.empty()) {
                const auto result=hota_scenario::ApplyRequestedMap(dialog,running.map_basename,game_thread,solo);
                applied=result.applied;
                if(!applied) State(Status::Failed,result.error.c_str());
            }
            if(!Failed()) {
                { std::lock_guard<std::mutex> lock(mutex);snapshot.map_applied=applied; }
                if(solo){
                    hota_start::Request start;start.token=running.token;start.action=hota_start::Action::SoloScaffoldStart;
                    start.map_basename=running.map_basename;std::string error;
                    if(!hota_start::Queue(start,&error))State(Status::Failed,error.c_str());
                    else State(Status::AwaitingScenarioConfiguration,"native_solo_scaffold_start_queued_input_requires_checkpoint_restore");
                }else State(Status::AwaitingScenarioConfiguration,"player_configuration_ready_and_start_not_automated");
            }
        } else State(Status::Failed,"native_network_setup_hook_was_bypassed");
    }
    return reinterpret_cast<Fn>(hook->GetDefaultFunc())(dialog,parameter);
}
}
const char* StatusName(Status s) noexcept {
    switch(s) {
    case Status::Idle:return "idle";case Status::Queued:return "queued";
    case Status::Dispatching:return "dispatching";case Status::SelectingNetwork:return "selecting_network";
    case Status::EstablishingNetwork:return "establishing_network";case Status::NetworkEstablished:return "network_established";
    case Status::AwaitingScenarioConfiguration:return "awaiting_scenario_configuration";default:return "failed";
    }
}
Snapshot GetSnapshot() { std::lock_guard<std::mutex> lock(mutex);return snapshot; }
bool Prepare(const Request& r,std::string* error) noexcept {
    try {
        if(!installed.load()) { Error(error,"session_hooks_not_installed");return false; }
        if(!r.token||!Ascii(r.player_name,20)||!Ascii(r.session_name,63)||
           (r.role!=Role::Host&&r.role!=Role::Join&&r.role!=Role::SoloScaffold)||(r.role==Role::Join&&!IPv4(r.address))||
           (r.role==Role::SoloScaffold&&r.map_basename.empty())||
           !Ascii(r.map_basename,120,true)||r.map_basename.find_first_of("/\\:")!=std::string::npos||
           r.map_basename.find("..")!=std::string::npos) { Error(error,"invalid_bootstrap_request");return false; }
        std::lock_guard<std::mutex> lock(mutex);
        if(busy.load()||(snapshot.status!=Status::Idle&&snapshot.status!=Status::Failed)) { Error(error,"bootstrap_already_active");return false; }
        queued=r;snapshot=Snapshot{};snapshot.token=r.token;snapshot.status=Status::Queued;return true;
    } catch(...) { Error(error,"bootstrap_request_allocation_failed");return false; }
}
bool CancelQueued() noexcept {
    try { std::lock_guard<std::mutex> lock(mutex);if(snapshot.status!=Status::Queued)return false;
        queued=Request{};snapshot=Snapshot{};return true; } catch(...) { return false; }
}
bool CancelPrepared(std::uint32_t token) noexcept {
    try { std::lock_guard<std::mutex> lock(mutex);if(snapshot.status!=Status::Queued||snapshot.token!=token)return false;
        queued=Request{};snapshot=Snapshot{};return true; } catch(...) { return false; }
}
bool Begin(std::uint32_t token) noexcept {
    if(GetCurrentThreadId()!=game_thread||!installed.load()||dispatching)return false;
    std::uint32_t depth=1,menu=1,network=1,manager=0,choice=0;
    if(!Read32(0x6aad88,depth)||depth||!Read32(0x6996b0,menu)||menu||
       !Read32(0x69959c,network)||network||!Read32(0x6992d0,manager)||!manager||
       !Read32(manager+0x38,choice)||choice!=0x65) {
        State(Status::Failed,"bootstrap_requires_clean_new_game_dispatch");return false;
    }
    try { std::lock_guard<std::mutex> lock(mutex);
        if(snapshot.status!=Status::Queued||snapshot.token!=token)return false;
        running=queued;snapshot.status=Status::Dispatching;dispatching=true;busy.store(true);
        scaffold_pending.store(running.role==Role::SoloScaffold);return true;
    } catch(...) { State(Status::Failed,"bootstrap_dispatch_allocation_failed");return false; }
}
bool Install(PatcherInstance* instance,std::string* error) noexcept {
    if(installed.load())return true;
    if(!instance) { Error(error,"missing_patcher");return false; }
    const unsigned char modal[]={0xe8,0xc3,0xed,0x10,0x00};
    const unsigned char configure[]={0x55,0x8b,0xec,0x83,0xec,0x28,0x56,0x8b,0xf1};
    if(!Bytes(0x4f0c58,modal,sizeof modal)||!Bytes(0x50fe20,configure,sizeof configure)) {
        Error(error,"native_bootstrap_instruction_mismatch");return false;
    }
    game_thread=GetCurrentThreadId();
    hooks[0]=instance->WriteHiHook(0x4d5b20,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(TypeHook));
    hooks[1]=instance->WriteHiHook(0x4f0c58,CALL_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(NetworkModalHook));
    hooks[2]=instance->WriteHiHook(0x584ec0,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(ScenarioHook));
    if(!hooks[0]||!hooks[1]||!hooks[2]||!hooks[0]->IsApplied()||!hooks[1]->IsApplied()||!hooks[2]->IsApplied()) {
        Uninstall();Error(error,"native_bootstrap_hook_failed");return false;
    }
    installed.store(true);return true;
}
bool Uninstall(std::string* error) noexcept {
    if(game_thread&&GetCurrentThreadId()!=game_thread) { Error(error,"uninstall_requires_game_thread");return false; }
    if(dispatching||failure_unwind_pending) { Error(error,"bootstrap_in_progress");return false; }
    bool success=true;
    for(auto& hook:hooks) if(hook) { if(hook->Destroy())hook=nullptr;else success=false; }
    if(success)installed.store(false);else Error(error,"native_bootstrap_unhook_failed");
    return success;
}
bool IsScaffoldRestorePending() noexcept{return scaffold_pending.load();}
bool EstablishSoloTransportAtBoundary(std::uint32_t token,std::string* error) noexcept {
    if(GetCurrentThreadId()!=game_thread||!scaffold_pending.load()||running.role!=Role::SoloScaffold||running.token!=token||dispatching){
        Error(error,"solo_transport_requires_owned_scaffold_boundary");return false;
    }
    std::uint32_t world=0,network=1,transport=1,scenario=1,depth=1,dialog_now=1,manager=0,choice=0;
    if(!Read32(0x699538,world)||!world||!Read32(0x69959c,network)||network||!Read32(0x69d858,transport)||transport||
       !Read32(0x69fc44,scenario)||scenario||!Read32(0x6aad88,depth)||depth||!Read32(0x69ca78,dialog_now)||dialog_now||
       !Read32(0x6992d0,manager)||!manager||!Read32(manager+0x38,choice)){
        Error(error,"solo_transport_requires_restored_inactive_network_adventure");return false;
    }
    // Native4F0C20's stack allocation is exactly100h bytes for this object.
    const unsigned char frame[]={0x81,0xec,0x00,0x01,0x00,0x00,0x8d,0x8d,0xf4,0xfe,0xff,0xff};
    const unsigned char destructor[]={0x89,0x1d,0x78,0xca,0x69,0};
    if(!Bytes(0x4f0c38,frame,sizeof frame)||!Bytes(0x50f0c9,destructor,sizeof destructor)){
        Error(error,"solo_native_dialog_lifecycle_bytes_mismatch");return false;
    }
    __declspec(align(16)) unsigned char native_dialog[0x100]{};
    bool constructed=false,success=false;
    dispatching=true; // prevents reentrant bootstrap during native provider work
    try{
        reinterpret_cast<void(__thiscall*)(void*)>(0x50e250)(native_dialog);constructed=true;
        running.role=Role::Host;success=Establish(native_dialog);running.role=Role::SoloScaffold;
        reinterpret_cast<void(__thiscall*)(void*)>(0x50f0a0)(native_dialog);constructed=false;
    }catch(...){
        running.role=Role::SoloScaffold;
        if(constructed)reinterpret_cast<void(__thiscall*)(void*)>(0x50f0a0)(native_dialog);
        success=false;
    }
    dispatching=false;
    const bool selector_restored=Write32(manager+0x38,choice);
    if(!success||!selector_restored){
        reinterpret_cast<void(__cdecl*)()>(0x5548a0)();
        State(Status::Failed,"solo_native_transport_establishment_failed");Error(error,"solo_native_transport_establishment_failed");return false;
    }
    if(!Read32(0x69d858,transport)||!transport||!Read32(0x69959c,network)||network!=1){
        State(Status::Failed,"solo_native_transport_postconditions_failed");Error(error,"solo_native_transport_postconditions_failed");return false;
    }
    State(Status::NetworkEstablished,"solo_transport_created_logical_roster_rebind_required");Error(error,"");return true;
}
bool ReleaseScaffoldGate(std::uint32_t token,std::string* error) noexcept {
    if(GetCurrentThreadId()!=game_thread||!scaffold_pending.load()||running.role!=Role::SoloScaffold||running.token!=token){
        Error(error,"scaffold_gate_identity_or_thread_mismatch");return false;
    }
    std::uint32_t world=0,network=0,transport=0,scenario=0,depth=1;
    if(!Read32(0x699538,world)||!world||!Read32(0x69959c,network)||network!=1||!Read32(0x69d858,transport)||!transport||
       !Read32(0x69fc44,scenario)||scenario||!Read32(0x6aad88,depth)||depth){Error(error,"scaffold_not_restored_to_native_network_adventure");return false;}
    scaffold_pending.store(false);Error(error,"");return true;
}
}
