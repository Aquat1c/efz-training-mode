#pragma once

#include <cstdint>

#include "../input/auto_action_motion_transaction.h"

namespace KaoriRecoilDuck {

// Starts Kaori's native two-command 44~66 recipe.  Admission reserves this
// fighter's mod-owned input lane, submits native 44, and returns that primary
// recipe generation so an ordinary delayed trigger can retain its exact
// selected row until the complete 44~66 sequence reaches move 251.
AutoActionMotionSubmitResult Begin(int playerNum, int characterId,
                                   uint64_t* recipeGenerationOut = nullptr);

// Recipe-level result. Pending spans both native command transactions;
// Accepted is published only after EFZ enters exact move 251.
AutoActionMotionOutcome GetOutcome(int playerNum, uint64_t recipeGeneration);

// Called once per internal battle update (and from the 64-Hz fallback when
// tick-integrated auto actions are disabled).  It submits native 66 only while
// backdash 164 is in EFZ's verified frame-index 4/5 window.
void Tick();

bool IsActive(int playerNum);
void Cancel(int playerNum, const char* reason);
void CancelAll(const char* reason);

} // namespace KaoriRecoilDuck
