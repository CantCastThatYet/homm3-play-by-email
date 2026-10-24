#include "directplay_port.hpp"
#include "directplay_port_core.hpp"
#include "vendor/patcher_x86.hpp"
#include <atomic>
#include <intrin.h>
namespace hota_port {
namespace {
LoHook* query_hook=nullptr;
LoHook* fill_hook=nullptr;
DWORD game_thread=0;
std::atomic<bool> installed{false},armed{false};
std::atomic<unsigned> configured_port{0},query_count{0},fill_count{0};
std::atomic<Error> last_error{Error::None};
thread_local WORD port_storage=0;
thread_local detail::Element elements[3]{};
thread_local bool inside=false;
void Failure(Error error) noexcept { Error expected=Error::None;last_error.compare_exchange_strong(expected,error); }
void ErrorText(std::string* out,const char* text) noexcept { if(out)try { *out=text; }catch(...){} }
bool Read(std::uintptr_t p,void* destination,SIZE_T size) noexcept {
    SIZE_T got=0;return p&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),destination,size,&got)&&got==size;
}
bool Bytes(std::uintptr_t p,const unsigned char* expected,SIZE_T count) noexcept {
    unsigned char actual[16]{};return count<=sizeof actual&&Read(p,actual,count)&&!std::memcmp(actual,expected,count);
}
void Amend(HookContext* context,bool fill) noexcept {
    if(!armed.load()||GetCurrentThreadId()!=game_thread||inside)return;
    inside=true;
    do {
        if(last_error.load()!=Error::None)break;
        if(!context) { Failure(Error::UnexpectedNativeFrame);break; }
        const auto stack=static_cast<std::uint32_t>(context->esp),frame=static_cast<std::uint32_t>(context->ebp);
        std::uint32_t args[5]{},network=0,lobby=0,mode=0,existing=1;
        if(!Read(stack,args,sizeof args)||!hp::Read(0x69d858,network)||!network||
           static_cast<std::uint32_t>(context->ebx)!=network||!hp::Read(network+0x58,lobby)||args[0]!=lobby||
           !hp::Read(0x698a40,mode)||mode!=2||frame<0x118||args[1]!=frame-0x118||
           !hp::Read(frame+0x10,existing)||existing||!args[4]) {
            Failure(Error::UnexpectedNativeFrame);break;
        }
        if(args[2]!=2||context->esi!=2) { Failure(Error::InvalidElements);break; }
        if((fill&&(query_count.load()!=1||fill_count.load()!=0||!args[3]))||
           (!fill&&(query_count.load()!=0||fill_count.load()!=0||args[3]))) {
            Failure(Error::UnexpectedSequence);break;
        }
        detail::Element original[2]{};GUID service{};
        if(!Read(args[1],original,sizeof original)||!Read(reinterpret_cast<std::uintptr_t>(original[0].data),&service,sizeof service)) {
            Failure(Error::InvalidElements);break;
        }
        if(!detail::Build(original,service,&port_storage,elements)) { Failure(Error::InvalidService);break; }
        char address[256]{};
        if(!Read(reinterpret_cast<std::uintptr_t>(original[1].data),address,original[1].size)||
           address[original[1].size-1]||std::memchr(address,0,original[1].size)!=address+original[1].size-1) {
            Failure(Error::InvalidAddress);break;
        }
        // Before the COM CALL, ESP points at explicit COM this, not a return
        // address. Substitute only the elements pointer/count. Original ESI,
        // native local array, allocation, HRESULT handling and ownership remain.
        const std::uint32_t replacements[2]={reinterpret_cast<std::uint32_t>(elements),3};
        SIZE_T written=0;
        if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(stack+4),replacements,sizeof replacements,&written)||written!=sizeof replacements) {
            Failure(Error::StackWriteFailed);break;
        }
        if(fill)++fill_count;else ++query_count;
    }while(false);
    inside=false;
}
int __stdcall QueryHook(LoHook*,HookContext* context) {
    __declspec(align(16)) unsigned char fp[512];_fxsave(fp);
    const DWORD saved=GetLastError();Amend(context,false);SetLastError(saved);_fxrstor(fp);return EXEC_DEFAULT;
}
int __stdcall FillHook(LoHook*,HookContext* context) {
    __declspec(align(16)) unsigned char fp[512];_fxsave(fp);
    const DWORD saved=GetLastError();Amend(context,true);SetLastError(saved);_fxrstor(fp);return EXEC_DEFAULT;
}
bool Destroy(LoHook*& hook) noexcept {
    if(!hook)return true;
    try { if(!hook->Destroy())return false;hook=nullptr;return true; }catch(...){return false;}
}
}
const char* ErrorName(Error e) noexcept {
    switch(e) {
    case Error::None:return "";case Error::NotInstalled:return "port_hooks_not_installed";
    case Error::WrongThread:return "port_requires_game_thread";case Error::AlreadyArmed:return "port_already_armed";
    case Error::InvalidPort:return "port_must_be_1024_to_65535";
    case Error::UnexpectedNativeFrame:return "port_native_frame_mismatch";
    case Error::InvalidElements:return "port_native_elements_invalid";
    case Error::InvalidService:return "port_native_tcp_elements_mismatch";
    case Error::InvalidAddress:return "port_native_address_invalid";
    case Error::UnexpectedSequence:return "port_compound_call_sequence_mismatch";
    default:return "port_argument_write_failed";
    }
}
Snapshot GetSnapshot() noexcept {
    return {installed.load(),armed.load(),static_cast<std::uint16_t>(configured_port.load()),query_count.load(),fill_count.load(),last_error.load()};
}
bool Arm(std::uint16_t port) noexcept {
    if(!installed.load()) { Failure(Error::NotInstalled);return false; }
    if(GetCurrentThreadId()!=game_thread) { Failure(Error::WrongThread);return false; }
    if(armed.load()) { Failure(Error::AlreadyArmed);return false; }
    if(port<1024) { Failure(Error::InvalidPort);return false; }
    configured_port.store(port);port_storage=port;query_count.store(0);fill_count.store(0);last_error.store(Error::None);armed.store(true);return true;
}
Snapshot Disarm() noexcept {
    if(GetCurrentThreadId()!=game_thread) { Failure(Error::WrongThread);return GetSnapshot(); }
    armed.store(false);return GetSnapshot();
}
bool Install(PatcherInstance* instance,std::string* error) noexcept {
    if(installed.load())return true;
    if(!instance||query_hook||fill_hook) { ErrorText(error,"port_invalid_or_owned_hooks");return false; }
    const unsigned char query[]={0xff,0x51,0x38,0x3d,0x1e,0x00,0x77,0x88};
    const unsigned char fill[]={0xff,0x51,0x38,0x85,0xc0,0x89,0x43,0x40};
    if(!Bytes(0x498aa0,query,sizeof query)||!Bytes(0x498b3b,fill,sizeof fill)) {
        ErrorText(error,"native_compound_address_sites_changed");return false;
    }
    game_thread=GetCurrentThreadId();
    try {
        query_hook=instance->CreateLoHook(0x498aa0,QueryHook);
        fill_hook=instance->CreateLoHook(0x498b3b,FillHook);
        if(!query_hook||!fill_hook||query_hook->Apply()<0||fill_hook->Apply()<0||!query_hook->IsApplied()||!fill_hook->IsApplied()) {
            Uninstall();ErrorText(error,"port_hook_install_failed");return false;
        }
        installed.store(true);return true;
    }catch(...) { Uninstall();ErrorText(error,"port_hook_install_exception");return false; }
}
bool Uninstall(std::string* error) noexcept {
    if(armed.load()||inside||(game_thread&&GetCurrentThreadId()!=game_thread)) { ErrorText(error,"port_uninstall_requires_idle_game_thread");return false; }
    const bool a=Destroy(query_hook),b=Destroy(fill_hook);
    if(a&&b)installed.store(false);else ErrorText(error,"port_hook_rollback_incomplete");
    return a&&b;
}
}
