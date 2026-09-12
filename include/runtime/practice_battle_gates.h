#pragma once
#include "hook_registry.h"
#include "practice_contract.h"

namespace Practice {
enum class BattleCleanupBranch : uint32_t { FadeToSelector=0, FadeToResult=1 };
struct BattleGateBinding {
    PracticeHooks::CallbackTicket firstTicket;
    PracticeHooks::CallbackTicket secondTicket;
    // Called under this exact physical gate's execution guard, even when its
    // ordinary admission has closed. The native old-world hold is independent.
    // READY alone permits native destruction. Every other exact ABI result
    // retains the old branch through the enclosing BattleLogic epilogue.
    uint32_t (__cdecl* beforeCleanup)(void*,BattleCleanupBranch)=nullptr;
    uintptr_t firstContinuation=0;
    uintptr_t secondContinuation=0;
    uintptr_t epilogue=0;
};
BattleGateBinding& BattleGates();
bool PrepareBattleCleanupSites();
bool BattleCleanupHeld(void* battle) noexcept;
void BattleCleanupReturned(void* battle) noexcept;
}
extern "C" {
void PracticeBattleCleanupGateA();
void PracticeBattleCleanupGateB();
unsigned char __fastcall PracticeResumeBattleCleanupA(void* battle,void*);
unsigned char __fastcall PracticeResumeBattleCleanupB(void* battle,void*);
}
