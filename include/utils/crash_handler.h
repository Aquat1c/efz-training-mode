#pragma once

#include <windows.h>

namespace CrashHandler {

void Install(HMODULE selfModule);
void WarmupSymbolMaps();

} // namespace CrashHandler