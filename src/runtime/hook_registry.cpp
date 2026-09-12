#include "runtime/hook_registry.h"
#include <algorithm>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace PracticeHooks {
namespace {
std::atomic<bool> depthTrackingFault{false};
#ifdef _WIN32
constexpr DWORD kDepthUninitialized=TLS_OUT_OF_INDEXES;
constexpr DWORD kDepthInitializing=TLS_OUT_OF_INDEXES-1;
constexpr DWORD kDepthUnavailable=TLS_OUT_OF_INDEXES-2;
std::atomic<DWORD> depthTlsIndex{kDepthUninitialized};

struct PreserveLastError {
    DWORD saved=GetLastError();
    ~PreserveLastError() {SetLastError(saved);}
};

void InitializeDepthTracking() noexcept {
    PreserveLastError preserve;
    DWORD expected=kDepthUninitialized;
    if (depthTlsIndex.compare_exchange_strong(expected,kDepthInitializing)) {
        const DWORD index=TlsAlloc();
        // The fixed TEB slots need no per-thread expansion allocation when a
        // callback first calls TlsSetValue. Do not accept expansion indices.
        if (index<TLS_MINIMUM_AVAILABLE) {
            depthTlsIndex.store(index,std::memory_order_release);
        } else {
            if (index!=TLS_OUT_OF_INDEXES) TlsFree(index);
            depthTrackingFault.store(true,std::memory_order_release);
            depthTlsIndex.store(kDepthUnavailable,std::memory_order_release);
        }
    } else {
        // Initialization occurs only at registry construction, before native
        // hooks can be created. Callback entry never initializes or waits.
        while(depthTlsIndex.load(std::memory_order_acquire)==kDepthInitializing) Sleep(0);
    }
    // Keep the one TLS index resident; freeing/reusing it while a foreign entry
    // can still reach this DLL would invalidate the self-retirement barrier.
}

bool ReadDepth(DWORD& index,uintptr_t& depth) noexcept {
    if (depthTrackingFault.load(std::memory_order_acquire)) return false;
    index=depthTlsIndex.load(std::memory_order_acquire);
    if (index>=TLS_MINIMUM_AVAILABLE) return false;
    SetLastError(ERROR_SUCCESS);
    depth=reinterpret_cast<uintptr_t>(TlsGetValue(index));
    if (GetLastError()!=ERROR_SUCCESS) {
        depthTrackingFault.store(true,std::memory_order_release);
        return false;
    }
    return true;
}

bool EnterDepth() noexcept {
    PreserveLastError preserve;
    DWORD index=0;uintptr_t depth=0;
    if (!ReadDepth(index,depth) || depth>=UINT32_MAX ||
        !TlsSetValue(index,reinterpret_cast<void*>(depth+1))) {
        depthTrackingFault.store(true,std::memory_order_release);
        return false;
    }
    return true;
}

void LeaveDepth() noexcept {
    PreserveLastError preserve;
    DWORD index=0;uintptr_t depth=0;
    if (!ReadDepth(index,depth) || !depth ||
        !TlsSetValue(index,reinterpret_cast<void*>(depth-1)))
        depthTrackingFault.store(true,std::memory_order_release);
}

bool OutsideCallback() noexcept {
    PreserveLastError preserve;
    DWORD index=0;uintptr_t depth=0;
    return ReadDepth(index,depth) && depth==0;
}
#else
// Compiler TLS is valid on the portable non-Windows test hosts. The Windows
// DLL never compiles this declaration and has no static TLS dependency here.
thread_local uint32_t executionDepth=0;
void InitializeDepthTracking() noexcept {}
bool EnterDepth() noexcept {
    if (depthTrackingFault.load() || executionDepth==UINT32_MAX) {
        depthTrackingFault.store(true);return false;
    }
    ++executionDepth;return true;
}
void LeaveDepth() noexcept {
    if (!executionDepth) depthTrackingFault.store(true);
    else --executionDepth;
}
bool OutsideCallback() noexcept {return !depthTrackingFault.load() && executionDepth==0;}
#endif
}

Registry::Registry(Backend backend):backend_(std::move(backend)) {
    InitializeDepthTracking();
}
ExecutionGuard::ExecutionGuard(ExecutionGuard&& other) noexcept
    : admitted_(other.admitted_), depthTracked_(other.depthTracked_), state_(other.state_) {
    other.state_=nullptr;
}
ExecutionGuard::~ExecutionGuard() {
    if (!state_) return;
    state_->outstanding.fetch_sub(1,std::memory_order_seq_cst);
    if (depthTracked_) LeaveDepth();
}
ExecutionGuard ExecutionGuard::Enter(const CallbackTicket& ticket) noexcept {
    ExecutionGuard guard;
    guard.state_=ticket.state.load(std::memory_order_acquire);
    if (!guard.state_) return guard;
    guard.state_->outstanding.fetch_add(1,std::memory_order_seq_cst);
    guard.depthTracked_=EnterDepth();
    // Depth tracking authorizes physical retirement, not feature admission.
    // A host can exhaust the fixed TLS slots before this DLL loads. Keep its
    // explicitly enabled callbacks working; OutsideCallback() still prevents
    // disabling/reclaiming their code when depth tracking is unavailable.
    guard.admitted_=guard.state_->admitted.load(std::memory_order_acquire);
    return guard;
}
bool Registry::Matches(const Record& r,const std::vector<unsigned char>& expected) const {
    std::vector<unsigned char> actual;
    return !expected.empty() && backend_.read && backend_.read(r.spec.rangeStart,expected.size(),actual) && actual==expected;
}
bool Registry::ProofCurrent(const RetirementProof& p) const {
    return p.safeContinuation && p.continuationReceipt && p.registryEpoch==epoch_ && OutsideCallback();
}
Outcome Registry::PrepareExternal(const TargetSpec& s,const Practice::PatchDescriptor& patch,
                                 Practice::PatchLedger& ledger,CallbackTicket& ticket,
                                 std::shared_ptr<void> continuationPin) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!backend_.read)return {Result::MissingBackend};
    if(!s.target || !s.detour || s.owner.empty() || s.abi.empty() || !s.modulePin ||
       !continuationPin || !s.moduleIncarnation || !patch.module.base || !patch.owner ||
       patch.module.incarnation!=s.moduleIncarnation || patch.address!=s.rangeStart ||
       reinterpret_cast<uintptr_t>(s.target)!=s.rangeStart || patch.expected!=s.preimage ||
       patch.expected.empty() || patch.expected.size()!=patch.replacement.size() ||
       s.rangeStart+s.preimage.size()<s.rangeStart)return {Result::Conflict};
    for(auto& r:records_) {
        if(r.spec.target==s.target) {
            if(r.externalLedger!=&ledger || r.ticket!=&ticket || r.spec.owner!=s.owner ||
               r.spec.abi!=s.abi || r.spec.detour!=s.detour || r.spec.moduleIncarnation!=s.moduleIncarnation ||
               r.externalPatch.owner!=patch.owner || r.externalPatch.module.base!=patch.module.base ||
               r.externalPatch.expected!=patch.expected || r.externalPatch.replacement!=patch.replacement)
                return {Result::Conflict,1};
            if(r.state==State::ExternalInstalling)return {Result::Pending,1};
            return Matches(r,r.state==State::Enabled?r.installed:r.spec.preimage)?Outcome{}:Outcome{Result::ForeignBytes,1};
        }
        if((s.rangeStart<r.spec.rangeStart+r.spec.preimage.size() &&
            r.spec.rangeStart<s.rangeStart+s.preimage.size()) ||
           (r.externalLedger==&ledger && r.externalPatch.owner==patch.owner))return {Result::Conflict};
    }
    if(ticket.state.load() || ledger.ObligationsForOwner(patch.owner) ||
       epoch_==std::numeric_limits<uint64_t>::max())return {Result::Conflict};
    Record prepared;prepared.spec=s;prepared.externalPatch=patch;
    prepared.installed=patch.replacement;prepared.externalLedger=&ledger;
    prepared.continuationPin=std::move(continuationPin);prepared.ticket=&ticket;
    if(!Matches(prepared,s.preimage))return {Result::ForeignBytes};
    // Durable executable metadata and callback state exist before the ledger
    // can touch even one byte. A partial install retains both ownership kinds.
    records_.push_back(std::move(prepared));
    ticket.state.store(records_.back().callbacks.get(),std::memory_order_release);
    ++epoch_;
    return {};
}
Outcome Registry::Acquire(const TargetSpec& s,void** original){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!backend_.create || !backend_.read) return {Result::MissingBackend};
    if(!s.target || !s.detour || s.owner.empty() || s.abi.empty() || !s.moduleIncarnation || !s.modulePin || s.preimage.empty()) return {Result::Conflict};
    for(auto& r:records_) if(r.spec.target==s.target){
        if(r.externalLedger)return {Result::Conflict};
        if(r.spec.owner!=s.owner || r.spec.abi!=s.abi || r.spec.detour!=s.detour || r.spec.moduleIncarnation!=s.moduleIncarnation || r.spec.rangeStart!=s.rangeStart || r.spec.preimage!=s.preimage) return {Result::Conflict};
        if(!Matches(r,r.state==State::Enabled?r.installed:r.spec.preimage)) return {Result::ForeignBytes};
        if (original && *original!=r.original) {
            // Never rewrite a slot visible to an enabled/bypassed native call.
            // A missing slot may only be recovered before its first enable.
            if (*original || r.state!=State::CreatedDisabled || r.admissionOpen ||
                r.callbacks->outstanding.load(std::memory_order_seq_cst))
                return {Result::Pending,1};
            *original=r.original;
        }
        return {};
    }
    for(const auto& r:records_)if(s.rangeStart<r.spec.rangeStart+r.spec.preimage.size() &&
        r.spec.rangeStart<s.rangeStart+s.preimage.size())return {Result::Conflict};
    if(epoch_==std::numeric_limits<uint64_t>::max()) return {Result::Conflict};
    Record r;r.spec=s;
    r.installed.reserve(s.preimage.size());
    if(!Matches(r,s.preimage)) return {Result::ForeignBytes};
    // Allocate every registry-owned object BEFORE creating the physical hook.
    records_.push_back(std::move(r));auto& owned=records_.back();
    owned.createStatus=backend_.create(s.target,s.detour,&owned.original);
    const int status=owned.createStatus;
    if(status!=backend_.ok){records_.pop_back();return {status==backend_.alreadyCreated?Result::Conflict:Result::BackendFailure,0,status};}
    ++epoch_;
    if(original) *original=owned.original;
    return {};
}
Outcome Registry::Enable(void* target){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!backend_.read) return {Result::MissingBackend};
    for(auto& r:records_) if(r.spec.target==target){
        if(r.state==State::Enabled){if(!Matches(r,r.installed))return {Result::ForeignBytes,1};if(!r.admissionOpen){if(epoch_==std::numeric_limits<uint64_t>::max())return {Result::Conflict,1};++epoch_;}r.admissionOpen=true;r.callbacks->admitted.store(true,std::memory_order_release);return {};}
        if(r.state==State::ExternalInstalling)return {Result::Pending,1};
        if(!Matches(r,r.spec.preimage)) return {Result::ForeignBytes,1};
        if(epoch_==std::numeric_limits<uint64_t>::max()) return {Result::Conflict,1};
        if(r.externalLedger) {
            r.state=State::ExternalInstalling;++epoch_;
            const auto installed=r.externalLedger->Acquire(r.externalPatch,r.externalSiteId);
            if(!installed.complete)return {Result::BackendFailure,1,static_cast<int>(installed.error)};
            r.state=State::Enabled;
            r.admissionOpen=true;r.callbacks->admitted.store(true,std::memory_order_release);
            return {};
        }
        if(!backend_.enable)return {Result::MissingBackend,1};
        r.enableStatus=backend_.enable(target);
        if(r.enableStatus!=backend_.ok && r.enableStatus!=backend_.alreadyEnabled)return {Result::BackendFailure,1,r.enableStatus};
        // Record executable reachability before any subsequent checked read fails.
        r.state=State::Enabled;++epoch_;
        if(!backend_.read(r.spec.rangeStart,r.spec.preimage.size(),r.installed)) return {Result::ForeignBytes,1};
        r.admissionOpen=true;r.callbacks->admitted.store(true,std::memory_order_release);return {};
    }
    return {Result::Conflict};
}
Outcome Registry::CloseAdmission(const std::string& owner){
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& r:records_) if(owner.empty()||r.spec.owner==owner){r.admissionOpen=false;r.callbacks->admitted.store(false,std::memory_order_release);}
    return {};
}
Outcome Registry::DisableOwnedTargets(const std::string& owner,const RetirementProof& p){
    std::lock_guard<std::mutex> lock(mutex_);
    Outcome result;
    std::vector<Record*> selected;
    for(auto& r:records_)if((owner.empty()||r.spec.owner==owner)&&
        (r.state==State::Enabled||r.state==State::ExternalInstalling))selected.push_back(&r);
    if(selected.empty()) return result;
    if(!ProofCurrent(p))return {Result::Pending,selected.size()};
    if(!backend_.read)return {Result::MissingBackend,selected.size()};
    std::vector<Record*> eligible;
    for(auto* r:selected){
        if(r->admissionOpen){result={Result::Pending, result.remaining+1};continue;}
        if(r->externalLedger) {
            const auto restored=r->externalLedger->RestoreOwner(r->externalPatch.owner);
            if(restored.complete && !r->externalLedger->ObligationsForOwner(r->externalPatch.owner) && Matches(*r,r->spec.preimage))
                r->state=State::DisabledAwaitingDrain;
            else result={Result::BackendFailure,result.remaining+1,static_cast<int>(restored.error)};
            continue;
        }
        if(!backend_.disable){result={Result::MissingBackend,result.remaining+1};continue;}
        if(!Matches(*r,r->installed)){result={Result::ForeignBytes,result.remaining+1};continue;}
        eligible.push_back(r);
    }
    const bool queued=backend_.queueDisable && backend_.applyQueued;
    if(queued){
        bool any=false;
        for(auto* r:eligible){r->queueStatus=backend_.queueDisable(r->spec.target);any|=r->queueStatus==backend_.ok;}
        if(any){const int status=backend_.applyQueued();for(auto* r:eligible)r->applyStatus=status;}
    }
    for(auto* r:eligible){
        // The queue may partially succeed. Verify each physical image independently;
        // individual disable also reconciles MinHook's own state, retaining all statuses.
        if(!Matches(*r,r->installed)&&!Matches(*r,r->spec.preimage)){result={Result::ForeignBytes,result.remaining+1};continue;}
        r->disableStatus=backend_.disable(r->spec.target);
        if((r->disableStatus==backend_.ok || r->disableStatus==backend_.alreadyDisabled)&&Matches(*r,r->spec.preimage))r->state=State::DisabledAwaitingDrain;
        else result={Result::BackendFailure,result.remaining+1,r->disableStatus};
    }
    return result;
}
Outcome Registry::ReclaimDrainedTargets(const std::string& owner,const RetirementProof& p){
    std::lock_guard<std::mutex> lock(mutex_);
    Outcome result;
    for(auto it=records_.begin();it!=records_.end();){
        auto& r=*it;if(!owner.empty()&&r.spec.owner!=owner){++it;continue;}
        if(!ProofCurrent(p)||!p.nativeEntriesUnreachable||!p.foreignChainsDrained||r.admissionOpen||r.state==State::Enabled||r.state==State::ExternalInstalling||r.callbacks->outstanding.load(std::memory_order_seq_cst)){
            result={Result::Pending,result.remaining+1};++it;continue;
        }
        if(r.externalLedger && r.externalLedger->ObligationsForOwner(r.externalPatch.owner)){
            result={Result::Pending,result.remaining+1};++it;continue;
        }
        if(!r.externalLedger && !backend_.remove){result={Result::MissingBackend,result.remaining+1};++it;continue;}
        if(!Matches(r,r.spec.preimage)){result={Result::ForeignBytes,result.remaining+1};++it;continue;}
        r.state=State::Reclaimable;
        r.removeStatus=r.externalLedger?backend_.ok:backend_.remove(r.spec.target);
        if(r.removeStatus==backend_.ok){if(r.ticket)r.ticket->state.store(nullptr,std::memory_order_release);it=records_.erase(it);}
        else{result={Result::BackendFailure,result.remaining+1,r.removeStatus};++it;}
    }
    return result;
}
Outcome Registry::BindCallback(void* target, CallbackTicket& ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& record:records_) {
        if (record.spec.target!=target) continue;
        if (record.ticket==&ticket) return {};
        // Binding is only possible before the first enable. Existing callbacks
        // must never observe a changed ticket or a replacement record.
        if (record.state!=State::CreatedDisabled || record.ticket ||
            ticket.state.load(std::memory_order_acquire)) return {Result::Conflict,1};
        record.ticket=&ticket;
        ticket.state.store(record.callbacks.get(),std::memory_order_release);
        return {};
    }
    return {Result::Conflict};
}
std::vector<Record> Registry::Snapshot() const {std::lock_guard<std::mutex> lock(mutex_);return records_;}
uint64_t Registry::Epoch() const {std::lock_guard<std::mutex> lock(mutex_);return epoch_;}
}
