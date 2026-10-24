// Synthetic transport acceptance/rejection tests. No game function or hook is invoked.
#include "solo_transport.cpp"
#include <cassert>
#include <iostream>
#include <thread>
namespace {bool accept_packet=true;unsigned accepted=0;}
namespace hota_solo_journal {
bool Accept(const PacketView& p) noexcept {if(!accept_packet)return false;assert(p.token==7&&p.sequence==1&&p.sender_dpid==1);++accepted;return true;}
Snapshot GetSnapshot() noexcept {return {};}
}
int main(){
    using namespace hota_solo_transport;
    std::array<unsigned char,0x100> net{};
    const auto pointer=reinterpret_cast<std::uint32_t>(net.data());
    auto reset=[&](){std::lock_guard<std::mutex> l(mutex);state=Snapshot{};state.active=true;state.installed=true;state.transport=pointer;state.local_dpid=1;state.ids[0]=1;state.ids[1]=0x7f010001;state.token=7;thread_id=GetCurrentThreadId();local_color=0;blocked.store(false);active_transport.store(true);accepted=0;accept_packet=true;};
    std::uint32_t packet[6]={0,0,0x8a2,sizeof packet,0,0};
    auto result_code=[&](){std::uint32_t v=0;std::memcpy(&v,net.data()+0x40,4);return v;};
    reset();assert(SendHook(nullptr,net.data(),packet,sizeof packet,1,0x7f010001,1)==1);assert(accepted==1&&state.packets==1&&result_code()==0&&!blocked.load());
    reset();accept_packet=false;assert(!SendHook(nullptr,net.data(),packet,sizeof packet,1,0x7f010001,1));assert(result_code()==0x80070057&&blocked.load()&&state.fault&&state.packets==0&&accepted==0);
    reset();packet[3]=25;assert(!SendHook(nullptr,net.data(),packet,sizeof packet,1,0x7f010001,1));assert(result_code()==0x80070057&&state.error=="invalid_native_packet_header"&&accepted==0);packet[3]=sizeof packet;
    reset();assert(!SendHook(nullptr,net.data(),packet,sizeof packet,9,0x7f010001,1));assert(result_code()==0x80070057&&accepted==0);
    reset();assert(!SendHook(nullptr,net.data(),packet,sizeof packet,1,0xdeadbeef,1));assert(result_code()==0x80070057&&accepted==0);
    reset();std::thread other([&](){assert(!SendHook(nullptr,net.data(),packet,sizeof packet,1,0,1));});other.join();assert(result_code()==0x80070057&&state.fault&&accepted==0);
    std::cout<<"Solo transport accepts valid packets and rejects invalid identity/header/thread/journal failure with bounded native HRESULT and held gameplay\n";
}
