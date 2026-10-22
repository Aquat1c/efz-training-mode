#pragma once
#include <cstdint>
#include <cstddef>

#include "mission/contact_event.h"

// Hooks the game's handlePlayerToPlayerCollision to capture the live
// attackerFrameData pointer each collision pass.
void InstallCollisionHook();
void SetCollisionHookActive(bool active);
void RemoveCollisionHook();

// Last seen attackerFrameData pointer for the given player (1 or 2); 0 if not
// yet observed.
uintptr_t GetCachedAttackDataForPlayer(int playerNum);

// Discovered byte offset from the player base pointer to the field holding the
// current attack data pointer, or -1 if not yet found.
int GetAttackDataOffsetForPlayer(int playerNum);
enum class CollisionLayoutStatus : uint8_t {NotAvailableYet,UnsupportedLayout,Ready};
CollisionLayoutStatus GetCollisionLayoutStatusForPlayer(int playerNum);

// Clears session-scoped cached frame-data pointers; structural discovery is
// retried from a new captured world/resource.
void ResetCollisionHookSessionCaches(const char* reason);

// Direct player-contact events are published from inside EFZ's 0x767F60
// resolver (entity contacts: the 0x7697D0 resolver), not inferred from a later
// monitor sample.
bool IsDirectContactHookReady();
bool IsEntityContactHookReady();
uint32_t GetContactEventEpoch();
uint32_t GetContactEventWatermark();
void ResetContactEventJournal(const char* reason);
Mission::Contact::ReadResult ReadCommittedContactEvents(
    uint32_t afterSequence, uint32_t expectedEpoch, uint32_t closedBatch,
    Mission::Contact::Event* out, size_t outCapacity);
