#include "solo_transport.hpp"
#include "supported_build.hpp"
#include "solo_journal.hpp"
#include "dplay_relay.hpp"
#include "common.hpp"
#include "vendor/patcher_x86.hpp"
#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
namespace hota_solo_transport { namespace {
HiHook *enumerate_hook=nullptr,*send_hook=nullptr;
std::uint32_t hd=0; DWORD thread_id=0;
std::mutex mutex; Snapshot state; std::atomic<bool> blocked{false},active_transport{false};
std::array<std::array<char,21>,8> names{}; unsigned char humans=0; int local_color=-1;
void Error(std::string* e,const char* s) noexcept {if(e)try{*e=s;}catch(...) {}}
bool Bytes(std::uintptr_t a,const unsigned char* b,std::size_t n) {unsigned char x[32]{};SIZE_T got=0;return n<=sizeof x&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(a),x,n,&got)&&got==n&&!std::memcmp(x,b,n);}
bool Write(std::uintptr_t a,const void* p,std::size_t n) {SIZE_T wrote=0;return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(a),p,n,&wrote)&&wrote==n;}
template<class T>bool Write(std::uintptr_t a,const T& t){return Write(a,&t,sizeof t);}
void Fault(const char* s) noexcept {blocked.store(true);try{std::lock_guard<std::mutex> l(mutex);state.fault=true;state.error=s;}catch(...) {}}
unsigned RejectSend(const char* reason) noexcept {
    Fault(reason);
    // Native5537D0 treats E_INVALIDARG as a bounded handled send failure:
    //553804 calls Send; its HRESULT branch skips generic retries/modal prompts.
    // Only the separate DPERR_INVALIDPLAYER branch synthesizes3F6 (553919).
    // Send still returns false; no journal acceptance or protocol ACK is made.
    Write(state.transport+0x40,std::uint32_t(0x80070057));
    return 0;
}
unsigned __stdcall EnumHook(HiHook* h,void* net,void* list,void* group,unsigned flags) {
    using Fn=unsigned char(__thiscall*)(void*,void*,void*,unsigned);
    auto original=reinterpret_cast<Fn>(h->GetDefaultFunc());
    if(!active_transport.load(std::memory_order_acquire)||reinterpret_cast<std::uintptr_t>(net)!=state.transport)return original(net,list,group,flags);
    if(GetCurrentThreadId()!=thread_id||group||flags){Fault("unsupported_solo_enumeration");return 0;}
    if(!original(net,list,group,flags)){Fault("native_local_enumeration_failed");return 0;}
    std::uint32_t count=0,array=0;
    if(!hp::Read(reinterpret_cast<std::uintptr_t>(list)+16,count)||count!=1||
       !hp::Read(reinterpret_cast<std::uintptr_t>(list)+8,array)||!array){Fault("unexpected_native_presence");return 0;}
    std::uint32_t first=0,id=0;
    if(!hp::Read(array,first)||!first||!hp::Read(first+0x100,id)||id!=state.local_dpid){Fault("native_local_presence_mismatch");return 0;}
    struct Name {unsigned size,flags;const char* short_name;const char* long_name;};
    using Add=unsigned(__thiscall*)(void*,unsigned,const Name*,unsigned);
    for(int c=0;c<8;++c)if((humans&(1u<<c))&&c!=local_color){
        const Name name{16,0,names[c].data(),names[c].data()};
        reinterpret_cast<Add>(0x497680)(net,state.ids[c],&name,0);
        std::uint32_t after=0;
        if(!hp::Read(reinterpret_cast<std::uintptr_t>(list)+16,after)||after!=++count||count>8){Fault("logical_presence_allocation_failed");return 0;}
    }
    return 1;
}
unsigned __stdcall SendHook(HiHook* h,void* net,const void* bytes,unsigned size,unsigned sender,unsigned receiver,unsigned reliable) {
    using Fn=unsigned char(__thiscall*)(void*,const void*,unsigned,unsigned,unsigned,unsigned);
    if(!active_transport.load(std::memory_order_acquire)||reinterpret_cast<std::uintptr_t>(net)!=state.transport)
        return reinterpret_cast<Fn>(h->GetDefaultFunc())(net,bytes,size,sender,receiver,reliable);
    int recipient=-1;bool known=receiver==0;
    for(int c=0;c<8;++c)if(state.ids[c]&&state.ids[c]==receiver){recipient=c;known=true;}
    if(blocked.load()||GetCurrentThreadId()!=thread_id||sender!=state.local_dpid||!known||size<20||size>hota_solo_journal::MaxPacketBytes)return RejectSend("invalid_solo_outbound_packet");
    std::uint32_t header[5]{};SIZE_T got=0;
    if(!ReadProcessMemory(GetCurrentProcess(),bytes,header,sizeof header,&got)||got!=sizeof header||header[3]!=size)return RejectSend("invalid_native_packet_header");
    hota_solo_journal::PacketView p;
    p.token=state.token;p.sequence=state.packets+1;p.local_color=local_color;p.recipient_color=recipient;
    p.sender_dpid=sender;p.receiver_dpid=receiver;p.reliable=reliable;p.data=static_cast<const std::uint8_t*>(bytes);p.size=size;
    if(!hota_solo_journal::Accept(p))return RejectSend("outbound_journal_rejected");
    {std::lock_guard<std::mutex> l(mutex);++state.packets;}
    Write(state.transport+0x40,std::uint32_t(0));return 1; // Local queue acceptance, never remote acknowledgment.
}
}
bool Install(PatcherInstance* patcher,std::uint32_t base,std::uint32_t tid,std::string* e) noexcept {
    try{
        if(!patcher||!base||tid!=GetCurrentThreadId()||state.installed){Error(e,"invalid_solo_transport_install");return false;}
        const unsigned char add[]={0x55,0x8b,0xec,0x53,0x8b,0xd9,0x68,4,1,0,0};
        const unsigned char enum_tail[]={0xc2,0x0c,0};const unsigned char send_tail[]={0xc2,0x14,0};
        if(!Bytes(0x497680,add,sizeof add)||!Bytes(0x496f2e,enum_tail,3)||!Bytes(0x497125,send_tail,3)){Error(e,"solo_transport_abi_mismatch");return false;}
        enumerate_hook=patcher->WriteHiHook(0x496ec0,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(EnumHook));
        if(enumerate_hook)send_hook=patcher->WriteHiHook(0x4970e0,SPLICE_,EXTENDED_,THISCALL_,reinterpret_cast<void*>(SendHook));
        if(!enumerate_hook||!send_hook){if(enumerate_hook&&enumerate_hook->Destroy())enumerate_hook=nullptr;Error(e,"solo_transport_hook_install_failed");return false;}
        hd=base;thread_id=tid;{std::lock_guard<std::mutex> l(mutex);state.installed=true;}return true;
    }catch(...){Error(e,"solo_transport_install_exception");return false;}
}
bool ActivateAtBoundary(std::uint32_t token,std::uint32_t world,int local,int current,std::uint8_t mask,const std::vector<std::uint8_t>& source_roster,std::string* e) noexcept {
    try{
        std::uint32_t w=0,net=0,network=0,dpid=0,roster=0;
        if(!state.installed||state.active||GetCurrentThreadId()!=thread_id||!token||local<0||local>7||current<0||current>7||!(mask&(1u<<local))||
           !hp::Read(0x699538,w)||w!=world||!hp::Read(0x69d858,net)||!net||!hp::Read(0x69959c,network)||network!=1||
           !hp::Read(0x69d658,dpid)||!dpid||!hp::Read(hd+hp_build::Rva(0x11b8c84),roster)||!roster){Error(e,"solo_transport_activation_state");return false;}
        std::array<std::uint32_t,8> ids{};unsigned human_count=0;
        for(int c=0;c<8;++c)if(mask&(1u<<c)){
            ++human_count;ids[c]=c==local?dpid:0x7f010000u|static_cast<unsigned>(c);
            if(c!=local&&ids[c]==dpid){Error(e,"solo_reserved_identity_collision");return false;}
            SIZE_T n=0; if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(world+0x20b9c+c*0x168),names[c].data(),20,&n)||n!=20){Error(e,"solo_player_name_unreadable");return false;}
            names[c][20]=0;
        }
        std::array<unsigned char,187> r{};
        if(source_roster.size()!=r.size()){Error(e,"solo_source_roster_size");return false;}
        std::memcpy(r.data(),source_roster.data(),r.size());
        if(r[0]!=human_count||human_count<2){Error(e,"solo_protection_roster_mismatch");return false;}
        // A relay rejoin occupies its own held seat in the running game: the other
        // humans are live relay players with their original identities, not
        // reserved offline placeholders, and packets go out through the relay.
        const bool relay=hp_relay::Rejoining();
        if(relay)for(unsigned i=0;i<r[0]&&i<8;++i){
            auto* row=r.data()+3+i*23;const unsigned c=row[4];std::uint32_t id=0;std::memcpy(&id,row,4);
            if(c>=8||!(mask&(1u<<c)))continue;
            if(c==static_cast<unsigned>(local)?id!=dpid:(!id||id==dpid)){Error(e,"relay_rejoin_identity_mismatch");return false;}
            ids[c]=id;
        }
        unsigned seen=0;for(unsigned i=0;i<r[0];++i){auto* row=r.data()+3+i*23;unsigned c=row[4];if(c>=8||!(mask&(1u<<c))||(seen&(1u<<c))){Error(e,"solo_roster_color_mismatch");return false;}seen|=1u<<c;std::memcpy(row,&ids[c],4);}
        if(seen!=mask){Error(e,"solo_roster_incomplete");return false;}
        // All identity validation precedes the first mutation. Failure after this
        // point is terminal for this isolated client; caller retains input gate.
        for(int c=0;c<8;++c)if(mask&(1u<<c)){
            if(!Write(world+0x20b98+c*0x168,ids[c])){Fault("solo_world_identity_write");return false;}
        }
        if(!Write(roster,r.data(),r.size())||!Write(0x6995a4,local)){Fault("solo_roster_write");return false;}
        for(unsigned i=0;i<8;++i){int color=-1;const auto record=hd+hp_build::Rva(0x2170d0)+i*0x7c;if(!hp::Read(record+0x70,color)){Fault("solo_peer_table_read");return false;}if(color>=0&&color<8&&(mask&(1u<<color)))if(!Write(record,ids[color])){Fault("solo_peer_table_write");return false;}}
        reinterpret_cast<void(__stdcall*)(int)>(hd+hp_build::Rva(0x2880))(current);
        // Native network-load00589DF0 uses local color for fog/view ownership,
        // independently of the current turn color restored by01002880.
        const unsigned view_mask=1u<<local;
        if(!Write(0x6977dc,local)||!Write(0x69cd08,view_mask)){Fault("solo_view_owner_write");return false;}
        // SP reload computed this before source E2 membership reconstruction.
        // Use the same pure native E1&&E2 predicate as the original loader.
        const auto control=reinterpret_cast<unsigned char(__thiscall*)(void*,int)>(0x4ce630)(reinterpret_cast<void*>(world),current);
        if(!Write(0x698a10,control)){Fault("solo_current_control_write");return false;}
        {std::lock_guard<std::mutex> l(mutex);state.token=token;state.transport=net;state.local_dpid=dpid;state.ids=ids;state.active=true;}
        humans=mask;local_color=local;
        if(!relay)active_transport.store(true,std::memory_order_release);
        // Exercise the installed enumeration path once before input can open.
        // Stack layout mirrors native5566D0; records and pointer array are native
        // allocations and released with the same native free used by that caller.
        std::uint32_t list[5]={0x640f34,0x19,0,0,0};
        using Enumerate=unsigned char(__thiscall*)(void*,void*,void*,unsigned);
        const bool enumerated=reinterpret_cast<Enumerate>(0x496ec0)(reinterpret_cast<void*>(net),list,nullptr,0)!=0;
        unsigned observed=0;bool valid=enumerated&&list[4]==human_count&&list[4]<=8&&list[2];
        if(list[4]<=8&&list[2])for(unsigned i=0;i<list[4];++i){
            std::uint32_t record=0,record_id=0;
            if(!hp::Read(list[2]+i*4,record)||!record||!hp::Read(record+0x100,record_id)){valid=false;continue;}
            bool found=false;for(unsigned c=0;c<8;++c)if(ids[c]&&ids[c]==record_id){if(observed&(1u<<c))valid=false;observed|=1u<<c;found=true;}
            if(!found)valid=false;
            reinterpret_cast<void(__cdecl*)(void*)>(0x60b0f0)(reinterpret_cast<void*>(record));
        }
        if(list[2])reinterpret_cast<void(__cdecl*)(void*)>(0x60b0f0)(reinterpret_cast<void*>(list[2]));
        if(!valid||observed!=mask){Fault("native_logical_roster_enumeration_not_verified");Error(e,"native_logical_roster_enumeration_not_verified");return false;}
        return true;
    }catch(...){Fault("solo_transport_activation_exception");Error(e,"solo_transport_activation_exception");return false;}
}
bool IsGameplayBlocked() noexcept {if(blocked.load())return true;auto j=hota_solo_journal::GetSnapshot();return j.failed!=0||!j.fatal_error.empty();}
Snapshot GetSnapshot(){std::lock_guard<std::mutex> l(mutex);return state;}
}
