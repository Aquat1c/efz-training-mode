#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <memory>
#include <vector>

namespace Practice {

class NativePatchModuleLease;
struct PatchModule {
    uintptr_t base;
    uint64_t incarnation;
    std::shared_ptr<const NativePatchModuleLease> lifetime;
};
// Acquire this lifetime token BEFORE qualifying a DLL's bytes/hash. A copied
// descriptor retains it, so qualification and acquisition cannot straddle an
// unload/reload. The native backend rejects raw base/incarnation-only records.
bool CaptureNativePatchModule(uintptr_t base, PatchModule& module);
struct PatchRegion {
    uintptr_t address;
    size_t size;
    uint32_t protection;
    uintptr_t allocationBase;
};
struct PatchDescriptor {
    PatchModule module;
    uintptr_t address;
    std::vector<uint8_t> expected;
    std::vector<uint8_t> replacement;
    uint64_t owner;
};
enum class PatchError {
    None, InvalidDescriptor, PinFailed, ModuleChanged, ForeignBytes,
    RegionUnavailable, ProtectionFailed, WriteFailed, FlushFailed,
    OwnerConflict, PendingRestoration
};
struct PatchResult {
    bool complete;
    uint32_t obligations;
    PatchError error;
};

// Only the OS memory/module boundary is replaceable by a test. The ledger,
// rollback decisions and retained obligations are the production component.
class PatchBackend {
public:
    virtual ~PatchBackend() = default;
    virtual bool Pin(const PatchModule&, uintptr_t& pin) noexcept = 0;
    virtual void Unpin(uintptr_t pin) noexcept = 0;
    virtual bool SameModule(const PatchModule&, uintptr_t pin) noexcept = 0;
    virtual bool Read(uintptr_t, void*, size_t) noexcept = 0;
    // Reports exact completed byte stores, including on a partial access fault.
    virtual size_t Write(uintptr_t, const void*, size_t) noexcept = 0;
    virtual bool QueryRegion(uintptr_t, PatchRegion&) noexcept = 0;
    virtual bool Protect(const PatchRegion&, uint32_t, uint32_t& previous) noexcept = 0;
    virtual bool Flush(uintptr_t, size_t) noexcept = 0;
};

// Call only at an owner's native-safe mutation continuation, after profile
// qualification. Serialization here coordinates our writers and shared pages;
// it does not stop native execution or make arbitrary preimages authorized.
// Backend and ledger must outlive any outstanding sites. Destruction does not
// silently restore bytes or release pins for unresolved obligations.
class PatchLedger {
public:
    explicit PatchLedger(PatchBackend& backend) : backend_(backend) {}
    PatchResult Acquire(const PatchDescriptor& descriptor, uint64_t& siteId);
    PatchResult RestoreOwner(uint64_t owner);
    uint32_t Obligations() const;
    uint32_t ObligationsForOwner(uint64_t owner) const;

private:
    struct Page {
        PatchRegion original;
        uint32_t references = 0;
        bool protectionPending = false;
    };
    struct Site {
        uint64_t id;
        PatchDescriptor descriptor;
        uintptr_t pin = 0;
        std::vector<uint8_t> current;
        std::vector<uint8_t> readBuffer;
        std::vector<uintptr_t> pages;
        bool flushPending = false;
        bool retiring = false;
        PatchError error = PatchError::None;
    };
    bool PreparePages(Site& site);
    bool MakeWritable(Site& site);
    bool RestoreProtections(Site& site);
    bool FlushPending(Site& site);
    bool RestoreSite(Site& site);
    void ReleaseSite(Site& site);
    bool MatchesCurrent(Site& site);
    uint32_t CountOwner(uint64_t owner) const;
    PatchBackend& backend_;
    mutable std::mutex mutex_;
    std::map<uintptr_t, Page> pages_;
    std::vector<Site> sites_;
    uint64_t nextId_ = 0;
};

PatchLedger& NativePatchLedger();
PatchBackend& NativePatchMemoryBackend();

} // namespace Practice
