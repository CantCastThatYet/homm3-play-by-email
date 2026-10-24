// Decoder-only synthetic tests; never initializes hooks or calls native code.
#include "resume_session.cpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <iterator>

// Link-only fail-closed seams: a parser test reaching an engine/journal action
// aborts instead of substituting a successful native operation.
namespace hota_session {
bool IsScaffoldRestorePending() noexcept {std::abort();}
bool EstablishSoloTransportAtBoundary(std::uint32_t,std::string*) noexcept {std::abort();}
bool ReleaseScaffoldGate(std::uint32_t,std::string*) noexcept {std::abort();}
}
namespace hota_solo_transport {
bool Install(PatcherInstance*,std::uint32_t,std::uint32_t,std::string*) noexcept {std::abort();}
bool ActivateAtBoundary(std::uint32_t,std::uint32_t,int,int,std::uint8_t,const std::vector<std::uint8_t>&,std::string*) noexcept {std::abort();}
bool IsGameplayBlocked() noexcept {std::abort();}
Snapshot GetSnapshot(){std::abort();}
}
namespace hota_solo_journal {
bool Initialize(const std::wstring&,std::string*) noexcept {std::abort();}
Snapshot GetSnapshot() noexcept {std::abort();}
}

namespace {
void Put(std::vector<unsigned char>& b,unsigned n){for(unsigned i=0;i<4;++i)b.push_back(static_cast<unsigned char>(n>>(i*8)));}
void At(std::vector<unsigned char>& b,std::size_t p,unsigned n){std::memcpy(b.data()+p,&n,4);}
std::vector<unsigned char> Make(unsigned assignments=2){
    hota_resume::Audit a;a.width=a.height=a.map_size=36;a.local=a.current=0;a.date={1,1,1};
    a.players[0].transport_id=1;a.players[0].human=a.players[0].local=1;a.players[1].transport_id=123;a.players[1].human=1;
    a.heroes={{0,1500,1200,5,5,0,0},{1,1500,1500,25,25,0,1},{-1,0,0,-1,-1,255,255}};
    auto audit=hota_resume::EncodeAudit(a);std::vector<unsigned char> roster(187),st(156),assignment(assignments,0xff);
    roster[0]=2;roster[1]=1;At(roster,3,1);roster[7]=0;At(roster,26,123);roster[30]=1;
    At(st,0x1210042-0x120fffa,1);At(st,0x1210016-0x120fffa,1);At(st,0x121007e-0x120fffa,assignments);
    st[0x53]=st[0x59]=1;st[0x54]=0xff;st[0x55]=1;st[0x5a]=0;st[0x5b]=0xff;
    std::vector<unsigned char> bytes{'H','P','R','1'};for(unsigned n:{1u,0u,1u,1u,1u,0u})Put(bytes,n);
    for(const auto& v:std::vector<std::vector<unsigned char>>{{1,2,3},{4,5},audit,roster,st,assignment,{9,8,7}}){Put(bytes,static_cast<unsigned>(v.size()));bytes.insert(bytes.end(),v.begin(),v.end());}
    return bytes;
}
bool Reject(const std::vector<unsigned char>& b){try{resume_session::Decode(b,7);return false;}catch(...){return true;}}
}
int main(int argc,char** argv){
    auto valid=Make();auto parsed=resume_session::Decode(valid,7);
    assert(parsed.token==7&&parsed.baseline==std::vector<unsigned char>({4,5})&&parsed.baseline_day_one==std::vector<unsigned char>({9,8,7}));
    assert(parsed.source.heroes.size()==3&&parsed.hero_assignment.size()==2); // native peer vector may be shorter
    for(std::size_t n=0;n<valid.size();++n)assert(Reject(std::vector<unsigned char>(valid.begin(),valid.begin()+n)));
    auto bad=valid;bad.push_back(0);assert(Reject(bad));
    bad=valid;At(bad,28,0xffffffff);assert(Reject(bad));
    bad=valid;At(bad,8,8);assert(Reject(bad));
    bad=valid;At(bad,12,8);assert(Reject(bad));
    bad=valid;At(bad,24,2);assert(Reject(bad));
    std::size_t at=28;for(unsigned i=0;i<4;++i){unsigned n=resume_session::U32(valid,at);at+=4+n;}
    const auto st=at+4;
    bad=valid;At(bad,st+4,1);assert(Reject(bad)); // deferred queue cannot silently vanish
    bad=valid;At(bad,st+0x84,100);assert(Reject(bad)); // assignment vector length mismatch
    bad=valid;At(bad,st+0x38,1);assert(Reject(bad)); // in-transition save rejected
    for(unsigned address:{0x121001au,0x121001eu,0x1210022u,0x1210026u,0x121002au,0x121002eu,0x1210036u,0x121003eu,0x1210046u}){bad=valid;At(bad,st+address-0x120fffa,1);assert(Reject(bad));}
    bad=valid;bad[st+0x53]=0;assert(Reject(bad));
    bad=valid;bad[st+0x55]=0;assert(Reject(bad));
    bad=valid;bad[st+0x5a]=255;assert(Reject(bad));
    bad=valid;bad[st+0x52]=3;assert(Reject(bad));
    assert(!Reject(Make(156)));
    bad=valid;At(bad,st+0x8c,1);assert(!Reject(bad)); //86 spans the native gameplay loop, not merge depth
    if(argc==2){std::ifstream file(argv[1],std::ios::binary);assert(file);std::vector<unsigned char> external((std::istreambuf_iterator<char>(file)),{});assert(!Reject(external));std::cout<<"Actual captured bundle decoded successfully\n";}
    std::cout<<"Resume bundle bounds, truncation, identity, empty-queue requirement, exact assignments and distinct baselines passed\n";
}
