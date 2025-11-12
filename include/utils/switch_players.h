#pragma once
#include <cstdint>

namespace SwitchPlayers {
    // Returns true if swap applied successfully
    bool ToggleLocalSide();
    // Force set local side to 0 (P1) or 1 (P2)
    bool SetLocalSide(int sideIdx);
    // Reset menu/control mapping to defaults for Character Select and menus:
    // - For EfzRevival: set Practice local=0 (P1), remote=1, align GUI_POS, refresh mapping block
    // - For vanilla: disable swapped routing (P1 controls -> P1)
    // Does NOT touch engine CPU flags (+4931/+4932) or active player (+4930)
    bool ResetControlMappingForMenusToP1();
}
