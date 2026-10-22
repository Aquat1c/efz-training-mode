#pragma once

#include "per_frame_sample.h"

namespace ComboOverlay {
    void Tick(const PerFrameSample& sample);
    void ClearDisplay();
    void ResetState(const char* reason);
}
