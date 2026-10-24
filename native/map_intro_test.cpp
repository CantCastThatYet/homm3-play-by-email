// Synthetic exact-text/request validation only; no engine calls or hooks.
#include "map_intro.cpp"
#include <cstdio>
int main() {
    using namespace hota_intro;
    unsigned char text[167]{};
    for(unsigned i=0;i<166;++i)text[i]=static_cast<unsigned char>((Nibble(known_hex[i*2])<<4)|Nibble(known_hex[i*2+1]));
    const auto address=reinterpret_cast<std::uintptr_t>(text);
    if(!KnownText(address,166)||KnownText(address,165)||KnownText(0,166))return 1;
    text[80]^=1;if(KnownText(address,166))return 2;text[80]^=1;
    text[166]=1;if(KnownText(address,166))return 3;text[166]=0;
    installed.store(true);game_thread=GetCurrentThreadId();
    Request request;request.token=5;request.expected_world=1;request.expected_local=0;request.day=1;request.week=1;request.month=1;
    request.map_basename="another-map.h3m";if(QueueKnownIntro(request))return 4;
    request.map_basename="Judgement Day.h3m";if(!QueueKnownIntro(request)||QueueKnownIntro(request)||!CancelQueued())return 5;
    request.expected_local=8;if(QueueKnownIntro(request))return 6;
    std::puts("Known intro exact-byte, length, terminator, map binding and queue tests passed; no game or hooks.");return 0;
}
