#pragma once

#include <cstdint>
#include "runtime/practice_contract.h"

enum class GameMode : uint8_t;
enum class GamePhase : uint8_t;

namespace CharacterHotswap {

inline constexpr int kPaletteSlotCount = 6;
inline constexpr int kCharacterSelectCount = 24;

struct PaletteSelection {
	int p1Color = 0;
	int p2Color = 0;
	bool p1UseCustomPalette = false;
	bool p2UseCustomPalette = false;
};

// Character arguments are character-select IDs, not CharacterSettings/CHAR_ID values.
bool QueueReload(int p1SelectId, int p2SelectId, int stageId, int bgmTrack);
bool QueueReload(int p1SelectId, int p2SelectId, int stageId,
                 const PaletteSelection& paletteSelection,
                 int bgmTrack); // -1 keeps native stage music

// Mission/tutorial title launches route Practice through Loading without
// Character Select (like replay playback). Best-effort; callers must fall back
// to QueueReload at Character Select when it is unavailable or the loading
// context is not in a safe, empty state.
bool InstallDirectPracticeBootstrap();
void UninstallDirectPracticeBootstrap();
bool QueueDirectPracticeLoad(int p1SelectId, int p2SelectId, int stageId,
                             const PaletteSelection& paletteSelection,
                             int bgmTrack); // -1 keeps native stage music
// True for the full owned Title/Match -> Loading -> Battle transaction, not
// merely until the first Loading-update callback consumes the request.
bool IsDirectPracticeLoadPending();
void CancelDirectPracticeLoad(const char* reason);

// One-shot receipt for the destination Practice session's new Match; accepts
// the prepared fighters/stage/palettes atomically.
bool ConsumeCompletedPracticeLoad(int p1SelectId, int p2SelectId, int stageId,
                                  const PaletteSelection& paletteSelection,
                                  int bgmTrack);

// A receipt is valid only for the destination session that produced it;
// match/session teardown must invalidate it immediately so an older tuple can
// never authorize setup in a later Match.
void InvalidateCompletedPracticeLoadReceipt();

uint32_t CaptureLoadingRequest(const EfzTmIdentityV1& identity);
void ConsumeLoadingRequest(const EfzTmIdentityV1& identity,uint32_t ticket,uint32_t nativeResult,uint32_t acceptedResult);

void OnBattleFrontendEntry(uintptr_t battleContext);
void OnSelectorReady(uintptr_t selectorContext);
uint32_t CaptureInitializedRequest(const EfzTmIdentityV1&);
void OnNativeFrontendInstalled();
void OnBattleInitialized(const EfzTmIdentityV1&,uint32_t ticket,uintptr_t battleContext,uintptr_t gameSystem);
void Tick(GamePhase currentPhase, GameMode currentMode);
bool IsBusy();
bool CanQueueReload();
const char* GetActionValueText();
const char* GetDisplayNameForSelectId(int selectId);
const char* GetResourceNameForSelectId(int selectId);
// Maps a mission/resource short name (e.g. "nagamori") to a character-select
// ID; -1 when unrecognized.
int GetSelectIdForResourceName(const char* resourceName);
bool ReadCurrentPaletteSelection(PaletteSelection& outSelection);
bool HasCustomPaletteFile(int selectId, int paletteIndex);
void SanitizePaletteSelection(int p1SelectId, int p2SelectId, PaletteSelection& selection);
// The .pal probe cache is process-lifetime; flush only when palette files
// are added or removed.
void InvalidateCustomPaletteCache();

} // namespace CharacterHotswap
