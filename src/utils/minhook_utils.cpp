#include "utils/minhook_utils.h"
#include "core/logger.h"
#include <Windows.h>
#include <map>
#include <cstring>
#include <sstream>

namespace {
bool ReadImage(uintptr_t address,size_t size,std::vector<unsigned char>& out) {
    if(!address || !size || address+size<address)return false;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)) || info.State!=MEM_COMMIT || info.Type!=MEM_IMAGE || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const DWORD protection=info.Protect&0xff;
    if(protection!=PAGE_EXECUTE && protection!=PAGE_EXECUTE_READ && protection!=PAGE_EXECUTE_READWRITE && protection!=PAGE_EXECUTE_WRITECOPY)return false;
    if(address+size>reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize)return false;
    out.resize(size);SIZE_T read=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),out.data(),size,&read)&&read==size;
}
PracticeHooks::Backend NativeBackend(){
    PracticeHooks::Backend b;
    b.create=[](void* t,void* d,void** o){return static_cast<int>(MH_CreateHook(t,d,o));};
    b.enable=[](void* t){return static_cast<int>(MH_EnableHook(t));};
    b.disable=[](void* t){return static_cast<int>(MH_DisableHook(t));};
    b.remove=[](void* t){return static_cast<int>(MH_RemoveHook(t));};
#ifndef EFZ_MINHOOK_NO_QUEUE_API
    b.queueDisable=[](void* t){return static_cast<int>(MH_QueueDisableHook(t));};
    b.applyQueued=[](){return static_cast<int>(MH_ApplyQueued());};
#endif
    b.read=ReadImage;b.ok=MH_OK;b.alreadyCreated=MH_ERROR_ALREADY_CREATED;
    b.alreadyEnabled=MH_ERROR_ENABLED;b.alreadyDisabled=MH_ERROR_DISABLED;
    return b;
}
void LogOutcome(const char* action,const char* category,const char* label,const PracticeHooks::Outcome& result){
    if(result.Complete())return;
    std::ostringstream os;os<<(category?category:"[HOOK]")<<" "<<action<<" "<<(label?label:"hook")
        <<" retained/pending result="<<static_cast<int>(result.result)<<" backend="<<result.backendStatus<<" remaining="<<result.remaining;
    LogOut(os.str(),true);
}
}
namespace MinHookUtils {
PracticeHooks::Registry& OwnedHooks(){
    // Resident lifetime: no process-detach destructor may release pins/trampolines
    // while callbacks or foreign chains still carry execution obligations.
    static auto* registry=new PracticeHooks::Registry(NativeBackend());return *registry;
}
PracticeHooks::Outcome CloseAdmission(const char* owner){return OwnedHooks().CloseAdmission(owner?owner:"");}
PracticeHooks::Outcome DisableOwnedTargets(const char* owner,const PracticeHooks::RetirementProof& proof){return OwnedHooks().DisableOwnedTargets(owner?owner:"",proof);}
PracticeHooks::Outcome ReclaimDrainedTargets(const char* owner,const PracticeHooks::RetirementProof& proof){return OwnedHooks().ReclaimDrainedTargets(owner?owner:"",proof);}
bool HasOwnedTarget(void* target){for(const auto& r:OwnedHooks().Snapshot())if(r.spec.target==target)return true;return false;}
bool EnableOwnedTargets(const char* owner){bool result=true;for(const auto& r:OwnedHooks().Snapshot())if(!owner||r.spec.owner==owner)result=OwnedHooks().Enable(r.spec.target).Complete()&&result;return result;}
bool CreateHook(void* target,void* detour,void** original,const char* category,const char* label,bool* alreadyCreated,PracticeHooks::CallbackTicket* ticket){
    if(alreadyCreated)*alreadyCreated=false;
    PracticeHooks::TargetSpec spec;
    for(const auto& r:OwnedHooks().Snapshot())if(r.spec.target==target){spec=r.spec;if(alreadyCreated)*alreadyCreated=true;break;}
    const bool existing=spec.target!=nullptr;
    // Existing callbacks use one original slot per detour. A second physical
    // target cannot replace that slot before the first target is reclaimed.
    for(const auto& record:OwnedHooks().Snapshot())
        if(record.spec.detour==detour && record.spec.target!=target) return false;
    spec.target=target;spec.detour=detour;spec.owner=category?category:"[HOOK]";
    // The stable call-site label identifies its declared native ABI; detour
    // identity is checked independently and may never replace an owned target.
    spec.abi=label?label:"hook";
    if(!existing){
        HMODULE module=nullptr;
        if(!target || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCSTR>(target),&module))return false;
        spec.modulePin=std::shared_ptr<void>(module,[](void* p){FreeLibrary(static_cast<HMODULE>(p));});
        // A new held module acquisition gets a never-reused incarnation. Existing
        // targets reuse their recorded incarnation and loader pin above.
        static std::atomic<uint64_t> nextIncarnation{1};
        uint64_t incarnation=nextIncarnation.load();
        do { if(!incarnation || incarnation==UINT64_MAX) return false; }
        while(!nextIncarnation.compare_exchange_weak(incarnation,incarnation+1));
        spec.moduleIncarnation=incarnation;
        // Include MinHook's possible five-byte hotpatch area above the entry.
        spec.rangeStart=reinterpret_cast<uintptr_t>(target)-5;
        if(!ReadImage(spec.rangeStart,21,spec.preimage))return false;
    }
    const auto result=OwnedHooks().Acquire(spec,original);
    LogOutcome("create",category,label,result);
    if (!result.Complete()) return false;
    if (ticket) {
        const auto bound=OwnedHooks().BindCallback(target,*ticket);
        LogOutcome("bind callback",category,label,bound);
        return bound.Complete();
    }
    return true;
}
bool EnableHook(void* target,const char* category,const char* label,bool* alreadyEnabled){
    if(alreadyEnabled)*alreadyEnabled=false;
    const auto result=OwnedHooks().Enable(target);LogOutcome("enable",category,label,result);return result.Complete();
}
bool DisableHook(void* target,const char* category,const char* label,bool* alreadyDisabled){
    if(alreadyDisabled)*alreadyDisabled=false;
    // Legacy callers supply no safe native continuation. Close admission and
    // retain the executable attachment; F07 must advance the explicit operation.
    CloseAdmission(category);
    const auto result=DisableOwnedTargets(category,{});LogOutcome("disable",category,label,result);
    return result.Complete() && (!target || HasOwnedTarget(target));
}
bool RemoveHook(void* target,const char* category,const char* label,bool* wasMissing){
    if(wasMissing)*wasMissing=false;
    if(!target)return true;
    if(!HasOwnedTarget(target))return false;
    CloseAdmission(category);
    const auto result=ReclaimDrainedTargets(category,{});LogOutcome("remove",category,label,result);return result.Complete()&&!HasOwnedTarget(target);
}
bool CreateAndEnableHook(void* target,void* detour,void** original,const char* category,const char* label,bool* alreadyCreated,bool* alreadyEnabled,PracticeHooks::CallbackTicket* ticket){
    return CreateHook(target,detour,original,category,label,alreadyCreated,ticket)&&EnableHook(target,category,label,alreadyEnabled);
}
}
