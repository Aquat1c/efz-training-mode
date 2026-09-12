#pragma once

#include "patch_ledger.h"

namespace Practice {

// Control-plane qualification of the loaded main image against the admitted
// Memorial executable's file hash and mapped PE identity. Result is immutable
// for process lifetime; native callbacks must use their captured context.
bool GetQualifiedMemorialImage(PatchModule& module);

} // namespace Practice
