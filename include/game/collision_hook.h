#pragma once
#include <cstdint>
#include <cstddef>

#include "mission/contact_event.h"

// Installs a hook on the game's handlePlayerToPlayerCollision function to capture
// the live attackerFrameData pointer each time collisions are processed.
void InstallCollisionHook();
void SetCollisionHookActive(bool active);
void RemoveCollisionHook();

// Returns the last seen attackerFrameData pointer for the given player (1 or 2).
// May return 0 if not yet observed.
uintptr_t GetCachedAttackDataForPlayer(int playerNum);

// Returns the discovered offset (in bytes) from the player base pointer to the
// field that holds the current attack data pointer, or -1 if not yet found.
int GetAttackDataOffsetForPlayer(int playerNum);

// Clears session-scoped cached frame-data pointers captured by the collision hook.
// Discovered structural offsets are intentionally preserved.
void ResetCollisionHookSessionCaches(const char* reason);

// Strict direct player-contact event source. Events are published from inside
// EFZ's 0x767F60 resolver, not inferred from a later monitor sample. Entity
// contacts use the separately profile-validated 0x7697D0 resolver producer.
bool IsDirectContactHookReady();
bool IsEntityContactHookReady();
uint32_t GetContactEventEpoch();
uint32_t GetContactEventWatermark();
void ResetContactEventJournal(const char* reason);
Mission::Contact::ReadResult ReadCommittedContactEvents(
    uint32_t afterSequence, uint32_t expectedEpoch, uint32_t closedBatch,
    Mission::Contact::Event* out, size_t outCapacity);
