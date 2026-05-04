#pragma once

#include <cstdint>

enum class GameMode : uint8_t;
enum class GamePhase : uint8_t;

namespace CharacterHotswap {

bool QueueReload(int p1CharId, int p2CharId, int stageId, unsigned short bgmTrack);
void Tick(GamePhase currentPhase, GameMode currentMode);
bool IsBusy();
bool CanQueueReload();
const char* GetActionValueText();

} // namespace CharacterHotswap