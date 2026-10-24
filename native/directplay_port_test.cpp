#include "directplay_port_core.hpp"
#include <cstdio>
int main() {
    using namespace hota_port::detail;
    const char address[]="127.0.0.1";WORD port=24777;
    Element input[2]={{service_type,sizeof(GUID),&tcp_service},{inet_type,sizeof address,address}},output[3]{};
    if(!Build(input,tcp_service,&port,output)||std::memcmp(input,output,sizeof input)||
       !Same(output[2].type,port_type)||output[2].size!=2||output[2].data!=&port||*static_cast<const WORD*>(output[2].data)!=24777)return 1;
    auto invalid=input[0];input[0].size=4;if(Build(input,tcp_service,&port,output))return 2;input[0]=invalid;
    input[1].size=0;if(Build(input,tcp_service,&port,output))return 3;input[1].size=257;if(Build(input,tcp_service,&port,output))return 4;
    input[1].size=sizeof address;input[1].type=port_type;if(Build(input,tcp_service,&port,output))return 5;input[1].type=inet_type;
    if(Build(input,port_type,&port,output)||Build(input,tcp_service,nullptr,output))return 6;
    port=0;if(Build(input,tcp_service,&port,output))return 7;
    port=65535;if(!Build(input,tcp_service,&port,output)||output[2].size!=2)return 8;
    std::puts("DirectPlay port element ABI/validation tests passed; no game, hooks, sockets or network changes.");return 0;
}
