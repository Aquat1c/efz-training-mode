#pragma once

#include <cstdint>

enum class GameMode : uint8_t;
enum class GamePhase : uint8_t;

namespace CharacterHotswap {

inline constexpr int kPaletteSlotCount = 6;

struct PaletteSelection {
	int p1Color = 0;
	int p2Color = 0;
	bool p1UseCustomPalette = false;
	bool p2UseCustomPalette = false;
};

bool QueueReload(int p1CharId, int p2CharId, int stageId, unsigned short bgmTrack);
bool QueueReload(int p1CharId, int p2CharId, int stageId, const PaletteSelection& paletteSelection, unsigned short bgmTrack);
void Tick(GamePhase currentPhase, GameMode currentMode);
bool IsBusy();
bool CanQueueReload();
const char* GetActionValueText();
const char* GetDisplayNameForSelectId(int selectId);
const char* GetResourceNameForSelectId(int selectId);
bool ReadCurrentPaletteSelection(PaletteSelection& outSelection);
bool HasCustomPaletteFile(int selectId, int paletteIndex);
void SanitizePaletteSelection(int p1CharId, int p2CharId, PaletteSelection& selection);
// Drops the cached results of probing on-disk custom .pal files. Probes are
// cheap individually but add up over a session; the cache is process-lifetime
// and only needs to be flushed when the user adds/removes palette files.
void InvalidateCustomPaletteCache();

} // namespace CharacterHotswap