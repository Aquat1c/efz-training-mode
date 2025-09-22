#pragma once
#include <cstdint>

namespace SwitchPlayers {
    // Returns true if swap applied successfully
    bool ToggleLocalSide();
    // Force set local side to 0 (P1) or 1 (P2)
    bool SetLocalSide(int sideIdx);
}
