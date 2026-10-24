// Standalone synthetic tests of the real pre-dispatch wrapper. No hooks are
// installed and no engine memory/functions are accessed. A null native chain
// makes accidental dispatch in a held case fail immediately instead of hiding it.
#include "adventure_actions.cpp"
#include <cstdio>
namespace {
bool held=false,return_latched=true,exercise_nested=false;
unsigned boundary_calls=0;
bool Gate(void*) noexcept { return held; }
bool Boundary(void*) noexcept {
    using namespace hota_adventure;
    if(!IsAtIdleBoundary()||IsProcessingIdle())std::abort();
    ++boundary_calls;held=true;
    if(exercise_nested) {
        Message nested{1,0,0,0,0,0,0,0};
        if(MessageHook(nullptr,0,&nested)!=0||!IsAtIdleBoundary()||IsProcessingIdle())std::abort();
    }
    return return_latched;
}
}
int main() {
    using namespace hota_adventure;
    game_thread=GetCurrentThreadId();installed.store(true);
    if(!ConfigureInputGate(Gate,nullptr)||!ConfigureIdleBoundary(Boundary,nullptr))return 1;
    Message input{0x200,0xd,0xc,0,0,0,0,0};held=true;
    if(MessageHook(nullptr,0,&input)||boundary_calls||IsAtIdleBoundary()||IsProcessingIdle())return 2;
    held=false;exercise_nested=true;
    if(!QueueEndTurn({42,0,1,1,1}))return 3;
    Message idle{};
    if(MessageHook(nullptr,0,&idle)||boundary_calls!=1||GetSnapshot().status!=Status::Refused||
       GetSnapshot().error!=Error::GameplayBlocked||GetSnapshot().token!=42||idle.command!=0)return 4;
    if(IsAtIdleBoundary()||IsProcessingIdle()||dispatch_depth)return 5;
    held=false;return_latched=false;exercise_nested=false;
    if(MessageHook(nullptr,0,&idle)||boundary_calls!=2||IsAtIdleBoundary()||IsProcessingIdle())return 6;
    if(MessageHook(nullptr,0,&input)||boundary_calls!=2)return 7;
    std::puts("Adventure held-dispatch, pre-chain latch, nested boundary and queued-action tests passed; no game or hooks.");
    return 0;
}
