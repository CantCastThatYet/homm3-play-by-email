#pragma once
#include "common.hpp"
#include <cstring>
namespace hota_port::detail {
// DPCOMPOUNDADDRESSELEMENT ABI, not a native game-object layout.
struct Element { GUID type; DWORD size; const void* data; };
static_assert(sizeof(Element)==24,"DirectPlay compound elements require x86 ABI");
inline constexpr GUID service_type={0x07d916c0,0xe0af,0x11cf,{0x9c,0x4e,0,0xa0,0xc9,5,0x42,0x5e}};
inline constexpr GUID inet_type={0xc4a54da0,0xe0af,0x11cf,{0x9c,0x4e,0,0xa0,0xc9,5,0x42,0x5e}};
inline constexpr GUID tcp_service={0x36e95ee0,0x8577,0x11cf,{0x96,0x0c,0,0x80,0xc7,0x53,0x4e,0x82}};
inline constexpr GUID port_type={0xe4524541,0x8ea5,0x11d1,{0x8a,0x96,0,0x60,0x97,0xb0,0x14,0x11}};
inline bool Same(const GUID& a,const GUID& b) noexcept { return !std::memcmp(&a,&b,sizeof a); }
inline bool Build(const Element (&input)[2],const GUID& service,const WORD* port,Element (&output)[3]) noexcept {
    if(!port||*port<1024||!Same(input[0].type,service_type)||input[0].size!=16||!input[0].data||
       !Same(service,tcp_service)||!Same(input[1].type,inet_type)||!input[1].size||input[1].size>256||!input[1].data)return false;
    output[0]=input[0];output[1]=input[1];output[2]={port_type,sizeof(WORD),port};return true;
}
}
