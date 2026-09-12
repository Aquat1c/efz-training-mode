#pragma once

#include "runtime/patch_ledger.h"

namespace FinalMemory {
constexpr uint64_t kPatchOwner = 0x464D0001u;

// Owns only acquisitions recorded by this feature. Empty restore/status never
// invokes discovery; a partial group is rolled back and remains retryable.
class PatchOwner {
public:
    explicit PatchOwner(Practice::PatchLedger& ledger) : ledger_(ledger) {}
    int Apply(const std::vector<Practice::PatchDescriptor>& descriptors) {
        if (active_) return 0;
        if (ledger_.ObligationsForOwner(kPatchOwner)) return 0;
        if (descriptors.empty()) return 0;
        try {
            for (const auto& descriptor : descriptors) {
                if (descriptor.owner != kPatchOwner) {
                    ledger_.RestoreOwner(kPatchOwner);
                    return 0;
                }
                uint64_t site = 0;
                if (!ledger_.Acquire(descriptor, site).complete) {
                    ledger_.RestoreOwner(kPatchOwner);
                    return 0;
                }
            }
        } catch (...) {
            // Acquire allocates before mutations, but earlier group members
            // may already be installed. Roll those back on allocation failure
            // as well as an explicit backend failure. Restore uses preallocated
            // read buffers; any failed native restoration remains in the ledger.
            ledger_.RestoreOwner(kPatchOwner);
            return 0;
        }
        active_ = true;
        return static_cast<int>(descriptors.size());
    }
    int Restore() {
        active_ = false;
        const auto before = ledger_.ObligationsForOwner(kPatchOwner);
        const auto result = ledger_.RestoreOwner(kPatchOwner);
        return static_cast<int>(before - result.obligations);
    }
    bool HasObligations() const { return ledger_.ObligationsForOwner(kPatchOwner) != 0; }
private:
    Practice::PatchLedger& ledger_;
    bool active_ = false;
};
} // namespace FinalMemory
