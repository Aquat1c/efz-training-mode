#pragma once
#include <cstdint>

// Control-plane ABI. Modules retain the table's provider for their callback lifetime.
// Every callback and direct transform uses this same recursive boundary. No IO
// may run while held. Negotiation commits only after the previous transform drains.
constexpr uint32_t EfzAudioLaneBgm = 1, EfzAudioLaneSe = 2, EfzAudioAllLanes = 3;
struct EfzAudioOwnerV1 {
    uint32_t size = sizeof(EfzAudioOwnerV1);
    uint32_t version = 1;
    uint64_t providerIncarnation = 0;
    uint32_t ownedLaneMask = 0;
    uint32_t inputsAreRawLaneMask = EfzAudioAllLanes;
    uint64_t acknowledgementGeneration = 0;
};
using EfzAudioOwnerCommitV1 = void (__cdecl*)(const EfzAudioOwnerV1*, void*);
struct EfzAudioOwnerApiV1 {
    uint32_t size;
    uint32_t version;
    uint64_t providerIncarnation;
    void (__cdecl* enter)();
    void (__cdecl* leave)();
    bool (__cdecl* query)(EfzAudioOwnerV1*);
    // Expected incarnation+generation must match exactly. The provider retires
    // requested lanes, acknowledges, and calls commit before unlocking.
    bool (__cdecl* retire)(const EfzAudioOwnerV1*, uint32_t lanes,
                          EfzAudioOwnerCommitV1 commit, void* context);
};
using EfzGetAudioOwnerApiV1 = const EfzAudioOwnerApiV1* (__cdecl*)();
