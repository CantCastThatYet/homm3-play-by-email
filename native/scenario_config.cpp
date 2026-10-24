#include "scenario_config.hpp"
#include "common.hpp"
#include <cstring>

static_assert(sizeof(void*)==4,"Scenario setup requires x86");
namespace hota_scenario {
namespace {
constexpr std::uint32_t stride=0xca4, filename_offset=0x33d, filename_capacity=251;
struct Vector { std::uint32_t begin=0,end=0,capacity=0; };
bool EqualAscii(const std::string& a,const std::string& b) {
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i) {
        unsigned char x=a[i],y=b[i];
        if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;
        if(x!=y)return false;
    }
    return true;
}
bool NameValid(const std::string& name) {
    if(name.empty()||name.size()>120||name.find_first_of("/\\:")!=std::string::npos||name.find("..")!=std::string::npos)return false;
    for(unsigned char c:name)if(c<32||c>126)return false;
    return name.size()>4&&EqualAscii(name.substr(name.size()-4),".h3m");
}
bool ReadName(std::uintptr_t entry,std::string& result) {
    char bytes[filename_capacity]{};SIZE_T got=0;
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(entry+filename_offset),bytes,sizeof bytes,&got)||got!=sizeof bytes)return false;
    const char* end=static_cast<const char*>(std::memchr(bytes,0,sizeof bytes));
    if(!end)return false;result.assign(bytes,static_cast<size_t>(end-bytes));return true;
}
bool ReadVector(std::uintptr_t dialog,Vector& value) {
    if(!hp::Read(dialog+0x1054,value))return false;
    if(!value.begin||value.end<value.begin||value.capacity<value.end||
       (value.end-value.begin)%stride||(value.capacity-value.begin)%stride)return false;
    return (value.end-value.begin)/stride<=16384&&(value.capacity-value.begin)/stride<=32768;
}
bool SameVector(const Vector& a,const Vector& b) { return a.begin==b.begin&&a.end==b.end&&a.capacity==b.capacity; }
bool ZeroByte(std::uintptr_t p) { unsigned char value=1;return hp::Read(p,value)&&!value; }
bool SameBytes(std::uintptr_t p,const unsigned char* expected,SIZE_T count) {
    unsigned char bytes[16]{};SIZE_T got=0;
    return count<=sizeof bytes&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),bytes,count,&got)&&got==count&&!std::memcmp(bytes,expected,count);
}
void Refuse(Result& r,const char* error) { r.status=Status::Refused;r.error=error; }
}
const char* StatusName(Status s) noexcept {
    switch(s) {case Status::NotRequested:return "not_requested";case Status::Applied:return "applied";
    case Status::Refused:return "refused";default:return "failed";}
}
Result ApplyRequestedMap(void* native_dialog,const std::string& basename,std::uint32_t expected_game_thread,bool solo_scaffold) noexcept {
    Result result;
    try {
        if(basename.empty())return result;
        if(!NameValid(basename)) { Refuse(result,"invalid_fixed_map_basename");return result; }
        if(!expected_game_thread||GetCurrentThreadId()!=expected_game_thread) { Refuse(result,"scenario_requires_game_thread");return result; }
        const auto dialog=reinterpret_cast<std::uintptr_t>(native_dialog);
        std::uint32_t active=0,scan_thread=1,scan_event=1,network_active=0,mode=0,network=0,table=0,host_fn=0;
        if(!dialog||dialog>UINT32_MAX-0x1970||!hp::Read(0x69fc44,active)||active!=dialog||
           !hp::Read(0x69fe28,scan_thread)||scan_thread||!hp::Read(0x69fe2c,scan_event)||scan_event) {
            Refuse(result,"native_scenario_not_ready");return result;
        }
        if(!ZeroByte(dialog+0x64)||!ZeroByte(dialog+0x65)||!ZeroByte(dialog+0x66)||
           !ZeroByte(dialog+0x37f)||!ZeroByte(0x69779c)) {
            Refuse(result,"fixed_map_requires_new_nonrandom_scenario");return result;
        }
        if(solo_scaffold){
            if(!hp::Read(0x69959c,network_active)||network_active||!hp::Read(0x69d858,network)||network||
               !hp::Read(0x698a40,mode)||mode==2||mode==3){
                Refuse(result,"solo_scaffold_requires_native_singleplayer_setup");return result;
            }
        }else if(!hp::Read(0x69959c,network_active)||!network_active||!hp::Read(0x698a40,mode)||mode!=2||
           !hp::Read(0x69d858,network)||!network||!hp::Read(network,table)||!table||
           !hp::Read(table+0x90,host_fn)||!host_fn||
           !reinterpret_cast<unsigned char(__thiscall*)(void*)>(host_fn)(reinterpret_cast<void*>(network))) {
            Refuse(result,"fixed_map_selection_requires_native_tcp_host");return result;
        }
        // Verified unchanged interior instructions in live pinned HotA. Entry
        // itself is detoured; call through it, never through a stale trampoline.
        const unsigned char begin_load[]={0x8b,0x83,0x54,0x10,0,0};
        const unsigned char end_load[]={0x8b,0x8b,0x58,0x10,0,0};
        const unsigned char stride_divisor[]={0xb8,0x71,0x23,0x10,0x05};
        const unsigned char filename_add[]={0x81,0xc7,0x3d,0x03,0,0};
        if(!SameBytes(0x5857de,begin_load,sizeof begin_load)||!SameBytes(0x5857ee,end_load,sizeof end_load)||
           !SameBytes(0x5857f6,stride_divisor,sizeof stride_divisor)||!SameBytes(0x58595f,filename_add,sizeof filename_add)) {
            Refuse(result,"native_scenario_layout_instructions_changed");return result;
        }
        Vector before;
        if(!ReadVector(dialog,before)) { Refuse(result,"native_map_vector_invalid");return result; }
        result.map_count=(before.end-before.begin)/stride;
        std::uint32_t match=UINT32_MAX;
        for(std::uint32_t i=0;i<result.map_count;++i) {
            std::string name;
            if(!ReadName(before.begin+i*stride,name)) { Refuse(result,"native_map_filename_unreadable");return result; }
            if(EqualAscii(name,basename)) {
                if(match!=UINT32_MAX) { Refuse(result,"native_map_basename_ambiguous");return result; }
                match=i;
            }
        }
        if(match==UINT32_MAX) { Refuse(result,"requested_map_not_in_native_current_list");return result; }
        Vector checked;
        if(!ReadVector(dialog,checked)||!SameVector(before,checked)) { Refuse(result,"native_map_list_changed");return result; }
        // This invokes existing HotA/HD chain; it owns header copying, participant
        // reset/assignment and native scenario network updates. No raw data writes.
        using Select=void(__thiscall*)(void*,int,unsigned char);
        result.native_select_called=true;
        reinterpret_cast<Select>(0x5857d0)(native_dialog,static_cast<int>(match),0);
        Vector after;std::uint32_t selected=UINT32_MAX,current=0,still_active=0;
        if(!hp::Read(0x69fc44,still_active)||still_active!=dialog||!ReadVector(dialog,after)||
           !hp::Read(dialog+0x374,selected)||selected>=(after.end-after.begin)/stride||
           !hp::Read(dialog+0x1060,current)||current!=after.begin+selected*stride||
           !ReadName(current,result.selected_basename)||!EqualAscii(result.selected_basename,basename)) {
            result.status=Status::Failed;result.error="native_map_selection_not_verified";return result;
        }
        result.selected_index=selected;result.status=Status::Applied;result.applied=true;return result;
    } catch(...) {
        result.status=Status::Failed;
        try { result.error="scenario_configuration_exception"; } catch(...) {}
        return result;
    }
}
}
