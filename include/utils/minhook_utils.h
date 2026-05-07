#pragma once

#include "../../3rdparty/minhook/include/MinHook.h"

namespace MinHookUtils {

bool CreateHook(void* target,
                void* detour,
                void** original,
                const char* category,
                const char* label,
                bool* alreadyCreated = nullptr);

bool EnableHook(void* target,
                const char* category,
                const char* label,
                bool* alreadyEnabled = nullptr);

bool DisableHook(void* target,
                 const char* category,
                 const char* label,
                 bool* alreadyDisabled = nullptr);

bool RemoveHook(void* target,
                const char* category,
                const char* label,
                bool* wasMissing = nullptr);

bool CreateAndEnableHook(void* target,
                         void* detour,
                         void** original,
                         const char* category,
                         const char* label,
                         bool* alreadyCreated = nullptr,
                         bool* alreadyEnabled = nullptr);

}