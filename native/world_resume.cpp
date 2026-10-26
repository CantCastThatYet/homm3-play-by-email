#include "world_resume.hpp"
#include "supported_build.hpp"
#include "memfile_import.hpp"
#include "common.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>
#if !defined(_M_IX86)
#error World resume requires the verified x86 ABI
#endif
namespace hota_resume {
namespace {
constexpr std::size_t kLimit=64u*1024u*1024u;
constexpr char kName[]="RMTGEXBS.BIN";
std::mutex mutex;
std::atomic<bool> installed{false},blocked{false},executing{false},queued{false};
std::uint32_t hd=0,game_thread=0;
Callbacks callbacks;
Request pending;
Snapshot snapshot;
hota_memfile::Importer importer;
unsigned load_calls=0,load_returns=0,load_depth=0;
bool last_load_match=false;
unsigned last_load_result=0;
std::uint32_t loading_world=0;
void ErrorText(std::string* out,const char* text) noexcept {if(out)try{*out=text;}catch(...) {}}
bool ReadBytes(std::uintptr_t address,void* out,std::size_t count) noexcept {
    SIZE_T got=0;return address&&count<=UINT32_MAX-address&&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),out,count,&got)&&got==count;
}
bool Bytes(std::uintptr_t address,const unsigned char* data,std::size_t count) noexcept {
    unsigned char actual[96]{};return count<=sizeof actual&&ReadBytes(address,actual,count)&&!std::memcmp(data,actual,count);
}
bool Geometry(const Audit& a) noexcept {
    return a.map_size>0&&a.map_size<=252&&a.width==a.map_size&&a.height==a.map_size&&a.underground<=1;
}
bool ValidDate(const std::array<std::uint16_t,3>& date) noexcept {
    return date[0]>=1&&date[0]<=7&&date[1]>=1&&date[1]<=4&&date[2]>=1;
}
bool FramesClear() noexcept {
    Frames f;
    return GetCurrentThreadId()==game_thread&&callbacks.read_frames&&callbacks.read_frames(callbacks.context,f)&&
        f.at_outer_idle_boundary&&f.input_gate_installed&&f.receive_gate_installed&&
        !f.save_depth&&!f.turn_depth&&!f.merge_depth&&!f.receive_depth&&!f.battle_depth&&
        !load_depth&&load_calls==load_returns;
}
bool Admission(void*) noexcept {return executing.load()&&blocked.load()&&FramesClear();}
const char* NativeQuietFailure(const Audit& a,bool allow_scaffold_render_flags=false) noexcept {
    std::uint32_t type=0,state=0,setup=0,path=0,dialog=0,wm=0,top=0;
    std::uint8_t transparent=0,centered=0;
    std::uint32_t modal=0,scenario=0,combat=0,combat_state=0,worker=0,event=0,closing=0;
    if(!hp::Read(a.adventure+0xc,type)||type!=0x400||!hp::Read(a.adventure+0x34,state)||state!=1)return "native_quiet_adventure_type_or_status";
    if(!hp::Read(a.adventure+0x5c,setup)||setup!=a.world+0x1fb70)return "native_quiet_setup_pointer";
    if(!hp::Read(a.adventure+0x4c,path)||path)return "native_quiet_path_state";
    if(!hp::Read(a.adventure+0x1ec,transparent)||transparent>1||(!allow_scaffold_render_flags&&transparent))return "native_quiet_transparent_hero";
    if(!hp::Read(a.adventure+0x20c,centered)||centered>1||(!allow_scaffold_render_flags&&centered))return "native_quiet_centered_hero";
    if(!hp::Read(a.adventure+0x44,dialog)||!dialog||!hp::Read(0x6992d0,wm)||!wm||
       !hp::Read(wm+0x54,top)||top!=dialog)return "native_quiet_adventure_dialog_not_top";
    if(!hp::Read(0x6aad88,modal)||modal||!hp::Read(0x69fc44,scenario)||scenario)return "native_quiet_modal_or_scenario";
    if(!hp::Read(0x699420,combat)||(combat&&(!hp::Read(combat+0x34,combat_state)||combat_state==1)))return "native_quiet_combat";
    if(!hp::Read(0x69fe28,worker)||worker||!hp::Read(0x69fe2c,event)||event)return "native_quiet_loading_worker";
    if(!hp::Read(0x697308,closing)||closing)return "native_quiet_closing";
    return nullptr;
}
bool NativeQuiet(const Audit& a) noexcept {return NativeQuietFailure(a)==nullptr;}
bool InvokeScaffoldRenderCleanup(std::uint32_t adventure) noexcept {
    using Cleanup=void(__thiscall*)(void*,int,int);
    __try {reinterpret_cast<Cleanup>(0x4175e0)(reinterpret_cast<void*>(adventure),0,0);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
const char* PrepareScaffoldRenderer(const Audit& a,const Request& request,bool& invoked) noexcept {
    const auto* original=NativeQuietFailure(a);if(!original)return nullptr;
    if(std::strcmp(original,"native_quiet_transparent_hero")&&std::strcmp(original,"native_quiet_centered_hero"))return original;
    std::uint32_t network=~0u,player=0,cleanup_hero_offset=0;int selected=-1;std::uint8_t centered=0;
    // This preparation applies only to a disposable SP scaffold about to be
    // discarded by an authenticated multiplayer checkpoint. Never post-load.
    if(!request.has_source_audit||request.source_roster.size()!=187||!FramesClear()||!blocked.load()||
       !hp::Read(0x69959c,network)||network!=0)return "scaffold_render_cleanup_requires_held_singleplayer_scaffold";
    if(const auto* why=NativeQuietFailure(a,true))return why;
    if(!hp::Read(0x69ccfc,player)||player!=a.world+0x20ad0+a.current*0x168||
       !hp::Read(player+4,selected)||selected<0||static_cast<std::size_t>(selected)>=a.heroes.size()||
       a.heroes[selected].owner!=a.current||!hp::Read(a.adventure+0x20c,centered)||centered!=1)
        return "scaffold_render_cleanup_selected_hero_invalid";
    // Verified against pinned PE and read-only live REA ev_9c49006f... .
    constexpr unsigned char entry[]={0x55,0x8b,0xec,0x83,0xec,8,0x8a,0x45,8,0x56,0x84,0xc0,0x57,0x8b,0xf1};
    constexpr unsigned char lea[]={0x8d,0xbc,0x4a};
    constexpr unsigned char clear_center[]={0xc6,0x86,0x0c,2,0,0,0};
    constexpr unsigned char clear_transparent[]={0x8a,0x96,0xf4,1,0,0,0x88,0x57,0x47,0xc6,0x86,0xec,1,0,0,0};
    constexpr unsigned char skip_pump[]={0x8a,0x45,0x0c,0x84,0xc0,0x0f,0x84,0xb6,0,0,0};
    constexpr unsigned char tail[]={0x5f,0x5e,0x8b,0xe5,0x5d,0xc2,8,0};
    if(!Bytes(0x4175e0,entry,sizeof entry)||!Bytes(0x41761a,clear_center,sizeof clear_center)||
       !Bytes(0x41763e,lea,sizeof lea)||!hp::Read(0x417641,cleanup_hero_offset)||cleanup_hero_offset!=a.hero_offset||
       !Bytes(0x4176a9,clear_transparent,sizeof clear_transparent)||!Bytes(0x4176c6,skip_pump,sizeof skip_pump)||
       !Bytes(0x417787,tail,sizeof tail))return "scaffold_render_cleanup_native_abi_mismatch";
    invoked=true;
    if(!InvokeScaffoldRenderCleanup(a.adventure))return "scaffold_render_cleanup_native_fault";
    if(!FramesClear())return "scaffold_render_cleanup_frames_changed";
    Audit after;
    if(!ReadAudit(after)||after.world!=a.world||after.adventure!=a.adventure||after.map!=a.map||!SameWorldContent(a,after))
        return "scaffold_render_cleanup_world_audit_changed";
    return NativeQuietFailure(after);
}
bool ValidRoster(const std::vector<std::uint8_t>& roster,const Audit& a) noexcept {
    if(roster.size()!=187||roster[0]<2||roster[0]>8)return false;
    unsigned mask=0;
    for(unsigned i=0;i<roster[0];++i) {
        const auto* record=roster.data()+3+i*23;std::uint32_t id=0;std::memcpy(&id,record,4);
        const unsigned color=record[4];
        if(color>7||!id||(mask&(1u<<color))||record[22]!=0||
           !a.players[color].human||a.players[color].transport_id!=id)return false;
        mask|=1u<<color;
        for(unsigned j=0;j<i;++j){std::uint32_t old=0;std::memcpy(&old,roster.data()+3+j*23,4);if(old==id)return false;}
    }
    unsigned expected=0;for(unsigned color=0;color<8;++color)if(a.players[color].human)expected|=1u<<color;
    return mask==expected;
}
const char* StageRosterFailure(const std::vector<std::uint8_t>& roster) noexcept {
    std::uint32_t destination=0,confirmed=0;std::uint8_t prior[187]{},after[187]{};SIZE_T count=0;
    if(roster.size()!=sizeof prior)return "source_protection_roster_invalid_size";
    if(!hp::Read(hd+hp_build::Rva(0x11b8c84),destination))return "source_protection_roster_pointer_unreadable";
    if(!destination)return "source_protection_roster_pointer_null";
    const std::uint64_t end=static_cast<std::uint64_t>(destination)+roster.size();
    if(end>0x100000000ull)return "source_protection_roster_range_overflow";
    // Check every covered region. Heap/HD allocations can legitimately be RWX
    // or copy-on-write; never change their existing protection to force a write.
    std::uint64_t at=destination;
    while(at<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(static_cast<std::uintptr_t>(at)),&info,sizeof info))return "source_protection_roster_query_failed";
        if(info.State!=MEM_COMMIT)return "source_protection_roster_region_not_committed";
        if(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return "source_protection_roster_region_guarded_or_inaccessible";
        const auto protection=info.Protect&0xff;
        if(protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&
           protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)
            return "source_protection_roster_region_not_writable";
        const std::uint64_t start=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const std::uint64_t next=start+info.RegionSize;
        if(start>at||next<=at||next>0x100000000ull)return "source_protection_roster_invalid_region_range";
        at=next;
    }
    if(!ReadBytes(destination,prior,sizeof prior))return "source_protection_roster_existing_bytes_unreadable";
    if(!hp::Read(hd+hp_build::Rva(0x11b8c84),confirmed)||confirmed!=destination)return "source_protection_roster_pointer_changed_before_write";
    if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(destination),roster.data(),roster.size(),&count))return "source_protection_roster_write_failed";
    if(count!=roster.size())return "source_protection_roster_short_write";
    if(!ReadBytes(destination,after,sizeof after))return "source_protection_roster_readback_failed";
    if(std::memcmp(after,roster.data(),sizeof after))return "source_protection_roster_readback_mismatch";
    if(!hp::Read(hd+hp_build::Rva(0x11b8c84),confirmed)||confirmed!=destination)return "source_protection_roster_pointer_changed_after_write";
    return nullptr;
}
bool WriteLocalOwner(int color) noexcept {
    if(color<0||color>7)return false;SIZE_T written=0;int actual=-1;
    return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(0x6995a4),&color,sizeof color,&written)&&
        written==sizeof color&&hp::Read(0x6995a4,actual)&&actual==color;
}
const char* ReconstructLoadedMembership(const Request& request,Audit& loaded,bool& attempted,bool& reconstructed) {
    if(!ValidSourceMembership(request.source_audit,request.expected_local))return "membership_reconstruction_invalid_source_local_controller";
    if(SameWorldContent(request.source_audit,loaded))return nullptr;
    if(!SameGameplayBeforeMembership(request.source_audit,loaded))return "post_load_source_gameplay_mismatch";
    std::uint32_t network=~0u;
    if(!request.has_source_audit||!ValidRoster(request.source_roster,request.source_audit)||
       !blocked.load()||!executing.load()||!FramesClear()||!hp::Read(0x69959c,network)||network!=0)
        return "membership_reconstruction_requires_held_singleplayer_and_bound_roster";
    // Keep the native reader's result separate. Only membership is reconstructed;
    // gameplay mismatches are never repaired or hidden by this operation.
    Audit current;
    if(!ReadAudit(current)||current.world!=loaded.world||current.adventure!=loaded.adventure||
       !SameWorldContent(current,loaded)||!NativeQuiet(current))return "membership_reconstruction_world_changed_before_write";
    attempted=true;
    for(unsigned color=0;color<8;++color) {
        const auto& source=request.source_audit.players[color];const auto& before=loaded.players[color];
        const auto player=loaded.world+0x20ad0+color*0x168;SIZE_T written=0;
        if(source.transport_id!=before.transport_id) {
            if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(player+0xc8),&source.transport_id,sizeof source.transport_id,&written)||
               written!=sizeof source.transport_id)return "membership_reconstruction_transport_id_write_failed";
        }
        if(source.human!=before.human) {
            if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(player+0xe2),&source.human,sizeof source.human,&written)||
               written!=sizeof source.human)return "membership_reconstruction_human_flag_write_failed";
        }
        if(source.local!=before.local) {
            if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(player+0xe1),&source.local,sizeof source.local,&written)||
               written!=sizeof source.local)return "membership_reconstruction_local_controller_write_failed";
        }
        std::uint32_t id=0;std::uint8_t human=0,local=0;
        if(!hp::Read(player+0xc8,id)||id!=source.transport_id||!hp::Read(player+0xe2,human)||human!=source.human||
           !hp::Read(player+0xe1,local)||local!=source.local)
            return "membership_reconstruction_seat_readback_failed";
    }
    Audit final;
    if(!FramesClear()||!hp::Read(0x69959c,network)||network!=0||!ReadAudit(final)||
       final.world!=loaded.world||final.adventure!=loaded.adventure||!NativeQuiet(final))return "membership_reconstruction_postwrite_state_invalid";
    if(!SameWorldContent(request.source_audit,final))return "membership_reconstruction_full_source_audit_mismatch";
    loaded=std::move(final);reconstructed=true;return nullptr;
}
bool WrapperSignature(std::uint32_t base) noexcept {
    // Entire93-byte wrapper, including relocated cookie and fixed engine calls.
    unsigned char expected[]={0x55,0x8b,0xec,0x83,0xec,0x14,0xa1,0,0,0,0,0x33,0xc5,0x89,0x45,0xfc,
        0x56,0x8b,0xf1,0xb8,0x20,0x83,0x55,0,0x6a,1,0x8d,0x4d,0xec,0xff,0xd0,0x6a,1,0x6a,0,
        0xc7,0x45,0xf8,0,0,0,0,0xb8,0xf0,0xef,0x4b,0,0x8b,0x0d,0x38,0x95,0x69,0,0x56,0xff,0xd0,
        0xbe,0x60,0x83,0x55,0,0x8d,0x4d,0xec,0xff,0xd6,0x8d,0x4d,0xec,0xc7,0x45,0xf8,0xff,0xff,0xff,0xff,
        0xff,0xd6,0x8b,0x4d,0xfc,0x33,0xcd,0x5e,0xe8,0xbd,0x60,0x12,0,0x8b,0xe5,0x5d,0xc3};
    const std::uint32_t cookie=base+hp_build::Rva(0x20e068);std::memcpy(expected+7,&cookie,4);
    // Bytes 85..88 are the relative call to the cookie check, which moves between HD builds.
    const std::uint32_t at=base+hp_build::Rva(0x9ef00);
    return expected[84]==0xe8&&Bytes(at,expected,85)&&Bytes(at+89,expected+89,sizeof expected-89);
}
// Small isolated SEH boundary: a fault is reported, never interpreted as rollback.
bool InvokeInner(std::uint32_t address) noexcept {
    using Inner=void(__thiscall*)(const char*);
    __try {reinterpret_cast<Inner>(address)(kName);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void Publish(Snapshot&& value) {std::lock_guard<std::mutex> lock(mutex);snapshot=std::move(value);}
}

bool ResolveHeroArrayAddress(std::uint32_t world,std::uint32_t displacement,unsigned count,std::uint32_t& begin) noexcept {
    // LEA r32,[base+disp32] discards carry. HotA's relocated array can lie
    // BELOW world; its negative displacement is represented by large uint32.
    const auto resolved=static_cast<std::uint32_t>(world+displacement);
    if(!world||!resolved||!count||count>1024||
       static_cast<std::uint64_t>(resolved)+static_cast<std::uint64_t>(count)*0x492>0x100000000ull)return false;
    begin=resolved;return true;
}
bool ReadAudit(Audit& output,std::string* error) noexcept {
    try {
        if(!installed.load()||GetCurrentThreadId()!=game_thread) {ErrorText(error,"audit_requires_initialized_game_thread");return false;}
        Audit a;int hero_count=0;
        if(!hp::Read(0x699538,a.world)||!a.world||a.world>UINT32_MAX-0x21618) {
            ErrorText(error,"world_audit_main_pointer_unreadable_or_overflow");return false;}
        if(!hp::Read(0x6992b8,a.adventure)||!a.adventure||a.adventure>UINT32_MAX-0x1000) {
            ErrorText(error,"world_audit_adventure_pointer_unreadable_or_overflow");return false;}
        if(!hp::Read(a.world+0x1fc40,a.map)||!a.map||!hp::Read(a.world+0x1fc44,a.map_size)||
           !hp::Read(a.world+0x1fc48,a.underground)||!hp::Read(0x6783c8,a.width)||!hp::Read(0x6783cc,a.height)) {
            ErrorText(error,"world_audit_map_fields_unreadable");return false;}
        if(!Geometry(a)) {ErrorText(error,"world_audit_geometry_invalid");return false;}
        if(!ReadBytes(a.world+0x1f63e,a.date.data(),6)||!ValidDate(a.date)) {
            ErrorText(error,"world_audit_date_unreadable_or_invalid");return false;}
        if(!hp::Read(0x6995a4,a.local)||a.local<0||a.local>7||!hp::Read(0x69ccf4,a.current)||a.current<0||a.current>7) {
            ErrorText(error,"world_audit_local_or_current_color_invalid");return false;}
        // HotA can relocate this array far beyond the original embedded offset.
        // Live evidence ev_e920ea... confirms LEA ESI,[EAX+EDX*2+disp32].
        constexpr unsigned char hero_lea[]={0x8d,0xb4,0x50};
        if(!Bytes(0x41c6b2,hero_lea,sizeof hero_lea)||!hp::Read(0x41c6b5,a.hero_offset)) {
            ErrorText(error,"world_audit_hero_array_lea_signature_invalid");return false;}
        if(!hp::Read(0x4bd145,hero_count)||hero_count<1||hero_count>1024) {
            ErrorText(error,"world_audit_hero_count_unreadable_or_invalid");return false;}
        std::uint32_t hero_base=0;
        if(!ResolveHeroArrayAddress(a.world,a.hero_offset,static_cast<unsigned>(hero_count),hero_base)) {
            ErrorText(error,"world_audit_hero_array_address_overflow");return false;}
        for(unsigned i=0;i<8;++i) {
            auto& p=a.players[i];const auto at=a.world+0x20ad0+i*0x168;
            if(!hp::Read(at+0xc8,p.transport_id)||!hp::Read(at+0xe1,p.local)||!hp::Read(at+0xe2,p.human)||
               p.local>1||p.human>1||!ReadBytes(at+0x9c,p.resources.data(),28)) {ErrorText(error,"player_audit_unreadable");return false;}
        }
        a.heroes.resize(hero_count);
        for(int i=0;i<hero_count;++i) {
            auto& h=a.heroes[i];const auto at=hero_base+static_cast<unsigned>(i)*0x492;
            if(!hp::Read(at,h.x)||!hp::Read(at+2,h.y)||!hp::Read(at+4,h.layer)||!hp::Read(at+0x1a,h.id)||
               !hp::Read(at+0x22,h.owner)||!hp::Read(at+0x49,h.max_movement)||!hp::Read(at+0x4d,h.movement)) {
                ErrorText(error,"hero_audit_unreadable");return false;
            }
        }
        output=std::move(a);return true;
    }catch(...) {ErrorText(error,"audit_allocation_failed");return false;}
}
bool SameGameplayBeforeMembership(const Audit& a,const Audit& b) noexcept {
    if(a.width!=b.width||a.height!=b.height||a.map_size!=b.map_size||a.underground!=b.underground||
       a.date!=b.date||a.local!=b.local||a.current!=b.current||a.heroes.size()!=b.heroes.size())return false;
    for(unsigned i=0;i<8;++i) {
        const auto& x=a.players[i];const auto& y=b.players[i];
        if(x.resources!=y.resources)return false;
    }
    for(std::size_t i=0;i<a.heroes.size();++i) {
        const auto& x=a.heroes[i];const auto& y=b.heroes[i];
        if(x.id!=y.id||x.x!=y.x||x.y!=y.y||x.layer!=y.layer||x.owner!=y.owner||x.max_movement!=y.max_movement||x.movement!=y.movement)return false;
    }
    return true;
}
bool SameWorldContent(const Audit& a,const Audit& b) noexcept {
    if(!SameGameplayBeforeMembership(a,b))return false;
    for(unsigned i=0;i<8;++i)if(a.players[i].transport_id!=b.players[i].transport_id||a.players[i].human!=b.players[i].human||a.players[i].local!=b.players[i].local)return false;
    return true;
}
bool ValidSourceMembership(const Audit& a,int expected_local) noexcept {
    if(expected_local<0||expected_local>7||a.local!=expected_local)return false;
    unsigned local_mask=0;
    for(unsigned color=0;color<8;++color) {
        const auto& seat=a.players[color];
        if(seat.local>1||seat.human>1||(seat.local&&!seat.human)||(seat.human&&!seat.transport_id))return false;
        if(seat.local)local_mask|=1u<<color;
    }
    return local_mask==(1u<<expected_local);
}
bool Initialize(std::uint32_t base,std::uint32_t thread,Callbacks cb,std::string* error) noexcept {
    try {
        if(installed.load()||!base||base>UINT32_MAX-0x220000||!thread||GetCurrentThreadId()!=thread||!cb.read_frames) {
            ErrorText(error,"invalid_world_resume_initialization");return false;
        }
        if(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))!=0x400000||
           hp::Sha256(hp::ModulePath(GetModuleHandleW(nullptr)))!="5aaab925f06cccf23bb09814767590a95b84a557eb33d244800520be4f1f18de"||
           !WrapperSignature(base)) {ErrorText(error,"world_reload_native_fingerprint_mismatch");return false;}
        const auto result=importer.InitializeNative(base,thread,Admission);
        if(!result) {ErrorText(error,hota_memfile::ErrorName(result.error));return false;}
        std::lock_guard<std::mutex> lock(mutex);hd=base;game_thread=thread;callbacks=cb;
        snapshot.status=Status::Idle;installed.store(true);return true;
    }catch(...) {ErrorText(error,"world_resume_initialization_failed");return false;}
}
bool Queue(const Request& value,std::string* error) noexcept {
    try {
        if(!installed.load()) {ErrorText(error,"world_resume_not_initialized");return false;}
        if(!value.token||value.expected_local<0||value.expected_local>7||!ValidDate(value.expected_date)||
           value.map_size<1||value.map_size>252||value.underground>1||value.local_snapshot.empty()||value.local_snapshot.size()>kLimit||
           (value.has_source_audit&&(!Geometry(value.source_audit)||value.source_audit.heroes.empty()||value.source_audit.heroes.size()>1024||
            value.source_audit.date!=value.expected_date||value.source_audit.local!=value.expected_local||
            value.source_audit.map_size!=value.map_size||value.source_audit.underground!=value.underground))||
           (!value.source_roster.empty()&&(!value.has_source_audit||!ValidRoster(value.source_roster,value.source_audit)||
            !ValidSourceMembership(value.source_audit,value.expected_local)))) {
            ErrorText(error,"invalid_or_unbound_world_resume_request");return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if(queued.load()||executing.load()||blocked.load()) {ErrorText(error,"world_resume_busy_or_already_held");return false;}
        pending=value;snapshot=Snapshot{};snapshot.status=Status::Queued;snapshot.token=value.token;queued.store(true);return true;
    }catch(...) {ErrorText(error,"world_resume_queue_allocation_failed");return false;}
}
bool IsGameplayBlocked() noexcept {return blocked.load();}
Snapshot GetSnapshot() {std::lock_guard<std::mutex> lock(mutex);auto copy=snapshot;copy.input_blocked=blocked.load();return copy;}
void ObserveLoadEnter(std::uint32_t world,const char* name,unsigned a,unsigned b) noexcept {
    if(!installed.load()||GetCurrentThreadId()!=game_thread)return;
    ++load_calls;++load_depth;char observed[sizeof kName]{};
    last_load_match=executing.load()&&load_depth==1&&world==loading_world&&a==0&&b==1&&
        ReadBytes(reinterpret_cast<std::uintptr_t>(name),observed,sizeof observed)&&!std::memcmp(observed,kName,sizeof kName);
}
void ObserveLoadReturn(unsigned eax) noexcept {
    if(!installed.load()||GetCurrentThreadId()!=game_thread)return;
    ++load_returns;if(load_depth)--load_depth;else last_load_match=false;
    last_load_result=eax&255u;
}
bool TryAtAdventureBoundary() noexcept {
    if(blocked.load())return true;
    if(!installed.load()||!queued.load()||GetCurrentThreadId()!=game_thread)return false;
    Snapshot result;bool owner_staged=false;int prior_owner=-1;
    try {
        Request request;
        {std::lock_guard<std::mutex> lock(mutex);if(!queued.load())return false;
         request=std::move(pending);queued.store(false);result.token=request.token;}
        // Latch before any possible nested dispatch. Even refusal remains held.
        blocked.store(true);executing.store(true);result.input_blocked=true;result.status=Status::Refused;
        auto fail=[&](const char* why) {result.error=why;
            if(owner_staged)result.local_owner_rollback_succeeded=WriteLocalOwner(prior_owner);
            executing.store(false);Publish(std::move(result));return true;};
        if(!FramesClear())return fail("native_frames_or_nested_dispatch_gate_not_quiescent");
        if(!ReadAudit(result.before,&result.error)) {executing.store(false);Publish(std::move(result));return true;}
        if(result.before.map_size!=request.map_size||result.before.underground!=request.underground)return fail("scaffold_geometry_mismatch");
        if(const auto* why=PrepareScaffoldRenderer(result.before,request,result.scaffold_render_cleanup_invoked))return fail(why);
        if(!WrapperSignature(hd))return fail("inner_loader_live_bytes_changed");
        if(!request.source_roster.empty())if(const auto* why=StageRosterFailure(request.source_roster))return fail(why);
        const auto prepared=importer.EnsureRoles({hota_memfile::Role::LocalSnapshot});
        if(!prepared)return fail(hota_memfile::ErrorName(prepared.error));
        std::vector<hota_memfile::Replacement> replacement;
        replacement.push_back({hota_memfile::Role::LocalSnapshot,std::move(request.local_snapshot)});
        const auto imported=importer.Replace(replacement,{256,kLimit,kLimit});
        if(!imported)return fail(hota_memfile::ErrorName(imported.error));
        result.stream_replaced=true;
        if(!FramesClear())return fail("native_frames_changed_after_import");
        if(const auto* why=NativeQuietFailure(result.before))return fail(why);
        // This is session-local identity, not serialized world ownership. Base
        //004BCA60 restores CURRENT from the saved header but not LOCAL6995A4.
        // Stage only the authenticated source local; never alter playerE1/E2 or
        // current, and verify the actual reader-restored current in source audit.
        prior_owner=result.before.local;owner_staged=true;result.local_owner_staged=true;
        if(!WriteLocalOwner(request.expected_local))return fail("source_local_owner_stage_failed");
        result.status=Status::Loading;result.load_calls_before=load_calls;result.load_returns_before=load_returns;
        {std::lock_guard<std::mutex> lock(mutex);snapshot=result;}
        loading_world=result.before.world;last_load_match=false;last_load_result=0;result.load_invoked=true;
        const bool returned=InvokeInner(hd+hp_build::Rva(0x9ef00));
        result.load_calls_after=load_calls;result.load_returns_after=load_returns;
        result.load_balanced=load_calls==result.load_calls_before+1&&load_returns==result.load_returns_before+1&&!load_depth;
        result.load_success=returned&&result.load_balanced&&last_load_match&&last_load_result!=0;
        result.status=Status::Failed;
        if(!result.load_success)return fail("inner_load_fault_failure_or_unmatched_engine_observation");
        if(!FramesClear())return fail("native_frames_not_balanced_after_load");
        if(!ReadAudit(result.after,&result.error)) {
            if(owner_staged)result.local_owner_rollback_succeeded=WriteLocalOwner(prior_owner);
            executing.store(false);Publish(std::move(result));return true;}
        result.audit_read=true;
        result.loaded_before_membership=result.after;
        if(result.after.world!=result.before.world||result.after.adventure!=result.before.adventure||
           result.after.width!=result.before.width||result.after.height!=result.before.height||
           result.after.map_size!=request.map_size||result.after.underground!=request.underground||
           result.after.date!=request.expected_date||result.after.local!=request.expected_local)
            return fail("post_load_world_date_owner_geometry_mismatch");
        if(const auto* why=NativeQuietFailure(result.after))return fail(why);
        if(request.has_source_audit) {
            if(const auto* why=ReconstructLoadedMembership(request,result.after,result.membership_reconstruction_attempted,result.membership_reconstructed))return fail(why);
            result.source_audit_matched=SameWorldContent(request.source_audit,result.after);
            if(!result.source_audit_matched)return fail("post_load_source_resources_movement_or_roster_mismatch");
            result.status=Status::SourceAuditMatched;
        }else result.status=Status::AwaitingAudit;
        executing.store(false);Publish(std::move(result));return true;
    }catch(...) {
        if(owner_staged)result.local_owner_rollback_succeeded=WriteLocalOwner(prior_owner);
        blocked.store(true);executing.store(false);
        try {result.status=Status::Failed;result.error="world_resume_exception_client_remains_held";Publish(std::move(result));}catch(...) {}
        return true;
    }
}
bool ReleaseAfterRebind(std::uint32_t token,const Audit& expected,RecoveryAuthorization authorize,void* context,std::string* error) noexcept {
    try {
        if(!installed.load()||!blocked.load()||executing.load()||!token||!authorize||!FramesClear()) {
            ErrorText(error,"recovery_release_requires_held_quiescent_game_boundary");return false;
        }
        Snapshot prior=GetSnapshot();
        if(prior.token!=token||prior.status!=Status::SourceAuditMatched||!prior.source_audit_matched) {
            ErrorText(error,"recovery_release_requires_matched_source_audit");return false;
        }
        Audit original=prior.after;
        // Only transport IDs may differ from the checkpoint's gameplay audit.
        for(unsigned i=0;i<8;++i)original.players[i].transport_id=expected.players[i].transport_id;
        if(!SameWorldContent(original,expected)) {ErrorText(error,"rebound_audit_changed_source_gameplay");return false;}
        Audit live;
        if(!ReadAudit(live,error)||live.world!=prior.after.world||live.adventure!=prior.after.adventure||
           !SameWorldContent(live,expected)||!NativeQuiet(live)||!authorize(context,token,live)||!FramesClear()) {
            ErrorText(error,"rebound_world_transport_st_or_ui_audit_not_authorized");return false;
        }
        // Authorization must be read-only; re-read after it before opening input.
        Audit final;if(!ReadAudit(final,error)||final.world!=live.world||final.adventure!=live.adventure||
            !SameWorldContent(live,final)||!NativeQuiet(final)||!FramesClear())return false;
        prior.after=std::move(final);prior.status=Status::Released;prior.input_blocked=false;Publish(std::move(prior));
        blocked.store(false);return true;
    }catch(...) {ErrorText(error,"recovery_release_failed_client_stays_held");return false;}
}
const char* StatusName(Status value) noexcept {
    switch(value) {case Status::Uninitialized:return "uninitialized";case Status::Idle:return "idle";case Status::Queued:return "queued";
    case Status::Loading:return "loading";case Status::Refused:return "refused_held";case Status::Failed:return "failed_held";
    case Status::AwaitingAudit:return "awaiting_audit_held";case Status::SourceAuditMatched:return "source_audit_matched_held";
    case Status::Released:return "released_after_rebind_audit";default:return "unknown";}
}

std::vector<std::uint8_t> EncodeAudit(const Audit& a) {
    // Every scalar encoded explicitly: no pointers, padding, native bool, or host
    // endianness in durable files. Arrays preserve native hero/color index order.
    if(!Geometry(a)||!ValidDate(a.date)||a.heroes.empty()||a.heroes.size()>1024||
       a.local<0||a.local>7||a.current<0||a.current>7)return {};
    std::vector<std::uint8_t> bytes;bytes.reserve(512+a.heroes.size()*20);
    auto u8=[&](std::uint8_t v){bytes.push_back(v);};
    auto u16=[&](std::uint16_t v){u8(static_cast<std::uint8_t>(v));u8(static_cast<std::uint8_t>(v>>8));};
    auto u32=[&](std::uint32_t v){u16(static_cast<std::uint16_t>(v));u16(static_cast<std::uint16_t>(v>>16));};
    u32(0x41523348);u32(1); // H3RA, format1
    u32(a.width);u32(a.height);u32(a.map_size);u8(a.underground);u8(static_cast<std::uint8_t>(a.local));u8(static_cast<std::uint8_t>(a.current));u8(0);
    for(auto d:a.date)u16(d);
    u16(0);u32(static_cast<std::uint32_t>(a.heroes.size()));
    for(const auto& p:a.players) {
        if(p.local>1||p.human>1)return {};
        u32(p.transport_id);u8(p.local);u8(p.human);u16(0);for(auto resource:p.resources)u32(resource);
    }
    for(const auto& h:a.heroes) {
        u32(h.id);u32(h.max_movement);u32(h.movement);u16(h.x);u16(h.y);u8(h.layer);u8(h.owner);u16(0);
    }
    return bytes;
}
bool DecodeAudit(const std::vector<std::uint8_t>& bytes,Audit& output,std::string* error) noexcept {
    try {
        constexpr std::size_t header=36,players=8*36;
        if(bytes.size()<header+players+20||bytes.size()>header+players+1024*20) {ErrorText(error,"invalid_audit_size");return false;}
        std::size_t at=0;
        auto u8=[&](){return bytes.at(at++);};
        auto u16=[&](){const unsigned lo=u8(),hi=u8();return static_cast<std::uint16_t>(lo|(hi<<8));};
        auto u32=[&](){const unsigned lo=u16(),hi=u16();return static_cast<std::uint32_t>(lo|(hi<<16));};
        if(u32()!=0x41523348||u32()!=1) {ErrorText(error,"unsupported_world_audit_format");return false;}
        Audit a;a.width=static_cast<std::int32_t>(u32());a.height=static_cast<std::int32_t>(u32());a.map_size=static_cast<std::int32_t>(u32());
        a.underground=u8();a.local=u8();a.current=u8();if(u8()!=0)return false;
        for(auto& d:a.date)d=u16();if(u16()!=0)return false;
        const auto count=u32();
        if(count<1||count>1024||bytes.size()!=header+players+count*20||!Geometry(a)||!ValidDate(a.date)||a.local>7||a.current>7) {
            ErrorText(error,"invalid_world_audit_geometry_date_or_count");return false;
        }
        for(auto& p:a.players) {
            p.transport_id=u32();p.local=u8();p.human=u8();if(u16()!=0||p.local>1||p.human>1)return false;
            for(auto& resource:p.resources)resource=static_cast<std::int32_t>(u32());
        }
        a.heroes.resize(count);
        for(auto& h:a.heroes) {
            h.id=static_cast<std::int32_t>(u32());h.max_movement=static_cast<std::int32_t>(u32());h.movement=static_cast<std::int32_t>(u32());
            h.x=static_cast<std::int16_t>(u16());h.y=static_cast<std::int16_t>(u16());h.layer=u8();h.owner=u8();if(u16()!=0)return false;
        }
        if(at!=bytes.size())return false;output=std::move(a);return true;
    }catch(...) {ErrorText(error,"world_audit_decode_failed");return false;}
}
std::string EncodeAuditHex(const Audit& a) {
    const auto bytes=EncodeAudit(a);std::string result;result.reserve(bytes.size()*2);
    constexpr char digits[]="0123456789abcdef";for(auto v:bytes){result.push_back(digits[v>>4]);result.push_back(digits[v&15]);}return result;
}
bool DecodeAuditHex(const std::string& hex,Audit& output) noexcept {
    if(hex.empty()||hex.size()%2||hex.size()>(36+8*36+1024*20)*2)return false;
    try {
        std::vector<std::uint8_t> bytes;bytes.reserve(hex.size()/2);
        auto nibble=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
        for(std::size_t i=0;i<hex.size();i+=2){const int a=nibble(hex[i]),b=nibble(hex[i+1]);if(a<0||b<0)return false;bytes.push_back(static_cast<std::uint8_t>((a<<4)|b));}
        return DecodeAudit(bytes,output);
    }catch(...) {return false;}
}
std::string SerializeAudit(const Audit& a) {
    std::ostringstream out;out<<"{\"schema\":1,\"map_size\":"<<a.map_size<<",\"width\":"<<a.width<<",\"height\":"<<a.height
        <<",\"underground\":"<<unsigned(a.underground)<<",\"local\":"<<a.local<<",\"current\":"<<a.current
        <<",\"day\":"<<a.date[0]<<",\"week\":"<<a.date[1]<<",\"month\":"<<a.date[2]<<",\"players\":[";
    for(unsigned i=0;i<8;++i) {if(i)out<<',';const auto& p=a.players[i];out<<"{\"color\":"<<i<<",\"transport_id\":"<<p.transport_id
        <<",\"local\":"<<unsigned(p.local)<<",\"human\":"<<unsigned(p.human)<<",\"resources\":[";
        for(unsigned j=0;j<7;++j){if(j)out<<',';out<<p.resources[j];}out<<"]}";}
    out<<"],\"heroes\":[";
    for(std::size_t i=0;i<a.heroes.size();++i){if(i)out<<',';const auto& h=a.heroes[i];out<<"{\"index\":"<<i<<",\"id\":"<<h.id<<",\"owner\":"<<unsigned(h.owner)
        <<",\"x\":"<<h.x<<",\"y\":"<<h.y<<",\"layer\":"<<unsigned(h.layer)<<",\"movement\":"<<h.movement<<",\"max_movement\":"<<h.max_movement<<'}';}
    out<<"],\"audit_hex\":\""<<EncodeAuditHex(a)<<"\"}";return out.str();
}
}
