#include "resume_session.hpp"
#include "session_bootstrap.hpp"
#include "world_resume.hpp"
#include "memfile_import.hpp"
#include "solo_transport.hpp"
#include "solo_journal.hpp"
#include "completion_observer.hpp"
#include "dplay_relay.hpp"
#include "common.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
static_assert(sizeof(hota_completion::Participant)==6,"Pinned ST participant layout");
namespace resume_session { namespace {
constexpr std::uint32_t MaxBytes=64u*1024u*1024u;
struct Bundle {std::uint32_t token=0,color=0,day=0,week=0,month=0,kind=0;std::vector<std::uint8_t> world,baseline,audit,roster,st,hero_assignment,baseline_day_one;hota_resume::Audit source;};
std::mutex mutex;Snapshot snapshot;Bundle queued;
std::atomic<bool> pending{false},held{false},verified{false};
DWORD game_thread=0;std::uint32_t hd=0;bool inside=false;
hota_memfile::Importer importer;
std::uint32_t owner_world=0;int owner_local=-1,expected_neighbor=-1;
std::array<std::uint16_t,3> owner_date{};
bool completion_pending=false;hota_resume::Audit completion_audit;
std::array<hota_completion::Participant,8> completion_rows{};
void Error(std::string* e,const char* text) noexcept {if(e)try{*e=text;}catch(...) {}}
void Stage(const char* s){std::lock_guard<std::mutex> l(mutex);snapshot.stage=s;}
void Fail(const std::string& s) noexcept {held.store(true);pending.store(false);try{std::lock_guard<std::mutex> l(mutex);snapshot.failed=true;snapshot.error=s;snapshot.stage="failed";}catch(...) {}}
bool Admission(void*) noexcept{return inside&&GetCurrentThreadId()==game_thread&&held.load();}
bool Write(std::uintptr_t at,const void* p,std::size_t n){SIZE_T got=0;return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),p,n,&got)&&got==n;}
bool Read(std::uintptr_t at,void* p,std::size_t n){SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),p,n,&got)&&got==n;}
std::uint32_t U32(const std::vector<std::uint8_t>& v,std::size_t p){std::uint32_t n=0;if(p>v.size()||v.size()-p<4)throw std::runtime_error("truncated_resume_bundle");std::memcpy(&n,v.data()+p,4);return n;}
std::uint32_t Field(const Bundle& b,std::uint32_t addr){return U32(b.st,addr-0x120fffa);}
bool Bytes(std::uintptr_t a,const unsigned char* expected,std::size_t size){unsigned char actual[32]{};return size<=sizeof actual&&Read(a,actual,size)&&!std::memcmp(actual,expected,size);}
Bundle Decode(const std::vector<std::uint8_t>& bytes,std::uint32_t token){
    if(bytes.size()<32||std::memcmp(bytes.data(),"HPR1",4)||U32(bytes,4)!=1)throw std::runtime_error("invalid_resume_bundle_header");
    Bundle b;b.token=token;b.color=U32(bytes,8);b.day=U32(bytes,12);b.week=U32(bytes,16);b.month=U32(bytes,20);b.kind=U32(bytes,24);
    if(!token||b.color>7||b.day<1||b.day>7||b.week<1||b.week>4||b.month<1||b.month>65535||b.kind>1)throw std::runtime_error("invalid_resume_identity");
    std::size_t at=28;for(auto* v:{&b.world,&b.baseline,&b.audit,&b.roster,&b.st,&b.hero_assignment,&b.baseline_day_one}){
        auto size=U32(bytes,at);at+=4;if(!size||size>MaxBytes||size>bytes.size()-at)throw std::runtime_error("invalid_resume_vector_length");
        v->assign(bytes.begin()+at,bytes.begin()+at+size);at+=size;
    }
    if(at!=bytes.size()||b.roster.size()!=187||b.st.size()!=156||b.hero_assignment.size()>4096)throw std::runtime_error("invalid_resume_state_sizes");
    std::string error;if(!hota_resume::DecodeAudit(b.audit,b.source,&error))throw std::runtime_error(error);
    if(b.source.local!=static_cast<int>(b.color)||b.source.current!=static_cast<int>(b.color)||b.source.date!=std::array<std::uint16_t,3>{static_cast<std::uint16_t>(b.day),static_cast<std::uint16_t>(b.week),static_cast<std::uint16_t>(b.month)})throw std::runtime_error("resume_identity_does_not_match_source_audit");
    if(Field(b,0x120fffe)!=0)throw std::runtime_error("resume_bundle_missing_deferred_packet_payloads");
    if(Field(b,0x1210042)!=1||Field(b,0x1210016)!=b.day+(b.week-1)*7+(b.month-1)*28||Field(b,0x121007e)!=b.hero_assignment.size())throw std::runtime_error("resume_st_date_or_hero_count_mismatch");
    // Running transition flags are not checkpoints. Eligibility8E is persistent,
    // not an active receive-depth indication, so it is preserved rather than tested.
    for(auto a:{0x121001au,0x121001eu,0x1210022u,0x1210026u,0x121002au,0x121002eu,
                0x1210032u,0x1210036u,0x121003au,0x121003eu,0x1210046u})if(Field(b,a))throw std::runtime_error("resume_checkpoint_inside_native_transition");
    unsigned mask=0,count=0,participant_mask=0;
    for(unsigned c=0;c<8;++c){const auto* row=b.st.data()+0x50+c*6;
        if(row[0]>1||row[1]>1||row[2]>2||row[3]>1||(row[4]!=255&&row[4]>7)||(row[5]!=255&&row[5]>7))throw std::runtime_error("invalid_st_participant_flags_or_links");
        if(b.source.players[c].human){mask|=1u<<c;++count;}
        if(row[3]){participant_mask|=1u<<c;if(!b.source.players[c].human)throw std::runtime_error("st_participant_not_human");}
    }
    if(!(participant_mask&(1u<<b.color)))throw std::runtime_error("local_color_not_in_active_st_group");
    const auto first=Field(b,0x121008a);if(first>7||!(participant_mask&(1u<<first)))throw std::runtime_error("invalid_st_first_participant");
    unsigned visited=0,previous=255,cursor=first;
    while(cursor!=255){
        if(cursor>7||!(participant_mask&(1u<<cursor))||(visited&(1u<<cursor)))throw std::runtime_error("invalid_st_participant_chain");
        const auto* row=b.st.data()+0x50+cursor*6;if(row[4]!=previous)throw std::runtime_error("inconsistent_st_predecessor");
        visited|=1u<<cursor;previous=cursor;cursor=row[5];
    }
    if(visited!=participant_mask)throw std::runtime_error("incomplete_st_participant_chain");
    if(!(mask&(1u<<b.color))||count<2||b.roster[0]!=count)throw std::runtime_error("resume_human_roster_mismatch");
    unsigned seen=0;for(unsigned i=0;i<count;++i){unsigned c=b.roster[3+i*23+4];if(c>7||!(mask&(1u<<c))||(seen&(1u<<c)))throw std::runtime_error("invalid_resume_roster_color");seen|=1u<<c;}
    if(seen!=mask)throw std::runtime_error("incomplete_resume_roster");
    return b;
}
bool RestoreST(const Bundle& b,std::string& error){
    std::uint32_t count=0;
    const auto heroes=static_cast<std::uint32_t>(b.hero_assignment.size());
    if(!hp::Read(hd+0x20fffe,count)||count){error="target_st_queue_not_empty";return false;}
    const unsigned char grow[]={0x55,0x8b,0xec,0x56,0x57,0x8b,0x7d,8,0x8b,0xf1,0x85,0xff};
    if(!Bytes(hd+0x1952c0,grow,sizeof grow)){error="native_st_vector_grow_abi_mismatch";return false;}
    std::uint32_t old_vector[3]{};
    if(!Read(hd+0x21007a,old_vector,sizeof old_vector)||old_vector[1]>old_vector[2]||old_vector[2]>4096||
       (old_vector[2]==0?old_vector[0]!=0:old_vector[0]==0)){error="target_st_vector_layout_invalid";return false;}
    if(old_vector[2]){std::vector<unsigned char> prior(old_vector[2]);if(!Read(old_vector[0],prior.data(),prior.size())){error="target_st_vector_unreadable";return false;}}
    reinterpret_cast<void(__thiscall*)(void*,unsigned)>(hd+0x1952c0)(reinterpret_cast<void*>(hd+0x21007a),heroes);
    std::uint32_t p=0,n=0,cap=0;if(!hp::Read(hd+0x21007a,p)||!p||!hp::Read(hd+0x21007e,n)||n!=heroes||!hp::Read(hd+0x210082,cap)||cap<n){error="native_st_vector_allocation_failed";return false;}
    //0120FFFA vector and0121007A vector retain native process-owned allocations.
    if(!Write(hd+0x210006,b.st.data()+12,0x74)||!Write(p,b.hero_assignment.data(),heroes)||
       !Write(hd+0x210086,b.st.data()+0x8c,16)){error="restore_st_write_failed";return false;}
    std::array<unsigned char,0x74> plain{};std::array<unsigned char,16> tail{};std::vector<unsigned char> assignment(heroes);
    if(!Read(hd+0x210006,plain.data(),plain.size())||std::memcmp(plain.data(),b.st.data()+12,plain.size())||
       !Read(hd+0x210086,tail.data(),tail.size())||std::memcmp(tail.data(),b.st.data()+0x8c,tail.size())||
       !Read(p,assignment.data(),heroes)||assignment!=b.hero_assignment){error="restored_st_verification_failed";return false;}
    return true;
}
// The scaffold's adventure manager started before any transport existed, so
// native Start skipped its network handler (00406F60: 004076BB..00407700).
// Without it ProcessMessage never polls game packets (00408710 calls
// [adventure+38]->vtable[1] = 00557C10). Recreate it exactly as Start does.
void AttachNetworkHandler(std::uint32_t adventure) {
    std::uint32_t handler=1,net=0;
    if(!adventure||!hp::Read(adventure+0x38,handler)||!hp::Read(0x69d858,net)||!net)throw std::runtime_error("relay_network_handler_state");
    if(handler)return;
    void* object=reinterpret_cast<void*(__cdecl*)(unsigned)>(0x617492)(12);
    if(!object)throw std::runtime_error("relay_network_handler_allocation");
    reinterpret_cast<void(__thiscall*)(void*)>(0x557ba0)(object);
    *static_cast<std::uint32_t*>(object)=0x63a684;
    *reinterpret_cast<void**>(adventure+0x38)=object;
    reinterpret_cast<void(__thiscall*)(void*,void*)>(0x553b70)(reinterpret_cast<void*>(net),object);
}
// The single-player scaffold loader renames seats (004B9E30 sets "Computer").
// In a network game each human's name is its network player name.
void RestorePlayerNames(std::uint32_t world) {
    const auto transport=hota_solo_transport::GetSnapshot();
    for(unsigned c=0;c<8;++c){
        std::string name;if(!transport.ids[c]||!hp_relay::RejoinPlayerName(transport.ids[c],name))continue;
        char field[20]{};std::memcpy(field,name.data(),name.size()<19?name.size():19);
        std::memcpy(reinterpret_cast<void*>(world+0x20b9c+c*0x168),field,sizeof field);
    }
}
bool Authorize(void* context,std::uint32_t token,const hota_resume::Audit& a) noexcept {
    try{
        const auto& b=*static_cast<const Bundle*>(context);auto transport=hota_solo_transport::GetSnapshot();
        std::uint32_t player=0,mask=0,view_mask=0;int local=-1,current=-1,view_owner=-1;unsigned char controlled=0;
        std::array<unsigned char,0x74> plain{};std::array<unsigned char,16> tail{};
        if(!inside||!held.load()||b.token!=token||!transport.active||transport.fault||transport.token!=token||
           !hp::Read(0x6995a4,local)||local!=a.local||!hp::Read(0x69ccf4,current)||current!=a.current||
           !hp::Read(0x69ccfc,player)||player!=a.world+0x20ad0+a.current*0x168||!hp::Read(0x69cd10,mask)||mask!=(1u<<a.current)||
           !hp::Read(0x6977dc,view_owner)||view_owner!=a.local||!hp::Read(0x69cd08,view_mask)||view_mask!=(1u<<a.local)||
           !hp::Read(0x698a10,controlled)||controlled!=(a.players[a.current].local&&a.players[a.current].human?1:0)||
           !Read(hd+0x210006,plain.data(),plain.size())||std::memcmp(plain.data(),b.st.data()+12,plain.size())||
           !Read(hd+0x210086,tail.data(),tail.size())||std::memcmp(tail.data(),b.st.data()+0x8c,tail.size()))return false;
        for(unsigned c=0;c<8;++c)if(a.players[c].human&&a.players[c].transport_id!=transport.ids[c])return false;
        std::uint32_t queue_count=0,roster=0,hero_vector=0,hero_count=0;
        std::array<unsigned char,187> live_roster{};
        if(!hp::Read(hd+0x20fffe,queue_count)||queue_count||!hp::Read(hd+0x11b8c84,roster)||!roster||
           !Read(roster,live_roster.data(),live_roster.size())||!hp::Read(hd+0x21007a,hero_vector)||!hero_vector||
           !hp::Read(hd+0x21007e,hero_count)||hero_count!=b.hero_assignment.size())return false;
        auto expected_roster=b.roster;
        for(unsigned i=0;i<expected_roster[0];++i){auto* row=expected_roster.data()+3+i*23;std::memcpy(row,&transport.ids[row[4]],4);}
        if(std::memcmp(live_roster.data(),expected_roster.data(),187))return false;
        std::vector<unsigned char> assignments(hero_count);
        return Read(hero_vector,assignments.data(),hero_count)&&assignments==b.hero_assignment;
    }catch(...){return false;}
}
}
bool Initialize(PatcherInstance* patcher,std::uint32_t base,std::uint32_t thread,std::string* error) noexcept {
    try{
        if(snapshot.initialized||!base||GetCurrentThreadId()!=thread){Error(error,"invalid_resume_session_initialize");return false;}
        if(!hota_solo_transport::Install(patcher,base,thread,error))return false;
        auto initialized=importer.InitializeNative(base,thread,Admission);
        if(!initialized){Error(error,hota_memfile::ErrorName(initialized.error));return false;}
        hd=base;game_thread=thread;std::lock_guard<std::mutex> l(mutex);snapshot.initialized=true;snapshot.stage="idle";return true;
    }catch(...){Error(error,"resume_session_initialize_exception");return false;}
}
bool QueueFile(const std::wstring& path,std::uint32_t token,std::string* error) noexcept {
    try{
        {std::lock_guard<std::mutex> l(mutex);if(!snapshot.initialized||snapshot.queued||snapshot.started){Error(error,"resume_session_already_owned_or_uninitialized");return false;}}
        const auto canonical=hp::Canonical(path);if(canonical.empty()){Error(error,"invalid_resume_bundle_path");return false;}
        hp::Handle file(CreateFileW(canonical.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        if(!file||!GetFileInformationByHandle(file.value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!GetFileSizeEx(file.value,&size)||size.QuadPart<32||size.QuadPart>MaxBytes){Error(error,"invalid_or_oversize_resume_bundle");return false;}
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));DWORD got=0;
        if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)||got!=bytes.size()){Error(error,"resume_bundle_read_failed");return false;}
        auto bundle=Decode(bytes,token);
        std::wstring journal=hp::Parent(canonical)+L"\\HeroesPost.packet-journal-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(hp::CreationTime(GetCurrentProcess()))+L"-"+std::to_wstring(token);
        if(!hota_solo_journal::Initialize(journal,error))return false;
        {std::lock_guard<std::mutex> l(mutex);queued=std::move(bundle);snapshot.queued=true;snapshot.token=token;snapshot.stage="queued";}
        pending.store(true);return true;
    }catch(const std::exception& ex){Error(error,ex.what());return false;}catch(...){Error(error,"resume_bundle_queue_exception");return false;}
}
bool TryAtAdventureBoundary() noexcept {
    if(!pending.load())return held.load();
    if(inside||GetCurrentThreadId()!=game_thread)return held.load();
    if(!hota_session::IsScaffoldRestorePending())return false;
    held.store(true);inside=true;pending.store(false);
    try{
        Bundle b;{std::lock_guard<std::mutex> l(mutex);b=queued;snapshot.started=true;snapshot.queued=false;}
        Stage("load_exact_checkpoint");hota_resume::Request request;request.token=b.token;request.expected_local=static_cast<int>(b.color);request.expected_date=b.source.date;
        request.map_size=b.source.map_size;request.underground=b.source.underground;request.local_snapshot=b.world;request.source_audit=b.source;request.has_source_audit=true;request.source_roster=b.roster;
        std::string error;if(!hota_resume::Queue(request,&error))throw std::runtime_error(error);
        if(!hota_resume::TryAtAdventureBoundary())throw std::runtime_error("native_reload_not_executed");
        auto loaded=hota_resume::GetSnapshot();if(!loaded.source_audit_matched)throw std::runtime_error(loaded.error.empty()?"native_loaded_world_audit_not_matched":loaded.error);
        Stage("establish_native_tcp");
        if(!hota_session::EstablishSoloTransportAtBoundary(b.token,&error))throw std::runtime_error(error);
        Stage("restore_day_baselines_and_st");
        auto ensured=importer.EnsureRoles({hota_memfile::Role::BaselineDayOne,hota_memfile::Role::BaselineLater});if(!ensured)throw std::runtime_error(hota_memfile::ErrorName(ensured.error));
        auto replaced=importer.Replace({{hota_memfile::Role::BaselineLater,b.baseline},{hota_memfile::Role::BaselineDayOne,b.baseline_day_one}});if(!replaced)throw std::runtime_error(hota_memfile::ErrorName(replaced.error));
        if(!RestoreST(b,error))throw std::runtime_error(error);
        Stage("activate_logical_humans");
        unsigned char humans=0;for(unsigned c=0;c<8;++c)if(b.source.players[c].human)humans|=static_cast<unsigned char>(1u<<c);
        if(!hota_solo_transport::ActivateAtBoundary(b.token,loaded.after.world,loaded.after.local,loaded.after.current,humans,b.roster,&error))throw std::runtime_error(error);
        auto expected=loaded.after;const auto transport=hota_solo_transport::GetSnapshot();for(unsigned c=0;c<8;++c)if(expected.players[c].human)expected.players[c].transport_id=transport.ids[c];
        Stage("refresh_native_adventure_ui");
        // Same controls/redraw sequence used by HD's post-rollback world reload.
        // No StartTurn, movement reset, day step, or scenario initialization.
        reinterpret_cast<void(__cdecl*)()>(hd+0x1753d0)();
        reinterpret_cast<void(__thiscall*)(void*,int,int)>(0x417380)(reinterpret_cast<void*>(loaded.after.adventure),1,0);
        Stage("audit_complete_recovery");
        if(!hota_resume::ReleaseAfterRebind(b.token,expected,Authorize,&b,&error))throw std::runtime_error(error);
        if(!hota_session::ReleaseScaffoldGate(b.token,&error))throw std::runtime_error(error);
        if(hp_relay::Rejoining()){AttachNetworkHandler(loaded.after.adventure);RestorePlayerNames(loaded.after.world);}
        // A marker checkpoint precedes EndTurn's tail. Loading its world does
        // not recreate that call stack or prove a native wait-state continuation.
        const bool completed=b.kind==1||b.st[0x50+b.color*6+1]!=0;
        {std::lock_guard<std::mutex> l(mutex);snapshot.verified=true;snapshot.completed_input_held=completed;snapshot.stage=completed?"completed_checkpoint_held":"verified";snapshot.error.clear();queued=Bundle{};}
        owner_world=expected.world;owner_local=expected.local;owner_date=expected.date;
        // A relay rejoin is a live participant: a completed seat answers the
        // day barrier and merges like any other client, so it is not held.
        verified.store(true);held.store(completed&&!hp_relay::Rejoining());inside=false;
        // The restored seat now matches its capture; deliver what the live game sent since.
        if(hp_relay::Rejoining())hp_relay::ReleaseGameMessages();
        return true; // first restored frame remains skipped
    }catch(const std::exception& ex){Fail(ex.what());}catch(...){Fail("resume_transaction_exception");}
    inside=false;return true;
}
void ObserveCompletion(const hota_completion::Snapshot& mark) noexcept {
    if(!verified.load()||GetCurrentThreadId()!=game_thread||hp_relay::Rejoining())return;
    try {
        {std::lock_guard<std::mutex> l(mutex);if(snapshot.completed_input_held)return;}
        held.store(true); // Before native tail redraw can pump nested dispatch.
        {std::lock_guard<std::mutex> l(mutex);snapshot.completed_input_held=true;snapshot.completion_marker_observed=true;snapshot.stage="completed_native_tail_pending";}
        if(!mark.marker_confirmed||!mark.metadata_stable||mark.world_address!=owner_world||mark.local_player!=owner_local||
           mark.current_player!=owner_local||mark.site_player!=owner_local||mark.multiplayer!=1||mark.st_enabled!=1||
           mark.merge_failed||mark.merge_succeeded||mark.stop_pending||owner_local<0||owner_local>7||
           std::memcmp(mark.world_date_words,owner_date.data(),6)||!mark.participants[owner_local].participates||mark.participants[owner_local].done!=1)
            throw std::runtime_error("solo_completed_marker_identity_or_phase_mismatch");
        bool remote_unfinished=false;
        for(unsigned c=0;c<8;++c){completion_rows[c]=mark.participants[c];if(static_cast<int>(c)!=owner_local&&mark.participants[c].participates&&!mark.participants[c].done)remote_unfinished=true;}
        if(!remote_unfinished)throw std::runtime_error("solo_completion_needs_real_remote_barrier_protocol");
        expected_neighbor=mark.source_site==hota_completion::SourceSite::SecondaryAfterDone?
            mark.participants[owner_local].predecessor:mark.participants[owner_local].successor;
        if(expected_neighbor<0||expected_neighbor>7||expected_neighbor==owner_local||!mark.participants[expected_neighbor].participates)
            throw std::runtime_error("solo_completed_neighbor_not_owned_human");
        std::string error;if(!hota_resume::ReadAudit(completion_audit,&error))throw std::runtime_error(error);
        if(completion_audit.world!=owner_world||completion_audit.local!=owner_local||completion_audit.current!=owner_local||completion_audit.date!=owner_date)
            throw std::runtime_error("solo_completion_world_changed_at_marker");
        completion_pending=true;
    }catch(const std::exception& ex){Fail(ex.what());}catch(...){Fail("solo_completion_observer_exception");}
}
void ObserveTurnReturned() noexcept {
    if(!completion_pending||GetCurrentThreadId()!=game_thread)return;
    completion_pending=false;
    try {
        {std::lock_guard<std::mutex> l(mutex);snapshot.completion_tail_returned=true;}
        hota_resume::Audit after;std::string error;
        if(!hota_resume::ReadAudit(after,&error))throw std::runtime_error(error);
        auto expected=completion_audit;expected.current=expected_neighbor;
        if(after.world!=owner_world||after.adventure!=completion_audit.adventure||!hota_resume::SameWorldContent(expected,after))
            throw std::runtime_error("solo_native_tail_changed_world_day_resources_heroes_or_ownership");
        std::uint32_t network=0,st=0,exit=1,protected_exit=1,merge_failed=1,merge_succeeded=1;
        std::array<hota_completion::Participant,8> rows{};
        if(!hp::Read(0x69959c,network)||network!=1||!hp::Read(hd+0x210042,st)||st!=1||
           !hp::Read(hd+0x217508,exit)||exit||!hp::Read(hd+0x2176ec,protected_exit)||protected_exit||
           !hp::Read(hd+0x210026,merge_failed)||merge_failed||!hp::Read(hd+0x21001a,merge_succeeded)||merge_succeeded||
           !Read(hd+0x21004a,rows.data(),48)||std::memcmp(rows.data(),completion_rows.data(),48))
            throw std::runtime_error("solo_native_tail_changed_st_or_human_participation");
        {std::lock_guard<std::mutex> l(mutex);snapshot.completion_tail_audit_passed=true;snapshot.stage="completed_turn_held";}
        // Remain held. Journal persistence and authenticated server checkpoint
        // receipt are separate facts; neither creates a native remote ACK here.
    }catch(const std::exception& ex){Fail(ex.what());}catch(...){Fail("solo_completion_tail_audit_exception");}
}
bool IsGameplayBlocked() noexcept {return held.load()||(verified.load()&&hota_solo_transport::IsGameplayBlocked());}
Snapshot GetSnapshot(){std::lock_guard<std::mutex> l(mutex);return snapshot;}
std::string SerializeSnapshot(){auto s=GetSnapshot();auto t=hota_solo_transport::GetSnapshot();auto j=hota_solo_journal::GetSnapshot();std::ostringstream o;o<<"{\"initialized\":"<<(s.initialized?"true":"false")<<",\"queued\":"<<(s.queued?"true":"false")<<",\"started\":"<<(s.started?"true":"false")<<",\"verified\":"<<(s.verified?"true":"false")<<",\"completedInputHeld\":"<<(s.completed_input_held?"true":"false")<<",\"completionMarkerObserved\":"<<(s.completion_marker_observed?"true":"false")<<",\"completionTailReturned\":"<<(s.completion_tail_returned?"true":"false")<<",\"completionTailAuditPassed\":"<<(s.completion_tail_audit_passed?"true":"false")<<",\"playable\":"<<(s.verified&&!s.completed_input_held&&!hota_solo_transport::IsGameplayBlocked()?"true":"false")<<",\"failed\":"<<(s.failed?"true":"false")<<",\"token\":"<<s.token<<",\"stage\":"<<hp::Json(s.stage)<<",\"error\":"<<hp::Json(s.error)<<",\"transportActive\":"<<(t.active?"true":"false")<<",\"transportFault\":"<<(t.fault?"true":"false")<<",\"transportError\":"<<hp::Json(t.error)<<",\"packetsAccepted\":"<<j.accepted<<",\"packetsDurable\":"<<j.durable<<",\"journalError\":"<<hp::Json(j.fatal_error)<<"}";return o.str();}
}
