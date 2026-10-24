#include "scenario_start.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include "vendor/patcher_x86.hpp"
static_assert(sizeof(void*)==4,"Scenario automation requires x86");
namespace hota_start {
namespace {
SRWLOCK lock=SRWLOCK_INIT;
std::unique_ptr<Request> queued;
Snapshot published;
std::atomic<bool> active{false},owned{false},busy{false},cancel{false};
struct State {
    HiHook* scenario=nullptr; HiHook* options=nullptr; HiHook* procedure=nullptr;
    DWORD thread=0; std::uintptr_t hd=0;
    std::unique_ptr<Request> request;
    Snapshot snapshot;
    unsigned scenario_depth=0, options_depth=0, procedure_depth=0;
    bool starting=false, options_owned=false, options_failed=false;
    std::uint32_t options_mode=UINT32_MAX;
} state;
void Error(std::string* out,const char* value) noexcept { if(out)try{*out=value;}catch(...){} }
bool Read(std::uintptr_t p,void* value,SIZE_T n) noexcept {
    SIZE_T got=0;
    return p && p<=UINT32_MAX && n<=UINT32_MAX-p+1 &&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),value,n,&got)&&got==n;
}
template<class T> bool Read(std::uintptr_t p,T& v) noexcept {return Read(p,&v,sizeof v);}
std::uintptr_t HD(std::uint32_t p) noexcept {return state.hd+(p-0x01000000);}
void Detail(const char* v) noexcept {strncpy_s(state.snapshot.detail,v,_TRUNCATE);}
void Publish(Status status,const char* detail=nullptr) noexcept {
    state.snapshot.status=status;
    if(detail)Detail(detail);
    AcquireSRWLockExclusive(&lock); published=state.snapshot; ReleaseSRWLockExclusive(&lock);
}
void Finish(Status status,const char* detail) noexcept {
    Publish(status,detail); state.request.reset(); cancel.store(false); busy.store(false);
}
void Claim() noexcept {
    if(state.request||!busy.load()||!TryAcquireSRWLockExclusive(&lock))return;
    state.request=std::move(queued);
    if(state.request){state.snapshot=Snapshot{};state.snapshot.token=state.request->token;state.snapshot.thread_id=state.thread;}
    ReleaseSRWLockExclusive(&lock);
}
bool Equal(const std::string& expected,const char* actual) noexcept {
    const size_t n=strnlen_s(actual,251);
    if(n!=expected.size())return false;
    for(size_t i=0;i<n;++i){unsigned char a=expected[i],b=actual[i];
        if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(a!=b)return false;}
    return true;
}
bool Valid(const Request& r) noexcept {
    if(!r.token||(r.action!=Action::SoloScaffoldStart&&(!r.peer_ids[0]||!r.peer_ids[1]||r.peer_ids[0]==r.peer_ids[1]))||
        (r.action!=Action::HostStart&&r.action!=Action::PeerAccept&&r.action!=Action::SoloScaffoldStart)||r.map_basename.empty()||r.map_basename.size()>120||
        r.map_basename.find_first_of("/\\:")!=std::string::npos||r.map_basename.find("..")!=std::string::npos)return false;
    for(unsigned char c:r.map_basename)if(c<32||c>126)return false;
    const auto month=r.date_mwd/100,week=r.date_mwd/10%10,day=r.date_mwd%10;
    return month>=1&&month<=99&&week>=1&&week<=4&&day>=1&&day<=7;
}
bool Ready(std::uint32_t dialog,bool require_host) noexcept {
    state.snapshot.setup_ready=false;state.snapshot.scenario=dialog;state.snapshot.assigned_humans=0;
    state.snapshot.pending_transfers=0;state.snapshot.colors[0]=state.snapshot.colors[1]=-1;
    if(!state.request){Detail("no_owned_request");return false;}
    std::uint32_t current=0,net=0,mode=0,network=0,vt=0,hostfn=0,scan=1,event=1,entry=0,transfers=0;
    unsigned char flags[3]{},random=1,campaign=1;
    if(!dialog||dialog>UINT32_MAX-0x1970||!Read(0x69fc44,current)||current!=dialog||
       !Read(dialog+0x64,flags,sizeof flags)||flags[0]||flags[1]||flags[2]||
       !Read(dialog+0x37f,random)||random||!Read(0x69779c,campaign)||campaign||
       !Read(0x69959c,net)||!Read(0x698a40,mode)||
       !Read(0x69fe28,scan)||scan||!Read(0x69fe2c,event)||event){Detail("scenario_or_native_scan_not_ready");return false;}
    const bool solo=state.request->action==Action::SoloScaffoldStart;
    char selected_name[251]{};
    if(solo){
        if(net||mode==2||mode==3||!Read(0x69d858,network)||network||
           !Read(dialog+0x1060,entry)||!entry||entry>UINT32_MAX-0xca4||!Read(entry+0x33d,selected_name,sizeof selected_name)||
           !std::memchr(selected_name,0,sizeof selected_name)||!Equal(state.request->map_basename,selected_name)){
            Detail("solo_scaffold_map_or_mode_mismatch");return false;
        }
        state.snapshot.setup_ready=true;Detail("native_singleplayer_scaffold_ready_for_checkpoint_load");return true;
    }
    if(!net||mode!=2){Detail("requires_native_tcp_setup");return false;}
    if(!Read(0x69d858,network)||!network||!Read(network,vt)||!vt||!Read(vt+0x90,hostfn)||!hostfn){Detail("native_network_role_unreadable");return false;}
    const bool host=reinterpret_cast<unsigned char(__thiscall*)(void*)>(hostfn)(reinterpret_cast<void*>(network))!=0;
    if(host!=require_host){Detail("request_network_role_mismatch");return false;}
    char name[251]{};
    if(!Read(dialog+0x1060,entry)||!entry||entry>UINT32_MAX-0xca4||!Read(entry+0x33d,name,sizeof name)||
       !std::memchr(name,0,sizeof name)||!Equal(state.request->map_basename,name)){Detail("requested_map_not_selected");return false;}
    unsigned seen=0,occupied=0;
    for(unsigned i=0;i<8;++i){std::uint32_t peer=0;std::int32_t color=-1;
        if(!Read(dialog+0x1064+i*0x7c,peer)||!Read(dialog+0x10d4+i*0x7c,color)){Detail("participants_unreadable");return false;}
        if(!peer)continue;++occupied;
        const int slot=peer==state.request->peer_ids[0]?0:peer==state.request->peer_ids[1]?1:-1;
        if(slot<0||color<0||color>7||(seen&(1u<<slot))){Detail("unexpected_or_unassigned_participant");return false;}
        seen|=1u<<slot;state.snapshot.peer_ids[slot]=peer;state.snapshot.colors[slot]=color;
        ++state.snapshot.assigned_humans;
    }
    if(occupied!=2||seen!=3||state.snapshot.colors[0]==state.snapshot.colors[1]){Detail("waiting_for_two_distinct_assigned_humans");return false;}
    if(require_host){std::uint32_t pending[8]{};
        if(!Read(dialog+0x388,transfers)||!transfers||!Read(transfers,pending,sizeof pending)){Detail("header_transfer_table_unreadable");return false;}
        for(auto p:pending)if(p)++state.snapshot.pending_transfers;
        if(state.snapshot.pending_transfers){Detail("waiting_for_native_header_transfers");return false;}
    }
    state.snapshot.setup_ready=true;Detail("native_setup_ready");return true;
}
struct Preserve {
    __declspec(align(16)) unsigned char fp[512]; DWORD error;
    Preserve() noexcept:error(GetLastError()){_fxsave(fp);}
    ~Preserve() noexcept{SetLastError(error);_fxrstor(fp);}
};

unsigned __stdcall ScenarioHook(HiHook* hook,void* self,std::uint32_t* message) {
    using Fn=unsigned(__thiscall*)(void*,std::uint32_t*);
    const auto previous=reinterpret_cast<Fn>(hook->GetDefaultFunc());
    if(!active.load()||GetCurrentThreadId()!=state.thread)return previous(self,message);
    ++state.scenario_depth;
    bool injected=false;
    {
        Preserve preserve;
        if(state.scenario_depth==1&&!state.options_depth){
            Claim();
            if(state.request&&cancel.load())Finish(Status::Cancelled,"cancelled_before_start");
            if(state.request){
                const bool host=state.request->action==Action::HostStart||state.request->action==Action::SoloScaffoldStart;
                std::uint32_t msg[8]{},depth=0;
                const bool ready=Ready(reinterpret_cast<std::uint32_t>(self),host);
                if(!host)Publish(Status::PeerArmed);
                else if(ready&&Read(reinterpret_cast<std::uintptr_t>(message),msg,sizeof msg)&&msg[0]==0&&
                        msg[7]==reinterpret_cast<std::uint32_t>(self)&&Read(0x6aad88,depth)&&depth==1){
                    AcquireSRWLockExclusive(&lock);
                    if(!cancel.load()){
                        message[0]=0x200;message[1]=0xd;message[2]=0xba;
                        state.starting=true;state.snapshot.start_event_sent=true;injected=true;
                        state.snapshot.status=Status::Starting;Detail("native_start_event_injected");published=state.snapshot;
                    }
                    ReleaseSRWLockExclusive(&lock);
                }else Publish(Status::WaitingForNativeSetup);
            }
        }
    }
    const unsigned result=previous(self,message);
    {
        Preserve preserve;
        if(injected){
            state.starting=false;state.snapshot.native_result=result;
            if(state.request){const bool accepted=result==2&&(state.snapshot.options_accepted||state.request->action==Action::SoloScaffoldStart);
                Finish(accepted?Status::HostStartReturned:Status::Refused,
                    accepted?"native_start_handler_closed_scenario":"native_start_declined_or_options_unconfirmed");}
        }
        --state.scenario_depth;
    }
    return result;
}

int __stdcall OptionsHook(HiHook* hook,int mode,std::uint32_t* bits,std::uint32_t* date) {
    using Fn=int(__fastcall*)(int,std::uint32_t*,std::uint32_t*);
    const auto previous=reinterpret_cast<Fn>(hook->GetDefaultFunc());
    if(!active.load()||GetCurrentThreadId()!=state.thread)return previous(mode,bits,date);
    ++state.options_depth;
    const bool outer=state.options_depth==1;
    {
        Preserve preserve;
        if(outer){
            Claim();state.options_owned=false;state.options_failed=false;state.options_mode=mode;
            if(state.request&&state.request->action!=Action::SoloScaffoldStart){
                const bool host=state.request->action==Action::HostStart;
                std::uint32_t scenario=0,in_bits=0,in_date=0;
                const bool expected=host?(mode==1&&state.starting):(mode==0);
                if(expected&&Read(0x69fc44,scenario)&&Ready(scenario,host)){
                    const bool proposal=host||(Read(reinterpret_cast<std::uintptr_t>(bits),in_bits)&&
                        Read(reinterpret_cast<std::uintptr_t>(date),in_date)&&(in_bits&4)&&in_date==state.request->date_mwd);
                    if(proposal){state.options_owned=true;state.snapshot.options_mode=mode;Publish(Status::ConfiguringOptions,"owned_native_options_scope");}
                    else {state.options_owned=true;state.options_failed=true;
                        state.snapshot.options_mode=mode;Publish(Status::Refused,"peer_options_proposal_mismatch");}
                }
            }
        }
    }
    const int result=previous(mode,bits,date);
    {
        Preserve preserve;
        if(outer){
            const bool peer=state.request&&state.request->action==Action::PeerAccept;
            if(state.options_owned){
                state.snapshot.native_result=result;
                if(result!=1||state.options_failed||!state.snapshot.options_accepted)
                    Publish(Status::Refused,"native_options_did_not_accept_verified_values");
                else Publish(Status::OptionsAccepted,"native_options_accepted");
                if(peer)Finish(state.snapshot.status==Status::OptionsAccepted?Status::PeerAccepted:Status::Refused,
                    state.snapshot.status==Status::OptionsAccepted?"peer_native_accept_returned_to_ack_sender":"peer_options_refused");
            }
            state.options_owned=false;
        }
        --state.options_depth;
    }
    return result;
}

bool Controls(void* dialog,std::uint32_t& check,std::uint32_t& edit,std::uint32_t& value,std::uint32_t& date) noexcept {
    using Find=std::uint32_t(__thiscall*)(void*,int);
    check=reinterpret_cast<Find>(0x5ff5b0)(dialog,0x7db);
    edit=reinterpret_cast<Find>(0x5ff5b0)(dialog,0x7de);
    std::uint32_t owner=0,text=0;short id=0;char number[12]{};
    if(!check||!edit||!Read(check+4,owner)||owner!=reinterpret_cast<std::uint32_t>(dialog)||
       !Read(check+0x10,id)||id!=0x7db||!Read(check+0x34,value)||value>1||
       !Read(edit+4,owner)||owner!=reinterpret_cast<std::uint32_t>(dialog)||!Read(edit+0x10,id)||id!=0x7de||
       !Read(edit+0x34,text)||!text)return false;
    date=0;
    for(unsigned i=0;i<sizeof number;++i){
        if(!Read(text+i,number[i]))return false;
        if(!number[i])return i>0;
        if(number[i]<'0'||number[i]>'9'||i>=4)return false;
        date=date*10+static_cast<unsigned>(number[i]-'0');
    }
    return false;
}
unsigned __stdcall ProcedureHook(HiHook* hook,void* dialog,std::uint32_t* message) {
    using Fn=unsigned(__stdcall*)(void*,std::uint32_t*);
    const auto previous=reinterpret_cast<Fn>(hook->GetDefaultFunc());
    bool accepting=false;
    if(active.load()&&GetCurrentThreadId()==state.thread){
        ++state.procedure_depth;
        {
            Preserve preserve;
            if(state.procedure_depth==1&&state.options_depth==1&&state.options_owned&&state.request){
                std::uint32_t mode=UINT32_MAX,msg[8]{},check=0,edit=0,value=0,date=0;
                if(Read(reinterpret_cast<std::uintptr_t>(dialog)+0x84,mode)&&mode==state.options_mode&&
                   Read(reinterpret_cast<std::uintptr_t>(message),msg,sizeof msg)&&msg[0]==0){
                    if(state.options_failed){message[0]=0x200;message[1]=0xd;message[2]=2;}
                    else if(!Controls(dialog,check,edit,value,date)){
                        state.options_failed=true;Publish(Status::Refused,"native_options_controls_unverified");
                    }else{
                        state.snapshot.checkbox=value;state.snapshot.date_mwd=date;
                        const bool host=state.request->action==Action::HostStart;
                        if(!host&&(value!=1||date!=state.request->date_mwd)){
                            state.options_failed=true;Publish(Status::Refused,"peer_displayed_options_mismatch");
                        }else if(host&&value!=1){message[0]=0x200;message[1]=0xc;message[2]=0x7db;}
                        else if(host&&date!=state.request->date_mwd){
                            std::uint32_t vt=0,setter=0;
                            if(!Read(edit,vt)||!vt||!Read(vt+0x34,setter)||setter!=0x5bbd00){
                                state.options_failed=true;Publish(Status::Refused,"native_edit_setter_mismatch");
                            }else{char text[12]{};sprintf_s(text,"%u",state.request->date_mwd);
                                reinterpret_cast<void(__thiscall*)(void*,const char*)>(setter)(reinterpret_cast<void*>(edit),text);}
                        }else{message[0]=0x200;message[1]=0xd;message[2]=1;accepting=true;}
                    }
                }
            }
        }
    }
    const unsigned result=previous(dialog,message);
    if(active.load()&&GetCurrentThreadId()==state.thread){
        Preserve preserve;
        if(accepting&&state.request){std::uint32_t enabled=0,date=0;
            state.snapshot.options_accepted=result==2&&Read(HD(0x121000a),enabled)&&enabled==1&&
                Read(HD(0x121000e),date)&&date==state.request->date_mwd;
            if(!state.snapshot.options_accepted){state.options_failed=true;Publish(Status::Refused,"native_options_commit_not_verified");}
        }
        --state.procedure_depth;
    }
    return result;
}

bool Bytes(std::uintptr_t address,const unsigned char* expected,size_t count) noexcept {
    unsigned char actual[24]{};return count<=sizeof actual&&Read(address,actual,count)&&!std::memcmp(actual,expected,count);
}
bool Validate() noexcept {
    const unsigned char proc[]={0x55,0x8b,0xec,0x6a,0xff};
    const unsigned char options[]={0x55,0x8b,0xec,0x83,0xec,0x14};
    const unsigned char ret4[]={0xc2,0x04,0};
    const unsigned char lookup[]={0x55,0x8b,0xec,0x8b,0x41,0x2c,0x85,0xc0};
    const unsigned char scenario[]={0x8b,0xf1,0xc6,0x45,0xff,0,0xe8,0x3c,0x5b,0xf6,0xff};
    return Bytes(HD(0x1186ee0),proc,sizeof proc)&&Bytes(HD(0x1189ee0),options,sizeof options)&&
        Bytes(HD(0x1189f84),ret4,sizeof ret4)&&Bytes(0x5ff5b0,lookup,sizeof lookup)&&
        Bytes(0x587fd9,scenario,sizeof scenario)&&Bytes(0x5bbdcd,ret4,sizeof ret4);
}
bool Destroy(HiHook*& hook) noexcept {if(!hook)return true;try{if(!hook->Destroy())return false;hook=nullptr;return true;}catch(...){return false;}}
bool Rollback() noexcept {
    active.store(false);
    const bool a=Destroy(state.procedure),b=Destroy(state.options),c=Destroy(state.scenario);
    owned.store(state.procedure||state.options||state.scenario);
    if(a&&b&&c){state=State{};AcquireSRWLockExclusive(&lock);queued.reset();ReleaseSRWLockExclusive(&lock);busy.store(false);cancel.store(false);}
    return a&&b&&c;
}
}
const char* StatusName(Status status) noexcept {
    switch(status){case Status::Idle:return "idle";case Status::Queued:return "queued";
    case Status::WaitingForNativeSetup:return "waiting_for_native_setup";case Status::PeerArmed:return "peer_armed";
    case Status::Starting:return "starting";case Status::ConfiguringOptions:return "configuring_options";
    case Status::OptionsAccepted:return "options_accepted";case Status::HostStartReturned:return "host_start_returned";
    case Status::PeerAccepted:return "peer_accepted";case Status::Cancelled:return "cancelled";default:return "refused";}
}
bool Install(PatcherInstance* patcher,std::uintptr_t hd,std::string* error) noexcept {
    if(!patcher||owned.load()||!hd||hd>UINT32_MAX-0x219f84){Error(error,"invalid_patcher_or_hd_base");return false;}
    state.hd=hd;state.thread=GetCurrentThreadId();
    if(!Validate()){state=State{};Error(error,"scenario_start_pinned_bytes_mismatch");return false;}
    try{
        state.scenario=patcher->CreateHiHook(0x587fd0,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(ScenarioHook));
        owned.store(state.scenario!=nullptr);
        state.options=patcher->CreateHiHook(static_cast<_ptr_>(HD(0x1189ee0)),SPLICE_,EXTENDED_,FASTCALL_,reinterpret_cast<void*>(OptionsHook));
        owned.store(state.scenario||state.options);
        state.procedure=patcher->CreateHiHook(static_cast<_ptr_>(HD(0x1186ee0)),SPLICE_,EXTENDED_,STDCALL_,reinterpret_cast<void*>(ProcedureHook));
        owned.store(state.scenario||state.options||state.procedure);
        if(!state.scenario||!state.options||!state.procedure||state.procedure->Apply()<0||state.options->Apply()<0||state.scenario->Apply()<0||
            !state.procedure->IsApplied()||!state.options->IsApplied()||!state.scenario->IsApplied()){
            Error(error,Rollback()?"scenario_start_apply_failed":"scenario_start_rollback_incomplete");return false;}
        active.store(true);Error(error,"");return true;
    }catch(...){Error(error,Rollback()?"scenario_start_exception":"scenario_start_rollback_incomplete");return false;}
}
bool Uninstall(std::string* error) noexcept {
    if(owned.load()&&(GetCurrentThreadId()!=state.thread||state.scenario_depth||state.options_depth||state.procedure_depth)){
        Error(error,"uninstall_requires_idle_installing_thread");return false;}
    const bool ok=Rollback();Error(error,ok?"":"scenario_start_rollback_incomplete");return ok;
}
bool HasOwnedHooks() noexcept{return owned.load();}
bool Queue(const Request& request,std::string* error) noexcept {
    if(!active.load()||!Valid(request)){Error(error,"invalid_or_uninstalled_start_request");return false;}
    std::uint32_t scenario=0,depth=0;unsigned char flags[3]{};
    if(!Read(0x69fc44,scenario)||!scenario||scenario>UINT32_MAX-0x1970||
       !Read(0x6aad88,depth)||!Read(scenario+0x64,flags,sizeof flags)||flags[0]||flags[1]||flags[2]){
        Error(error,"queue_requires_native_new_scenario_without_nested_dialog");return false;}
    // Solo scaffold is queued by the original ScenarioRun entry interceptor,
    // after the native ctor/map selection and BEFORE its modal Run increments
    // depth. External TCP requests still require the already-running depth1 UI.
    const bool scaffold_entry=request.action==Action::SoloScaffoldStart&&
        GetCurrentThreadId()==state.thread&&depth==0&&!state.scenario_depth&&!state.options_depth&&!state.procedure_depth;
    if(depth!=1&&!scaffold_entry){Error(error,"queue_requires_scenario_run_entry_or_single_modal");return false;}
    if(request.action==Action::SoloScaffoldStart){std::uint32_t network=1,transport=1,mode=2;
        if(GetCurrentThreadId()!=state.thread||!Read(0x69959c,network)||network||!Read(0x69d858,transport)||transport||
           !Read(0x698a40,mode)||mode==2||mode==3){Error(error,"solo_queue_requires_game_thread_native_sp_context");return false;}}

    try{
        auto copy=std::make_unique<Request>(request);
        AcquireSRWLockExclusive(&lock);
        bool expected=false;
        if(!active.load()||!busy.compare_exchange_strong(expected,true)){ReleaseSRWLockExclusive(&lock);Error(error,"start_request_already_pending");return false;}
        cancel.store(false);queued=std::move(copy);published=Snapshot{};published.token=request.token;published.status=Status::Queued;
        ReleaseSRWLockExclusive(&lock);Error(error,"");return true;
    }catch(...){Error(error,"request_allocation_failed");return false;}
}
bool CancelWaiting() noexcept {
    AcquireSRWLockExclusive(&lock);const auto status=published.status;
    const bool allowed=status==Status::Queued||status==Status::WaitingForNativeSetup||status==Status::PeerArmed;
    if(allowed)cancel.store(true);
    ReleaseSRWLockExclusive(&lock);return allowed;
}
Snapshot GetSnapshot() noexcept {AcquireSRWLockShared(&lock);Snapshot s=published;ReleaseSRWLockShared(&lock);return s;}
}
