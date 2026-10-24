// Standalone synthetic bounds/identity tests. Does not load or execute Heroes3.
// Include gives this test access to the private validators without exporting a
// production memory-write/test API. Never link this TU into the adapter DLL.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace {
struct Segment {std::uintptr_t base;std::vector<unsigned char> bytes;};
std::vector<Segment> segments;
unsigned char* FixtureAddress(std::uintptr_t at,std::size_t size) noexcept {
    for(auto& s:segments)if(at>=s.base&&at-s.base<=s.bytes.size()&&size<=s.bytes.size()-(at-s.base))
        return s.bytes.data()+(at-s.base);
    return nullptr;
}
bool FixtureRead(std::uintptr_t at,void* out,std::size_t size) noexcept {
    const auto* bytes=FixtureAddress(at,size);if(!bytes)return false;
    std::memcpy(out,bytes,size);return true;
}
}
#define HEROESPOST_DEPARTURE_TEST_READER FixtureRead
#include "departure_pause.cpp"
#undef HEROESPOST_DEPARTURE_TEST_READER
namespace {
bool boundary_ok=false;
unsigned latch_calls=0;
bool GuardBoundary(void*) noexcept {return boundary_ok;}
void OnLatch(const hota_departure::Snapshot&,void*) noexcept {++latch_calls;}
void Check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
void Allocate(std::uintptr_t at,std::size_t size){
    Check(size&&at<=UINT32_MAX-size,"synthetic segment bounds");
    for(const auto& s:segments)Check(at+size<=s.base||at>=s.base+s.bytes.size(),"synthetic segments disjoint");
    segments.push_back({at,std::vector<unsigned char>(size)});
}
void PutBytes(std::uintptr_t at,const void* bytes,std::size_t size){
    auto* out=FixtureAddress(at,size);Check(out!=nullptr,"synthetic write in range");std::memcpy(out,bytes,size);
}
template<class T>void Put(std::uintptr_t at,T value){PutBytes(at,&value,sizeof value);}
}
int main(){
    using namespace hota_departure;
    Allocate(0x690000,0x20000);Allocate(0x10200000,0x20000);Allocate(0x111b0000,0x10000);
    Allocate(0x30000000,0x30000);Allocate(0x31000000,0x10000);Allocate(0x32000000,0x10000);
    Allocate(0x33000000,0x10000);Allocate(0x34000000,0x10000);
    hd=0x10000000;
    Request r;r.token=1;r.checkpoint_token=2;r.world_address=0x30000000;r.local_dpid=1;r.remote_dpid=33;
    r.local_color=0;r.remote_color=1;r.date_words[0]=r.date_words[1]=r.date_words[2]=1;
    Check(Valid(r),"valid request");Request bad=r;bad.remote_dpid=1;Check(!Valid(bad),"same DPID refused");
    bad=r;bad.date_words[0]=8;Check(!Valid(bad),"invalid day refused");
    Snapshot s;s.transport=0x31000000;
    Put(s.transport+0x94,1u);Put(s.transport+0x6c,0x32000000u);Put(s.transport+0x70,0x32001000u);
    Put(s.transport+0x74,0x32000000u);Put(0x32000000,0x33000000u);
    const std::uint32_t packet[]={UINT32_MAX,33,0x3f6,24,0,33};PutBytes(0x33000000,packet,sizeof packet);
    Check(Front(s.transport,s)&&s.packet_words[1]==33,"bounded queue front read");
    Put(s.transport+0x94,65537u);Check(!Front(s.transport,s),"excess queue count refused");Put(s.transport+0x94,1u);
    Put(s.transport+0x74,0x32001000u);Check(!Front(s.transport,s),"cursor at end refused");Put(s.transport+0x74,0x32000001u);
    Check(!Front(s.transport,s),"unaligned cursor refused");Put(s.transport+0x74,0x32000000u);
    Put(s.transport+0x70,0x32002000u);Check(!Front(s.transport,s),"wrong block capacity refused");Put(s.transport+0x70,0x32001000u);
    Put(0x699538,r.world_address);Put(0x69d858,s.transport);Put(0x69959c,1u);Put(0x6995a4,0u);Put(0x69ccf4,0u);Put(0x69d658,1u);
    Put(s.transport+0x56,static_cast<unsigned char>(1));Put(HD(0x12123b4),0x34000100u);Put(HD(0x1210042),1u);
    PutBytes(r.world_address+0x1f63e,r.date_words,sizeof r.date_words);
    Put(HD(0x121004a)+3,static_cast<unsigned char>(1));Put(HD(0x121004a)+6+3,static_cast<unsigned char>(1));
    Put(HD(0x121004a)+6+1,static_cast<unsigned char>(1));
    Put(r.world_address+0x20b98,1u);Put(r.world_address+0x20b98+0x168,33u);
    Put(r.world_address+0x20bb2,static_cast<unsigned char>(1));Put(r.world_address+0x20bb2+0x168,static_cast<unsigned char>(1));
    Put(HD(0x21b8c84),0x34000000u);Put(0x34000000,static_cast<unsigned char>(2));Put(0x34000003,1u);
    Put(0x34000003+0x17,33u);Put(0x34000007+0x17,static_cast<unsigned char>(1));
    Check(NativeState(r,s),"matching completed remote with head role zero accepted");
    Put(HD(0x121004a)+1,static_cast<unsigned char>(1));Check(!NativeState(r,s),"completed host barrier refused");
    Put(HD(0x121004a)+1,static_cast<unsigned char>(0));
    Put(HD(0x121004a)+6+1,static_cast<unsigned char>(0));Check(!NativeState(r,s),"unfinished remote refused");
    Put(HD(0x121004a)+6+1,static_cast<unsigned char>(1));Put(0x34000003+0x17,0u);
    Check(!NativeState(r,s),"prefilter removed HD peer refused");Put(0x34000003+0x17,33u);
    Put(HD(0x12176ec),1u);Check(!NativeState(r,s),"already protected-exit state refused");Put(HD(0x12176ec),0u);
    Put(0x69959c,0u);Check(!NativeState(r,s),"already offline world refused");Put(0x69959c,1u);
    Put(0x6aad88,1u);Check(!NativeState(r,s),"modal refused");Put(0x6aad88,0u);
    Check(NativeState(r,s),"restored synthetic valid guard");
    Check(!s.safe_logout&&!s.resume_supported&&!s.durable_checkpoint_verified,"no durability or resume claim");
    // Test the adapter-facing boundary decision with synthetic data only.
    game_thread=GetCurrentThreadId();guard=GuardBoundary;latched=OnLatch;installed.store(true);
    Check(Arm(r),"arm synthetic boundary request");
    Check(!TryHoldAtAdventureBoundary()&&GetSnapshot().status==Status::Refused,"missing boundary guard refuses first latch");
    ++r.token;boundary_ok=true;Check(Arm(r),"rearm synthetic boundary request");
    Check(TryHoldAtAdventureBoundary()&&IsGameplayBlocked()&&latch_calls==1,"explicit boundary acquires hold");
    Check(TryHoldAtAdventureBoundary()&&latch_calls==1,"held boundary does not relatch");
    unsigned char decompressed=99;
    Check(PollHook(nullptr,reinterpret_cast<void*>(s.transport),1,&decompressed)==nullptr&&decompressed==0,"held poll does not delegate");
    std::uint32_t cursor=0,count=0;Read(s.transport+0x74,cursor);Read(s.transport+0x94,count);
    Check(cursor==0x32000000&&count==1,"hold preserves native queue ownership and cursor");
    Check(!CancelArmed()&&!Uninstall(),"held state has no cancellation/uninstall release");
    std::puts("departure_pause synthetic guards passed; no native engine execution");return 0;
}
