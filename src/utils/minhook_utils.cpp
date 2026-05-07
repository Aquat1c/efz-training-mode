#include "../include/utils/minhook_utils.h"

#include "../include/core/logger.h"
#include "../include/utils/utilities.h"

#include <cstdio>
#include <string>

namespace {

std::string FormatHookAddress(void* target) {
    char buffer[32] = {};
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "0x%08IX", reinterpret_cast<uintptr_t>(target));
    return std::string(buffer);
}

void LogMinHookFailure(const char* action, void* target, const char* category, const char* label, MH_STATUS status) {
    const char* statusText = MH_StatusToString(status);
    std::string message = std::string(category ? category : "[HOOK]")
        + " Failed to "
        + (action ? action : "process")
        + " "
        + (label ? label : "hook")
        + " at "
        + FormatHookAddress(target)
        + " status="
        + (statusText ? statusText : "<unknown>");
    LogOut(message, true);
}

}

namespace MinHookUtils {

bool CreateHook(void* target,
                void* detour,
                void** original,
                const char* category,
                const char* label,
                bool* alreadyCreated) {
    if (alreadyCreated) {
        *alreadyCreated = false;
    }
    if (!target) {
        LogOut(std::string(category ? category : "[HOOK]") + " Cannot create " + (label ? label : "hook") + ": null target", true);
        return false;
    }

    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) {
        return true;
    }
    if (status == MH_ERROR_ALREADY_CREATED) {
        if (alreadyCreated) {
            *alreadyCreated = true;
        }
        return true;
    }

    LogMinHookFailure("create", target, category, label, status);
    return false;
}

bool EnableHook(void* target,
                const char* category,
                const char* label,
                bool* alreadyEnabled) {
    if (alreadyEnabled) {
        *alreadyEnabled = false;
    }
    if (!target) {
        LogOut(std::string(category ? category : "[HOOK]") + " Cannot enable " + (label ? label : "hook") + ": null target", true);
        return false;
    }

    const MH_STATUS status = MH_EnableHook(target);
    if (status == MH_OK) {
        return true;
    }
    if (status == MH_ERROR_ENABLED) {
        if (alreadyEnabled) {
            *alreadyEnabled = true;
        }
        return true;
    }

    LogMinHookFailure("enable", target, category, label, status);
    return false;
}

bool DisableHook(void* target,
                 const char* category,
                 const char* label,
                 bool* alreadyDisabled) {
    if (alreadyDisabled) {
        *alreadyDisabled = false;
    }
    if (!target) {
        LogOut(std::string(category ? category : "[HOOK]") + " Cannot disable " + (label ? label : "hook") + ": null target", true);
        return false;
    }

    const MH_STATUS status = MH_DisableHook(target);
    if (status == MH_OK) {
        return true;
    }
    if (status == MH_ERROR_DISABLED) {
        if (alreadyDisabled) {
            *alreadyDisabled = true;
        }
        return true;
    }

    LogMinHookFailure("disable", target, category, label, status);
    return false;
}

bool RemoveHook(void* target,
                const char* category,
                const char* label,
                bool* wasMissing) {
    if (wasMissing) {
        *wasMissing = false;
    }
    if (!target) {
        return true;
    }

    const MH_STATUS status = MH_RemoveHook(target);
    if (status == MH_OK) {
        return true;
    }
    if (status == MH_ERROR_NOT_CREATED) {
        if (wasMissing) {
            *wasMissing = true;
        }
        return true;
    }

    LogMinHookFailure("remove", target, category, label, status);
    return false;
}

bool CreateAndEnableHook(void* target,
                         void* detour,
                         void** original,
                         const char* category,
                         const char* label,
                         bool* alreadyCreated,
                         bool* alreadyEnabled) {
    if (!CreateHook(target, detour, original, category, label, alreadyCreated)) {
        return false;
    }
    return EnableHook(target, category, label, alreadyEnabled);
}

}