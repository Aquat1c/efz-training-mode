// Revival DLL memory introspection and session field manipulation.

#include "netplay/bridge/takeover_internal.h"
#include "netplay/bridge/desync_monitor.h"
#include "netplay/bridge/frontend_return.h"
#include "netplay/bridge/gameplay_exit_recovery.h"
#include "netplay/core/mod_settings.h"
#include "crash_handler.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <float.h>
#include <intrin.h>
#include <xmmintrin.h>

#include <windows.h>
#include <psapi.h>  // For PROCESS_MEMORY_COUNTERS type only; psapi.dll loaded at runtime

namespace netplay::bridge::takeover
{

static bool IsRevival102jProfile();

bool IsReadableRange(const void* address, size_t size)
{
    if (address == nullptr || size == 0)
    {
        return false;
    }

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + size;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT)
        {
            return false;
        }

        const DWORD rawProt = mbi.Protect;
        if ((rawProt & PAGE_GUARD) != 0 || (rawProt & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD prot = rawProt & 0xFFu;
        const bool readable =
            prot == PAGE_READONLY ||
            prot == PAGE_READWRITE ||
            prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE ||
            prot == PAGE_EXECUTE_WRITECOPY;
        if (!readable)
        {
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }
        cursor = regionEnd;
    }

    return true;
}

bool IsWritableRange(void* address, size_t size)
{
    if (address == nullptr || size == 0)
    {
        return false;
    }

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + size;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT)
        {
            return false;
        }

        const DWORD rawProt = mbi.Protect;
        if ((rawProt & PAGE_GUARD) != 0 || (rawProt & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD prot = rawProt & 0xFFu;
        const bool writable =
            prot == PAGE_READWRITE ||
            prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READWRITE ||
            prot == PAGE_EXECUTE_WRITECOPY;
        if (!writable)
        {
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }
        cursor = regionEnd;
    }

    return true;
}

bool SafeReadInt(const void* address, int* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(int)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const int*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool SafeReadPtr(const void* address, uintptr_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uintptr_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uintptr_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool SafeReadByte(const void* address, uint8_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uint8_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

static bool SafeReadWord(const void* address, uint16_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uint16_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uint16_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

static bool SafeReadDouble(const void* address, double* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(double)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const double*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

static bool SafeReadDword(const void* address, uint32_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uint32_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

static bool BridgePatchVerboseLoggingEnabled()
{
    return netplay::mod_settings::IsVerboseBridgePatchLoggingEnabled();
}

static bool SyncDiagnosticsEnabled()
{
    return netplay::mod_settings::IsVerboseSyncDiagnosticsEnabled();
}

static bool DetailedDiagnosticReportsEnabled()
{
    return netplay::mod_settings::AreAllVerboseLogsEnabled();
}

static void LogBytesIfVerbose(const char* context, uintptr_t address, size_t size)
{
    if (!BridgePatchVerboseLoggingEnabled())
    {
        return;
    }

    static constexpr size_t kMaxBytes = 32;
    const size_t count = size < kMaxBytes ? size : kMaxBytes;
    uint8_t bytes[kMaxBytes] = {};
    for (size_t i = 0; i < count; ++i)
    {
        if (!SafeReadByte(reinterpret_cast<const void*>(address + i), &bytes[i]))
        {
            mod::Log(
                "BRIDGE_PATCH_DIAG[%s]: unreadable byte window addr=0x%08lX "
                "size=%zu failedAt=%zu",
                context != nullptr ? context : "?",
                static_cast<unsigned long>(address),
                size,
                i);
            return;
        }
    }

    char hex[kMaxBytes * 3] = {};
    size_t used = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const int written = std::snprintf(
            hex + used,
            sizeof(hex) - used,
            "%s%02X",
            i == 0 ? "" : " ",
            static_cast<unsigned>(bytes[i]));
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(hex) - used)
        {
            break;
        }
        used += static_cast<size_t>(written);
    }

    mod::Log(
        "BRIDGE_PATCH_DIAG[%s]: bytes addr=0x%08lX size=%zu%s [%s]",
        context != nullptr ? context : "?",
        static_cast<unsigned long>(address),
        count,
        size > count ? " truncated" : "",
        hex);
}

static void LogSessionVtableSlotsIfVerbose(
    const char* caller,
    uintptr_t sessionPtr,
    uintptr_t vtablePtr,
    uintptr_t imageBase)
{
    if (!BridgePatchVerboseLoggingEnabled())
    {
        return;
    }

    const unsigned long vtableRva =
        (imageBase != 0 && vtablePtr >= imageBase)
            ? static_cast<unsigned long>(vtablePtr - imageBase)
            : 0UL;
    mod::Log(
        "BRIDGE_PATCH_DIAG[%s]: session=0x%08lX vtable=0x%08lX "
        "vtableRVA=0x%08lX",
        caller != nullptr ? caller : "?",
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(vtablePtr),
        vtableRva);

    for (size_t slot = 0; slot < 10; ++slot)
    {
        uintptr_t target = 0;
        if (!SafeReadPtr(
                reinterpret_cast<const void*>(vtablePtr + slot * sizeof(uintptr_t)),
                &target))
        {
            mod::Log(
                "BRIDGE_PATCH_DIAG[%s]: vtable[%zu] unreadable",
                caller != nullptr ? caller : "?",
                slot);
            continue;
        }

        const unsigned long targetRva =
            (imageBase != 0 && target >= imageBase)
                ? static_cast<unsigned long>(target - imageBase)
                : 0UL;
        mod::Log(
            "BRIDGE_PATCH_DIAG[%s]: vtable[%zu]=0x%08lX rva=0x%08lX",
            caller != nullptr ? caller : "?",
            slot,
            static_cast<unsigned long>(target),
            targetRva);
    }
}

bool ReadModuleImageRange(HMODULE module, uintptr_t* outBase, uintptr_t* outEnd)
{
    if (module == nullptr || outBase == nullptr || outEnd == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos = {};
    if (!IsReadableRange(reinterpret_cast<const void*>(base), sizeof(dos)))
    {
        return false;
    }

    __try
    {
        dos = *reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
    {
        return false;
    }

    const uintptr_t ntAddress = base + static_cast<uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS32 nt = {};
    if (!IsReadableRange(reinterpret_cast<const void*>(ntAddress), sizeof(nt)))
    {
        return false;
    }

    __try
    {
        nt = *reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntAddress);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt.OptionalHeader.SizeOfImage == 0)
    {
        return false;
    }

    const uintptr_t end = base + static_cast<uintptr_t>(nt.OptionalHeader.SizeOfImage);
    if (end <= base)
    {
        return false;
    }

    *outBase = base;
    *outEnd = end;
    return true;
}

int ReadRoleFlagFromRevival()
{
    EnsureActiveRevivalProfile();

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return -1;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    for (size_t i = 0; i < g_activeRevival->roleFlagOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->roleFlagOffsets[i];
        int role = -1;
        if (SafeReadInt(reinterpret_cast<const void*>(base + offset), &role)
            && role >= 0 && role <= 3)
        {
            return role;
        }
    }
    return -1;
}

bool IsSessionPointerByVtable(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd)
{
    // Reject null / low-memory sentinels that can appear at fallback offsets.
    if (sessionPtr < 0x00100000u)
    {
        return false;
    }

    // Session objects are C++ classes; first word must be a vtable inside EfzRevival.dll.
    uintptr_t vtable = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable))
    {
        return false;
    }
    if (revivalImageBase == 0 || revivalImageEnd <= revivalImageBase || vtable < revivalImageBase || vtable >= revivalImageEnd)
    {
        return false;
    }

    return true;
}

bool IsLikelySessionPointer(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd)
{
    if (!IsSessionPointerByVtable(sessionPtr, revivalImageBase, revivalImageEnd))
    {
        return false;
    }

    // 1.02j has role-specific object sizes and layouts.  Applying the
    // Rollback delay/ping offsets to Practice, Compact, Spectator, or Replay
    // reads unrelated fields (and goes out of bounds for Practice).  Exact
    // verified vtable identity is a stronger validator for this build.
    if (IsRevival102jProfile())
    {
        uintptr_t vtable = 0;
        if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable)
            || vtable < revivalImageBase)
        {
            return false;
        }
        switch (vtable - revivalImageBase)
        {
        case 0x0016FEB0u: // Compact
        case 0x0016FEF0u: // Rollback
        case 0x0016FF20u: // Spectator
        case 0x0016FF50u: // Replay
        case 0x0016FF80u: // Practice
            return true;
        default:
            return false;
        }
    }

    int delayFrames = -1;
    int pingMs = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay), &delayFrames))
    {
        return false;
    }
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetPingMs), &pingMs))
    {
        return false;
    }

    const bool delayLooksValid = delayFrames >= 0 && delayFrames < 128;
    const bool pingLooksValid = pingMs >= 0 && pingMs < 60000;
    return delayLooksValid || pingLooksValid;
}

uintptr_t ReadSessionPointerFromRevival()
{
    EnsureActiveRevivalProfile();

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        g_lastSessionPtrOffset = 0;
        return 0;
    }

    g_lastSessionPtrOffset = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = base;
        revivalImageEnd = base + 0x02000000u;
    }

    for (size_t i = 0; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->sessionPtrOffsets[i];
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(base + offset), &sessionPtr)
            && IsLikelySessionPointer(sessionPtr, revivalImageBase, revivalImageEnd))
        {
            g_lastSessionPtrOffset = offset;
            g_lastValidatedSessionPtr = sessionPtr;
            return sessionPtr;
        }
    }
    return 0;
}

uintptr_t ReadSessionPointerFromRevivalLoose()
{
    EnsureActiveRevivalProfile();

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = base;
        revivalImageEnd = base + 0x02000000u;
    }

    for (size_t i = 0; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->sessionPtrOffsets[i];
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(base + offset), &sessionPtr)
            && IsSessionPointerByVtable(sessionPtr, revivalImageBase, revivalImageEnd))
        {
            return sessionPtr;
        }
    }

    return 0;
}

uintptr_t ReadSessionPointerForMutation(bool* outUsedCached)
{
    if (outUsedCached != nullptr)
    {
        *outUsedCached = false;
    }

    const uintptr_t strictSessionPtr = ReadSessionPointerFromRevival();
    if (strictSessionPtr != 0)
    {
        return strictSessionPtr;
    }

    const uintptr_t cachedSessionPtr = g_lastValidatedSessionPtr;
    if (cachedSessionPtr == 0)
    {
        return 0;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = reinterpret_cast<uintptr_t>(revival);
        revivalImageEnd = revivalImageBase + 0x02000000u;
    }

    if (!IsSessionPointerByVtable(cachedSessionPtr, revivalImageBase, revivalImageEnd))
    {
        return 0;
    }

    if (IsRevival102jProfile())
    {
        if (outUsedCached != nullptr)
        {
            *outUsedCached = true;
        }
        return cachedSessionPtr;
    }

    int activePlayer = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(cachedSessionPtr + g_activeRevival->sessionOffsetActivePlayer), &activePlayer))
    {
        return 0;
    }
    if (activePlayer != 0 && activePlayer != 1)
    {
        return 0;
    }

    if (outUsedCached != nullptr)
    {
        *outUsedCached = true;
    }
    return cachedSessionPtr;
}

bool ReadRevivalSyncFlags(RevivalSyncFlags* outFlags)
{
    if (outFlags == nullptr)
    {
        return false;
    }

    *outFlags = RevivalSyncFlags{};
    bool hasAny = false;

    int gameMode = -1;
    if (SafeReadInt(reinterpret_cast<const void*>(g_activeRevival->addrGameModeCurrentIndex), &gameMode))
    {
        outFlags->gameMode = gameMode;
        hasAny = true;
    }

    uintptr_t mode0Struct = 0;
    if (SafeReadPtr(reinterpret_cast<const void*>(g_activeRevival->addrGameModeStructTable), &mode0Struct) && mode0Struct != 0)
    {
        uint8_t mode0Flag = 0;
        if (SafeReadByte(reinterpret_cast<const void*>(mode0Struct + 1084u), &mode0Flag))
        {
            outFlags->mode0Flag1084 = static_cast<int>(mode0Flag);
            hasAny = true;
        }
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr)
    {
        uintptr_t globalStatePtr = 0;
        const uintptr_t globalStatePtrAddr = reinterpret_cast<uintptr_t>(revival) + g_activeRevival->globalStatePtrOffset;
        if (SafeReadPtr(reinterpret_cast<const void*>(globalStatePtrAddr), &globalStatePtr) && globalStatePtr != 0)
        {
            uint8_t sessionByte = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + g_activeRevival->globalStateOffsetSessionByte), &sessionByte))
            {
                outFlags->sessionByte = static_cast<int>(sessionByte);
                hasAny = true;
            }

            uint8_t globalFlag4964 = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + g_activeRevival->globalStateOffsetFlag4964), &globalFlag4964))
            {
                outFlags->globalFlag4964 = static_cast<int>(globalFlag4964);
                hasAny = true;
            }

            uint8_t globalFlag4965 = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + g_activeRevival->globalStateOffsetFlag4965), &globalFlag4965))
            {
                outFlags->globalFlag4965 = static_cast<int>(globalFlag4965);
                hasAny = true;
            }
        }
    }

    const bool sessionLooksRollback = outFlags->sessionByte == 0 || outFlags->sessionByte == 1 || outFlags->sessionByte == 2;
    const bool isPlayerSync = (outFlags->gameMode == 3 && outFlags->mode0Flag1084 == 4 && sessionLooksRollback);
    const bool isSpectateSync = (outFlags->gameMode == 8 && outFlags->mode0Flag1084 == 4);
    outFlags->inRollbackSyncState = isPlayerSync || isSpectateSync;
    outFlags->inRollbackActiveState = (outFlags->gameMode == 3 && outFlags->mode0Flag1084 == 4 && outFlags->sessionByte == 2)
        || isSpectateSync;
    return hasAny;
}

// Forward-declared here (before RefreshRuntimeStatus) because the spectator
// name-reading path needs it.  Defined alongside the DLL exit-process patch
// save/restore logic further below.
static bool g_dllExitProcessPatchesSaved = false;

int ComputeDelaySetupReadyFromPromptState(
    int promptSerial,
    int promptServedSerial,
    int phase,
    int roleFlag)
{
    if (promptSerial > 0)
    {
        return 1;
    }

    if (promptServedSerial > 0)
    {
        static int s_lastSuppressedServedSerial = 0;
        if (s_lastSuppressedServedSerial != promptServedSerial)
        {
            s_lastSuppressedServedSerial = promptServedSerial;
            mod::Log(
                "DelayPromptState: suppressed stale served-only signal promptSerial=%d servedSerial=%d phase=%d role=%d",
                promptSerial,
                promptServedSerial,
                phase,
                roleFlag);
        }
    }

    return 0;
}

void RefreshRuntimeStatus(NetbridgeStatus* ioStatus)
{
    if (ioStatus == nullptr)
    {
        return;
    }

    // --- Timing guard: detect slow reads from Revival session memory ------
    LARGE_INTEGER rrsQpcStart = {}, rrsQpcEnd = {};
    QueryPerformanceCounter(&rrsQpcStart);

    ioStatus->syncGameMode = -1;
    ioStatus->syncMode0Flag1084 = -1;
    ioStatus->syncSessionByte = -1;
    ioStatus->syncGlobalFlag4964 = -1;
    ioStatus->syncGlobalFlag4965 = -1;
    ioStatus->pingMs = -1;
    ioStatus->rollbackFrames = -1;
    ioStatus->delayPromptSerial = 0;
    ioStatus->delayPromptServedSerial = 0;
    ioStatus->spectateConfirmPromptSerial = 0;
    ioStatus->spectateConfirmPromptServedSerial = 0;
    ioStatus->spectateConfirmPromptKind = static_cast<int>(NetbridgeSpectatePromptKind::None);
    ioStatus->localInitApplied = g_localInitAppliedForSession ? 1 : 0;
    ioStatus->delaySetupReady = 0;
    ioStatus->vsHumanSyncReady = 0;
    ioStatus->p1Name[0] = '\0';
    ioStatus->p2Name[0] = '\0';
    ioStatus->sessionScoresValid = 0;
    ioStatus->sessionNamesValid = 0;

    LONG delayPromptSerial = 0;
    LONG delayPromptServedSerial = 0;
    ReadDelayPromptSignal(&delayPromptSerial, &delayPromptServedSerial);
    ioStatus->delayPromptSerial = static_cast<int>(delayPromptSerial);
    ioStatus->delayPromptServedSerial = static_cast<int>(delayPromptServedSerial);

    LONG spectateConfirmSerial = 0;
    LONG spectateConfirmServedSerial = 0;
    int spectateConfirmPromptKind = static_cast<int>(NetbridgeSpectatePromptKind::None);
    ReadSpectateConfirmPromptSignal(&spectateConfirmSerial, &spectateConfirmServedSerial, &spectateConfirmPromptKind);
    ioStatus->spectateConfirmPromptSerial = static_cast<int>(spectateConfirmSerial);
    ioStatus->spectateConfirmPromptServedSerial = static_cast<int>(spectateConfirmServedSerial);
    ioStatus->spectateConfirmPromptKind = spectateConfirmPromptKind;

    LONG consoleErrorSerial = 0;
    ReadConsoleError(&consoleErrorSerial, ioStatus->consoleErrorText, sizeof(ioStatus->consoleErrorText));
    ioStatus->consoleErrorSerial = static_cast<int>(consoleErrorSerial);

    DelayPromptMetrics promptMetrics = g_delayPromptMetrics;
    if (g_hostBlock != nullptr)
    {
        const LONG metricsSerial = InterlockedCompareExchange(&g_hostBlock->delayMetricsSerial, 0, 0);
        if (metricsSerial > 0)
        {
            promptMetrics.serial = static_cast<int>(metricsSerial);
            promptMetrics.averagePingMs = g_hostBlock->delayAveragePingMs;
            promptMetrics.minPingMs = g_hostBlock->delayMinPingMs;
            promptMetrics.maxPingMs = g_hostBlock->delayMaxPingMs;
            promptMetrics.recommendedDelay = g_hostBlock->delayRecommended;
            promptMetrics.minDelay = g_hostBlock->delayRangeMin;
            promptMetrics.maxDelay = g_hostBlock->delayRangeMax;
        }
        const LONG inputSerial = InterlockedCompareExchange(&g_hostBlock->delayInputSerial, 0, 0);
        promptMetrics.inputSerial = static_cast<int>(inputSerial);
        promptMetrics.inputValue = g_hostBlock->delayInputValue;
    }
    g_delayPromptMetrics = promptMetrics;

    RevivalSyncFlags syncFlags = {};
    if (ReadRevivalSyncFlags(&syncFlags))
    {
        ioStatus->syncGameMode = syncFlags.gameMode;
        ioStatus->syncMode0Flag1084 = syncFlags.mode0Flag1084;
        ioStatus->syncSessionByte = syncFlags.sessionByte;
        ioStatus->syncGlobalFlag4964 = syncFlags.globalFlag4964;
        ioStatus->syncGlobalFlag4965 = syncFlags.globalFlag4965;
        ioStatus->vsHumanSyncReady = syncFlags.inRollbackSyncState ? 1 : 0;
    }

    int roleFlag = ReadRoleFlagFromRevival();
    if (roleFlag < 0)
    {
        roleFlag = g_localRoleFlag;
    }
    ioStatus->roleFlag = roleFlag;

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    if (sessionPtr == 0)
    {
        if (promptMetrics.averagePingMs >= 0)
        {
            ioStatus->pingMs = promptMetrics.averagePingMs;
        }
        if (promptMetrics.recommendedDelay >= 0)
        {
            ioStatus->rollbackFrames = promptMetrics.recommendedDelay;
        }
        ioStatus->delaySetupReady =
            ComputeDelaySetupReadyFromPromptState(
                ioStatus->delayPromptSerial,
                ioStatus->delayPromptServedSerial,
                ioStatus->phase,
                ioStatus->roleFlag);
        return;
    }

    // Online and spectator sessions have different layouts.  The legacy
    // spectator object (1.02e-i) stores raw wchar_t buffers, while 1.02j's
    // MinGW rewrite stores basic_string<wchar_t>-style objects and moves the
    // score counters.  Never apply the rollback profile's fields to a
    // spectator: several offsets remain readable but name unrelated data.
    const bool isOnlineSession    = (g_localRoleFlag == kLocalRoleOnline);
    const bool isSpectatorSession = (g_localRoleFlag == kLocalRoleSpectate);
    const bool isRevival102j      = IsRevival102jProfile();

    // Legacy spectator layout (1.02e-i).
    constexpr uintptr_t kLegacySpectatorOffsetP1Wins = 128;
    constexpr uintptr_t kLegacySpectatorOffsetP2Wins = 132;
    constexpr uintptr_t kLegacySpectatorOffsetP1Name = 154;  // raw wchar_t[64]
    constexpr uintptr_t kLegacySpectatorOffsetP2Name = 282;  // raw wchar_t[64]

    // 1.02j spectator layout (object size 0x6A8).  Name fields are MinGW
    // wstring objects: pointer at +0, uint32 length at +4, SSO storage at +8.
    constexpr uintptr_t kSpectator102jOffsetP1NameObject = 0x170;
    constexpr uintptr_t kSpectator102jOffsetP2NameObject = 0x188;
    constexpr uintptr_t kSpectator102jOffsetP1Wins = 0x1C4;
    constexpr uintptr_t kSpectator102jOffsetP2Wins = 0x1C8;

    int delayFrames = -1;
    int pingMs = -1;
    int sessionActivePlayer = -1;
    int sessionP1Wins = 0;
    int sessionP2Wins = 0;
    bool sessionScoresRead = false;
    if (isOnlineSession)
    {
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay), &delayFrames);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetPingMs), &pingMs);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer), &sessionActivePlayer);
        sessionScoresRead =
            SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetP1Wins), &sessionP1Wins)
            && SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetP2Wins), &sessionP2Wins);
    }
    else if (isSpectatorSession)
    {
        // Spectator sessions have no activePlayer, inputDelay, or ping -
        // only wins are meaningful.
        const uintptr_t p1WinsOffset = isRevival102j
            ? kSpectator102jOffsetP1Wins
            : kLegacySpectatorOffsetP1Wins;
        const uintptr_t p2WinsOffset = isRevival102j
            ? kSpectator102jOffsetP2Wins
            : kLegacySpectatorOffsetP2Wins;
        sessionScoresRead =
            SafeReadInt(reinterpret_cast<const void*>(sessionPtr + p1WinsOffset), &sessionP1Wins)
            && SafeReadInt(reinterpret_cast<const void*>(sessionPtr + p2WinsOffset), &sessionP2Wins);
    }

    // Expose activePlayer and wins through the bridge status.
    if (sessionActivePlayer == 0 || sessionActivePlayer == 1)
    {
        ioStatus->activePlayer = sessionActivePlayer;
    }
    if (sessionScoresRead
        && sessionP1Wins >= 0 && sessionP1Wins < 1000
        && sessionP2Wins >= 0 && sessionP2Wins < 1000)
    {
        ioStatus->sessionP1Wins = sessionP1Wins;
        ioStatus->sessionP2Wins = sessionP2Wins;
        ioStatus->sessionScoresValid = 1;
    }

    if (delayFrames >= 0 && delayFrames < 128)
    {
        ioStatus->rollbackFrames = delayFrames;
    }
    if (pingMs >= 0 && pingMs < 60000)
    {
        ioStatus->pingMs = pingMs;
    }
    else if (promptMetrics.averagePingMs >= 0)
    {
        ioStatus->pingMs = promptMetrics.averagePingMs;
    }

    if (ioStatus->rollbackFrames < 0 && promptMetrics.recommendedDelay >= 0)
    {
        ioStatus->rollbackFrames = promptMetrics.recommendedDelay;
    }

    // Read a raw null-terminated wchar_t[] buffer (NOT an std::wstring SSO
    // object) and convert to UTF-8 into outText.  The source buffers are
    // wchar_t[64] (128 bytes) inside the 276-byte config snapshot at
    // session + configStructOffset + 14 / + 142.
    auto tryReadInlineName = [](uintptr_t baseAddress, char* outText, size_t outSize) -> void {
        if (outText == nullptr || outSize == 0)
        {
            return;
        }
        outText[0] = '\0';

        // The raw config Name buffers are wchar_t[64] (128 bytes).
        // We read up to 63 characters; the 64th is guaranteed null by
        // the memset(0, 0x100) in the config struct constructor.
        constexpr size_t kMaxChars = 63;
        wchar_t wide[kMaxChars + 1] = {};
        if (!IsReadableRange(reinterpret_cast<const void*>(baseAddress), kMaxChars * sizeof(wchar_t)))
        {
            return;
        }

        __try
        {
            std::memcpy(wide, reinterpret_cast<const void*>(baseAddress), kMaxChars * sizeof(wchar_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        wide[kMaxChars] = L'\0';

        size_t n = 0;
        while (n < kMaxChars && wide[n] != L'\0')
        {
            if (wide[n] < 0x20)
            {
                return;
            }
            ++n;
        }
        if (n == 0 || n >= kMaxChars)
        {
            return;
        }

        // Convert to UTF-8.  If the full name exceeds the output buffer,
        // WideCharToMultiByte returns 0 (ERROR_INSUFFICIENT_BUFFER), so we
        // fall back to a safe prefix guaranteed to fit ((outSize-1)/3 chars
        // → worst-case 3 bytes/char for BMP code-points).
        int converted = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(n),
                                            outText, static_cast<int>(outSize - 1),
                                            nullptr, nullptr);
        if (converted <= 0 && n > 0)
        {
            const int safeLen = static_cast<int>((outSize - 1) / 3);
            if (safeLen > 0)
            {
                converted = WideCharToMultiByte(CP_UTF8, 0, wide,
                                                (safeLen < static_cast<int>(n)) ? safeLen : static_cast<int>(n),
                                                outText, static_cast<int>(outSize - 1),
                                                nullptr, nullptr);
            }
        }
        if (converted > 0)
        {
            outText[converted] = '\0';
        }
    };

    // Read the 1.02j MinGW basic_string<wchar_t> representation used by the
    // spectator object.  We intentionally consume only pointer+length: the
    // pointer already targets either the object's +8 SSO buffer or its heap
    // allocation, so no capacity/layout guess is required.
    auto tryReadMinGwWstring = [](uintptr_t objectAddress, char* outText, size_t outSize) -> void {
        if (outText == nullptr || outSize == 0)
        {
            return;
        }
        outText[0] = '\0';

        uintptr_t charsAddress = 0;
        int signedLength = 0;
        if (!SafeReadPtr(reinterpret_cast<const void*>(objectAddress), &charsAddress)
            || !SafeReadInt(reinterpret_cast<const void*>(objectAddress + sizeof(uintptr_t)), &signedLength)
            || charsAddress == 0
            || signedLength <= 0
            || signedLength > 63)
        {
            return;
        }

        const size_t length = static_cast<size_t>(signedLength);
        if (!IsReadableRange(
                reinterpret_cast<const void*>(charsAddress),
                length * sizeof(wchar_t)))
        {
            return;
        }

        wchar_t wide[64] = {};
        __try
        {
            std::memcpy(wide, reinterpret_cast<const void*>(charsAddress),
                        length * sizeof(wchar_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        for (size_t i = 0; i < length; ++i)
        {
            if (wide[i] < 0x20 || wide[i] == 0x7F)
            {
                return;
            }
        }

        int converted = WideCharToMultiByte(
            CP_UTF8, 0, wide, signedLength,
            outText, static_cast<int>(outSize - 1), nullptr, nullptr);
        if (converted <= 0)
        {
            const int safeLength = static_cast<int>((outSize - 1) / 3);
            if (safeLength > 0)
            {
                converted = WideCharToMultiByte(
                    CP_UTF8, 0, wide,
                    safeLength < signedLength ? safeLength : signedLength,
                    outText, static_cast<int>(outSize - 1), nullptr, nullptr);
            }
        }
        if (converted > 0)
        {
            outText[converted] = '\0';
        }
    };

    auto sanitizeInlineName = [](char* text, size_t textSize) -> void {
        if (text == nullptr || textSize == 0 || text[0] == '\0')
        {
            return;
        }

        size_t len = 0;
        int questionMarks = 0;
        for (; len + 1 < textSize && text[len] != '\0'; ++len)
        {
            const unsigned char c = static_cast<unsigned char>(text[len]);
            if (c < 0x20 || c == 0x7F)
            {
                text[0] = '\0';
                return;
            }
            if (c == '?')
            {
                ++questionMarks;
            }
        }
        if (len == 0 || len + 1 >= textSize)
        {
            text[0] = '\0';
            return;
        }

        // Reject mojibake-like names (mostly '?' from failed conversion).
        if (questionMarks >= 3 && questionMarks * 2 >= static_cast<int>(len))
        {
            text[0] = '\0';
        }
    };

    const bool allowSessionInlineNames =
        isOnlineSession
        && (syncFlags.inRollbackSyncState
            || syncFlags.inRollbackActiveState
            || static_cast<NetbridgePhase>(ioStatus->phase) == NetbridgePhase::Connected);

    if (allowSessionInlineNames)
    {
        tryReadInlineName(sessionPtr + g_activeRevival->sessionOffsetP1Name, ioStatus->p1Name, sizeof(ioStatus->p1Name));
        tryReadInlineName(sessionPtr + g_activeRevival->sessionOffsetP2Name, ioStatus->p2Name, sizeof(ioStatus->p2Name));
        sanitizeInlineName(ioStatus->p1Name, sizeof(ioStatus->p1Name));
        sanitizeInlineName(ioStatus->p2Name, sizeof(ioStatus->p2Name));
    }
    else if (isSpectatorSession && g_dllExitProcessPatchesSaved)
    {
        // Spectator names are populated from Init_Spec by the role-specific
        // post-init.  1.02j uses MinGW wstring objects; older builds use raw
        // wchar_t[64] buffers.
        //
        // We gate on g_dllExitProcessPatchesSaved (set at the end of the
        // init sequence) instead of phase==Connected because spectator
        // sessions may not be promoted to Connected until the next
        // takeover::Tick() call - and TickExportOnly() (per-frame tick
        // hook) only calls RefreshRuntimeStatus(), not the full Tick().
        if (isRevival102j)
        {
            tryReadMinGwWstring(
                sessionPtr + kSpectator102jOffsetP1NameObject,
                ioStatus->p1Name, sizeof(ioStatus->p1Name));
            tryReadMinGwWstring(
                sessionPtr + kSpectator102jOffsetP2NameObject,
                ioStatus->p2Name, sizeof(ioStatus->p2Name));
        }
        else
        {
            tryReadInlineName(
                sessionPtr + kLegacySpectatorOffsetP1Name,
                ioStatus->p1Name, sizeof(ioStatus->p1Name));
            tryReadInlineName(
                sessionPtr + kLegacySpectatorOffsetP2Name,
                ioStatus->p2Name, sizeof(ioStatus->p2Name));
        }
        sanitizeInlineName(ioStatus->p1Name, sizeof(ioStatus->p1Name));
        sanitizeInlineName(ioStatus->p2Name, sizeof(ioStatus->p2Name));
    }

    // Only expose player names when we have both sides; showing a single local
    // nickname during delay setup looks misleading.
    if (ioStatus->p1Name[0] == '\0' || ioStatus->p2Name[0] == '\0')
    {
        ioStatus->p1Name[0] = '\0';
        ioStatus->p2Name[0] = '\0';
    }
    else
    {
        ioStatus->sessionNamesValid = 1;
    }

    ioStatus->delaySetupReady =
        ComputeDelaySetupReadyFromPromptState(
            ioStatus->delayPromptSerial,
            ioStatus->delayPromptServedSerial,
            ioStatus->phase,
            ioStatus->roleFlag);

    // --- End timing guard ---------------------------------------------------
    QueryPerformanceCounter(&rrsQpcEnd);
    {
        static uint32_t s_rrsSlowCount = 0;
        LARGE_INTEGER freq = {};
        QueryPerformanceFrequency(&freq);
        const double elapsedMs =
            static_cast<double>(rrsQpcEnd.QuadPart - rrsQpcStart.QuadPart)
            * 1000.0 / static_cast<double>(freq.QuadPart);
        if (elapsedMs > 2.0)
        {
            ++s_rrsSlowCount;
            if (s_rrsSlowCount <= 10 || (s_rrsSlowCount % 200 == 0))
            {
                mod::Log(
                    "PERF_WARN: RefreshRuntimeStatus took %.2fms "
                    "(slowCount=%u) - reading session memory is slow",
                    elapsedMs, s_rrsSlowCount);
            }
        }
    }
}

bool SetLocalRoleFlag(int roleFlag, const char* reason)
{
    if (g_localInitFn == nullptr)
    {
        return false;
    }

    if (roleFlag < 0 || roleFlag > 3)
    {
        return false;
    }

    if (g_localRoleFlag == roleFlag)
    {
        return true;
    }

    LogRevival102jDeepSnapshot("SetLocalRoleFlag.01.entry");

    LogInitWriteSnapshot("SetLocalRoleFlag_pre");

    // Destroy the current session to prevent leaking the old object.
    const uintptr_t oldSessionPtr = ReadSessionPointerFromRevival();
    DestroyCurrentSession("SetLocalRoleFlag");
    LogRevival102jDeepStep("SetLocalRoleFlag.02.old_session_destroyed");

    // Prevent init() from chaining another trampoline at 0x401582.
    mod::Log(
        "SetLocalRoleFlag: about to save EXE hook bytes before init() "
        "roleFlag=%d oldSession=0x%08lX",
        roleFlag, static_cast<unsigned long>(oldSessionPtr));
    SaveExeFrameHookBytes();
    // Restore original (pre-hook) bytes at mode-ctor hook sites BEFORE
    // init() so the new trampoline copies clean EXE bytes instead of
    // stale hooks from a previous session's mode (prevents chaining).
    RestoreModeCtorOriginalBytes();
    ResetModeConstructorTrampolineCache();
    LogRevival102jDeepSnapshot("SetLocalRoleFlag.03.exported_init_pre");

    // Dump the 10 bytes at 0x401582 right before init().
    {
        uint8_t pre[10] = {};
        memcpy(pre, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "SetLocalRoleFlag: 0x401582 pre-init  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            pre[0], pre[1], pre[2], pre[3], pre[4],
            pre[5], pre[6], pre[7], pre[8], pre[9]);
    }

    int localParams[2] = {roleFlag, 102};
    mod::Log("SetLocalRoleFlag: calling init(mode=%d, magic=%d)",
             localParams[0], localParams[1]);
    CloseMirrorLogFiles();
    const int result = g_localInitFn(localParams);
    LogRevival102jDeepSnapshot("SetLocalRoleFlag.04.exported_init_returned");

    // Dump the 10 bytes AFTER init() to see what sub_1006F160 wrote.
    {
        uint8_t post[10] = {};
        memcpy(post, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "SetLocalRoleFlag: 0x401582 post-init "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            post[0], post[1], post[2], post[3], post[4],
            post[5], post[6], post[7], post[8], post[9]);
    }

    // Undo the EXE frame-hook chain growth at 0x401582.
    mod::Log("SetLocalRoleFlag: restoring saved EXE hook bytes (roleFlag=%d)", roleFlag);
    RestoreExeFrameHookBytes();
    // Mode-ctor originals were already restored before init().
    // For non-local modes (online/spectate/tournament), init() installs
    // fresh hooks at 0x763E50/0x763F04 that intercept mode transitions;
    // those hooks are correct and don't chain through stale trampolines.

    // Verify the restore worked.
    {
        uint8_t verify[10] = {};
        memcpy(verify, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "SetLocalRoleFlag: 0x401582 restored  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            verify[0], verify[1], verify[2], verify[3], verify[4],
            verify[5], verify[6], verify[7], verify[8], verify[9]);
    }

    // Fix up relative instructions in mode-constructor trampolines.
    FixupModeConstructorTrampolines("SetLocalRoleFlag");

    g_localRoleFlag = roleFlag;

    const uintptr_t newSessionPtr = ReadSessionPointerFromRevival();
    LogInitWriteSnapshot("SetLocalRoleFlag_post");
    LogRevival102jDeepSnapshot("SetLocalRoleFlag.99.complete");

    mod::Log("Takeover: local role switch mode=%d result=%d reason=%s oldSession=0x%08lX newSession=0x%08lX",
        roleFlag, result, reason != nullptr ? reason : "",
        static_cast<unsigned long>(oldSessionPtr),
        static_cast<unsigned long>(newSessionPtr));
    return true;
}

bool SetRoleFlagDirect(int roleFlag, const char* reason)
{
    if (roleFlag < 0 || roleFlag > 3)
    {
        return false;
    }

    if (g_localRoleFlag == roleFlag)
    {
        return true;
    }

    // Write roleFlag directly to every Revival DLL global location
    // WITHOUT calling init(). This changes how other mods (training
    // mode, rich presence) see the current mode without creating a
    // new session object or applying session-specific EXE patches.
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("Takeover: SetRoleFlagDirect failed (Revival DLL not loaded) reason=%s",
            reason != nullptr ? reason : "");
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    int written = 0;
    for (size_t i = 0; i < g_activeRevival->roleFlagOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->roleFlagOffsets[i];
        if (offset == 0)
        {
            continue;
        }
        int* const ptr = reinterpret_cast<int*>(base + offset);
        if (IsWritableRange(ptr, sizeof(int)))
        {
            *ptr = roleFlag;
            ++written;
        }
    }

    const int oldRole = g_localRoleFlag;
    g_localRoleFlag = roleFlag;
    mod::Log("Takeover: direct role flag %d -> %d (wrote %d globals) reason=%s",
        oldRole, roleFlag, written, reason != nullptr ? reason : "");
    LogRevival102jDeepSnapshot("SetRoleFlagDirect.complete");
    return written > 0;
}

static bool IsRevival102jProfile();
static void NormalizeRevivalFpuState(const char* context, uintptr_t sessionPtr);

// ---------------------------------------------------------------------------
// NeutralizeTournamentAutoNav - zero all entries in the tournament session's
// auto-navigation input queue.
//
// After init(3,102), the tournament constructor populates a deque with 22
// byte-pair entries that simulate controller presses to auto-navigate from
// the title screen through character select and into a fight.  This is
// designed for Concerto's unattended bracket flow.
//
// For our interactive use we zero every entry (making them idle/no-input)
// while keeping the queue non-empty (element count stays at 22).  This
// prevents phantom button presses during character select AND avoids the
// mode-0 ExitProcess trigger that fires when the queue is empty.
//
// The queue is naturally consumed over 22 frames as idle inputs.  When the
// match ends and returns to mode 0, the queue is empty → ExitProcess → our
// IAT hook intercepts and routes to the netplay menu.
//
// MSVC deque<uint16_t> layout (byte offsets from deque start):
//   +0  proxy / allocator
//   +4  pointer to block-pointer array
//   +8  block slot count (power of 2)
//   +12 start offset
//   +16 element count
//
// Each block holds 8 elements of 2 bytes (16 bytes per block).
// Block index = (abs_index >> 3) & (block_count - 1).
// ---------------------------------------------------------------------------
bool NeutralizeTournamentAutoNav()
{
    const uintptr_t sessionPtr = ReadSessionPointerFromRevivalLoose();
    if (sessionPtr == 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: no session pointer");
        return false;
    }

    if (g_activeRevival == nullptr || g_activeRevival->tournamentInputQueueOffset == 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: no queue offset in profile");
        return false;
    }

    const uintptr_t dequeAddr = sessionPtr + g_activeRevival->tournamentInputQueueOffset;

    if (IsRevival102jProfile())
    {
        uintptr_t mapPtr = 0;
        int mapSize = 0;
        uintptr_t startCur = 0;
        uintptr_t startFirst = 0;
        uintptr_t startLast = 0;
        uintptr_t startNode = 0;
        uintptr_t finishCur = 0;
        uintptr_t finishFirst = 0;
        uintptr_t finishLast = 0;
        uintptr_t finishNode = 0;

        const bool readOk =
            SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x00), &mapPtr)
            && SafeReadInt(reinterpret_cast<const void*>(dequeAddr + 0x04), &mapSize)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x08), &startCur)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x0C), &startFirst)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x10), &startLast)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x14), &startNode)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x18), &finishCur)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x1C), &finishFirst)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x20), &finishLast)
            && SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 0x24), &finishNode);
        if (!readOk)
        {
            mod::Log(
                "NeutralizeTournamentAutoNav: 1.02j deque layout unreadable "
                "session=0x%08lX deque=0x%08lX",
                static_cast<unsigned long>(sessionPtr),
                static_cast<unsigned long>(dequeAddr));
            return false;
        }

        const bool sane =
            mapPtr != 0
            && mapSize > 0
            && mapSize <= 1024
            && startNode != 0
            && finishNode != 0
            && startFirst <= startCur
            && startCur <= startLast
            && finishFirst <= finishCur
            && finishCur <= finishLast
            && startLast >= startFirst
            && finishLast >= finishFirst;
        if (!sane)
        {
            mod::Log(
                "NeutralizeTournamentAutoNav: 1.02j deque sanity failed "
                "map=0x%08lX size=%d start=%08lX/%08lX/%08lX node=%08lX "
                "finish=%08lX/%08lX/%08lX node=%08lX",
                static_cast<unsigned long>(mapPtr),
                mapSize,
                static_cast<unsigned long>(startCur),
                static_cast<unsigned long>(startFirst),
                static_cast<unsigned long>(startLast),
                static_cast<unsigned long>(startNode),
                static_cast<unsigned long>(finishCur),
                static_cast<unsigned long>(finishFirst),
                static_cast<unsigned long>(finishLast),
                static_cast<unsigned long>(finishNode));
            return false;
        }

        if (startNode != finishNode)
        {
            mod::Log(
                "NeutralizeTournamentAutoNav: 1.02j multi-block deque skipped "
                "startNode=0x%08lX finishNode=0x%08lX",
                static_cast<unsigned long>(startNode),
                static_cast<unsigned long>(finishNode));
            return false;
        }

        if (finishCur <= startCur)
        {
            mod::Log(
                "NeutralizeTournamentAutoNav: 1.02j queue already empty "
                "start=0x%08lX finish=0x%08lX",
                static_cast<unsigned long>(startCur),
                static_cast<unsigned long>(finishCur));
            return false;
        }

        const size_t byteCount = finishCur - startCur;
        if ((byteCount & 1u) != 0
            || byteCount > 512
            || !IsWritableRange(reinterpret_cast<void*>(startCur), byteCount))
        {
            mod::Log(
                "NeutralizeTournamentAutoNav: 1.02j element range invalid "
                "start=0x%08lX finish=0x%08lX bytes=%zu",
                static_cast<unsigned long>(startCur),
                static_cast<unsigned long>(finishCur),
                byteCount);
            return false;
        }

        LogBytesIfVerbose("NeutralizeTournamentAutoNav.102j.before", startCur, byteCount);
        std::memset(reinterpret_cast<void*>(startCur), 0, byteCount);
        LogBytesIfVerbose("NeutralizeTournamentAutoNav.102j.after", startCur, byteCount);
        mod::Log(
            "NeutralizeTournamentAutoNav: 1.02j zeroed %zu queued bytes "
            "(%zu 2-byte inputs)",
            byteCount,
            byteCount / 2);
        return true;
    }

    // Read deque metadata.
    uintptr_t blockArrayPtr = 0;
    int blockCount = 0;
    int elementCount = 0;

    if (!SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 4), &blockArrayPtr) || blockArrayPtr == 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: failed to read block-array pointer");
        return false;
    }
    if (!SafeReadInt(reinterpret_cast<const void*>(dequeAddr + 8), &blockCount) || blockCount <= 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: failed to read block count");
        return false;
    }
    if (!SafeReadInt(reinterpret_cast<const void*>(dequeAddr + 16), &elementCount) || elementCount <= 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: failed to read element count (got %d)", elementCount);
        return false;
    }

    // Number of blocks that contain data.
    const int usedBlocks = (elementCount + 7) / 8;
    int zeroed = 0;

    for (int i = 0; i < usedBlocks && i < blockCount; ++i)
    {
        uintptr_t blockPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(blockArrayPtr + 4 * static_cast<uintptr_t>(i)), &blockPtr)
            && blockPtr != 0)
        {
            // Zero all 8 element slots (16 bytes) in this block, making
            // every entry the idle byte pair (0, 0).
            std::memset(reinterpret_cast<void*>(blockPtr), 0, 16);
            ++zeroed;
        }
    }

    mod::Log(
        "NeutralizeTournamentAutoNav: zeroed %d/%d blocks, %d elements remain (idle)",
        zeroed, usedBlocks, elementCount);
    return zeroed > 0;
}

// ---------------------------------------------------------------------------
// Tournament EXE patch save / restore.
//
// The tournament constructor patches 4 locations in the EFZ executable:
//   0x763F04 (7 bytes) - inline hook → sub_1006E260
//   0x763E50 (7 bytes) - inline hook → sub_1006E260
//   0x754C1A (1 byte)  - byte set to 0
//   0x7599ED (20 bytes) - NOP pad
//
// In Concerto the process exits after every match, so these are never
// reverted.  For our in-process mode switching we save the original bytes
// before init(3,102) and restore them after intercepting ExitProcess.
// ---------------------------------------------------------------------------

static uint8_t g_savedTournamentPatches[kMaxTournamentExePatches][kMaxTournamentExePatchBytes];
static bool g_tournamentPatchesSaved = false;

bool SaveTournamentExePatches()
{
    if (g_activeRevival == nullptr || g_activeRevival->tournamentExePatchCount == 0)
    {
        return false;
    }

    for (size_t i = 0; i < g_activeRevival->tournamentExePatchCount; ++i)
    {
        const uintptr_t addr = g_activeRevival->tournamentExePatchAddr[i];
        const size_t size = g_activeRevival->tournamentExePatchSize[i];
        if (size == 0 || size > kMaxTournamentExePatchBytes)
        {
            continue;
        }
        char label[64] = {};
        std::snprintf(label, sizeof(label), "SaveTournamentExePatches[%zu]", i);
        LogBytesIfVerbose(label, addr, size);
        LogRevival102jDeepBytes("TournamentPatches.save", label, addr, size);
        std::memcpy(g_savedTournamentPatches[i],
                    reinterpret_cast<const void*>(addr), size);
    }

    g_tournamentPatchesSaved = true;
    mod::Log("SaveTournamentExePatches: saved %zu patch regions",
             g_activeRevival->tournamentExePatchCount);
    return true;
}

bool RestoreTournamentExePatches()
{
    if (!g_tournamentPatchesSaved || g_activeRevival == nullptr)
    {
        mod::Log("RestoreTournamentExePatches: nothing to restore");
        return false;
    }

    int restored = 0;
    for (size_t i = 0; i < g_activeRevival->tournamentExePatchCount; ++i)
    {
        const uintptr_t addr = g_activeRevival->tournamentExePatchAddr[i];
        const size_t size = g_activeRevival->tournamentExePatchSize[i];
        if (size == 0 || size > kMaxTournamentExePatchBytes)
        {
            continue;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(addr), size,
                           PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            char beforeLabel[72] = {};
            std::snprintf(
                beforeLabel,
                sizeof(beforeLabel),
                "RestoreTournamentExePatches[%zu].before",
                i);
            LogBytesIfVerbose(beforeLabel, addr, size);
            LogRevival102jDeepBytes("TournamentPatches.restore_pre", beforeLabel, addr, size);
            std::memcpy(reinterpret_cast<void*>(addr),
                        g_savedTournamentPatches[i], size);
            char afterLabel[72] = {};
            std::snprintf(
                afterLabel,
                sizeof(afterLabel),
                "RestoreTournamentExePatches[%zu].after",
                i);
            LogBytesIfVerbose(afterLabel, addr, size);
            LogRevival102jDeepBytes("TournamentPatches.restore_post", afterLabel, addr, size);
            VirtualProtect(reinterpret_cast<void*>(addr), size,
                           oldProtect, &oldProtect);
            ++restored;
        }
    }

    g_tournamentPatchesSaved = false;
    mod::Log("RestoreTournamentExePatches: restored %d/%zu patches",
             restored, g_activeRevival->tournamentExePatchCount);
    return restored > 0;
}

// ---------------------------------------------------------------------------
// DLL ExitProcess call-site patches
//
// The Revival DLL's tournament tick calls ExitProcess(0) when the game
// mode returns to 0 (title screen).  Each call is guarded by a Jcc
// instruction (jz or jnz).  By patching the Jcc byte to 0xEB (jmp short),
// the conditional becomes unconditional, making ExitProcess unreachable.
//
// These patches are applied before init(3,102) and restored after the
// tournament session is cleaned up.
// ---------------------------------------------------------------------------

static uint8_t g_savedDllExitProcessBytes[RevivalAddressProfile::kMaxExitProcessPatches];
static uint8_t g_savedDllExitNearJccBytes[RevivalAddressProfile::kMaxExitProcessNearJccPatches][6];
static uint8_t g_savedRevival102jTournamentExitGuard[2];
static bool g_revival102jTournamentExitGuardSaved = false;
static constexpr uintptr_t kRevival102jTournamentExitGuardRva = 0x0004683Fu;
// g_dllExitProcessPatchesSaved is declared earlier (before RefreshRuntimeStatus).

static bool IsRevival102jProfile()
{
    return g_activeRevival != nullptr
        && g_activeRevival->versionTag != nullptr
        && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0;
}

static bool SaveAndApplyRevival102jExitProcessPatches()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    auto* const guard =
        reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(revival)
            + kRevival102jTournamentExitGuardRva);
    LogBytesIfVerbose(
        "SaveAndApplyDllExitProcessPatches.102j_guard.before",
        reinterpret_cast<uintptr_t>(guard),
        2);
    if (guard[0] == 0x90 && guard[1] == 0x90)
    {
        g_savedRevival102jTournamentExitGuard[0] = 0x74;
        g_savedRevival102jTournamentExitGuard[1] = 0x7A;
        g_revival102jTournamentExitGuardSaved = true;
        g_dllExitProcessPatchesSaved = true;
        mod::Log(
            "SaveAndApplyDllExitProcessPatches: 1.02j tournament guard already patched "
            "RVA 0x%lX",
            static_cast<unsigned long>(kRevival102jTournamentExitGuardRva));
        return true;
    }
    if (guard[0] != 0x74 || guard[1] != 0x7A)
    {
        mod::Log(
            "SaveAndApplyDllExitProcessPatches: 1.02j tournament guard mismatch "
            "RVA 0x%lX expected 74 7A found %02X %02X",
            static_cast<unsigned long>(kRevival102jTournamentExitGuardRva),
            static_cast<unsigned>(guard[0]),
            static_cast<unsigned>(guard[1]));
        return false;
    }

    g_savedRevival102jTournamentExitGuard[0] = guard[0];
    g_savedRevival102jTournamentExitGuard[1] = guard[1];

    DWORD oldProtect = 0;
    if (!VirtualProtect(guard, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        mod::Log(
            "SaveAndApplyDllExitProcessPatches: 1.02j tournament guard "
            "VirtualProtect failed err=%lu",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    guard[0] = 0x90;
    guard[1] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(guard, 2, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), guard, 2);
    LogBytesIfVerbose(
        "SaveAndApplyDllExitProcessPatches.102j_guard.after",
        reinterpret_cast<uintptr_t>(guard),
        2);

    g_revival102jTournamentExitGuardSaved = true;
    g_dllExitProcessPatchesSaved = true;
    mod::Log(
        "SaveAndApplyDllExitProcessPatches: 1.02j tournament guard "
        "RVA 0x%lX patched 74 7A -> 90 90",
        static_cast<unsigned long>(kRevival102jTournamentExitGuardRva));
    return true;
}

bool SaveAndApplyDllExitProcessPatches()
{
    if (g_dllExitProcessPatchesSaved)
    {
        // H4 diagnostic: this early-out means the next restore will use
        // stale saved bytes from the previous session's save.  Log a
        // warning so we can detect this in the trace.
        mod::Log("SaveAndApplyDllExitProcessPatches: SKIPPED (flag already true) "
                 "- H4: next restore will use previously saved bytes!");
        return true; // Already applied - don't overwrite saved originals.
    }
    if (g_activeRevival == nullptr)
    {
        return false;
    }
    if (IsRevival102jProfile())
    {
        LogRevival102jDeepSnapshot("ExitGuard.save_apply_pre");
        const bool result = SaveAndApplyRevival102jExitProcessPatches();
        LogRevival102jDeepSnapshot("ExitGuard.save_apply_post");
        return result;
    }
    if (g_activeRevival->exitProcessPatchCount == 0
        && g_activeRevival->exitProcessNearJccCount == 0)
    {
        if (g_activeRevival->versionTag != nullptr
            && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0)
        {
            g_dllExitProcessPatchesSaved = true;
            mod::Log(
                "SaveAndApplyDllExitProcessPatches: no safe 1.02j call-site patches; "
                "using ExitProcess IAT hook only");
            return true;
        }
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    int applied = 0;

    // --- Single-byte Jcc patches (0x74/0x75 → 0xEB) ---
    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        // Save original byte.
        g_savedDllExitProcessBytes[i] = *ptr;

        // Verify the original byte matches the expected Jcc opcode.
        const uint8_t expected = g_activeRevival->exitProcessPatchOriginal[i];
        if (*ptr != expected)
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: site %zu at RVA 0x%lX: "
                     "expected 0x%02X, found 0x%02X - skipping",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(*ptr));
            continue;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *ptr = 0xEB; // jmp short (unconditional)
            VirtualProtect(ptr, 1, oldProtect, &oldProtect);
            ++applied;
            mod::Log("SaveAndApplyDllExitProcessPatches: site %zu RVA 0x%lX: "
                     "patched 0x%02X -> 0xEB",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(expected));
        }
        else
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: site %zu RVA 0x%lX: "
                     "VirtualProtect failed err=%lu",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned long>(GetLastError()));
        }
    }

    // --- 6-byte near-Jcc NOP patches (0F 84/85 rel32 → 6× NOP) ---
    int nearApplied = 0;
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        // Save original 6 bytes.
        std::memcpy(g_savedDllExitNearJccBytes[i], ptr, 6);

        // Validate: expect 0F 84 (jz near) or 0F 85 (jnz near).
        if (ptr[0] != 0x0F || (ptr[1] != 0x84 && ptr[1] != 0x85))
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: near-Jcc %zu at RVA 0x%lX: "
                     "expected 0F 84/85, found %02X %02X - skipping",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(ptr[0]),
                     static_cast<unsigned>(ptr[1]));
            continue;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            std::memset(ptr, 0x90, 6); // 6× NOP
            DWORD ignored = 0;
            VirtualProtect(ptr, 6, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), ptr, 6);
            ++nearApplied;
            mod::Log("SaveAndApplyDllExitProcessPatches: near-Jcc %zu RVA 0x%lX: "
                     "patched %02X %02X -> 6xNOP",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(g_savedDllExitNearJccBytes[i][0]),
                     static_cast<unsigned>(g_savedDllExitNearJccBytes[i][1]));
        }
        else
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: near-Jcc %zu RVA 0x%lX: "
                     "VirtualProtect failed err=%lu",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned long>(GetLastError()));
        }
    }

    g_dllExitProcessPatchesSaved = true;
    mod::Log("SaveAndApplyDllExitProcessPatches: applied %d/%zu single-byte + "
             "%d/%zu near-Jcc patches",
             applied, g_activeRevival->exitProcessPatchCount,
             nearApplied, g_activeRevival->exitProcessNearJccCount);

    // --- Post-apply verification pass ---
    int verifyFail = 0;
    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
        if (rva == 0) continue;
        const auto* ptr = reinterpret_cast<const uint8_t*>(base + rva);
        if (*ptr != 0xEB)
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: VERIFY FAIL site %zu RVA 0x%lX: "
                     "expected 0xEB, found 0x%02X",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(*ptr));
            ++verifyFail;
        }
    }
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
        if (rva == 0) continue;
        const auto* ptr = reinterpret_cast<const uint8_t*>(base + rva);
        if (ptr[0] != 0x90 || ptr[1] != 0x90)
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: VERIFY FAIL near-Jcc %zu RVA 0x%lX: "
                     "expected 90 90, found %02X %02X",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(ptr[0]),
                     static_cast<unsigned>(ptr[1]));
            ++verifyFail;
        }
    }
    if (verifyFail > 0)
    {
        mod::Log("SaveAndApplyDllExitProcessPatches: WARNING - %d patches failed verification!",
                 verifyFail);
    }
    else
    {
        mod::Log("SaveAndApplyDllExitProcessPatches: all patches verified OK");
    }

    return (applied + nearApplied) > 0;
}

bool RestoreDllExitProcessPatches()
{
    if (!g_dllExitProcessPatchesSaved || g_activeRevival == nullptr)
    {
        mod::Log("RestoreDllExitProcessPatches: SKIPPED (saved=%d profile=%p)",
                 g_dllExitProcessPatchesSaved ? 1 : 0,
                 static_cast<const void*>(g_activeRevival));
        return false;
    }

    if (IsRevival102jProfile())
    {
        LogRevival102jDeepSnapshot("ExitGuard.restore_pre");
        if (!g_revival102jTournamentExitGuardSaved)
        {
            mod::Log(
                "RestoreDllExitProcessPatches: 1.02j tournament guard not saved");
            g_dllExitProcessPatchesSaved = false;
            return false;
        }

        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival == nullptr)
        {
            return false;
        }

        auto* const guard =
            reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(revival)
                + kRevival102jTournamentExitGuardRva);
        LogBytesIfVerbose(
            "RestoreDllExitProcessPatches.102j_guard.before",
            reinterpret_cast<uintptr_t>(guard),
            2);
        DWORD oldProtect = 0;
        if (!VirtualProtect(guard, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            mod::Log(
                "RestoreDllExitProcessPatches: 1.02j tournament guard "
                "VirtualProtect failed err=%lu",
                static_cast<unsigned long>(GetLastError()));
            return false;
        }

        guard[0] = g_savedRevival102jTournamentExitGuard[0];
        guard[1] = g_savedRevival102jTournamentExitGuard[1];
        DWORD ignored = 0;
        VirtualProtect(guard, 2, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), guard, 2);
        LogBytesIfVerbose(
            "RestoreDllExitProcessPatches.102j_guard.after",
            reinterpret_cast<uintptr_t>(guard),
            2);

        g_revival102jTournamentExitGuardSaved = false;
        g_dllExitProcessPatchesSaved = false;
        mod::Log(
            "RestoreDllExitProcessPatches: 1.02j tournament guard "
            "RVA 0x%lX restored %02X %02X",
            static_cast<unsigned long>(kRevival102jTournamentExitGuardRva),
            static_cast<unsigned>(guard[0]),
            static_cast<unsigned>(guard[1]));
        LogRevival102jDeepSnapshot("ExitGuard.restore_post");
        return true;
    }

    // Log the saved bytes we're about to restore (H4 diagnostic)
    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        mod::Log("RestoreDllExitProcessPatches: will restore site[%zu] RVA=0x%lX "
                 "savedByte=0x%02X",
                 i,
                 static_cast<unsigned long>(g_activeRevival->exitProcessPatchRva[i]),
                 static_cast<unsigned>(g_savedDllExitProcessBytes[i]));
    }
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        mod::Log("RestoreDllExitProcessPatches: will restore nearJcc[%zu] RVA=0x%lX "
                 "savedBytes=%02X %02X %02X %02X %02X %02X",
                 i,
                 static_cast<unsigned long>(g_activeRevival->exitProcessNearJccRva[i]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][0]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][1]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][2]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][3]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][4]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][5]));
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    int restored = 0;

    // --- Restore single-byte Jcc patches ---
    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *ptr = g_savedDllExitProcessBytes[i];
            VirtualProtect(ptr, 1, oldProtect, &oldProtect);
            ++restored;
        }
    }

    // --- Restore 6-byte near-Jcc patches ---
    int nearRestored = 0;
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            std::memcpy(ptr, g_savedDllExitNearJccBytes[i], 6);
            DWORD ignored = 0;
            VirtualProtect(ptr, 6, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), ptr, 6);
            ++nearRestored;
        }
    }

    g_dllExitProcessPatchesSaved = false;
    mod::Log("RestoreDllExitProcessPatches: restored %d/%zu single-byte + "
             "%d/%zu near-Jcc patches",
             restored, g_activeRevival->exitProcessPatchCount,
             nearRestored, g_activeRevival->exitProcessNearJccCount);
    return (restored + nearRestored) > 0;
}

bool AreDllExitPatchesSaved()
{
    return g_dllExitProcessPatchesSaved;
}

// ---------------------------------------------------------------------------
// ForceLocalPlayInit - unconditionally call init(2,102) to create a fresh
// local play session, then invoke the version-specific post-init method
// before any other hooks dispatch to the new session.
//
// ---------------------------------------------------------------------------
// DestroyCurrentSession - tear down the current Revival session object
// BEFORE calling init() to create a new one.
//
// Root cause fix for the 2nd-session crash: init() allocates a new session
// via operator new, runs the constructor, and writes the pointer to
// dword_100A02CC - WITHOUT freeing or destructing the old session.  Every
// init() call therefore leaks the previous session's memory and OS handles.
// After several init() calls, heap corruption from these leaked objects
// causes a vtable dispatch crash in EFZ_GameMode_InvokeAdvance.
//
// Legacy MSVC builds use their scalar-deleting-destructor ABI.  The 1.02j
// MinGW build instead has a full destructor in vtable[0] and a no-argument
// deleting thunk in vtable[1].  Its exact role vtables and thunks are
// verified against the raw DLL before this code is enabled.
//
// 1.02j session sizes from exported init() at RVA 0x12E2B0:
//   Mode 0 (Online):     0x778
//   Mode 1 (Spectator):  0x6A8
//   Mode 2 (Local play): 0x2F0
//   Mode 3 (Tournament): 0x380
// ---------------------------------------------------------------------------
bool DestroyCurrentSession(const char* caller)
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || g_activeRevival == nullptr
        || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        mod::Log("%s: DestroyCurrentSession skipped (DLL not loaded)", caller);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);

    // Read current session pointer from dword_100A02CC.
    const uintptr_t sessionGlobalAddr =
        base + g_activeRevival->sessionPtrOffsets[0];
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionGlobalAddr),
                     &sessionPtr)
        || sessionPtr == 0)
    {
        mod::Log("%s: DestroyCurrentSession skipped (session NULL)", caller);
        return false;
    }

    const int currentRole = g_localRoleFlag;

    mod::Log(
        "%s: DestroyCurrentSession starting session=0x%08lX role=%d",
        caller,
        static_cast<unsigned long>(sessionPtr),
        currentRole);
    LogRevival102jDeepSnapshot("DestroyCurrentSession.01.entry");

    uintptr_t capturedOriginalVtable = 0;
    const bool restoredNeutralizedVtable =
        RestoreNeutralizedSessionVtableForCleanup(
            sessionPtr,
            &capturedOriginalVtable);
    if (restoredNeutralizedVtable)
    {
        mod::Log(
            "%s: DestroyCurrentSession recovered neutralized identity "
            "session=0x%08lX originalVtable=0x%08lX",
            caller,
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(capturedOriginalVtable));
        LogRevival102jDeepSnapshot("DestroyCurrentSession.02.identity_restored");
    }

    // Preserve the legacy handle cleanup.  The 1.02j profile has different
    // object layouts and its verified deleting destructor owns all fields.
    if (!IsRevival102jProfile()
        && (currentRole == kLocalRoleOnline || currentRole == kLocalRoleSpectate))
    {
        uintptr_t helperHandle = 0;
        if (SafeReadPtr(
                reinterpret_cast<const void*>(
                    sessionPtr + g_activeRevival->sessionOffsetHelperHandle),
                &helperHandle)
            && helperHandle != 0
            && helperHandle != static_cast<uintptr_t>(~uintptr_t(0)))
        {
            const BOOL closed =
                CloseHandle(reinterpret_cast<HANDLE>(helperHandle));
            mod::Log(
                "%s: DestroyCurrentSession closed helperHandle=0x%08lX result=%d",
                caller,
                static_cast<unsigned long>(helperHandle),
                closed);
        }
    }

    // Read vtable pointer.
    uintptr_t vtablePtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtablePtr)
        || vtablePtr == 0)
    {
        mod::Log(
            "%s: DestroyCurrentSession WARN vtable NULL session=0x%08lX, zeroing ptr only",
            caller,
            static_cast<unsigned long>(sessionPtr));
        goto zero_globals;
    }

    mod::Log(
        "%s: DestroyCurrentSession vtable=0x%08lX (RVA 0x%08lX)",
        caller,
        static_cast<unsigned long>(vtablePtr),
        static_cast<unsigned long>(vtablePtr - base));

    if (IsRevival102jProfile())
    {
        struct SessionDtor102j
        {
            uintptr_t vtableRva;
            uintptr_t deletingDtorRva;
            const char* name;
        };
        static const SessionDtor102j kSessionDtors[] = {
            {0x0016FEB0u, 0x00048550u, "compact"},
            {0x0016FEF0u, 0x00058580u, "rollback"},
            {0x0016FF20u, 0x00062900u, "spectator"},
            {0x0016FF50u, 0x00075CD0u, "replay"},
            {0x0016FF80u, 0x00080630u, "practice"},
        };

        const uintptr_t vtableRva = vtablePtr - base;
        const SessionDtor102j* matched = nullptr;
        for (const auto& candidate : kSessionDtors)
        {
            if (candidate.vtableRva == vtableRva)
            {
                matched = &candidate;
                break;
            }
        }
        if (matched == nullptr)
        {
            mod::Log(
                "%s: DestroyCurrentSession 1.02j skipped unknown/neutralized "
                "vtable RVA=0x%08lX, zeroing globals only",
                caller,
                static_cast<unsigned long>(vtableRva));
            goto zero_globals;
        }

        uintptr_t deletingDtor = 0;
        const uintptr_t expectedDeletingDtor = base + matched->deletingDtorRva;
        if (!SafeReadPtr(
                reinterpret_cast<const void*>(vtablePtr + sizeof(uintptr_t)),
                &deletingDtor)
            || deletingDtor != expectedDeletingDtor)
        {
            mod::Log(
                "%s: DestroyCurrentSession 1.02j %s deleting dtor mismatch "
                "actual=0x%08lX expected=0x%08lX, zeroing globals only",
                caller,
                matched->name,
                static_cast<unsigned long>(deletingDtor),
                static_cast<unsigned long>(expectedDeletingDtor));
            goto zero_globals;
        }

        bool destructorCompleted = false;
        LogRevival102jDeepStep("DestroyCurrentSession.03.deleting_dtor_call");
        __try
        {
            using MinGwDeletingDtorFn = void(__fastcall*)(void* thisPtr, void* edx);
            auto dtorFn = reinterpret_cast<MinGwDeletingDtorFn>(deletingDtor);
            dtorFn(reinterpret_cast<void*>(sessionPtr), nullptr);
            destructorCompleted = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            mod::Log(
                "%s: DestroyCurrentSession 1.02j %s deleting dtor SEH "
                "exception=0x%08lX",
                caller,
                matched->name,
                static_cast<unsigned long>(GetExceptionCode()));
        }
        mod::Log(
            "%s: DestroyCurrentSession 1.02j %s deleting dtor=0x%08lX "
            "completed=%d",
            caller,
            matched->name,
            static_cast<unsigned long>(deletingDtor),
            destructorCompleted ? 1 : 0);
        LogRevival102jDeepStep(
            destructorCompleted
                ? "DestroyCurrentSession.04.deleting_dtor_returned"
                : "DestroyCurrentSession.04.deleting_dtor_failed");
        goto zero_globals;
    }

    // Read vtable[0] - the scalar deleting destructor.
    {
        uintptr_t vtableSlot0 = 0;
        if (!SafeReadPtr(reinterpret_cast<const void*>(vtablePtr),
                         &vtableSlot0)
            || vtableSlot0 == 0)
        {
            mod::Log(
                "%s: DestroyCurrentSession WARN vtable[0] NULL vtable=0x%08lX, zeroing ptr only",
                caller,
                static_cast<unsigned long>(vtablePtr));
            goto zero_globals;
        }

        // Validate that vtable[0] points into the Revival DLL image.
        uintptr_t revBase = 0, revEnd = 0;
        if (ReadModuleImageRange(revival, &revBase, &revEnd))
        {
            if (vtableSlot0 < revBase || vtableSlot0 >= revEnd)
            {
                mod::Log(
                    "%s: DestroyCurrentSession SKIPPED - vtable[0]=0x%08lX "
                    "outside DLL [0x%08lX..0x%08lX], zeroing ptr only",
                    caller,
                    static_cast<unsigned long>(vtableSlot0),
                    static_cast<unsigned long>(revBase),
                    static_cast<unsigned long>(revEnd));
                goto zero_globals;
            }
        }

        // Call vtable[0](session, 1) as __thiscall.
        // Flag 1 = destruct AND free (operator delete).
        // This is exactly what sub_1006D810 does during mid-game mode swap.
        typedef void(__thiscall* ScalarDeletingDtorFn)(void*, int);
        auto dtorFn = reinterpret_cast<ScalarDeletingDtorFn>(vtableSlot0);

        mod::Log(
            "%s: DestroyCurrentSession calling dtor vtable[0]=0x%08lX "
            "session=0x%08lX role=%d",
            caller,
            static_cast<unsigned long>(vtableSlot0),
            static_cast<unsigned long>(sessionPtr),
            currentRole);

        dtorFn(reinterpret_cast<void*>(sessionPtr), 1);

        mod::Log("%s: DestroyCurrentSession destructor completed", caller);
    }

zero_globals:
    // Zero ALL session pointer globals to prevent stale references.
    int zeroed = 0;
    for (size_t i = 0; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->sessionPtrOffsets[i];
        if (offset != 0)
        {
            uintptr_t* addr = reinterpret_cast<uintptr_t*>(base + offset);
            if (IsWritableRange(addr, sizeof(uintptr_t)))
            {
                *addr = 0;
                ++zeroed;
            }
        }
    }

    // Reset the mode-constructor trampoline fixup cache.  The destructor
    // unhooks 0x763E50/0x763F04 and frees the old trampolines.  If a
    // subsequent init() allocates a new trampoline at the same heap
    // address, the g_lastFixedTrampoline[] guard must NOT skip it -
    // the new trampoline has fresh unrelocated bytes that need fixup.
    ResetModeConstructorTrampolineCache();

    mod::Log(
        "%s: DestroyCurrentSession done session=0x%08lX role=%d globalsZeroed=%d",
        caller,
        static_cast<unsigned long>(sessionPtr),
        currentRole,
        zeroed);
    LogRevival102jDeepSnapshot("DestroyCurrentSession.99.globals_zeroed");

    return true;
}

// ---------------------------------------------------------------------------
// InvokeSessionVtableInit - legacy Revival builds need an immediate session
// vtable slot 1 call after init() so fields initialized by the frame hook are
// ready before any other JMP-patched dispatch runs.
//
// 1.02j is different: the MinGW vtables have slot 1 as a scalar deleting
// destructor/free thunk (confirmed in the raw PseudoC and verifier), while
// frame dispatch uses the vtable+0x0C path.  Calling slot 1 on 1.02j corrupts
// or frees the newly-created spectator/practice object, so this helper skips
// that profile explicitly and only logs the slots when diagnostics are on.
// ---------------------------------------------------------------------------
bool InvokeSessionVtableInit(const char* caller)
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || g_activeRevival == nullptr
        || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (DLL not loaded)", caller);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t sessionGlobalAddr =
        base + g_activeRevival->sessionPtrOffsets[0];
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionGlobalAddr),
                     &sessionPtr)
        || sessionPtr == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (session NULL)", caller);
        return false;
    }

    uintptr_t vtablePtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtablePtr)
        || vtablePtr == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (vtable NULL)", caller);
        return false;
    }

    LogSessionVtableSlotsIfVerbose(caller, sessionPtr, vtablePtr, base);
    if (IsRevival102jProfile())
    {
        uintptr_t vtableSlot1 = 0;
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(vtablePtr + sizeof(uintptr_t)),
            &vtableSlot1);
        mod::Log(
            "%s: InvokeSessionVtableInit skipped for 1.02j "
            "(vtable[1]=0x%08lX is MinGW deleting destructor/free path)",
            caller,
            static_cast<unsigned long>(vtableSlot1));
        return false;
    }

    uintptr_t vtableSlot1 = 0;
    if (!SafeReadPtr(
            reinterpret_cast<const void*>(vtablePtr + sizeof(uintptr_t)),
            &vtableSlot1)
        || vtableSlot1 == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (vtable[1] NULL)", caller);
        return false;
    }

    // Legacy MSVC Revival builds: __thiscall with this in ECX, no extra args.
    typedef void(__thiscall* SessionInitFn)(void*);
    auto initFn = reinterpret_cast<SessionInitFn>(vtableSlot1);
    initFn(reinterpret_cast<void*>(sessionPtr));
    mod::Log(
        "%s: InvokeSessionVtableInit vtable[1] 0x%08lX called on session 0x%08lX",
        caller,
        static_cast<unsigned long>(vtableSlot1),
        static_cast<unsigned long>(sessionPtr));
    return true;
}

static bool InvokeRevival102jLocalPostInit(const char* caller)
{
    if (!IsRevival102jProfile())
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || g_activeRevival == nullptr
        || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        mod::Log("%s: 1.02j local post-init skipped (DLL not loaded)", caller);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t sessionGlobalAddr =
        base + g_activeRevival->sessionPtrOffsets[0];
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionGlobalAddr),
                     &sessionPtr)
        || sessionPtr == 0)
    {
        mod::Log("%s: 1.02j local post-init skipped (session NULL)", caller);
        return false;
    }

    uintptr_t vtablePtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtablePtr)
        || vtablePtr == 0)
    {
        mod::Log("%s: 1.02j local post-init skipped (vtable NULL)", caller);
        return false;
    }

    constexpr uintptr_t kPracticeVtableRva102j = 0x0016FF80u;
    constexpr uintptr_t kPracticePostInitRva102j = 0x0007DE40u;
    constexpr uintptr_t kPracticeNetplayCtrlOffset102j = 0x02DCu;
    constexpr uintptr_t kPracticeNetplayAuxOffset102j = 0x02E0u;

    const uintptr_t expectedVtable = base + kPracticeVtableRva102j;
    if (vtablePtr != expectedVtable)
    {
        mod::Log(
            "%s: 1.02j local post-init skipped (unexpected vtable=0x%08lX expected=0x%08lX)",
            caller,
            static_cast<unsigned long>(vtablePtr),
            static_cast<unsigned long>(expectedVtable));
        LogSessionVtableSlotsIfVerbose(caller, sessionPtr, vtablePtr, base);
        return false;
    }

    uintptr_t postInit = 0;
    if (!SafeReadPtr(
            reinterpret_cast<const void*>(vtablePtr + 2u * sizeof(uintptr_t)),
            &postInit)
        || postInit == 0)
    {
        mod::Log("%s: 1.02j local post-init skipped (vtable[2] NULL)", caller);
        return false;
    }

    const uintptr_t expectedPostInit = base + kPracticePostInitRva102j;
    if (postInit != expectedPostInit)
    {
        mod::Log(
            "%s: 1.02j local post-init skipped (vtable[2]=0x%08lX expected=0x%08lX)",
            caller,
            static_cast<unsigned long>(postInit),
            static_cast<unsigned long>(expectedPostInit));
        LogSessionVtableSlotsIfVerbose(caller, sessionPtr, vtablePtr, base);
        return false;
    }

    uintptr_t netplayCtrlBefore = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(sessionPtr + kPracticeNetplayCtrlOffset102j),
        &netplayCtrlBefore);

    using LocalPostInit102jFn = void(__fastcall*)(void* thisPtr, void* edx);
    auto initFn = reinterpret_cast<LocalPostInit102jFn>(postInit);
    initFn(reinterpret_cast<void*>(sessionPtr), nullptr);

    uintptr_t netplayCtrlAfter = 0;
    uintptr_t netplayAuxAfter = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(sessionPtr + kPracticeNetplayCtrlOffset102j),
        &netplayCtrlAfter);
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(sessionPtr + kPracticeNetplayAuxOffset102j),
        &netplayAuxAfter);

    mod::Log(
        "%s: 1.02j local post-init vtable[2]=0x%08lX called session=0x%08lX "
        "netplayCtrl 0x%08lX -> 0x%08lX aux=0x%08lX",
        caller,
        static_cast<unsigned long>(postInit),
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(netplayCtrlBefore),
        static_cast<unsigned long>(netplayCtrlAfter),
        static_cast<unsigned long>(netplayAuxAfter));

    if (netplayCtrlAfter == 0)
    {
        mod::Log(
            "%s: 1.02j local post-init WARNING netplayCtrl still NULL "
            "(compact/practice input writer would crash)",
            caller);
        return false;
    }

    return true;
}

bool IsRevival102jSpectatorPostInitReady(const char* caller)
{
    if (!IsRevival102jProfile())
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || g_activeRevival == nullptr
        || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t sessionPtr = 0;
    uintptr_t vtablePtr = 0;
    if (!SafeReadPtr(
            reinterpret_cast<const void*>(
                base + g_activeRevival->sessionPtrOffsets[0]),
            &sessionPtr)
        || sessionPtr == 0
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtablePtr)
        || vtablePtr != base + 0x0016FF20u)
    {
        return false;
    }

    DWORD helperExitCode = 0;
    const bool helperAlive =
        g_revivalProcess != nullptr
        && g_revivalProcessId != 0
        && GetExitCodeProcess(g_revivalProcess, &helperExitCode) != FALSE
        && helperExitCode == STILL_ACTIVE;

    const char* const initWireName = RevivalWireName("Init");
    HANDLE initMap = OpenFileMappingA(FILE_MAP_READ, FALSE, initWireName);
    LONG initHead = 0;
    LONG initTail = 0;
    bool initPayloadReady = false;
    if (initMap != nullptr)
    {
        const volatile LONG* const header =
            static_cast<const volatile LONG*>(
                MapViewOfFile(initMap, FILE_MAP_READ, 0, 0, 8));
        if (header != nullptr)
        {
            initHead = header[0];
            initTail = header[1];
            initPayloadReady = initHead != initTail;
            UnmapViewOfFile(const_cast<LONG*>(header));
        }
        CloseHandle(initMap);
    }

    const bool ready = helperAlive && initPayloadReady;
    static DWORD s_lastNotReadyLogTick = 0;
    const DWORD now = GetTickCount();
    if (ready
        || s_lastNotReadyLogTick == 0
        || now - s_lastNotReadyLogTick >= 1000u)
    {
        if (!ready)
        {
            s_lastNotReadyLogTick = now;
        }
        mod::Log(
            "%s: 1.02j spectator post-init readiness ready=%d "
            "helperPid=%lu helperAlive=%d helperExit=%lu "
            "wire='%s' initHead=%ld initTail=%ld payload=%d",
            caller != nullptr ? caller : "spectator_ready",
            ready ? 1 : 0,
            static_cast<unsigned long>(g_revivalProcessId),
            helperAlive ? 1 : 0,
            static_cast<unsigned long>(helperExitCode),
            initWireName != nullptr ? initWireName : "",
            static_cast<long>(initHead),
            static_cast<long>(initTail),
            initPayloadReady ? 1 : 0);
        LogRevival102jDeepStep(
            ready
                ? "SpectatorReadiness.ready"
                : "SpectatorReadiness.not_ready_periodic");
    }
    return ready;
}

bool InvokeRevival102jSpectatorPostInit(const char* caller)
{
    LogRevival102jDeepStep("SpectatorPostInit.01.entry");
    if (!IsRevival102jProfile())
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || g_activeRevival == nullptr
        || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        mod::Log("%s: 1.02j spectator post-init skipped (DLL not loaded)", caller);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(
            reinterpret_cast<const void*>(
                base + g_activeRevival->sessionPtrOffsets[0]),
            &sessionPtr)
        || sessionPtr == 0)
    {
        mod::Log("%s: 1.02j spectator post-init skipped (session NULL)", caller);
        return false;
    }

    uintptr_t vtablePtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtablePtr)
        || vtablePtr == 0)
    {
        mod::Log("%s: 1.02j spectator post-init skipped (vtable NULL)", caller);
        return false;
    }

    constexpr uintptr_t kSpectatorVtableRva102j = 0x0016FF20u;
    constexpr uintptr_t kSpectatorPostInitRva102j = 0x0005E640u;
    constexpr uintptr_t kSpectatorHelperHandleOffset102j = 0x0064u;
    constexpr uintptr_t kSpectatorHelperPidOffset102j = 0x02E0u;
    constexpr uintptr_t kSpectatorNetplayCtrlOffset102j = 0x0588u;
    const uintptr_t expectedVtable = base + kSpectatorVtableRva102j;
    if (vtablePtr != expectedVtable)
    {
        mod::Log(
            "%s: 1.02j spectator post-init skipped "
            "(vtable=0x%08lX expected=0x%08lX)",
            caller,
            static_cast<unsigned long>(vtablePtr),
            static_cast<unsigned long>(expectedVtable));
        LogSessionVtableSlotsIfVerbose(caller, sessionPtr, vtablePtr, base);
        return false;
    }
    LogRevival102jDeepSnapshot("SpectatorPostInit.02.validated_pre_call");

    uintptr_t postInit = 0;
    const uintptr_t expectedPostInit = base + kSpectatorPostInitRva102j;
    if (!SafeReadPtr(
            reinterpret_cast<const void*>(vtablePtr + 2u * sizeof(uintptr_t)),
            &postInit)
        || postInit != expectedPostInit)
    {
        mod::Log(
            "%s: 1.02j spectator post-init skipped "
            "(vtable[2]=0x%08lX expected=0x%08lX)",
            caller,
            static_cast<unsigned long>(postInit),
            static_cast<unsigned long>(expectedPostInit));
        LogSessionVtableSlotsIfVerbose(caller, sessionPtr, vtablePtr, base);
        return false;
    }

    uintptr_t netplayCtrlBefore = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(
            sessionPtr + kSpectatorNetplayCtrlOffset102j),
        &netplayCtrlBefore);

    bool callCompleted = false;
    __try
    {
        using SpectatorPostInit102jFn =
            void(__fastcall*)(void* thisPtr, void* edx);
        auto initFn = reinterpret_cast<SpectatorPostInit102jFn>(postInit);
        initFn(reinterpret_cast<void*>(sessionPtr), nullptr);
        callCompleted = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "%s: 1.02j spectator post-init SEH exception=0x%08lX",
            caller,
            static_cast<unsigned long>(GetExceptionCode()));
    }

    uintptr_t netplayCtrlAfter = 0;
    uintptr_t helperHandleRaw = 0;
    int helperPid = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(
            sessionPtr + kSpectatorNetplayCtrlOffset102j),
        &netplayCtrlAfter);
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(
            sessionPtr + kSpectatorHelperHandleOffset102j),
        &helperHandleRaw);
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            sessionPtr + kSpectatorHelperPidOffset102j),
        &helperPid);

    const HANDLE helperHandle = reinterpret_cast<HANDLE>(helperHandleRaw);
    DWORD helperHandlePid = 0;
    DWORD helperExitCode = 0;
    bool helperHandleAlive = false;
    if (helperHandle != nullptr && helperHandle != INVALID_HANDLE_VALUE)
    {
        helperHandlePid = GetProcessId(helperHandle);
        helperHandleAlive =
            GetExitCodeProcess(helperHandle, &helperExitCode) != FALSE
            && helperExitCode == STILL_ACTIVE;
    }
    const bool helperPidMatches =
        g_revivalProcessId != 0
        && helperPid == static_cast<int>(g_revivalProcessId)
        && helperHandlePid == g_revivalProcessId;
    LogRevival102jDeepSnapshot("SpectatorPostInit.03.call_returned");
    mod::Log(
        "%s: 1.02j spectator post-init vtable[2]=0x%08lX "
        "session=0x%08lX netplayCtrl 0x%08lX -> 0x%08lX "
        "pid=%d/%lu handle=0x%08lX handlePid=%lu exit=%lu alive=%d completed=%d",
        caller,
        static_cast<unsigned long>(postInit),
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(netplayCtrlBefore),
        static_cast<unsigned long>(netplayCtrlAfter),
        helperPid,
        static_cast<unsigned long>(g_revivalProcessId),
        static_cast<unsigned long>(helperHandleRaw),
        static_cast<unsigned long>(helperHandlePid),
        static_cast<unsigned long>(helperExitCode),
        helperHandleAlive ? 1 : 0,
        callCompleted ? 1 : 0);

    if (!callCompleted
        || netplayCtrlAfter == 0
        || !helperPidMatches
        || !helperHandleAlive)
    {
        mod::Log(
            "%s: 1.02j spectator post-init FAILED "
            "(completed=%d ctrl=%d pidMatch=%d handleAlive=%d; "
            "spectator main tick would immediately quit)",
            caller,
            callCompleted ? 1 : 0,
            netplayCtrlAfter != 0 ? 1 : 0,
            helperPidMatches ? 1 : 0,
            helperHandleAlive ? 1 : 0);
        return false;
    }
    LogRevival102jDeepStep("SpectatorPostInit.99.success");
    return true;
}

bool ForceLocalPlayInit()
{
    if (g_localInitFn == nullptr)
    {
        mod::Log("ForceLocalPlayInit: init function not available");
        return false;
    }

    IncrementForceLocalPlayInitCount();
    const int callCount = GetForceLocalPlayInitCount();
    LogRevival102jDeepSnapshot("ForceLocalPlayInit.01.entry");
    if (netplay::bridge::recovery::HasGameplayExitMenuEntryStarted()
        || netplay::bridge::recovery::HasGameplayExitMenuEntryBeenConsumed()
        || netplay::bridge::recovery::WasGameplayExitRecoveryCompleted()
        || netplay::bridge::frontend_return::HasConsumedNetplayMenuContinuation())
    {
        mod::Log(
            "GAMEPLAY_EXIT_INVARIANT_VIOLATION name=force_local_after_menu_entry callCount=%d state=%s frontendReturnState=%s",
            callCount,
            netplay::bridge::recovery::CurrentGameplayExitRecoveryStateName(),
            netplay::bridge::frontend_return::CurrentStateName());
    }

    // --- Full snapshot BEFORE init() ---
    LogInitWriteSnapshot("ForceLocalPlayInit_pre");

    // If we were the client (P2 / joiner), Revival swapped the P1/P2
    // input-config blocks during StartInitPlayer.  Reverse that swap now
    // BEFORE destroying the session so controls return to their default
    // layout for local play.
    if (g_netplayRole == kNetplayRoleClient)
    {
        ReverseInputSwapIfClient();
    }
    g_netplayRole = kNetplayRoleNone;

    // 1.02j installs 0x401642 once, outside exported init(). Capture that
    // persistent hook before the session destructor gets any opportunity to
    // change process hooks. Legacy builds retain their old save ordering.
    const uintptr_t oldSessionPtr = ReadSessionPointerFromRevival();
    if (IsRevival102jProfile())
    {
        SaveExeDispatchHookBytes();
    }
    DestroyCurrentSession("ForceLocalPlayInit");
    LogRevival102jDeepStep("ForceLocalPlayInit.02.old_session_destroyed");

    // Prevent init() from chaining another trampoline at 0x401582.
    mod::Log(
        "ForceLocalPlayInit: about to save EXE hook bytes before init() "
        "oldSession=0x%08lX callCount=%d",
        static_cast<unsigned long>(oldSessionPtr), callCount);
    SaveExeFrameHookBytes();
    if (!IsRevival102jProfile())
    {
        SaveExeDispatchHookBytes();
    }
    // Restore original (pre-hook) bytes at mode-ctor hook sites BEFORE
    // init() so the new trampoline copies clean EXE bytes instead of
    // stale hooks from a previous session's mode (prevents chaining
    // trampolines across sessions - root cause of the 0x26D19881 crash).
    RestoreModeCtorOriginalBytes();
    ResetModeConstructorTrampolineCache();
    LogRevival102jDeepSnapshot("ForceLocalPlayInit.03.exported_init_pre");

    // Dump the 10 bytes at 0x401582 right before init() for verification.
    {
        uint8_t pre[10] = {};
        memcpy(pre, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "ForceLocalPlayInit: 0x401582 pre-init  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            pre[0], pre[1], pre[2], pre[3], pre[4],
            pre[5], pre[6], pre[7], pre[8], pre[9]);
    }

    int localParams[2] = {kLocalRoleLocalPlay, 102};
    mod::Log("ForceLocalPlayInit: calling init(mode=%d, magic=%d)",
             localParams[0], localParams[1]);
    CloseMirrorLogFiles();
    const int result = g_localInitFn(localParams);
    LogRevival102jDeepSnapshot("ForceLocalPlayInit.04.exported_init_returned_unrestored");

    // Dump the 10 bytes AFTER init() to see what sub_1006F160 wrote.
    {
        uint8_t post[10] = {};
        memcpy(post, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "ForceLocalPlayInit: 0x401582 post-init "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            post[0], post[1], post[2], post[3], post[4],
            post[5], post[6], post[7], post[8], post[9]);
    }

    // Undo the trampoline chain growth at 0x401582 - restore saved bytes.
    mod::Log("ForceLocalPlayInit: restoring saved EXE hook bytes");
    RestoreExeFrameHookBytes();
    // Restore the saved 0x401642 state. For 1.02j this is the one-time
    // persistent frame dispatcher; exported init() does not reinstall it.
    RestoreExeDispatchHookBytes();
    if (IsRevival102jProfile())
    {
        const bool titleDispatchOk =
            RestoreExeDispatchHookForTitle("ForceLocalPlayInit");
        mod::Log(
            "ForceLocalPlayInit: 1.02j persistent title dispatch result=%d",
            titleDispatchOk ? 1 : 0);
    }
    // Mode-ctor originals were already restored before init(); local play
    // init(2,102) does not install hooks at 0x763E50/0x763F04, so the
    // originals are still in place.  No further action needed.

    // Verify the restore worked.
    {
        uint8_t verify[10] = {};
        memcpy(verify, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "ForceLocalPlayInit: 0x401582 restored  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            verify[0], verify[1], verify[2], verify[3], verify[4],
            verify[5], verify[6], verify[7], verify[8], verify[9]);
    }

    // Fix up relative instructions in mode-constructor trampolines.
    FixupModeConstructorTrampolines("ForceLocalPlayInit");
    LogRevival102jDeepSnapshot("ForceLocalPlayInit.05.hooks_restored_and_fixed");

    g_localRoleFlag = kLocalRoleLocalPlay;

    const uintptr_t newSessionPtr = ReadSessionPointerFromRevival();

    // --- Full snapshot AFTER init() (before vtable[1]) ---
    LogInitWriteSnapshot("ForceLocalPlayInit_post_init");
    LogRevival102jDeepSnapshot("ForceLocalPlayInit.06.local_session_constructed");

    mod::Log(
        "ForceLocalPlayInit: init(2,102) result=%d callCount=%d oldSession=0x%08lX newSession=0x%08lX",
        result, callCount,
        static_cast<unsigned long>(oldSessionPtr),
        static_cast<unsigned long>(newSessionPtr));

    // Legacy builds need the post-init vtable slot here.  1.02j uses MinGW
    // vtables where slot 1 is a deleting destructor; its installer calls
    // vtable[2] after init(), which initializes the practice netplay control
    // at +0x2DC used by the shared input writer.
    const bool vtableInitOk = IsRevival102jProfile()
        ? InvokeRevival102jLocalPostInit("ForceLocalPlayInit")
        : InvokeSessionVtableInit("ForceLocalPlayInit");

    // --- Full snapshot AFTER legacy vtable init / 1.02j post-init ---
    LogInitWriteSnapshot(
        vtableInitOk
            ? (IsRevival102jProfile()
                ? "ForceLocalPlayInit_post_vtable2_102j"
                : "ForceLocalPlayInit_post_vtable1")
            : (IsRevival102jProfile()
                ? "ForceLocalPlayInit_post_vtable2_102j_failed"
                : "ForceLocalPlayInit_post_vtable1_skipped"));

    LogRevival102jDeepSnapshot(
        vtableInitOk
            ? "ForceLocalPlayInit.99.post_init_success"
            : "ForceLocalPlayInit.99.post_init_failed");

    return true;
}

// ---------------------------------------------------------------------------
// ReverseInputSwapIfClient - calls Revival's input-swap helper to toggle the
// P1/P2 input-config swap back to its original state.  Older MSVC builds call
// this as a __thiscall helper on dword_100A0760.  1.02j decompiles as a
// MinGW __fastcall helper, but its only argument is still passed in ECX, so
// the same one-argument call shape is intentional and verifier-covered.
// Only fires when g_netplayRole == kNetplayRoleClient, meaning we joined as
// P2 and Revival swapped the two controller blocks during StartInitPlayer.
// The swap is a toggle, so calling it a second time restores the layout.
//
// Must be called during session teardown (ForceLocalPlayInit) BEFORE the
// session object is destroyed, though the swap targets a persistent global
// structure (dword_100A0760[20]+448) that survives session changes.
// ---------------------------------------------------------------------------
bool ReverseInputSwapIfClient()
{
    if (g_netplayRole != kNetplayRoleClient)
    {
        mod::Log("ReverseInputSwapIfClient: skipped (role=%d, not client)",
                 g_netplayRole);
        return false;
    }

    if (g_activeRevival == nullptr
        || g_activeRevival->inputSwapPairRva == 0
        || g_activeRevival->renderContextBaseOffset == 0)
    {
        mod::Log("ReverseInputSwapIfClient: skipped (address profile incomplete)");
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("ReverseInputSwapIfClient: skipped (Revival DLL not loaded)");
        return false;
    }

    const uintptr_t dllBase = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t contextBaseAddr = dllBase + g_activeRevival->renderContextBaseOffset;

    // EFZ_Obj_SubStruct448_CleanupPair is __thiscall with
    // this = &dword_100A0760 = dllBase + renderContextBaseOffset.
    typedef int(__thiscall* SwapInputsFn)(void* thisPtr);
    auto swapFn = reinterpret_cast<SwapInputsFn>(
        dllBase + g_activeRevival->inputSwapPairRva);

    __try
    {
        const int result = swapFn(reinterpret_cast<void*>(contextBaseAddr));
        mod::Log(
            "ReverseInputSwapIfClient: swap reversed OK (fn=0x%08lX this=0x%08lX result=%d)",
            static_cast<unsigned long>(dllBase + g_activeRevival->inputSwapPairRva),
            static_cast<unsigned long>(contextBaseAddr),
            result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "ReverseInputSwapIfClient: EXCEPTION calling swap fn=0x%08lX this=0x%08lX",
            static_cast<unsigned long>(dllBase + g_activeRevival->inputSwapPairRva),
            static_cast<unsigned long>(contextBaseAddr));
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// SaveRenderContext / RestoreRenderContext - saves the EfzRender* pointer
// (dword_100A0778) before entering tournament mode, and writes it back when
// needed.  Tournament cleanup code (sub_1006CC30) zeros this global before
// calling ExitProcess, and init(2,102) can zero it again.  The EfzRender
// object itself lives in EFZ.exe and its pointer never changes, so
// restoring the saved value is always safe.
// ---------------------------------------------------------------------------
static uintptr_t g_savedRenderContext = 0;
static bool      g_renderContextSaved = false;

bool SaveRenderContext()
{
    if (g_activeRevival == nullptr)
    {
        mod::Log("SaveRenderContext: skipped (no active Revival profile)");
        return false;
    }
    if (g_activeRevival->renderContextGlobalOffset == 0)
    {
        mod::Log(
            "SaveRenderContext: skipped version=%s renderContextGlobalOffset=0",
            g_activeRevival->versionTag != nullptr ? g_activeRevival->versionTag : "(null)");
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("SaveRenderContext: skipped (EfzRevival.dll not loaded)");
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t addr = base + g_activeRevival->renderContextGlobalOffset;
    uintptr_t value = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(addr), &value)
        || value == 0)
    {
        mod::Log("SaveRenderContext: dword_100A0778 is NULL or unreadable");
        return false;
    }

    g_savedRenderContext = value;
    g_renderContextSaved = true;

    // H2 diagnostic: log the init-once guard to show whether future init()
    // calls will refresh the render context (guard==0) or skip it (guard!=0).
    int initOnceGuard = -1;
    if (g_activeRevival->initOnceGuardOffset != 0)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initOnceGuardOffset),
            &initOnceGuard);
    }
    mod::Log("SaveRenderContext: saved EfzRender* 0x%08lX from offset 0x%lX "
             "initOnceGuard=0x%08X (H2: %s)",
             static_cast<unsigned long>(value),
             static_cast<unsigned long>(g_activeRevival->renderContextGlobalOffset),
             static_cast<unsigned>(initOnceGuard),
             (initOnceGuard & 0xFF) != 0
                 ? "guard SET - init() will NOT refresh renderCtx"
                 : "guard CLEAR - init() will refresh renderCtx");
    return true;
}

static bool RestoreRenderContextInternal(bool consumeSaved)
{
    if (g_savedRenderContext == 0)
    {
        return false;
    }

    if (g_activeRevival == nullptr
        || g_activeRevival->renderContextGlobalOffset == 0)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    if (!g_renderContextSaved)
    {
        mod::Log(
            "RestoreRenderContext: using retained EfzRender* 0x%08lX "
            "after saved flag was consumed",
            static_cast<unsigned long>(g_savedRenderContext));
    }

    // H2 diagnostic: read current render context before we overwrite it
    uintptr_t currentRenderCtx = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->renderContextGlobalOffset),
        &currentRenderCtx);

    auto* ptr = reinterpret_cast<uintptr_t*>(
        base + g_activeRevival->renderContextGlobalOffset);
    *ptr = g_savedRenderContext;
    mod::Log("RestoreRenderContext: restored EfzRender* 0x%08lX (was 0x%08lX, %s)",
             static_cast<unsigned long>(g_savedRenderContext),
             static_cast<unsigned long>(currentRenderCtx),
             currentRenderCtx == 0 ? "was NULL - H2 confirmed stale!"
                                   : (currentRenderCtx == g_savedRenderContext
                                          ? "unchanged"
                                          : "was different"));
    if (consumeSaved)
    {
        g_renderContextSaved = false;
    }
    return true;
}

bool RestoreRenderContext()
{
    return RestoreRenderContextInternal(/*consumeSaved=*/true);
}

bool RestoreRenderContextForGameplayExitCleanup()
{
    return RestoreRenderContextInternal(/*consumeSaved=*/false);
}

void MarkRenderContextConsumedForGameplayExitCleanup()
{
    g_renderContextSaved = false;
}

struct Revival102jTextState
{
    uintptr_t render;
    uintptr_t font;
    uintptr_t queueSentinel;
    uintptr_t queueHead;
    uint32_t queueCount;
    uint8_t enabled;
    bool renderRead;
    bool fontRead;
    bool enabledRead;
    bool queueRead;
    bool queueHeadRead;
    bool queueCountRead;
};

static Revival102jTextState ReadRevival102jTextState()
{
    Revival102jTextState state = {};
    if (!IsRevival102jProfile() || g_activeRevival == nullptr)
    {
        return state;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return state;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    state.renderRead = SafeReadPtr(
        reinterpret_cast<const void*>(
            base + g_activeRevival->renderContextGlobalOffset),
        &state.render);
    if (!state.renderRead || state.render == 0)
    {
        return state;
    }

    state.fontRead = SafeReadPtr(
        reinterpret_cast<const void*>(state.render + 0x74u),
        &state.font);
    state.enabledRead = SafeReadByte(
        reinterpret_cast<const void*>(state.render + 0x78u),
        &state.enabled);
    state.queueRead = SafeReadPtr(
        reinterpret_cast<const void*>(state.render + 0x7Cu),
        &state.queueSentinel);
    if (state.queueRead && state.queueSentinel != 0)
    {
        state.queueHeadRead = SafeReadPtr(
            reinterpret_cast<const void*>(state.queueSentinel),
            &state.queueHead);
        state.queueCountRead = SafeReadDword(
            reinterpret_cast<const void*>(state.queueSentinel + 8u),
            &state.queueCount);
    }
    return state;
}

static void LogRevival102jTextState(const char* context)
{
    if (!BridgePatchVerboseLoggingEnabled() || !IsRevival102jProfile())
    {
        return;
    }

    const Revival102jTextState state = ReadRevival102jTextState();
    mod::Log(
        "REVIVAL_TEXT_STATE[%s] render=0x%08lX read=%d font=0x%08lX "
        "fontRead=%d enabled=%u enabledRead=%d queue=0x%08lX "
        "queueRead=%d head=0x%08lX headRead=%d count=%lu countRead=%d",
        context != nullptr ? context : "?",
        static_cast<unsigned long>(state.render),
        state.renderRead ? 1 : 0,
        static_cast<unsigned long>(state.font),
        state.fontRead ? 1 : 0,
        static_cast<unsigned>(state.enabled),
        state.enabledRead ? 1 : 0,
        static_cast<unsigned long>(state.queueSentinel),
        state.queueRead ? 1 : 0,
        static_cast<unsigned long>(state.queueHead),
        state.queueHeadRead ? 1 : 0,
        static_cast<unsigned long>(state.queueCount),
        state.queueCountRead ? 1 : 0);
}

bool ClearRevivalTextWithCurrentRenderContext()
{
    if (g_activeRevival == nullptr
        || g_activeRevival->clearTextRva == 0)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t fnAddr = base + g_activeRevival->clearTextRva;
    void* contextBase = reinterpret_cast<void*>(
        base + g_activeRevival->renderContextBaseOffset);

    LogRevival102jTextState("clear_before");
    __try
    {
        if (g_activeRevival->renderContextBaseOffset != 0
            && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0)
        {
            typedef void(__fastcall* ClearTextWithContextFn)(void* thisPtr, void* edx);
            auto clearFn = reinterpret_cast<ClearTextWithContextFn>(fnAddr);
            clearFn(contextBase, nullptr);
        }
        else
        {
            typedef void(__cdecl* ClearTextFn)();
            auto clearFn = reinterpret_cast<ClearTextFn>(fnAddr);
            clearFn();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("ClearRevivalText: SEH exception 0x%08lX at 0x%08lX",
                 static_cast<unsigned long>(GetExceptionCode()),
                 static_cast<unsigned long>(fnAddr));
        return false;
    }

    return true;
}

bool DisableRevivalTextRenderingWithCurrentRenderContext();

// ---------------------------------------------------------------------------
// ClearRevivalText - clear the Revival text overlay buffer.
//
// The EfzRender object lives in EFZ.exe memory. Revival's wrapper function
// EFZ_Render_ClearText (RVA clearTextRva = 0x6C070) reads the global
// dword_100A0778 and issues a direct IAT call to EfzRender::clearTextRender.
//
// Tournament cleanup code (sub_1006CC30) zeroes dword_100A0778 before calling
// ExitProcess, and init(2,102) may zero it again.  We restore it from the
// saved value each time before calling clearTextRender so it always sees a
// valid pointer.
//
// The call is wrapped in SEH as a safety net - if the EfzRender object is
// in a bad state, we log and return false instead of crashing.
// ---------------------------------------------------------------------------
bool ClearRevivalText()
{
    if (g_activeRevival == nullptr
        || g_activeRevival->clearTextRva == 0)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    // Restore the EfzRender* global if it was zeroed by cleanup code.
    if (g_savedRenderContext == 0)
    {
        (void)SaveRenderContext();
    }
    if (!RestoreRenderContext())
    {
        mod::Log("ClearRevivalText: failed to restore render context");
        return false;
    }

    return ClearRevivalTextWithCurrentRenderContext();
}

static bool RevivalTextRenderingHelperAvailable()
{
    if (g_activeRevival == nullptr
        || g_activeRevival->setTextEnabledRva == 0
        || (g_activeRevival->renderContextBaseOffset == 0
            && g_activeRevival->renderContextGlobalOffset < 0x18))
    {
        return false;
    }

    LogRevival102jTextState("clear_after");
    return true;
}

static bool SetRevivalTextRenderingEnabledWithCurrentRenderContextInternal(
    bool enable,
    const char* reason)
{
    const char* const reasonTag =
        (reason != nullptr && reason[0] != '\0') ? reason : "unknown";
    if (!RevivalTextRenderingHelperAvailable())
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t fnAddr = base + g_activeRevival->setTextEnabledRva;
    const uintptr_t contextBaseOffset =
        g_activeRevival->renderContextBaseOffset != 0
            ? g_activeRevival->renderContextBaseOffset
            : g_activeRevival->renderContextGlobalOffset - 0x18;
    void* contextBase = reinterpret_cast<void*>(
        base + contextBaseOffset);

    LogRevival102jTextState(enable ? "set_enabled_before" : "set_disabled_before");
    __try
    {
        // __thiscall: this in ECX, bool arg on stack.
        // Use __fastcall with a dummy EDX parameter.
        typedef void(__fastcall* SetTextEnabledFn)(void* thisPtr, void* edx, int enable);
        auto setFn = reinterpret_cast<SetTextEnabledFn>(fnAddr);
        setFn(contextBase, nullptr, enable ? 1 : 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "SetRevivalTextRendering: SEH exception 0x%08lX at 0x%08lX "
            "enable=%d reason=%s",
            static_cast<unsigned long>(GetExceptionCode()),
            static_cast<unsigned long>(fnAddr),
            enable ? 1 : 0,
            reasonTag);
        return false;
    }

    LogRevival102jTextState(enable ? "set_enabled_after" : "set_disabled_after");
    mod::Log(
        "SetRevivalTextRendering: text rendering %s reason=%s "
        "fn=0x%08lX context=0x%08lX",
        enable ? "enabled" : "disabled",
        reasonTag,
        static_cast<unsigned long>(fnAddr),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(contextBase)));

    if (IsRevival102jProfile())
    {
        const Revival102jTextState state = ReadRevival102jTextState();
        const uint8_t expected = enable ? 1u : 0u;
        if (!state.enabledRead || state.enabled != expected)
        {
            mod::Log(
                "SetRevivalTextRendering: 1.02j readback FAILED "
                "expected=%u actual=%u readable=%d reason=%s",
                static_cast<unsigned>(expected),
                static_cast<unsigned>(state.enabled),
                state.enabledRead ? 1 : 0,
                reasonTag);
            return false;
        }
    }
    return true;
}

bool SetRevivalTextRenderingEnabled(bool enable, const char* reason)
{
    const char* const reasonTag =
        (reason != nullptr && reason[0] != '\0') ? reason : "unknown";
    if (!RevivalTextRenderingHelperAvailable())
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    // Ensure the EfzRender* global is valid (init(2,102) may have zeroed it).
    if (g_savedRenderContext == 0)
    {
        (void)SaveRenderContext();
    }
    if (!RestoreRenderContext())
    {
        mod::Log(
            "SetRevivalTextRendering: failed to restore render context "
            "enable=%d reason=%s",
            enable ? 1 : 0,
            reasonTag);
        return false;
    }

    return SetRevivalTextRenderingEnabledWithCurrentRenderContextInternal(
        enable,
        reasonTag);
}

// ---------------------------------------------------------------------------
// DisableRevivalTextRendering - disable the EFZ.exe text overlay.
//
// This mirrors the logic that the character-select mode transition
// (sub_10078600) uses to hide tournament nicknames / win counts:
//
//     EFZ_Render_SetTextEnabled(&dword_100A0760, false);
//
// The DLL wrapper at setTextEnabledRva (sub_1006C030) is __thiscall:
//     void __thiscall SetTextEnabled(void* contextBase, bool enable)
// where contextBase is &dword_100A0760 (renderContextGlobalOffset - 0x18).
// It internally calls EfzRender::setRenderText(contextBase[6], enable)
// via the IAT, so dword_100A0778 must be valid (call RestoreRenderContext
// first).
// ---------------------------------------------------------------------------
bool DisableRevivalTextRendering()
{
    return SetRevivalTextRenderingEnabled(
        false,
        "DisableRevivalTextRendering");
}

bool ResetRevivalTextRenderingAfterCleanup(const char* reason)
{
    const char* const reasonTag =
        (reason != nullptr && reason[0] != '\0')
            ? reason
            : "ResetRevivalTextRenderingAfterCleanup";
    return SetRevivalTextRenderingEnabled(
        IsRevival102jProfile(),
        reasonTag);
}

bool ShouldRepeatPostExitTextCleanup()
{
    // 1.02j's native frame driver clears and rebuilds this list every frame.
    // Re-clearing from title updates can erase a newly-entered session's text.
    return !IsRevival102jProfile();
}

bool SetRevivalTextRenderingEnabledWithCurrentRenderContext(
    bool enable,
    const char* reason)
{
    return SetRevivalTextRenderingEnabledWithCurrentRenderContextInternal(
        enable,
        reason);
}

bool DisableRevivalTextRenderingWithCurrentRenderContext()
{
    return SetRevivalTextRenderingEnabledWithCurrentRenderContextInternal(
        false,
        "DisableRevivalTextRenderingWithCurrentRenderContext");
}

bool ResetRevivalTextRenderingAfterCleanupWithCurrentRenderContext(
    const char* reason)
{
    const char* const reasonTag =
        (reason != nullptr && reason[0] != '\0')
            ? reason
            : "ResetRevivalTextRenderingAfterCleanupWithCurrentRenderContext";
    return SetRevivalTextRenderingEnabledWithCurrentRenderContextInternal(
        IsRevival102jProfile(),
        reasonTag);
}

namespace
{
struct RevivalGraphicsPatchSite
{
    uintptr_t addr;
    uint8_t disabledBytes[5];
};

constexpr RevivalGraphicsPatchSite kRevivalGraphicsPatchSites[] = {
    {0x00409A90u, {0xE9, 0x17, 0x03, 0x00, 0x00}},
    {0x0040A0B0u, {0xE9, 0x79, 0x03, 0x00, 0x00}},
    {0x0040A440u, {0xE9, 0x59, 0x03, 0x00, 0x00}},
    {0x0040A7B0u, {0xE9, 0x1E, 0x03, 0x00, 0x00}},
    {0x0040B300u, {0xE9, 0x2C, 0x01, 0x00, 0x00}},
    {0x0040B44Cu, {0xE9, 0x76, 0x01, 0x00, 0x00}},
    {0x0040AAE0u, {0xE9, 0x20, 0x03, 0x00, 0x00}},
};

constexpr uintptr_t kRevival102jMemorialPauseIntegrationRva = 0x000777A0u;
constexpr uintptr_t kRevival102jGraphicsPatchStateOffset = 21u;

uintptr_t ResolveEfzImageVaForPatchVerify(uintptr_t va)
{
    constexpr uintptr_t kEfzImageBase = 0x00400000u;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (base == 0 || va < kEfzImageBase)
    {
        return va;
    }
    return base + (va - kEfzImageBase);
}

bool ReadFiveBytesForPatchVerify(uintptr_t address, uint8_t* outBytes)
{
    if (outBytes == nullptr)
    {
        return false;
    }

    for (size_t i = 0; i < 5; ++i)
    {
        if (!SafeReadByte(
                reinterpret_cast<const void*>(address + i),
                &outBytes[i]))
        {
            return false;
        }
    }
    return true;
}

bool VerifyRevivalGraphicsPatchSitesEnabled(const char* reason)
{
    bool allOk = true;
    for (size_t i = 0; i < sizeof(kRevivalGraphicsPatchSites) / sizeof(kRevivalGraphicsPatchSites[0]); ++i)
    {
        const RevivalGraphicsPatchSite& site = kRevivalGraphicsPatchSites[i];
        const uintptr_t runtimeAddr = ResolveEfzImageVaForPatchVerify(site.addr);
        uint8_t actual[5] = {};
        const bool readOk = ReadFiveBytesForPatchVerify(runtimeAddr, actual);
        const bool matchesDisabled =
            readOk
            && actual[0] == site.disabledBytes[0]
            && actual[1] == site.disabledBytes[1]
            && actual[2] == site.disabledBytes[2]
            && actual[3] == site.disabledBytes[3]
            && actual[4] == site.disabledBytes[4];
        const bool ok = readOk && !matchesDisabled;
        allOk = allOk && ok;
        mod::Log(
            "REVIVAL_GRAPHICS_PATCH_SITE_VERIFY addr=0x%08lX expected=not_%02X%02X%02X%02X%02X actual=%02X%02X%02X%02X%02X ok=%d reason=%s",
            static_cast<unsigned long>(site.addr),
            site.disabledBytes[0],
            site.disabledBytes[1],
            site.disabledBytes[2],
            site.disabledBytes[3],
            site.disabledBytes[4],
            actual[0],
            actual[1],
            actual[2],
            actual[3],
            actual[4],
            ok ? 1 : 0,
            (reason != nullptr && reason[0] != '\0') ? reason : "unknown");
    }
    return allOk;
}
}

int GetRevivalGraphicsPatchState()
{
    if (g_activeRevival == nullptr)
    {
        return -1;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return -1;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    if (IsRevival102jProfile())
    {
        if (g_activeRevival->renderContextBaseOffset == 0)
        {
            return -1;
        }

        uint8_t state = 0;
        if (!SafeReadByte(
                reinterpret_cast<const void*>(
                    base
                    + g_activeRevival->renderContextBaseOffset
                    + kRevival102jGraphicsPatchStateOffset),
                &state))
        {
            return -1;
        }

        return static_cast<int>(state);
    }

    if (g_activeRevival->initOnceGuardOffset == 0)
    {
        return -1;
    }

    uint16_t guard = 0;
    if (!SafeReadWord(
            reinterpret_cast<const void*>(
                base + g_activeRevival->initOnceGuardOffset),
            &guard))
    {
        return -1;
    }

    return static_cast<int>((guard >> 8) & 0x00FFu);
}

static bool ApplyRevival102jGraphicsPatchSetEnabled(
    const char* reasonTag,
    uintptr_t revivalBase,
    int stateBefore)
{
    if (g_activeRevival == nullptr
        || g_activeRevival->renderContextBaseOffset == 0)
    {
        mod::Log(
            "REVIVAL_GRAPHICS_PATCH_RESTORE_APPLY_102J_SKIPPED reason=%s "
            "renderContextBaseOffset=0x%08lX",
            reasonTag,
            static_cast<unsigned long>(
                g_activeRevival != nullptr
                    ? g_activeRevival->renderContextBaseOffset
                    : 0));
        return false;
    }

    void* const patchContext = reinterpret_cast<void*>(
        revivalBase + g_activeRevival->renderContextBaseOffset);
    const uintptr_t fnAddr =
        revivalBase + kRevival102jMemorialPauseIntegrationRva;

    mod::Log(
        "REVIVAL_GRAPHICS_PATCH_RESTORE_APPLY_102J_BEGIN reason=%s "
        "fn=0x%08lX patchCtx=0x%08lX stateBefore=%d",
        reasonTag,
        static_cast<unsigned long>(fnAddr),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(patchContext)),
        stateBefore);

    __try
    {
        // 1.02j is MinGW-built, but this helper still receives patch_ctx in
        // ECX and the enable flag on the stack.
        using TogglePatchSetFn =
            void(__fastcall*)(void* thisPtr, void* edx, int enable);
        auto togglePatchSet = reinterpret_cast<TogglePatchSetFn>(fnAddr);
        togglePatchSet(patchContext, nullptr, 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "REVIVAL_GRAPHICS_PATCH_RESTORE_APPLY_102J_EXCEPTION reason=%s "
            "code=0x%08lX fn=0x%08lX patchCtx=0x%08lX",
            reasonTag,
            static_cast<unsigned long>(GetExceptionCode()),
            static_cast<unsigned long>(fnAddr),
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(patchContext)));
        return false;
    }

    const int stateAfter = GetRevivalGraphicsPatchState();
    mod::Log(
        "REVIVAL_GRAPHICS_PATCH_RESTORE_APPLY_102J_END reason=%s "
        "stateAfter=%d",
        reasonTag,
        stateAfter);
    return stateAfter == 1;
}

bool EnsureRevivalGraphicsPatchSetEnabled(const char* reason)
{
    const char* reasonTag =
        (reason != nullptr && reason[0] != '\0') ? reason : "unknown";
    const int stateBefore = GetRevivalGraphicsPatchState();
    mod::Log(
        "REVIVAL_GRAPHICS_PATCH_RESTORE_CHECK reason=%s stateBefore=%d",
        reasonTag,
        stateBefore);

    bool applyOk = stateBefore == 1;
    if (stateBefore != 1)
    {
        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival != nullptr && g_activeRevival != nullptr)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
            if (IsRevival102jProfile())
            {
                applyOk = ApplyRevival102jGraphicsPatchSetEnabled(
                    reasonTag,
                    base,
                    stateBefore);
            }
            else if (g_activeRevival->clearTextRva != 0)
            {
                // In the 1.02e/1.02i decomp, EFZ_Patch_ToggleSet is at
                // clearTextRva + 0x6D0. Keep that legacy helper isolated
                // from 1.02j, whose MinGW refactor moved this code.
                const uintptr_t fnAddr =
                    base
                    + g_activeRevival->clearTextRva
                    + 0x6D0u;
                __try
                {
                    using TogglePatchSetFn =
                        char(__thiscall*)(void* context, char enable);
                    auto togglePatchSet =
                        reinterpret_cast<TogglePatchSetFn>(fnAddr);
                    const char rawResult = togglePatchSet(nullptr, 1);
                    // The legacy toggle helper returns its patch-map
                    // iteration predicate, which is 0 after a COMPLETED
                    // full restore - the raw byte is not a success flag.
                    // Success is judged by the verified postcondition
                    // (stateAfter/sitesOk) below; keep the raw byte as
                    // diagnostics only.
                    applyOk = true;
                    mod::Log(
                        "REVIVAL_GRAPHICS_PATCH_RESTORE_APPLY_LEGACY reason=%s "
                        "rawResult=%d (diagnostic only)",
                        reasonTag,
                        static_cast<int>(rawResult));
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    applyOk = false;
                    mod::Log(
                        "REVIVAL_GRAPHICS_PATCH_RESTORE_APPLY_EXCEPTION reason=%s code=0x%08lX",
                        reasonTag,
                        static_cast<unsigned long>(GetExceptionCode()));
                }
            }
        }
    }

    mod::Log(
        "REVIVAL_GRAPHICS_PATCH_RESTORE_APPLY reason=%s result=%d stateBefore=%d",
        reasonTag,
        applyOk ? 1 : 0,
        stateBefore);

    const int stateAfter = GetRevivalGraphicsPatchState();
    const bool sitesOk = VerifyRevivalGraphicsPatchSitesEnabled(reasonTag);
    const bool ok = applyOk && stateAfter == 1 && sitesOk;
    mod::Log(
        "REVIVAL_GRAPHICS_PATCH_RESTORE_VERIFY reason=%s stateAfter=%d ok=%d",
        reasonTag,
        stateAfter,
        ok ? 1 : 0);
    return ok;
}

void ResetDebugCounters(SharedBlock* block)
{
    if (block == nullptr)
    {
        return;
    }
    block->delayPromptSerial = 0;
    block->delayPromptServedSerial = 0;
    block->delayMetricsSerial = 0;
    block->delayAveragePingMs = -1;
    block->delayMinPingMs = -1;
    block->delayMaxPingMs = -1;
    block->delayRecommended = -1;
    block->delayRangeMin = 0;
    block->delayRangeMax = 20;
    block->delayInputSerial = 0;
    block->delayInputServedSerial = 0;
    block->delayInputValue = -1;
    block->spectateConfirmPromptSerial = 0;
    block->spectateConfirmPromptServedSerial = 0;
    block->spectateConfirmPromptKind = 0;
    block->spectateConfirmInputSerial = 0;
    block->spectateConfirmInputServedSerial = 0;
    block->spectateConfirmInputValue = 0;
    block->dbgReadConsoleHits = 0;
    block->dbgReadConsoleAutoHits = 0;
    block->dbgCreateProcessHits = 0;
    block->dbgWriteProcessHits = 0;
    block->dbgCreateRemoteThreadHits = 0;
}

bool InvokeStartInitPlayer(int initMode)
{
    if (initMode != kLocalRoleOnline)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("Takeover: InvokeStartInitPlayer skipped (DLL not loaded)");
        return false;
    }

    if (!ActiveRevivalProfileSupportsSessionStart())
    {
        const char* const tag =
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "(null)";
        const uintptr_t sessionOffset =
            (g_activeRevival != nullptr && g_activeRevival->sessionPtrOffsetCount > 0)
                ? g_activeRevival->sessionPtrOffsets[0]
                : 0;
        mod::Log(
            "Takeover: InvokeStartInitPlayer skipped (unsupported profile version=%s "
            "sessionCount=%u session0=0x%08lX startInit=0x%08lX)",
            tag,
            (g_activeRevival != nullptr) ? static_cast<unsigned>(g_activeRevival->sessionPtrOffsetCount) : 0u,
            static_cast<unsigned long>(sessionOffset),
            static_cast<unsigned long>((g_activeRevival != nullptr) ? g_activeRevival->startInitPlayerRva : 0));
        return false;
    }

    const uintptr_t dllBase = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t fnAddr = dllBase + g_activeRevival->startInitPlayerRva;

    // Read the session pointer DIRECTLY from the DLL's global variable
    // (dword_100A02CC at RVA 0xA02CC) without the heuristic validation that
    // ReadSessionPointerFromRevival() performs.  That validation can return a
    // stale / wrong pointer from a previous session or from unrelated memory
    // that happens to pass the vtable + delay/ping heuristic.  Since we only
    // call this immediately after init() stored the freshly-allocated session
    // into that global, a raw read is both correct and sufficient.
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(dllBase + g_activeRevival->sessionPtrOffsets[0]), &sessionPtr)
        || sessionPtr == 0 || sessionPtr < 0x00100000u)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer skipped (session pointer unavailable dllBase=0x%08lX raw=0x%08lX)",
            static_cast<unsigned long>(dllBase),
            static_cast<unsigned long>(sessionPtr));
        return false;
    }

    // Verify the session's initComplete flag is still 0 (un-initialized) on
    // builds that still have that field.  1.02j removed the DWORD, so offset 0
    // means "not available" rather than "read from session vtable".
    int initComplete = -1;
    const bool hasInitComplete = g_activeRevival->sessionOffsetInitComplete != 0;
    if (hasInitComplete
        && SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInitComplete), &initComplete)
        && initComplete == 1)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer skipped (already initialized session=0x%08lX initComplete=%d)",
            static_cast<unsigned long>(sessionPtr),
            initComplete);
        return true;
    }

    if (hasInitComplete)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer calling StartInitPlayer version=%s session=0x%08lX fn=0x%08lX initComplete=%d",
            g_activeRevival->versionTag,
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(fnAddr),
            initComplete);
    }
    else
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer calling StartInitPlayer version=%s session=0x%08lX fn=0x%08lX initComplete=n/a",
            g_activeRevival->versionTag,
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(fnAddr));
    }

    NormalizeRevivalFpuState("pre_start_init_player", sessionPtr);

    __try
    {
        const bool isRevival102j =
            g_activeRevival->versionTag != nullptr
            && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0;
        if (isRevival102j)
        {
            // 1.02j is a MinGW refactor build.  The same logical method is
            // emitted as __fastcall with the object pointer in ECX.
            typedef void (__fastcall *StartInitPlayerFastcallFn)(void* session, void* unusedEdx);
            StartInitPlayerFastcallFn fn = reinterpret_cast<StartInitPlayerFastcallFn>(fnAddr);
            fn(reinterpret_cast<void*>(sessionPtr), nullptr);
        }
        else
        {
            // Older Revival builds are MSVC __thiscall: ECX = this, no
            // explicit first parameter on the stack.
            typedef void (__thiscall *StartInitPlayerThiscallFn)(void* session);
            StartInitPlayerThiscallFn fn = reinterpret_cast<StartInitPlayerThiscallFn>(fnAddr);
            fn(reinterpret_cast<void*>(sessionPtr));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer EXCEPTION session=0x%08lX fn=0x%08lX",
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(fnAddr));
        return false;
    }

    NormalizeRevivalFpuState("post_start_init_player", sessionPtr);

    // Verify the session is now marked as initialized.
    int postInitComplete = hasInitComplete ? -1 : 1;
    if (hasInitComplete)
    {
        (void)SafeReadInt(reinterpret_cast<const void*>(
            sessionPtr + g_activeRevival->sessionOffsetInitComplete), &postInitComplete);
    }

    int activePlayer = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer), &activePlayer);
    if (hasInitComplete)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer completed session=0x%08lX initComplete=%d activePlayer=%d",
            static_cast<unsigned long>(sessionPtr),
            postInitComplete,
            activePlayer);
    }
    else
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer completed session=0x%08lX initComplete=n/a activePlayer=%d",
            static_cast<unsigned long>(sessionPtr),
            activePlayer);
    }

    return !hasInitComplete || postInitComplete == 1;
}

void StabilizeOnlineSessionBindingAfterInit(int initMode)
{
    if (initMode != kLocalRoleOnline)
    {
        return;
    }

    if (g_revivalProcessId == 0)
    {
        mod::Log("Takeover: post-init session binding skipped (helper pid unavailable)");
        return;
    }

    bool usedCachedSessionPtr = false;
    const uintptr_t sessionPtr = ReadSessionPointerForMutation(&usedCachedSessionPtr);
    if (sessionPtr == 0)
    {
        mod::Log(
            "Takeover: post-init session binding skipped (session pointer unavailable role=%d helperPid=%lu strict=0 cached=0x%08lX)",
            initMode,
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(g_lastValidatedSessionPtr));
        return;
    }

    const char* const sessionPtrSource = usedCachedSessionPtr ? "cached" : "strict";

    int helperPidField = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid), &helperPidField);

    uintptr_t helperHandleFieldRaw = 0;
    (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle), &helperHandleFieldRaw);
    HANDLE helperHandleField = reinterpret_cast<HANDLE>(helperHandleFieldRaw);

    DWORD helperHandlePid = 0;
    DWORD helperHandleExitCode = 0xFFFFFFFFu;
    if (helperHandleField != nullptr && helperHandleField != INVALID_HANDLE_VALUE)
    {
        helperHandlePid = GetProcessId(helperHandleField);
        if (!GetExitCodeProcess(helperHandleField, &helperHandleExitCode))
        {
            helperHandleExitCode = 0xFFFFFFFEu;
        }
    }
    const DWORD expectedPid = g_revivalProcessId;
    const bool pidMatches = helperPidField == static_cast<int>(expectedPid);
    const bool handleMatches =
        helperHandleField != nullptr
        && helperHandleField != INVALID_HANDLE_VALUE
        && helperHandlePid == expectedPid;

    bool patchedPid = false;
    bool patchedHandle = false;
    HANDLE replacementHandle = nullptr;
    bool replacementIsOwnedHandle = false;

    if (!pidMatches || !handleMatches)
    {
        const void* const pidFieldAddress = reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid);
        const void* const handleFieldAddress = reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle);
        if (!IsWritableRange(const_cast<void*>(pidFieldAddress), sizeof(int))
            || !IsWritableRange(const_cast<void*>(handleFieldAddress), sizeof(uintptr_t)))
        {
            mod::Log(
                "Takeover: post-init session binding skipped (fields not writable session=0x%08lX pidWritable=%d handleWritable=%d)",
                static_cast<unsigned long>(sessionPtr),
                IsWritableRange(const_cast<void*>(pidFieldAddress), sizeof(int)) ? 1 : 0,
                IsWritableRange(const_cast<void*>(handleFieldAddress), sizeof(uintptr_t)) ? 1 : 0);
            return;
        }

        if (!handleMatches)
        {
            replacementHandle = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, expectedPid);
            replacementIsOwnedHandle = (replacementHandle != nullptr);
            if (replacementHandle == nullptr && g_revivalProcess != nullptr)
            {
                const DWORD hostHandlePid = GetProcessId(g_revivalProcess);
                if (hostHandlePid == expectedPid)
                {
                    HANDLE duplicated = nullptr;
                    if (DuplicateHandle(
                            GetCurrentProcess(),
                            g_revivalProcess,
                            GetCurrentProcess(),
                            &duplicated,
                            PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                            FALSE,
                            0))
                    {
                        replacementHandle = duplicated;
                        replacementIsOwnedHandle = true;
                    }
                    else
                    {
                        replacementHandle = g_revivalProcess;
                        replacementIsOwnedHandle = false;
                    }
                }
            }
        }

        __try
        {
            if (!pidMatches)
            {
                *reinterpret_cast<int*>(const_cast<void*>(pidFieldAddress)) = static_cast<int>(expectedPid);
                patchedPid = true;
            }
            if (!handleMatches && replacementHandle != nullptr)
            {
                *reinterpret_cast<uintptr_t*>(const_cast<void*>(handleFieldAddress)) = reinterpret_cast<uintptr_t>(replacementHandle);
                patchedHandle = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (replacementIsOwnedHandle && replacementHandle != nullptr)
            {
                CloseHandle(replacementHandle);
            }
            mod::Log("Takeover: post-init session binding patch raised exception");
            return;
        }

        if (patchedHandle
            && helperHandleField != nullptr
            && helperHandleField != INVALID_HANDLE_VALUE
            && helperHandleField != g_revivalProcess
            && helperHandleField != replacementHandle)
        {
            CloseHandle(helperHandleField);
        }
        else if (!patchedHandle && replacementIsOwnedHandle && replacementHandle != nullptr)
        {
            CloseHandle(replacementHandle);
        }

        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid), &helperPidField);
        helperHandleFieldRaw = 0;
        (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle), &helperHandleFieldRaw);
        helperHandleField = reinterpret_cast<HANDLE>(helperHandleFieldRaw);
        helperHandlePid = 0;
        helperHandleExitCode = 0xFFFFFFFFu;
        if (helperHandleField != nullptr && helperHandleField != INVALID_HANDLE_VALUE)
        {
            helperHandlePid = GetProcessId(helperHandleField);
            if (!GetExitCodeProcess(helperHandleField, &helperHandleExitCode))
            {
                helperHandleExitCode = 0xFFFFFFFEu;
            }
        }
    }

    mod::Log(
        "Takeover: post-init session binding mode=%d source=%s session=0x%08lX pidField=%d expectedPid=%lu handle=0x%08lX handlePid=%lu handleExit=%ld patched(pid=%d handle=%d)",
        initMode,
        sessionPtrSource,
        static_cast<unsigned long>(sessionPtr),
        helperPidField,
        static_cast<unsigned long>(expectedPid),
        static_cast<unsigned long>(helperHandleFieldRaw),
        static_cast<unsigned long>(helperHandlePid),
        static_cast<long>(helperHandleExitCode),
        patchedPid ? 1 : 0,
        patchedHandle ? 1 : 0);
}

void RepairRollbackHistoryBindingsIfNeeded()
{
    if (!g_localInitAppliedForSession)
    {
        return;
    }

    // Only online sessions (mode 0) have the rollback history bindings.
    // Spectator sessions (mode 1) have a different, smaller layout where
    // the online-session offsets (activePlayer, queuePlayer, historyPtrs)
    // overlap with the Config object.  Running this repair on a spectator
    // session would write heap addresses into Config WString fields,
    // corrupting them and likely crashing on the next string operation.
    if (g_localRoleFlag != kLocalRoleOnline)
    {
        return;
    }

    bool usedCachedSessionPtr = false;
    const uintptr_t sessionPtr = ReadSessionPointerForMutation(&usedCachedSessionPtr);
    if (sessionPtr == 0)
    {
        return;
    }
    const char* const sessionPtrSource = usedCachedSessionPtr ? "cached" : "strict";

    int activePlayer = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer), &activePlayer))
    {
        return;
    }
    if (activePlayer != 0 && activePlayer != 1)
    {
        return;
    }

    int queuePlayer = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer), &queuePlayer);

    uintptr_t historyPrimaryPtr = 0;
    uintptr_t historySecondaryPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryPtr), &historyPrimaryPtr)
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryPtr), &historySecondaryPtr))
    {
        return;
    }

    const uintptr_t expectedPrimary =
        sessionPtr + ((activePlayer == 0) ? g_activeRevival->sessionOffsetHistoryPrimaryVec : g_activeRevival->sessionOffsetHistorySecondaryVec);
    const uintptr_t expectedSecondary =
        sessionPtr + ((activePlayer == 0) ? g_activeRevival->sessionOffsetHistorySecondaryVec : g_activeRevival->sessionOffsetHistoryPrimaryVec);
    const int expectedQueuePlayer = (activePlayer == 0) ? 1 : 0;

    const bool queuePlayerInvalid = (queuePlayer != 0 && queuePlayer != 1) || queuePlayer != expectedQueuePlayer;
    const bool needsRepair =
        (historyPrimaryPtr != expectedPrimary)
        || (historySecondaryPtr != expectedSecondary)
        || queuePlayerInvalid;
    if (!needsRepair)
    {
        return;
    }

    void* const primaryField = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryPtr);
    void* const secondaryField = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryPtr);
    void* const queueField = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer);
    if (!IsWritableRange(primaryField, sizeof(uintptr_t))
        || !IsWritableRange(secondaryField, sizeof(uintptr_t))
        || !IsWritableRange(queueField, sizeof(int)))
    {
        const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "Takeover: skipped history binding repair source=%s session=0x%08lX writable=%d/%d/%d count=%ld",
                sessionPtrSource,
                static_cast<unsigned long>(sessionPtr),
                IsWritableRange(primaryField, sizeof(uintptr_t)) ? 1 : 0,
                IsWritableRange(secondaryField, sizeof(uintptr_t)) ? 1 : 0,
                IsWritableRange(queueField, sizeof(int)) ? 1 : 0,
                static_cast<long>(hits));
        }
        return;
    }

    __try
    {
        *reinterpret_cast<uintptr_t*>(primaryField) = expectedPrimary;
        *reinterpret_cast<uintptr_t*>(secondaryField) = expectedSecondary;
        *reinterpret_cast<int*>(queueField) = expectedQueuePlayer;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "Takeover: exception while repairing history bindings source=%s session=0x%08lX count=%ld",
                sessionPtrSource,
                static_cast<unsigned long>(sessionPtr),
                static_cast<long>(hits));
        }
        return;
    }

    const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
    if (hits <= 8 || (hits % 64) == 0)
    {
        mod::Log(
            "Takeover: repaired history bindings source=%s session=0x%08lX active=%d queue=%d->%d hist=0x%08lX/0x%08lX -> 0x%08lX/0x%08lX count=%ld",
            sessionPtrSource,
            static_cast<unsigned long>(sessionPtr),
            activePlayer,
            queuePlayer,
            expectedQueuePlayer,
            static_cast<unsigned long>(historyPrimaryPtr),
            static_cast<unsigned long>(historySecondaryPtr),
            static_cast<unsigned long>(expectedPrimary),
            static_cast<unsigned long>(expectedSecondary),
            static_cast<long>(hits));
    }
}

static constexpr uintptr_t kRevival102jRollbackVtableRva = 0x0016FEF0u;
static constexpr size_t kRevival102jRollbackReadInputSlot = 5u;
static constexpr uintptr_t kRevival102jRollbackReadInputRva = 0x00052740u;
static constexpr uintptr_t kRevival102jOffsetLocalSide = 0x308u;
static constexpr uintptr_t kRevival102jOffsetCurrentFrame = 0x324u;
static constexpr uintptr_t kRevival102jOffsetLocalInputs = 0x374u;
static constexpr uintptr_t kRevival102jOffsetRemoteInputs = 0x380u;
static constexpr uintptr_t kRevival102jOffsetPredictedInputs = 0x38Cu;

struct TwoByteVectorSnapshot
{
    uintptr_t begin;
    uintptr_t end;
    uintptr_t capacity;
    int length;
    bool valid;
};

static TwoByteVectorSnapshot ReadTwoByteVectorSnapshot(uintptr_t vectorAddress)
{
    TwoByteVectorSnapshot snapshot = {};
    snapshot.length = -1;

    if (!SafeReadPtr(reinterpret_cast<const void*>(vectorAddress), &snapshot.begin)
        || !SafeReadPtr(reinterpret_cast<const void*>(vectorAddress + 4u), &snapshot.end)
        || !SafeReadPtr(reinterpret_cast<const void*>(vectorAddress + 8u), &snapshot.capacity))
    {
        return snapshot;
    }

    if (snapshot.end < snapshot.begin || snapshot.capacity < snapshot.end)
    {
        return snapshot;
    }

    const uintptr_t byteCount = snapshot.end - snapshot.begin;
    if ((byteCount & 1u) != 0 || byteCount > 0x00200000u)
    {
        return snapshot;
    }

    snapshot.length = static_cast<int>(byteCount >> 1);
    snapshot.valid = true;
    return snapshot;
}

static bool ReadTwoByteVectorLowByte(
    const TwoByteVectorSnapshot& snapshot,
    int index,
    uint8_t* outValue)
{
    if (outValue == nullptr
        || !snapshot.valid
        || snapshot.begin == 0
        || snapshot.length <= 0
        || index < 0
        || index >= snapshot.length)
    {
        return false;
    }

    return SafeReadByte(
        reinterpret_cast<const void*>(
            snapshot.begin + static_cast<uintptr_t>(index) * 2u),
        outValue);
}

static bool WriteSafeInputByte(char* out, uint8_t value)
{
    if (out == nullptr || !IsWritableRange(out, sizeof(uint8_t)))
    {
        return false;
    }

    __try
    {
        *reinterpret_cast<uint8_t*>(out) = value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return true;
}

static volatile LONG g_revival102jSafeInputFallbacks = 0;
static bool g_revival102jSafeReadInputInstalled = false;
static uintptr_t g_revival102jOriginalReadInput = 0;

using Revival102jReadInputFn = char* (__thiscall*)(void*, char*, int);

static void LogRevival102jSafeInputFallback(
    const char* reason,
    uintptr_t sessionPtr,
    int side,
    int localSide,
    int currentFrame,
    int requestedIndex,
    int fallbackIndex,
    uint8_t value,
    const TwoByteVectorSnapshot& localInputs,
    const TwoByteVectorSnapshot& remoteInputs,
    const TwoByteVectorSnapshot& predictedInputs)
{
    const LONG count = InterlockedIncrement(&g_revival102jSafeInputFallbacks);
    if (count > 8 && !BridgePatchVerboseLoggingEnabled() && (count % 128) != 0)
    {
        return;
    }

    mod::Log(
        "REVIVAL_102J_SAFE_INPUT_FALLBACK reason=%s count=%ld session=0x%08lX "
        "side=%d localSide=%d frame=%d requestedIndex=%d fallbackIndex=%d "
        "value=0x%02X localLen=%d remoteLen=%d predictedLen=%d "
        "local=0x%08lX/0x%08lX/0x%08lX remote=0x%08lX/0x%08lX/0x%08lX "
        "predicted=0x%08lX/0x%08lX/0x%08lX",
        reason != nullptr ? reason : "?",
        static_cast<long>(count),
        static_cast<unsigned long>(sessionPtr),
        side,
        localSide,
        currentFrame,
        requestedIndex,
        fallbackIndex,
        static_cast<unsigned>(value),
        localInputs.length,
        remoteInputs.length,
        predictedInputs.length,
        static_cast<unsigned long>(localInputs.begin),
        static_cast<unsigned long>(localInputs.end),
        static_cast<unsigned long>(localInputs.capacity),
        static_cast<unsigned long>(remoteInputs.begin),
        static_cast<unsigned long>(remoteInputs.end),
        static_cast<unsigned long>(remoteInputs.capacity),
        static_cast<unsigned long>(predictedInputs.begin),
        static_cast<unsigned long>(predictedInputs.end),
        static_cast<unsigned long>(predictedInputs.capacity));
}

static char* __fastcall Revival102jSafeReadInputHook(
    void* session,
    void* /*edx*/,
    char* out,
    int side)
{
    const uintptr_t sessionPtr = reinterpret_cast<uintptr_t>(session);
    int localSide = -1;
    int currentFrame = -1;
    int requestedIndex = -1;
    int fallbackIndex = -1;
    uint8_t value = 0;
    bool haveValue = false;
    bool usedFallback = false;
    const char* fallbackReason = "neutral";
    bool originalReadSafe = false;

    TwoByteVectorSnapshot localInputs = {};
    localInputs.length = -1;
    TwoByteVectorSnapshot remoteInputs = {};
    remoteInputs.length = -1;
    TwoByteVectorSnapshot predictedInputs = {};
    predictedInputs.length = -1;

    if (sessionPtr != 0)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + kRevival102jOffsetLocalSide),
            &localSide);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + kRevival102jOffsetCurrentFrame),
            &currentFrame);

        localInputs = ReadTwoByteVectorSnapshot(
            sessionPtr + kRevival102jOffsetLocalInputs);
        remoteInputs = ReadTwoByteVectorSnapshot(
            sessionPtr + kRevival102jOffsetRemoteInputs);
        predictedInputs = ReadTwoByteVectorSnapshot(
            sessionPtr + kRevival102jOffsetPredictedInputs);

        if (currentFrame >= 0)
        {
            if (side == localSide)
            {
                requestedIndex = currentFrame;
                originalReadSafe =
                    localInputs.valid
                    && localInputs.begin != 0
                    && requestedIndex >= 0
                    && requestedIndex < localInputs.length;
                if (!originalReadSafe)
                {
                    fallbackReason = "local_oob";
                }
            }
            else
            {
                if (remoteInputs.valid
                    && remoteInputs.begin != 0
                    && remoteInputs.length > 0)
                {
                    requestedIndex =
                        currentFrame < remoteInputs.length
                            ? currentFrame
                            : (remoteInputs.length - 1);
                    originalReadSafe = true;
                }
                else
                {
                    requestedIndex = remoteInputs.length - 1;
                    fallbackReason =
                        remoteInputs.valid ? "remote_empty" : "remote_invalid";
                }
            }

            if (originalReadSafe && g_revival102jOriginalReadInput != 0)
            {
                auto original =
                    reinterpret_cast<Revival102jReadInputFn>(
                        g_revival102jOriginalReadInput);
                return original(session, out, side);
            }

            if (!haveValue
                && side != localSide
                && predictedInputs.length > 0)
            {
                fallbackIndex =
                    currentFrame < predictedInputs.length
                        ? currentFrame
                        : (predictedInputs.length - 1);
                haveValue = ReadTwoByteVectorLowByte(
                    predictedInputs,
                    fallbackIndex,
                    &value);
                if (haveValue)
                {
                    usedFallback = true;
                    fallbackReason =
                        (fallbackReason != nullptr
                         && std::strcmp(fallbackReason, "remote_empty") == 0)
                            ? "remote_empty_predicted"
                            : "predicted";
                }
            }
        }
    }

    if (!haveValue)
    {
        value = 0;
        usedFallback = true;
        fallbackReason =
            (sessionPtr == 0) ? "null_session" :
            (currentFrame < 0 ? "bad_frame" : "neutral");
    }

    if (!WriteSafeInputByte(out, value))
    {
        LogRevival102jSafeInputFallback(
            "write_failed",
            sessionPtr,
            side,
            localSide,
            currentFrame,
            requestedIndex,
            fallbackIndex,
            value,
            localInputs,
            remoteInputs,
            predictedInputs);
        return out;
    }

    if (usedFallback)
    {
        LogRevival102jSafeInputFallback(
            fallbackReason,
            sessionPtr,
            side,
            localSide,
            currentFrame,
            requestedIndex,
            fallbackIndex,
            value,
            localInputs,
            remoteInputs,
            predictedInputs);
    }

    return out;
}

bool InstallRevival102jSafeInputReadPatch(const char* caller)
{
    if (!IsRevival102jProfile())
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log(
            "REVIVAL_102J_SAFE_INPUT_PATCH_SKIPPED caller=%s reason=dll_missing",
            caller != nullptr ? caller : "?");
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t expectedOriginal = base + kRevival102jRollbackReadInputRva;
    const uintptr_t hook =
        reinterpret_cast<uintptr_t>(&Revival102jSafeReadInputHook);
    const uintptr_t slotAddress =
        base + kRevival102jRollbackVtableRva
        + kRevival102jRollbackReadInputSlot * sizeof(uintptr_t);

    uintptr_t current = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(slotAddress), &current))
    {
        mod::Log(
            "REVIVAL_102J_SAFE_INPUT_PATCH_SKIPPED caller=%s reason=slot_unreadable "
            "slot=0x%08lX",
            caller != nullptr ? caller : "?",
            static_cast<unsigned long>(slotAddress));
        return false;
    }

    if (current == hook)
    {
        g_revival102jSafeReadInputInstalled = true;
        mod::Log(
            "REVIVAL_102J_SAFE_INPUT_PATCH_INSTALLED caller=%s state=already "
            "slot=0x%08lX hook=0x%08lX original=0x%08lX",
            caller != nullptr ? caller : "?",
            static_cast<unsigned long>(slotAddress),
            static_cast<unsigned long>(hook),
            static_cast<unsigned long>(g_revival102jOriginalReadInput));
        return true;
    }

    if (current != expectedOriginal
        && (g_revival102jOriginalReadInput == 0
            || current != g_revival102jOriginalReadInput))
    {
        mod::Log(
            "REVIVAL_102J_SAFE_INPUT_PATCH_SKIPPED caller=%s reason=slot_mismatch "
            "slot=0x%08lX current=0x%08lX expected=0x%08lX hook=0x%08lX",
            caller != nullptr ? caller : "?",
            static_cast<unsigned long>(slotAddress),
            static_cast<unsigned long>(current),
            static_cast<unsigned long>(expectedOriginal),
            static_cast<unsigned long>(hook));
        return false;
    }

    g_revival102jOriginalReadInput = current;

    DWORD oldProtect = 0;
    auto* const slot = reinterpret_cast<uintptr_t*>(slotAddress);
    if (!VirtualProtect(slot, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        mod::Log(
            "REVIVAL_102J_SAFE_INPUT_PATCH_SKIPPED caller=%s reason=protect_failed "
            "slot=0x%08lX err=%lu",
            caller != nullptr ? caller : "?",
            static_cast<unsigned long>(slotAddress),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    __try
    {
        *slot = hook;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(uintptr_t), oldProtect, &ignored);
        mod::Log(
            "REVIVAL_102J_SAFE_INPUT_PATCH_SKIPPED caller=%s reason=write_exception "
            "slot=0x%08lX",
            caller != nullptr ? caller : "?",
            static_cast<unsigned long>(slotAddress));
        return false;
    }

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(uintptr_t), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(uintptr_t));
    g_revival102jSafeReadInputInstalled = true;

    mod::Log(
        "REVIVAL_102J_SAFE_INPUT_PATCH_INSTALLED caller=%s state=patched "
        "slot=0x%08lX original=0x%08lX expected=0x%08lX hook=0x%08lX",
        caller != nullptr ? caller : "?",
        static_cast<unsigned long>(slotAddress),
        static_cast<unsigned long>(current),
        static_cast<unsigned long>(expectedOriginal),
        static_cast<unsigned long>(hook));
    return true;
}

// ---------------------------------------------------------------------------
// NetplayFrameHook - wraps sub_1006E590 (the DLL per-frame dispatcher,
// RVA 0x6E590 in EfzRevival.dll 1.02e) with a setjmp recovery point.
//
// For tournament sessions, Jcc patches make ExitProcess calls unreachable.
// For netplay (online/spectate) sessions, no Jcc patches are applied, so
// ExitProcess CAN fire from EFZ_Main_RollbackLoopTick when the peer process
// terminates.  Because ExitProcess is called on the EFZ.exe main game loop
// thread (via the frame hook dispatch chain), NeutralizeExitProcess must NOT
// hang that thread with Sleep(INFINITE).
//
// This hook installs a 6-byte JMP at sub_1006E590's entry so every frame
// passes through OurFrameDispatch.  OurFrameDispatch sets a jmp_buf before
// calling the original function.  NeutralizeExitProcess then longjmp()s
// back here instead of sleeping, allowing the main loop to continue normally.
// On the next title-screen frame, ConsumeRevivalExitInterception runs the
// full cleanup and re-enters the netplay menu.
//
// sub_1006E590 first 6 bytes (complete instructions, safe trampoline unit):
//   55        push ebp
//   8B EC     mov  ebp, esp
//   83 E4 C0  and  esp, 0xC0   (64-byte stack alignment)
// ---------------------------------------------------------------------------

// Frame hook RVA is now profile-driven: g_activeRevival->frameHookRva

// The jmp_buf and active-flag are read by NeutralizeExitProcess in
// iat_stubs.cpp.  They are declared extern in takeover_internal.h.
jmp_buf         g_netplayFrameJmpBuf    = {};
volatile bool   g_netplayFrameJmpActive = false;

// Fallback recovery context armed by HookedTitleUpdateImpl while title/menu
// logic is executing. Used when ExitProcess fires outside OurFrameDispatch.
jmp_buf         g_netplayUiJmpBuf       = {};
volatile bool   g_netplayUiJmpActive    = false;

// Trampoline: first 6 original bytes + near JMP back to original+6.
static uint8_t g_frameHookTrampoline[12] = {};
static bool    g_frameHookInstalled      = false;

using FrameDispatchFn = void (*)();
static FrameDispatchFn g_origFrameDispatch = nullptr;

// ---------------------------------------------------------------------------
// EXE frame-hook trampoline chain prevention
//
// Revival DLL's init() calls sub_1006F160(0x401582, 10, sub_1006E590) EVERY
// TIME it runs.  sub_1006F160 is an inline-hook installer that:
//   1. Reads the current 10 bytes at 0x401582 into a new malloc'd trampoline
//      (raw memcpy - no relocation of relative branches).
//   2. Overwrites 0x401582 with a JMP to the new trampoline + NOP padding.
//
// After the very first init() (run by Revival DLL at startup), 0x401582
// contains a JMP to Trampoline-1, whose displaced bytes are the genuine
// original EXE instructions - safe to execute from any address.
//
// When our code calls init() again (Tick_init_handshake, ForceLocalPlayInit,
// SetLocalRoleFlag), sub_1006F160 creates Trampoline-2 whose displaced bytes
// are the raw JMP from 0x401582 → Trampoline-1.  But that JMP is an E9
// rel32 displacement calculated for 0x401582; executing it from Trampoline-2
// produces a wild target address → EIP crash (0x22FFF281, 0x2702FBB9, etc.).
//
// Fix: save the 10 bytes at 0x401582 BEFORE every init() call from our code
// and restore them AFTER.  init() still creates/registers the session object
// and writes all DLL globals; only the trampoline chain growth is undone.
// ---------------------------------------------------------------------------
static constexpr uintptr_t kExeFrameHookAddr = 0x401582u;
static constexpr size_t    kExeFrameHookSize = 10u;
static uint8_t g_exeFrameHookSaved[kExeFrameHookSize] = {};
static bool    g_exeFrameHookSavedValid = false;

void SaveExeFrameHookBytes()
{
    LogRevival102jDeepBytes(
        "ExeFrameHook.save_pre",
        "efz.0x401582",
        kExeFrameHookAddr,
        0x20u);
    memcpy(g_exeFrameHookSaved,
           reinterpret_cast<const void*>(kExeFrameHookAddr),
           kExeFrameHookSize);
    g_exeFrameHookSavedValid = true;
    mod::Log("SaveExeFrameHookBytes: saved %zu bytes at 0x%08lX "
             "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
             kExeFrameHookSize,
             static_cast<unsigned long>(kExeFrameHookAddr),
             g_exeFrameHookSaved[0], g_exeFrameHookSaved[1],
             g_exeFrameHookSaved[2], g_exeFrameHookSaved[3],
             g_exeFrameHookSaved[4], g_exeFrameHookSaved[5],
             g_exeFrameHookSaved[6], g_exeFrameHookSaved[7],
             g_exeFrameHookSaved[8], g_exeFrameHookSaved[9]);
    LogBytesIfVerbose("SaveExeFrameHookBytes.window", kExeFrameHookAddr, 16);
}

void RestoreExeFrameHookBytes()
{
    if (!g_exeFrameHookSavedValid)
    {
        mod::Log("RestoreExeFrameHookBytes: no saved bytes - skipped");
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kExeFrameHookAddr),
                        kExeFrameHookSize,
                        PAGE_EXECUTE_READWRITE,
                        &oldProtect))
    {
        mod::Log("RestoreExeFrameHookBytes: VirtualProtect failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        return;
    }

    memcpy(reinterpret_cast<void*>(kExeFrameHookAddr),
           g_exeFrameHookSaved,
           kExeFrameHookSize);

    VirtualProtect(reinterpret_cast<void*>(kExeFrameHookAddr),
                   kExeFrameHookSize,
                   oldProtect,
                   &oldProtect);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(kExeFrameHookAddr),
                          kExeFrameHookSize);

    // Invalidate the saved buffer so a subsequent Restore without a
    // matching Save gives a clear diagnostic instead of silently
    // writing stale bytes from a previous session.
    g_exeFrameHookSavedValid = false;

    // Read back for verification.
    uint8_t verify[kExeFrameHookSize] = {};
    memcpy(verify,
           reinterpret_cast<const void*>(kExeFrameHookAddr),
           kExeFrameHookSize);
    mod::Log("RestoreExeFrameHookBytes: restored %zu bytes at 0x%08lX "
             "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
             kExeFrameHookSize,
             static_cast<unsigned long>(kExeFrameHookAddr),
             verify[0], verify[1], verify[2], verify[3], verify[4],
             verify[5], verify[6], verify[7], verify[8], verify[9]);
    LogBytesIfVerbose("RestoreExeFrameHookBytes.window", kExeFrameHookAddr, 16);
    LogRevival102jDeepBytes(
        "ExeFrameHook.restore_post",
        "efz.0x401582",
        kExeFrameHookAddr,
        0x20u);
}
// ---------------------------------------------------------------------------
// Save / restore 8 bytes at EXE address 0x401642. Legacy Revival builds may
// replace this site during init(). In 1.02j, install_exe_hooks writes a
// process-lifetime E9 to perFrameTickRva once, while exported init() only
// installs the separate 0x401582 hook. Removing 0x401642 therefore disables
// every later session's native frame driver, including overlay redraw.
// ---------------------------------------------------------------------------
static constexpr uintptr_t kExeDispatchHookAddr = 0x401642u;
static constexpr size_t    kExeDispatchHookSize = 8u;
static const uint8_t kExeDispatchOriginalBytes[kExeDispatchHookSize] = {
    0xFF, 0x52, 0x04, 0xA2, 0x48, 0x01, 0x79, 0x00
};
static uint8_t g_exeDispatchHookSaved[kExeDispatchHookSize] = {};
static bool    g_exeDispatchHookSavedValid = false;
static uint8_t g_exeDispatchPersistentHook[kExeDispatchHookSize] = {};
static bool    g_exeDispatchPersistentHookValid = false;

static bool ExeDispatchBytesMatch(
    const uint8_t* lhs,
    const uint8_t* rhs)
{
    return lhs != nullptr
        && rhs != nullptr
        && std::memcmp(lhs, rhs, kExeDispatchHookSize) == 0;
}

static bool IsVerifiedRevival102jDispatchHook(
    const uint8_t* bytes,
    uintptr_t* outTarget = nullptr)
{
    if (!IsRevival102jProfile() || bytes == nullptr || bytes[0] != 0xE9)
    {
        return false;
    }

    int32_t displacement = 0;
    std::memcpy(&displacement, bytes + 1, sizeof(displacement));
    const uintptr_t target = static_cast<uintptr_t>(
        static_cast<intptr_t>(kExeDispatchHookAddr + 5u) + displacement);
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr || g_activeRevival == nullptr)
    {
        return false;
    }

    const uintptr_t expected =
        reinterpret_cast<uintptr_t>(revival) + g_activeRevival->perFrameTickRva;
    if (outTarget != nullptr)
    {
        *outTarget = target;
    }
    return target == expected;
}

static void CaptureRevival102jDispatchHookIfVerified(
    const uint8_t* bytes,
    const char* caller)
{
    uintptr_t target = 0;
    if (!IsVerifiedRevival102jDispatchHook(bytes, &target))
    {
        return;
    }

    if (!g_exeDispatchPersistentHookValid
        || !ExeDispatchBytesMatch(g_exeDispatchPersistentHook, bytes))
    {
        std::memcpy(
            g_exeDispatchPersistentHook,
            bytes,
            kExeDispatchHookSize);
        g_exeDispatchPersistentHookValid = true;
        mod::Log(
            "%s: captured verified 1.02j persistent 0x401642 hook "
            "target=0x%08lX",
            caller != nullptr ? caller : "CaptureRevival102jDispatchHook",
            static_cast<unsigned long>(target));
    }
}

static bool WriteExeDispatchHookBytes(
    const char* caller,
    const uint8_t* bytes,
    const char* reason)
{
    if (bytes == nullptr)
    {
        return false;
    }

    uint8_t before[kExeDispatchHookSize] = {};
    LogRevival102jDeepBytes(
        "ExeDispatch.write_pre",
        "efz.0x401642",
        kExeDispatchHookAddr,
        0x20u);
    memcpy(before,
           reinterpret_cast<const void*>(kExeDispatchHookAddr),
           kExeDispatchHookSize);

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kExeDispatchHookAddr),
                        kExeDispatchHookSize,
                        PAGE_EXECUTE_READWRITE,
                        &oldProtect))
    {
        mod::Log(
            "%s: %s VirtualProtect failed addr=0x%08lX err=%lu",
            caller != nullptr ? caller : "WriteExeDispatchHookBytes",
            reason != nullptr ? reason : "write",
            static_cast<unsigned long>(kExeDispatchHookAddr),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    memcpy(reinterpret_cast<void*>(kExeDispatchHookAddr),
           bytes,
           kExeDispatchHookSize);

    VirtualProtect(reinterpret_cast<void*>(kExeDispatchHookAddr),
                   kExeDispatchHookSize,
                   oldProtect,
                   &oldProtect);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(kExeDispatchHookAddr),
                          kExeDispatchHookSize);

    uint8_t verify[kExeDispatchHookSize] = {};
    memcpy(verify,
           reinterpret_cast<const void*>(kExeDispatchHookAddr),
           kExeDispatchHookSize);
    mod::Log(
        "%s: %s 8 bytes at 0x%08lX old=[%02X %02X %02X %02X %02X %02X %02X %02X] "
        "new=[%02X %02X %02X %02X %02X %02X %02X %02X]",
        caller != nullptr ? caller : "WriteExeDispatchHookBytes",
        reason != nullptr ? reason : "wrote",
        static_cast<unsigned long>(kExeDispatchHookAddr),
        before[0], before[1], before[2], before[3],
        before[4], before[5], before[6], before[7],
        verify[0], verify[1], verify[2], verify[3],
        verify[4], verify[5], verify[6], verify[7]);
    LogBytesIfVerbose("WriteExeDispatchHookBytes.window", kExeDispatchHookAddr, 16);
    LogRevival102jDeepBytes(
        "ExeDispatch.write_post",
        "efz.0x401642",
        kExeDispatchHookAddr,
        0x20u);
    return ExeDispatchBytesMatch(verify, bytes);
}

void SaveExeDispatchHookBytes()
{
    LogRevival102jDeepBytes(
        "ExeDispatch.save_pre",
        "efz.0x401642",
        kExeDispatchHookAddr,
        0x20u);
    memcpy(g_exeDispatchHookSaved,
           reinterpret_cast<const void*>(kExeDispatchHookAddr),
           kExeDispatchHookSize);
    g_exeDispatchHookSavedValid = true;
    CaptureRevival102jDispatchHookIfVerified(
        g_exeDispatchHookSaved,
        "SaveExeDispatchHookBytes");
    mod::Log("SaveExeDispatchHookBytes: saved %zu bytes at 0x%08lX "
             "[%02X %02X %02X %02X %02X %02X %02X %02X]",
             kExeDispatchHookSize,
             static_cast<unsigned long>(kExeDispatchHookAddr),
             g_exeDispatchHookSaved[0], g_exeDispatchHookSaved[1],
             g_exeDispatchHookSaved[2], g_exeDispatchHookSaved[3],
             g_exeDispatchHookSaved[4], g_exeDispatchHookSaved[5],
             g_exeDispatchHookSaved[6], g_exeDispatchHookSaved[7]);
    LogBytesIfVerbose("SaveExeDispatchHookBytes.window", kExeDispatchHookAddr, 16);
}

void RestoreExeDispatchHookBytes()
{
    if (!g_exeDispatchHookSavedValid)
    {
        mod::Log("RestoreExeDispatchHookBytes: no saved bytes - skipped");
        return;
    }

    if (WriteExeDispatchHookBytes(
            "RestoreExeDispatchHookBytes",
            g_exeDispatchHookSaved,
            "restored saved"))
    {
        g_exeDispatchHookSavedValid = false;
    }
}

void RestoreExeDispatchHookBytesAfterSessionInit(int initMode)
{
    if (g_exeDispatchHookSavedValid)
    {
        RestoreExeDispatchHookBytes();
    }
    else
    {
        mod::Log(
            "RestoreExeDispatchHookBytesAfterSessionInit: no saved bytes mode=%d",
            initMode);
    }

    if (IsRevival102jProfile())
    {
        const bool ok = RestoreExeDispatchHookForTitle(
            "RestoreExeDispatchHookBytesAfterSessionInit");
        mod::Log(
            "RestoreExeDispatchHookBytesAfterSessionInit: 1.02j mode=%d "
            "persistentHook=%d",
            initMode,
            ok ? 1 : 0);
    }
}

bool RestoreExeDispatchHookForTitle(const char* caller)
{
    if (!IsRevival102jProfile())
    {
        return false;
    }

    uint8_t current[kExeDispatchHookSize] = {};
    memcpy(current,
           reinterpret_cast<const void*>(kExeDispatchHookAddr),
           kExeDispatchHookSize);
    uintptr_t target = 0;
    if (IsVerifiedRevival102jDispatchHook(current, &target))
    {
        CaptureRevival102jDispatchHookIfVerified(current, caller);
        mod::Log(
            "%s: 1.02j persistent 0x401642 hook already active "
            "target=0x%08lX",
            caller != nullptr ? caller : "RestoreExeDispatchHookForTitle",
            static_cast<unsigned long>(target));
        return true;
    }

    if (!g_exeDispatchPersistentHookValid
        && g_exeDispatchHookSavedValid)
    {
        CaptureRevival102jDispatchHookIfVerified(
            g_exeDispatchHookSaved,
            caller);
    }

    if (!g_exeDispatchPersistentHookValid)
    {
        mod::Log(
            "%s: 1.02j persistent 0x401642 hook unavailable current=%s "
            "[%02X %02X %02X %02X %02X %02X %02X %02X]",
            caller != nullptr ? caller : "RestoreExeDispatchHookForTitle",
            ExeDispatchBytesMatch(current, kExeDispatchOriginalBytes)
                ? "vanilla"
                : "unknown",
            current[0], current[1], current[2], current[3],
            current[4], current[5], current[6], current[7]);
        return false;
    }

    return WriteExeDispatchHookBytes(
        caller != nullptr ? caller : "RestoreExeDispatchHookForTitle",
        g_exeDispatchPersistentHook,
        "restored verified 1.02j persistent frame dispatch hook");
}

// ---------------------------------------------------------------------------
// Save / restore bytes at EXE addresses 0x763E50 (7 bytes) and 0x763F04
// (7 bytes) - the mode-constructor hook sites.  Prevents trampoline chain
// growth in the same way SaveExeFrameHookBytes prevents it at 0x401582.
//
// Each init() call has the mode constructor run sub_1006F160 which reads
// the bytes at the hook site, allocates a new trampoline, copies the bytes,
// and overwrites the hook site with JMP trampoline.  Without save/restore
// the chain grows by one link per init() call - leaking ~16 bytes of
// malloc'd memory per link and adding hot-path PUSHAD/POPFD overhead.
// ---------------------------------------------------------------------------
static constexpr size_t kModeCtorHookCount = 2;
static constexpr uintptr_t kModeCtorHookAddrs[kModeCtorHookCount] = {
    0x763E50, 0x763F04
};
static constexpr size_t kModeCtorHookPatchSize = 7;
static uint8_t g_modeCtorHookSaved[kModeCtorHookCount][kModeCtorHookPatchSize] = {};
static bool    g_modeCtorHookSavedValid = false;

// Permanent copy of the original (pre-hook) EXE bytes at the mode-ctor
// hook sites.  Captured once on first use and never overwritten.  Used to
// restore a clean state before every init() call so that new trampolines
// never chain through stale hooks from a previous session's mode.
static uint8_t g_modeCtorOriginalBytes[kModeCtorHookCount][kModeCtorHookPatchSize] = {};
static bool    g_modeCtorOriginalsSaved = false;

static void SaveModeCtorOriginalBytesOnce()
{
    if (g_modeCtorOriginalsSaved)
        return;
    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        memcpy(g_modeCtorOriginalBytes[i],
               reinterpret_cast<const void*>(kModeCtorHookAddrs[i]),
               kModeCtorHookPatchSize);
        char label[64] = {};
        std::snprintf(label, sizeof(label), "SaveModeCtorOriginalBytesOnce[%zu]", i);
        LogBytesIfVerbose(label, kModeCtorHookAddrs[i], 16);
    }
    g_modeCtorOriginalsSaved = true;
    mod::Log(
        "SaveModeCtorOriginalBytesOnce: captured "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        g_modeCtorOriginalBytes[0][0], g_modeCtorOriginalBytes[0][1],
        g_modeCtorOriginalBytes[0][2], g_modeCtorOriginalBytes[0][3],
        g_modeCtorOriginalBytes[0][4], g_modeCtorOriginalBytes[0][5],
        g_modeCtorOriginalBytes[0][6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        g_modeCtorOriginalBytes[1][0], g_modeCtorOriginalBytes[1][1],
        g_modeCtorOriginalBytes[1][2], g_modeCtorOriginalBytes[1][3],
        g_modeCtorOriginalBytes[1][4], g_modeCtorOriginalBytes[1][5],
        g_modeCtorOriginalBytes[1][6]);
}

void SaveModeCtorHookBytes()
{
    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        memcpy(g_modeCtorHookSaved[i],
               reinterpret_cast<const void*>(kModeCtorHookAddrs[i]),
               kModeCtorHookPatchSize);
        char label[64] = {};
        std::snprintf(label, sizeof(label), "SaveModeCtorHookBytes[%zu]", i);
        LogBytesIfVerbose(label, kModeCtorHookAddrs[i], 16);
    }
    g_modeCtorHookSavedValid = true;

    mod::Log(
        "SaveModeCtorHookBytes: saved 0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        g_modeCtorHookSaved[0][0], g_modeCtorHookSaved[0][1],
        g_modeCtorHookSaved[0][2], g_modeCtorHookSaved[0][3],
        g_modeCtorHookSaved[0][4], g_modeCtorHookSaved[0][5],
        g_modeCtorHookSaved[0][6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        g_modeCtorHookSaved[1][0], g_modeCtorHookSaved[1][1],
        g_modeCtorHookSaved[1][2], g_modeCtorHookSaved[1][3],
        g_modeCtorHookSaved[1][4], g_modeCtorHookSaved[1][5],
        g_modeCtorHookSaved[1][6]);
}

void DiscardModeCtorHookBytes()
{
    if (g_modeCtorHookSavedValid)
    {
        g_modeCtorHookSavedValid = false;
        mod::Log("DiscardModeCtorHookBytes: discarded saved bytes without restoring");
    }
}

void RestoreModeCtorOriginalBytes()
{
    SaveModeCtorOriginalBytesOnce();
    if (!g_modeCtorOriginalsSaved)
    {
        mod::Log("RestoreModeCtorOriginalBytes: no originals captured - skipped");
        return;
    }

    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                            kModeCtorHookPatchSize,
                            PAGE_EXECUTE_READWRITE,
                            &oldProtect))
        {
            mod::Log(
                "RestoreModeCtorOriginalBytes: VirtualProtect(0x%08lX) failed err=%lu",
                static_cast<unsigned long>(kModeCtorHookAddrs[i]),
                static_cast<unsigned long>(GetLastError()));
            continue;
        }

        memcpy(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
               g_modeCtorOriginalBytes[i],
               kModeCtorHookPatchSize);
        char label[72] = {};
        std::snprintf(label, sizeof(label), "RestoreModeCtorOriginalBytes[%zu]", i);
        LogBytesIfVerbose(label, kModeCtorHookAddrs[i], 16);

        VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                       kModeCtorHookPatchSize,
                       oldProtect,
                       &oldProtect);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                              kModeCtorHookPatchSize);
    }

    // Read back for verification.
    uint8_t v0[kModeCtorHookPatchSize] = {};
    uint8_t v1[kModeCtorHookPatchSize] = {};
    memcpy(v0, reinterpret_cast<const void*>(kModeCtorHookAddrs[0]), kModeCtorHookPatchSize);
    memcpy(v1, reinterpret_cast<const void*>(kModeCtorHookAddrs[1]), kModeCtorHookPatchSize);
    mod::Log(
        "RestoreModeCtorOriginalBytes: restored "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        v0[0], v0[1], v0[2], v0[3], v0[4], v0[5], v0[6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        v1[0], v1[1], v1[2], v1[3], v1[4], v1[5], v1[6]);
}

void RestoreModeCtorHookBytes()
{
    if (!g_modeCtorHookSavedValid)
    {
        mod::Log("RestoreModeCtorHookBytes: no saved bytes - skipped");
        return;
    }

    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                            kModeCtorHookPatchSize,
                            PAGE_EXECUTE_READWRITE,
                            &oldProtect))
        {
            mod::Log(
                "RestoreModeCtorHookBytes: VirtualProtect(0x%08lX) failed err=%lu",
                static_cast<unsigned long>(kModeCtorHookAddrs[i]),
                static_cast<unsigned long>(GetLastError()));
            continue;
        }

        memcpy(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
               g_modeCtorHookSaved[i],
               kModeCtorHookPatchSize);
        char label[64] = {};
        std::snprintf(label, sizeof(label), "RestoreModeCtorHookBytes[%zu]", i);
        LogBytesIfVerbose(label, kModeCtorHookAddrs[i], 16);

        VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                       kModeCtorHookPatchSize,
                       oldProtect,
                       &oldProtect);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                              kModeCtorHookPatchSize);
    }

    g_modeCtorHookSavedValid = false;

    // Read back for verification.
    uint8_t v0[kModeCtorHookPatchSize] = {};
    uint8_t v1[kModeCtorHookPatchSize] = {};
    memcpy(v0, reinterpret_cast<const void*>(kModeCtorHookAddrs[0]), kModeCtorHookPatchSize);
    memcpy(v1, reinterpret_cast<const void*>(kModeCtorHookAddrs[1]), kModeCtorHookPatchSize);
    mod::Log(
        "RestoreModeCtorHookBytes: restored 0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        v0[0], v0[1], v0[2], v0[3], v0[4], v0[5], v0[6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        v1[0], v1[1], v1[2], v1[3], v1[4], v1[5], v1[6]);
}

// ---------------------------------------------------------------------------
// Mode-constructor trampoline fixup
//
// Revival DLL's mode constructors (online=sub_10073440, spectator=sub_100749A0,
// tournament=sub_100792D0) hook EXE addresses 0x763E50 and 0x763F04 via
// sub_1006F160.  The trampoline installer (sub_1006F060) copies the original
// bytes into a malloc'd trampoline WITHOUT fixing up relative instructions
// (E8 CALL rel32, E9 JMP rel32, 0F 8x near Jcc rel32).
//
// On re-initialization, new trampolines are allocated at different heap
// addresses.  Any relative instruction in the copied bytes computes its target
// as: trampoline_addr + offset + insn_length + original_displacement.  Since
// the original displacement was calculated for the EXE location (not the
// trampoline), the computed target is wrong by (trampoline_addr - exe_addr) →
// wild EIP crash (0x210132F9, 0x22FFF281, 0x2702FBB9, etc.).
//
// Trampoline layout from sub_1006F060 for a N-byte hook:
//   +0: 0x60 PUSHAD
//   +1: 0x9C PUSHFD
//   +2: 0xE8 rel32 → CALL callback
//   +7: 0x9D POPFD
//   +8: 0x61 POPAD
//   +9: [N bytes copied from original hook site]
//   +9+N: 0xE9 rel32 → JMP back to original+N
// ---------------------------------------------------------------------------
struct ModeCtorHookSite {
    uintptr_t hookAddr;
    size_t    patchSize;
};

static constexpr ModeCtorHookSite kModeCtorHookSites[] = {
    { 0x763E50, 7 },
    { 0x763F04, 7 },
};

static constexpr size_t kModeCtorTrampolinePrologueSize = 9; // PUSHAD+PUSHFD+CALL+POPFD+POPAD

// Track trampoline addresses we've already fixed up, per hook site.
// If the same trampoline is seen again, its displacements are already
// relative to the trampoline - re-fixing would drift the targets
// further with each call, eventually causing a wild-EIP crash.
static uintptr_t g_lastFixedTrampoline[sizeof(kModeCtorHookSites) /
                                        sizeof(kModeCtorHookSites[0])] = {};

void ResetModeConstructorTrampolineCache()
{
    memset(g_lastFixedTrampoline, 0, sizeof(g_lastFixedTrampoline));
    mod::Log("ResetModeConstructorTrampolineCache: cleared %zu entries",
             sizeof(g_lastFixedTrampoline) / sizeof(g_lastFixedTrampoline[0]));
}

void FixupModeConstructorTrampolines(const char* caller)
{
    for (size_t siteIdx = 0;
         siteIdx < sizeof(kModeCtorHookSites) / sizeof(kModeCtorHookSites[0]);
         ++siteIdx)
    {
        const auto& site = kModeCtorHookSites[siteIdx];
        // Check if the hook site starts with E9 (JMP near rel32).
        uint8_t firstByte = 0;
        if (!SafeReadByte(reinterpret_cast<uint8_t*>(site.hookAddr), &firstByte)
            || firstByte != 0xE9)
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX not hooked (byte=0x%02X), skip",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                static_cast<unsigned>(firstByte));
            continue;
        }

        // Read the E9 displacement to find the trampoline address.
        int32_t jmpDisp = 0;
        memcpy(&jmpDisp, reinterpret_cast<const void*>(site.hookAddr + 1), 4);
        const uintptr_t trampAddr = site.hookAddr + 5 + jmpDisp;

        // If we already fixed this trampoline, its displacements are already
        // correct (relative to the trampoline).  Re-fixing would interpret
        // them as relative to the EXE hook site, drifting the target each
        // call until it becomes a wild EIP.
        if (trampAddr == g_lastFixedTrampoline[siteIdx])
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX tramp=%p already fixed, skip",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                reinterpret_cast<void*>(trampAddr));
            continue;
        }

        // Verify trampoline prologue: must be PUSHAD (0x60) PUSHFD (0x9C).
        uint8_t prologue[2] = {};
        memcpy(prologue, reinterpret_cast<const void*>(trampAddr), 2);
        if (prologue[0] != 0x60 || prologue[1] != 0x9C)
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX tramp=%p bad prologue [%02X %02X], skip",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                reinterpret_cast<void*>(trampAddr),
                prologue[0], prologue[1]);
            continue;
        }

        // Read the original bytes from trampoline + prologue offset.
        const size_t origOff = kModeCtorTrampolinePrologueSize;
        uint8_t origBytes[16] = {};
        memcpy(origBytes,
               reinterpret_cast<const void*>(trampAddr + origOff),
               site.patchSize);

        mod::Log(
            "%s: modeCtorFixup 0x%08lX tramp=%p orig=[%02X %02X %02X %02X %02X %02X %02X]",
            caller,
            static_cast<unsigned long>(site.hookAddr),
            reinterpret_cast<void*>(trampAddr),
            origBytes[0], origBytes[1], origBytes[2], origBytes[3],
            origBytes[4], origBytes[5], origBytes[6]);

        bool anyFixed = false;

        // Scan for E8 (CALL rel32) and E9 (JMP rel32) - 5-byte instructions.
        for (size_t i = 0; i + 5 <= site.patchSize; ++i)
        {
            if (origBytes[i] != 0xE8 && origBytes[i] != 0xE9)
                continue;

            int32_t origDisp = 0;
            memcpy(&origDisp, &origBytes[i + 1], 4);

            // Absolute target the original instruction was meant to reach.
            const uintptr_t origInsnAddr = site.hookAddr + i;
            const uintptr_t absTarget = origInsnAddr + 5 +
                                        static_cast<uintptr_t>(static_cast<uint32_t>(origDisp));

            // Correct displacement from the trampoline location.
            const uintptr_t newInsnAddr = trampAddr + origOff + i;
            const int32_t newDisp =
                static_cast<int32_t>(absTarget - (newInsnAddr + 5));

            // Write the fixed-up displacement.
            DWORD oldProtect = 0;
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 1), 4,
                           PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(reinterpret_cast<void*>(newInsnAddr + 1), &newDisp, 4);
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 1), 4,
                           oldProtect, &oldProtect);

            mod::Log(
                "%s: modeCtorFixup 0x%08lX fixed E%X at +%zu: "
                "origDisp=0x%08X newDisp=0x%08X absTarget=%p",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                static_cast<unsigned>(origBytes[i]),
                i,
                static_cast<unsigned>(origDisp),
                static_cast<unsigned>(newDisp),
                reinterpret_cast<void*>(absTarget));

            anyFixed = true;
            i += 4; // skip displacement bytes
        }

        // Scan for 0F 8x (near conditional JMP rel32) - 6-byte instructions.
        for (size_t i = 0; i + 6 <= site.patchSize; ++i)
        {
            if (origBytes[i] != 0x0F || (origBytes[i + 1] & 0xF0) != 0x80)
                continue;

            int32_t origDisp = 0;
            memcpy(&origDisp, &origBytes[i + 2], 4);

            const uintptr_t origInsnAddr = site.hookAddr + i;
            const uintptr_t absTarget = origInsnAddr + 6 +
                                        static_cast<uintptr_t>(static_cast<uint32_t>(origDisp));

            const uintptr_t newInsnAddr = trampAddr + origOff + i;
            const int32_t newDisp =
                static_cast<int32_t>(absTarget - (newInsnAddr + 6));

            DWORD oldProtect = 0;
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 2), 4,
                           PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(reinterpret_cast<void*>(newInsnAddr + 2), &newDisp, 4);
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 2), 4,
                           oldProtect, &oldProtect);

            mod::Log(
                "%s: modeCtorFixup 0x%08lX fixed 0F %02X at +%zu: "
                "origDisp=0x%08X newDisp=0x%08X absTarget=%p",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                static_cast<unsigned>(origBytes[i + 1]),
                i,
                static_cast<unsigned>(origDisp),
                static_cast<unsigned>(newDisp),
                reinterpret_cast<void*>(absTarget));

            anyFixed = true;
            i += 5;
        }

        // Remember this trampoline so we don't re-fix it on later calls.
        g_lastFixedTrampoline[siteIdx] = trampAddr;

        if (anyFixed)
        {
            FlushInstructionCache(
                GetCurrentProcess(),
                reinterpret_cast<void*>(trampAddr + origOff),
                site.patchSize);
        }
        else
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX no relative instructions found",
                caller,
                static_cast<unsigned long>(site.hookAddr));
        }
    }
}

// g_frameRecoveryPending: set by RunFrameDispatch on longjmp, read and cleared
// by OurFrameDispatch outside the setjmp scope so C++ code (ForceLocalPlayInit)
// can run safely.
static volatile bool g_frameRecoveryPending = false;

// g_tickRecoveryPending: same pattern but for RunPerFrameTickDispatch /
// OurPerFrameTickHook.  The per-frame tick is the ACTUAL every-frame entry
// point (sub_1006E570 → vtable[2] → RollbackLoopTick).  ExitProcess most
// commonly fires here when the peer process dies mid-match.
static volatile bool g_tickRecoveryPending = false;

// RunFrameDispatch - MSVC C4611 guard: no C++ objects with destructors in scope.
// Only POD types here.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
static void RunFrameDispatch()
{
    g_netplayFrameJmpActive = true;
    if (setjmp(g_netplayFrameJmpBuf) != 0)
    {
        // longjmp path: ExitProcess was intercepted during this frame tick.
        // Signal OurFrameDispatch to execute C++ recovery outside setjmp scope.
        g_netplayFrameJmpActive = false;
        g_frameRecoveryPending = true;
        return;
    }
    // Normal path: dispatch through the original sub_1006E590 trampoline.
    if (g_origFrameDispatch != nullptr)
    {
        g_origFrameDispatch();
    }
    g_netplayFrameJmpActive = false;
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// Per-frame tick hook on sub_1006E570 (RVA 0x6E570)
//
// BACKGROUND
// ----------
// sub_1006E590 (RVA 0x6E590) runs ONCE during DLL init.  Among other things
// it patches EXE address 0x401642 to JMP into sub_1006E570.  From that point
// the EXE's main loop calls sub_1006E570 on EVERY frame.
//
// sub_1006E570 is __thiscall, so the detour must preserve a valid ECX ABI.
// In verified 1.02h code the wrapper reloads dword_100A02EC and dispatches
// through that session's vtable; the incoming ECX does not select or double-
// run the rollback session. Keep the current-session compatibility argument
// for the other supported builds, but do not treat an ECX difference as the
// Nayuki RNG root cause.
// ---------------------------------------------------------------------------

// Per-frame tick RVA is now profile-driven: g_activeRevival->perFrameTickRva.
// 1.02j's MinGW prologue needs an 8-byte steal, so keep room for
// stolen bytes + JMP rel32 + padding.
static uint8_t g_perFrameTickTrampoline[16] = {};
static bool    g_perFrameTickInstalled      = false;
static bool    g_perFrameMismatchLogged     = false;

// Guard flag: true while g_origPerFrameTick is running.  ForceLocalPlayInit
// must NOT run during this window because sub_1006E570 -> vtable[2] ->
// RollbackLoopTick is using the current session as 'this'. Destroying it
// mid-tick causes a use-after-free crash in SetEvent(this[2]).
static volatile bool g_insideFrameTick = false;
static volatile bool g_deferredCancelCleanup = false;
static char g_deferredCancelCleanupReason[64] = {};
static uint8_t g_deferredCancelCleanupSourceScreen = 0xFF;
static int g_deferredCancelCleanupSourceRole = -1;
static uint32_t g_deferredCancelCleanupSourceFrame = 0;
static volatile LONG g_onlineMatchEscGracefulQuitArmed = 0;
static volatile LONG g_localBattleEscQuitRingIgnoreArmed = 0;
static volatile LONG* g_quitRingHeader = nullptr;
static volatile LONG g_scheduledGracefulQuitTeardownActive = 0;
static DWORD g_localBattleEscQuitRingIgnoreStartMs = 0;
static DWORD g_scheduledGracefulQuitTeardownStartMs = 0;
static LONG g_scheduledGracefulQuitHead = 0;
static LONG g_scheduledGracefulQuitTail = 0;
static DWORD g_scheduledGracefulQuitHelperPid = 0;
static char g_scheduledGracefulQuitPhase[16] = {};
static uint32_t g_frameTick = 0;           // monotonic per-frame counter

// Hard-fallback watchdog: counts consecutive frames where the Revival child
// process is dead but no existing recovery mechanism (ExitProcess interception,
// consoleErrorSerial, spectator ESC) has fired.  After a grace period the
// watchdog forces a full cleanup and return to the netplay menu.
static unsigned int g_watchdogDeadFrameCount = 0;
static constexpr unsigned int kWatchdogGraceFrames = 30; // ~0.5s at 60fps
static constexpr DWORD kLocalBattleEscQuitRingIgnoreWindowMs = 500u;
static constexpr DWORD kScheduledGracefulQuitTeardownDelayMs = 500u;

// __thiscall trampoline: ECX = this, no other args.
using PerFrameTickFn = int (__thiscall *)(void* thisPtr);
static PerFrameTickFn g_origPerFrameTick = nullptr;

struct GameplayStallSample
{
    uint8_t screen = 0xFF;
    int mode = -1;
    uint32_t frameTick = 0;
    bool originalTickBypassed = false;
    bool originalTickSkipped = false;
    bool originalTickRan = false;
    int syncFrame = -1;
    bool syncFrameValid = false;
    LONG consoleErrorSerial = 0;
    int phase = -1;
    int role = -1;
    DWORD nowMs = 0;
    bool recoveryInProgress = false;
    bool pendingMenuEntry = false;
    bool recoveryCompleted = false;
};

struct GameplayStallTrackerState
{
    bool active = false;
    bool syncFrameTracked = false;
    DWORD stallStartMs = 0;
    DWORD lastProgressLogMs = 0;
    DWORD lastSyncFrameChangeMs = 0;
    uint32_t stallStartFrameTick = 0;
    uint32_t lastSyncFrameChangeTick = 0;
    uint32_t bypassCount = 0;
    int lastSyncFrame = -1;
    uint8_t screen = 0xFF;
    int mode = -1;
};

static GameplayStallTrackerState g_gameplayStall = {};
static volatile LONG g_gameplayStallLocalProcessCloseActive = 0;
static constexpr DWORD kGameplayStallConsoleErrorMs = 250u;
// Tier B measures committed-frame progress (both players' inputs applied).
// Revival reports genuine peer death through a console error well before
// this (tier A), so tier B only exists for silent wedges - keep it above
// any lag spike a live connection could still recover from.
static constexpr DWORD kGameplayStallSyncFrameMs = 10000u;
static constexpr DWORD kGameplayStallWallTimeoutMs = 30000u;
static constexpr DWORD kGameplayStallProgressLogMs = 1000u;

static uint8_t ReadCurrentScreenIndexForRecovery()
{
    uint8_t screen = 0xFF;
    __try
    {
        screen = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return screen;
}

static int ReadCurrentGameModeForRecovery()
{
    constexpr uintptr_t kGameSystemPtr = 0x0079010C;
    constexpr uint32_t kModeOffset = 4964;

    uint8_t mode = 0xFF;
    __try
    {
        const uint32_t gameSys =
            *reinterpret_cast<const volatile uint32_t*>(kGameSystemPtr);
        if (gameSys != 0)
        {
            mode = *reinterpret_cast<const volatile uint8_t*>(gameSys + kModeOffset);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return mode == 0xFF ? -1 : static_cast<int>(mode);
}

static bool IsGameplayExitRecoveryScreen(uint8_t screen)
{
    return screen == 3 || screen == 5;
}

static bool ReadGameplaySyncFrameForRecovery(uintptr_t sessionPtr, int* outSyncFrame)
{
    if (outSyncFrame == nullptr)
    {
        return false;
    }
    *outSyncFrame = -1;
    // This is a RollbackSession field.  SpectatorSession has an unrelated
    // value at the same profile-relative address (1.02j commonly reads
    // 10800 there), so it must never participate in stall recovery.
    if (sessionPtr == 0
        || g_activeRevival == nullptr
        || g_localRoleFlag != kLocalRoleOnline)
    {
        return false;
    }

    // gmBase+16 is the battle commit cursor (1.02e-i session+732, 1.02j
    // session+828): it is re-anchored to currentFrame on every battle entry
    // and advances only when a frame is committed with BOTH players' inputs,
    // so it stalls exactly when the input exchange stalls.  Do NOT use
    // gmBase+20 (the Sync-feed counter that fills the spectator queue): its
    // currentFrame equality guard breaks permanently when a screen
    // transition lands mid-prediction (e.g. battle ESC during rollback,
    // routine on 1.02j delay=0), and Revival never repairs it - tier-B then
    // tears down sessions whose battles are still advancing.
    const uintptr_t gmBase =
        sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot;
    return SafeReadInt(
        reinterpret_cast<const void*>(gmBase + 16),
        outSyncFrame);
}

struct RevivalRemoteInputDiagSnapshot
{
    bool valid = false;
    uintptr_t session = 0;
    uint8_t screen = 0xFF;
    int state = -1;
    int currentFrame = -1;
    int inputDelay = -1;
    int windowBaseDelay = -1;
    int activePlayer = -1;
    int localLen = -1;
    int remoteLen = -1;
    uint32_t pingStruct[4] = {0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu};
    int waitDelay = -1;
    int patchState = -1;
    int initGuardLow = -1;
    bool pauseRemoteInput = false;
};

static int ReadTwoByteSpanLengthForDiag(uintptr_t spanAddr)
{
    uintptr_t begin = 0;
    uintptr_t end = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(spanAddr), &begin)
        || !SafeReadPtr(reinterpret_cast<const void*>(spanAddr + 4), &end)
        || begin == 0
        || end < begin)
    {
        return -1;
    }

    return static_cast<int>((end - begin) >> 1);
}

static void CaptureRevivalRemoteInputDiag(
    uintptr_t sessionPtr,
    RevivalRemoteInputDiagSnapshot* outSnapshot)
{
    if (outSnapshot == nullptr)
    {
        return;
    }

    *outSnapshot = RevivalRemoteInputDiagSnapshot{};
    outSnapshot->session = sessionPtr;
    outSnapshot->screen = ReadCurrentScreenIndexForRecovery();

    if (sessionPtr == 0 || g_activeRevival == nullptr)
    {
        return;
    }

    outSnapshot->valid = true;
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot + 4),
        &outSnapshot->state);
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
        &outSnapshot->currentFrame);
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            sessionPtr + g_activeRevival->sessionOffsetInputDelay),
        &outSnapshot->inputDelay);
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            sessionPtr + g_activeRevival->sessionOffsetInputDelay + 4),
        &outSnapshot->windowBaseDelay);
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
        &outSnapshot->activePlayer);

    outSnapshot->localLen = ReadTwoByteSpanLengthForDiag(
        sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryVec);
    outSnapshot->remoteLen = ReadTwoByteSpanLengthForDiag(
        sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryVec);

    const uintptr_t pingBase =
        sessionPtr + g_activeRevival->sessionOffsetPingStructBase;
    for (int i = 0; i < 4; ++i)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(pingBase + static_cast<uintptr_t>(i) * 4u),
            reinterpret_cast<int*>(&outSnapshot->pingStruct[i]));
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
        uint16_t guard = 0;
        if (g_activeRevival->initOnceGuardOffset != 0
            && SafeReadWord(
                reinterpret_cast<const void*>(
                    base + g_activeRevival->initOnceGuardOffset),
                &guard))
        {
            outSnapshot->initGuardLow = static_cast<int>(guard & 0x00FFu);
            outSnapshot->patchState = static_cast<int>((guard >> 8) & 0x00FFu);
        }

        uintptr_t timerPtr = 0;
        double timerScalar = 0.0;
        if (g_activeRevival->timerPtrOffset != 0
            && SafeReadPtr(
                reinterpret_cast<const void*>(
                    base + g_activeRevival->timerPtrOffset),
                &timerPtr)
            && timerPtr != 0
            && SafeReadDouble(reinterpret_cast<const void*>(timerPtr + 32),
                &timerScalar)
            && timerScalar > 0.0)
        {
            outSnapshot->waitDelay = static_cast<int>(
                ceil(static_cast<double>(outSnapshot->pingStruct[3])
                     / (2.0 * timerScalar)));
        }
    }

    if (outSnapshot->waitDelay < 0)
    {
        outSnapshot->waitDelay = 0;
    }

    const int waitLoopCandidate =
        outSnapshot->localLen - outSnapshot->waitDelay - 1;
    outSnapshot->pauseRemoteInput =
        outSnapshot->valid
        && outSnapshot->state == 3
        && outSnapshot->windowBaseDelay == 0
        && outSnapshot->localLen >= 0
        && outSnapshot->remoteLen >= 0
        && waitLoopCandidate >= outSnapshot->remoteLen;
}

// Master gate for the per-frame Revival tick diagnostics: REVIVAL_TICK_ENTER /
// REVIVAL_TICK_EXIT / REVIVAL_PAUSE_REMOTE_INPUT / REVIVAL_BATCH_RENDER_*.
// These fire on EVERY online gameplay frame and accounted for ~91% of the log
// volume - and that logging cost was itself stalling the tick (multi-hundred-ms
// PERF_WARN spikes → dropped frames → desync). Off by default; flip to true
// only when actively debugging the netplay tick.
static constexpr bool kLogRevivalTickDiag = false;

static bool ShouldLogRevivalRemoteInputDiag(
    const RevivalRemoteInputDiagSnapshot& snapshot)
{
    return kLogRevivalTickDiag
        && snapshot.valid
        && g_localRoleFlag == kLocalRoleOnline
        && (snapshot.screen == 3 || snapshot.state == 3);
}

static const char* GameplayStallOriginToString(
    netplay::bridge::recovery::GameplayExitOrigin origin)
{
    switch (origin)
    {
    case netplay::bridge::recovery::GameplayExitOrigin::BypassStallConsoleError:
        return "bypass_stall_console_error";
    case netplay::bridge::recovery::GameplayExitOrigin::BypassStallSyncFrameStalled:
        return "bypass_stall_syncframe_stalled";
    case netplay::bridge::recovery::GameplayExitOrigin::BypassStallWallTimeout:
        return "bypass_stall_wall_timeout";
    default:
        return "unknown";
    }
}

static DWORD GameplayStallElapsedMs(const GameplayStallSample& sample)
{
    return g_gameplayStall.active
        ? sample.nowMs - g_gameplayStall.stallStartMs
        : 0u;
}

static DWORD GameplayStallSyncFrameStallMs(const GameplayStallSample& sample)
{
    if (!g_gameplayStall.syncFrameTracked)
    {
        return GameplayStallElapsedMs(sample);
    }
    return sample.nowMs - g_gameplayStall.lastSyncFrameChangeMs;
}

static void ResetGameplayStallTracker(
    const char* reason,
    const GameplayStallSample& sample,
    bool clearSyncTracking)
{
    if (g_gameplayStall.active)
    {
        mod::Log(
            "STALL_TRACK_RESET reason=%s previousStallMs=%lu previousBypassCount=%u previousCommitFrame=%d currentCommitFrame=%d screen=%u mode=%d",
            reason != nullptr ? reason : "unknown",
            static_cast<unsigned long>(GameplayStallElapsedMs(sample)),
            g_gameplayStall.bypassCount,
            g_gameplayStall.lastSyncFrame,
            sample.syncFrameValid ? sample.syncFrame : -1,
            static_cast<unsigned>(sample.screen),
            sample.mode);
    }

    g_gameplayStall.active = false;
    g_gameplayStall.stallStartMs = 0;
    g_gameplayStall.lastProgressLogMs = 0;
    g_gameplayStall.stallStartFrameTick = 0;
    g_gameplayStall.bypassCount = 0;
    g_gameplayStall.screen = 0xFF;
    g_gameplayStall.mode = -1;

    if (clearSyncTracking)
    {
        g_gameplayStall.syncFrameTracked = false;
        g_gameplayStall.lastSyncFrame = -1;
        g_gameplayStall.lastSyncFrameChangeMs = 0;
        g_gameplayStall.lastSyncFrameChangeTick = 0;
    }
}

void NotifyLocalProcessCloseForGameplayStall()
{
    InterlockedExchange(&g_gameplayStallLocalProcessCloseActive, 1);

    GameplayStallSample sample = {};
    sample.screen = ReadCurrentScreenIndexForRecovery();
    sample.mode = ReadCurrentGameModeForRecovery();
    sample.frameTick = g_frameTick;
    sample.nowMs = GetTickCount();
    sample.role = g_localRoleFlag;
    sample.recoveryInProgress =
        netplay::bridge::recovery::IsGameplayExitRecoveryInProgress();
    sample.pendingMenuEntry =
        netplay::bridge::recovery::HasPendingGameplayExitMenuEntry();
    sample.recoveryCompleted =
        netplay::bridge::recovery::WasGameplayExitRecoveryCompleted();
    const NetbridgeStatus status = netplay::bridge::GetStatus();
    sample.phase = status.phase;

    ResetGameplayStallTracker("local_process_close", sample, true);
}

void ClearLocalProcessCloseForGameplayStall()
{
    InterlockedExchange(&g_gameplayStallLocalProcessCloseActive, 0);
}

bool IsLocalProcessCloseForGameplayStallActive()
{
    return InterlockedCompareExchange(&g_gameplayStallLocalProcessCloseActive, 0, 0) != 0;
}

static void BeginGameplayStallTracker(
    const char* reason,
    const GameplayStallSample& sample)
{
    const bool startsFromSyncFreeze =
        sample.role == kLocalRoleOnline
        && !sample.originalTickBypassed
        && !sample.originalTickSkipped
        && sample.screen == 3
        && sample.syncFrameValid
        && g_gameplayStall.syncFrameTracked;

    g_gameplayStall.active = true;
    g_gameplayStall.stallStartMs = startsFromSyncFreeze
        ? g_gameplayStall.lastSyncFrameChangeMs
        : sample.nowMs;
    g_gameplayStall.lastProgressLogMs = sample.nowMs;
    g_gameplayStall.stallStartFrameTick = startsFromSyncFreeze
        ? g_gameplayStall.lastSyncFrameChangeTick
        : sample.frameTick;
    g_gameplayStall.bypassCount =
        (sample.originalTickBypassed || sample.originalTickSkipped) ? 1u : 0u;
    g_gameplayStall.screen = sample.screen;
    g_gameplayStall.mode = sample.mode;

    if (sample.syncFrameValid && !g_gameplayStall.syncFrameTracked)
    {
        g_gameplayStall.syncFrameTracked = true;
        g_gameplayStall.lastSyncFrame = sample.syncFrame;
        g_gameplayStall.lastSyncFrameChangeMs = sample.nowMs;
        g_gameplayStall.lastSyncFrameChangeTick = sample.frameTick;
    }

    mod::Log(
        "STALL_TRACK_BEGIN screen=%u mode=%d frameTick=%u commitFrame=%d consoleErrorSerial=%ld reason=%s",
        static_cast<unsigned>(sample.screen),
        sample.mode,
        sample.frameTick,
        sample.syncFrameValid ? sample.syncFrame : -1,
        static_cast<long>(sample.consoleErrorSerial),
        reason != nullptr ? reason : "unknown");
}

static void LogGameplayStallProgressIfDue(const GameplayStallSample& sample)
{
    const DWORD stallMs = GameplayStallElapsedMs(sample);
    if (sample.nowMs - g_gameplayStall.lastProgressLogMs
        < kGameplayStallProgressLogMs)
    {
        return;
    }

    g_gameplayStall.lastProgressLogMs = sample.nowMs;
    mod::Log(
        "STALL_TRACK_PROGRESS screen=%u mode=%d stallMs=%lu commitFrame=%d commitFrameStallMs=%lu bypassCount=%u consoleErrorSerial=%ld recoveryInProgress=%d phase=%d role=%d",
        static_cast<unsigned>(sample.screen),
        sample.mode,
        static_cast<unsigned long>(stallMs),
        sample.syncFrameValid ? sample.syncFrame : -1,
        static_cast<unsigned long>(GameplayStallSyncFrameStallMs(sample)),
        g_gameplayStall.bypassCount,
        static_cast<long>(sample.consoleErrorSerial),
        sample.recoveryInProgress ? 1 : 0,
        sample.phase,
        sample.role);
}

static void LogGameplayStallSuppressed(
    const char* reason,
    const GameplayStallSample& sample)
{
    mod::Log(
        "STALL_RECOVERY_SUPPRESSED reason=%s screen=%u mode=%d stallMs=%lu recoveryInProgress=%d pendingMenuEntry=%d completed=%d phase=%d role=%d",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned>(sample.screen),
        sample.mode,
        static_cast<unsigned long>(GameplayStallElapsedMs(sample)),
        sample.recoveryInProgress ? 1 : 0,
        sample.pendingMenuEntry ? 1 : 0,
        sample.recoveryCompleted ? 1 : 0,
        sample.phase,
        sample.role);
}

static bool TriggerGameplayStallRecovery(
    netplay::bridge::recovery::GameplayExitOrigin origin,
    const char* tier,
    const GameplayStallSample& sample)
{
    const char* originTag = GameplayStallOriginToString(origin);
    mod::Log(
        "STALL_RECOVERY_TRIGGER origin=%s tier=%s screen=%u mode=%d stallMs=%lu commitFrame=%d commitFrameStallMs=%lu bypassCount=%u consoleErrorSerial=%ld frameTick=%u phase=%d role=%d",
        originTag,
        tier != nullptr ? tier : "?",
        static_cast<unsigned>(sample.screen),
        sample.mode,
        static_cast<unsigned long>(GameplayStallElapsedMs(sample)),
        sample.syncFrameValid ? sample.syncFrame : -1,
        static_cast<unsigned long>(GameplayStallSyncFrameStallMs(sample)),
        g_gameplayStall.bypassCount,
        static_cast<long>(sample.consoleErrorSerial),
        sample.frameTick,
        sample.phase,
        sample.role);

    const bool started = netplay::bridge::recovery::BeginGameplayExitRecovery(origin);
    if (!started)
    {
        LogGameplayStallSuppressed("begin_recovery_failed", sample);
    }
    ResetGameplayStallTracker("shared_recovery_started", sample, true);
    return true;
}

static bool UpdateGameplayStallTracker(
    const char* reason,
    uintptr_t currentSession,
    bool originalTickBypassed,
    bool originalTickSkipped,
    bool originalTickRan)
{
    GameplayStallSample sample = {};
    sample.screen = ReadCurrentScreenIndexForRecovery();
    sample.mode = ReadCurrentGameModeForRecovery();
    sample.frameTick = g_frameTick;
    sample.originalTickBypassed = originalTickBypassed;
    sample.originalTickSkipped = originalTickSkipped;
    sample.originalTickRan = originalTickRan;
    sample.nowMs = GetTickCount();
    sample.role = g_localRoleFlag;
    sample.recoveryInProgress =
        netplay::bridge::recovery::IsGameplayExitRecoveryInProgress();
    sample.pendingMenuEntry =
        netplay::bridge::recovery::HasPendingGameplayExitMenuEntry();
    sample.recoveryCompleted =
        netplay::bridge::recovery::WasGameplayExitRecoveryCompleted();
    if (g_hostBlock != nullptr)
    {
        sample.consoleErrorSerial =
            InterlockedCompareExchange(&g_hostBlock->consoleErrorSerial, 0, 0);
    }
    // sessionOffsetGameModeSnapshot belongs to the rollback/online object.
    // A 1.02j spectator session has a different MinGW layout; reading the
    // rollback offset there produced the stable unrelated value 10800 and
    // falsely triggered tier-B recovery exactly five seconds into gameplay.
    sample.syncFrameValid =
        sample.role == kLocalRoleOnline
        && ReadGameplaySyncFrameForRecovery(currentSession, &sample.syncFrame);

    if (!IsGameplayExitRecoveryScreen(sample.screen))
    {
        ResetGameplayStallTracker("screen_left_gameplay", sample, true);
        return false;
    }

    if (InterlockedCompareExchange(
            &g_gameplayStallLocalProcessCloseActive,
            0,
            0) != 0)
    {
        ResetGameplayStallTracker("local_process_close", sample, true);
        return false;
    }

    const NetbridgeStatus status = netplay::bridge::GetStatus();
    sample.phase = status.phase;

    if (sample.recoveryInProgress
        || sample.pendingMenuEntry
        || sample.recoveryCompleted)
    {
        if (g_gameplayStall.active
            || sample.originalTickBypassed
            || sample.originalTickSkipped)
        {
            LogGameplayStallSuppressed("shared_recovery_active", sample);
        }
        ResetGameplayStallTracker("shared_recovery_active", sample, true);
        return sample.recoveryInProgress || sample.pendingMenuEntry;
    }

    if (sample.phase != static_cast<int>(NetbridgePhase::Connected))
    {
        ResetGameplayStallTracker("phase_left_connected", sample, true);
        return false;
    }

    if (sample.role != kLocalRoleOnline && sample.role != kLocalRoleSpectate)
    {
        ResetGameplayStallTracker("role_not_netplay", sample, true);
        return false;
    }

    if (!g_localInitAppliedForSession || !g_dllExitProcessPatchesSaved)
    {
        ResetGameplayStallTracker("session_not_active", sample, true);
        return false;
    }

    if (currentSession == 0)
    {
        ResetGameplayStallTracker("session_pointer_missing", sample, true);
        return false;
    }

    if (g_revivalProcess == nullptr)
    {
        ResetGameplayStallTracker("helper_inactive", sample, true);
        return false;
    }

    if (sample.role == kLocalRoleOnline && sample.syncFrameValid)
    {
        if (!g_gameplayStall.syncFrameTracked)
        {
            g_gameplayStall.syncFrameTracked = true;
            g_gameplayStall.lastSyncFrame = sample.syncFrame;
            g_gameplayStall.lastSyncFrameChangeMs = sample.nowMs;
            g_gameplayStall.lastSyncFrameChangeTick = sample.frameTick;
        }
        else if (sample.syncFrame != g_gameplayStall.lastSyncFrame)
        {
            ResetGameplayStallTracker("syncFrame_advanced", sample, false);
            g_gameplayStall.syncFrameTracked = true;
            g_gameplayStall.lastSyncFrame = sample.syncFrame;
            g_gameplayStall.lastSyncFrameChangeMs = sample.nowMs;
            g_gameplayStall.lastSyncFrameChangeTick = sample.frameTick;
            return false;
        }
    }
    else if (sample.role == kLocalRoleOnline
        && sample.originalTickRan
        && !sample.originalTickBypassed)
    {
        ResetGameplayStallTracker("syncFrame_unavailable_after_normal_tick", sample, true);
        return false;
    }

    const DWORD syncFrameStallMs = GameplayStallSyncFrameStallMs(sample);
    const bool syncFrameFrozen =
        sample.role == kLocalRoleOnline
        && sample.screen == 3
        && sample.syncFrameValid
        && g_gameplayStall.syncFrameTracked
        && syncFrameStallMs >= kGameplayStallConsoleErrorMs;
    const bool startCandidate =
        sample.originalTickBypassed
        || sample.originalTickSkipped
        || (sample.screen == 3 && syncFrameFrozen)
        || sample.consoleErrorSerial > 0;

    if (!g_gameplayStall.active)
    {
        if (!startCandidate)
        {
            return false;
        }
        BeginGameplayStallTracker(reason, sample);
    }
    else if (sample.originalTickBypassed || sample.originalTickSkipped)
    {
        ++g_gameplayStall.bypassCount;
    }

    LogGameplayStallProgressIfDue(sample);

    const DWORD stallMs = GameplayStallElapsedMs(sample);
    if (sample.consoleErrorSerial > 0
        && IsGameplayExitRecoveryScreen(sample.screen)
        && stallMs >= kGameplayStallConsoleErrorMs)
    {
        return TriggerGameplayStallRecovery(
            netplay::bridge::recovery::GameplayExitOrigin::BypassStallConsoleError,
            "A",
            sample);
    }

    if (sample.role == kLocalRoleOnline
        && sample.screen == 3
        && sample.syncFrameValid
        && stallMs >= kGameplayStallSyncFrameMs
        && syncFrameStallMs >= kGameplayStallSyncFrameMs)
    {
        return TriggerGameplayStallRecovery(
            netplay::bridge::recovery::GameplayExitOrigin::BypassStallSyncFrameStalled,
            "B",
            sample);
    }

    if (sample.screen == 3
        && stallMs >= kGameplayStallWallTimeoutMs)
    {
        return TriggerGameplayStallRecovery(
            netplay::bridge::recovery::GameplayExitOrigin::BypassStallWallTimeout,
            "C",
            sample);
    }

    return false;
}

static bool ShouldSuppressOldGameplayExitTeardown(const char* reason)
{
    if (netplay::bridge::recovery::ShouldSuppressLegacyGameplayExitCleanup())
    {
        mod::Log(
            "GAMEPLAY_EXIT_RECOVERY_SUPPRESS_OLD_TEARDOWN reason=%s inProgress=%d pendingMenu=%d completed=%d origin=%s state=%s frontendReturnState=%s",
            reason != nullptr ? reason : "unknown",
            netplay::bridge::recovery::IsGameplayExitRecoveryInProgress() ? 1 : 0,
            netplay::bridge::recovery::HasPendingGameplayExitMenuEntry() ? 1 : 0,
            netplay::bridge::recovery::WasGameplayExitRecoveryCompleted() ? 1 : 0,
            netplay::bridge::recovery::CurrentGameplayExitRecoveryOrigin(),
            netplay::bridge::recovery::CurrentGameplayExitRecoveryStateName(),
            netplay::bridge::frontend_return::CurrentStateName());
        return true;
    }
    return false;
}

uint32_t GetGameplayExitRecoveryFrameTick()
{
    return g_frameTick;
}

bool IsGameplayExitRecoveryInsideFrameTick()
{
    return g_insideFrameTick;
}

bool ClearDeferredCancelCleanupForRecovery(const char* reason)
{
    const bool wasSet = g_deferredCancelCleanup;
    g_deferredCancelCleanup = false;
    mod::Log(
        "GAMEPLAY_EXIT_CLEAR_STALE_DEFERRED_CLEANUP oldPending=%d reason=%s sourceReason=%s sourceScreen=%u sourceRole=%d sourceFrame=%u",
        wasSet ? 1 : 0,
        reason != nullptr ? reason : "unknown",
        g_deferredCancelCleanupReason[0] != '\0' ? g_deferredCancelCleanupReason : "none",
        static_cast<unsigned>(g_deferredCancelCleanupSourceScreen),
        g_deferredCancelCleanupSourceRole,
        g_deferredCancelCleanupSourceFrame);
    g_deferredCancelCleanupReason[0] = '\0';
    g_deferredCancelCleanupSourceScreen = 0xFF;
    g_deferredCancelCleanupSourceRole = -1;
    g_deferredCancelCleanupSourceFrame = 0;
    return wasSet;
}

bool SuppressDeferredCancelCleanupAfterGameplayRecovery(const char* origin)
{
    const bool wasSet = g_deferredCancelCleanup;
    mod::Log(
        "GAMEPLAY_EXIT_SUPPRESS_DEFERRED_AFTER_MENU_ENTRY pending=%d origin=%s sourceReason=%s sourceScreen=%u state=%s frontendReturnState=%s",
        wasSet ? 1 : 0,
        origin != nullptr ? origin : "unknown",
        g_deferredCancelCleanupReason[0] != '\0' ? g_deferredCancelCleanupReason : "none",
        static_cast<unsigned>(g_deferredCancelCleanupSourceScreen),
        netplay::bridge::recovery::CurrentGameplayExitRecoveryStateName(),
        netplay::bridge::frontend_return::CurrentStateName());
    if (wasSet)
    {
        mod::Log("GAMEPLAY_EXIT_INVARIANT_VIOLATION name=deferred_cleanup_after_menu_entry");
    }
    (void)ClearDeferredCancelCleanupForRecovery("after_menu_entry");
    return wasSet;
}

bool IsDeferredCancelCleanupPending()
{
    return g_deferredCancelCleanup;
}

bool IsDeferredCancelCleanupGameplaySource()
{
    return g_deferredCancelCleanup
        && (g_deferredCancelCleanupSourceScreen == 3
            || g_deferredCancelCleanupSourceScreen == 5);
}

const char* CurrentDeferredCancelCleanupReason()
{
    return g_deferredCancelCleanupReason[0] != '\0'
        ? g_deferredCancelCleanupReason
        : "none";
}

uint8_t CurrentDeferredCancelCleanupSourceScreen()
{
    return g_deferredCancelCleanupSourceScreen;
}

static bool EnsureQuitRingHeader();
static void ReleaseQuitRingHeader();
static bool ConsumeGracefulQuitRingSignal(LONG* outHeadBefore, LONG* outTailBefore);
static void ResetLocalBattleEscQuitRingIgnore();
static void ArmLocalBattleEscQuitRingIgnore();
static bool ConsumeLocalBattleEscQuitRingIgnore(
    const char* phaseTag,
    LONG quitHeadBefore,
    LONG quitTailBefore);
static void ResetScheduledGracefulQuitTeardown();
static void ScheduleGracefulQuitTeardown(const char* phaseTag, LONG quitHeadBefore, LONG quitTailBefore);
static char FinalizeGracefulQuitTeardown(const char* phaseTag, LONG quitHeadBefore, LONG quitTailBefore, const char* originTag);
static char RecoverFromQuitRingSignal(const char* phaseTag, LONG quitHeadBefore, LONG quitTailBefore);

// --- Double-speed diagnostics -------------------------------------------
// Wall-clock time tracking: measure actual FPS by comparing timeGetTime()
// between heartbeats.
static DWORD g_lastHeartbeatTimeMs = 0;
static uint32_t g_lastHeartbeatFrameTick = 0;

// Toggle tracking: gameSys+4968 toggles exactly once per EXE main loop
// iteration.  If our hook sees the same toggle value on consecutive calls,
// the hook is being invoked more than once per main loop frame.
static uint32_t g_lastToggleValue = 0xFFFFFFFFu;
static uint32_t g_toggleSameCount = 0;  // consecutive same-toggle detections
static bool     g_toggleDiagLogged = false;

// Return-address tracking: capture distinct call sites that invoke the
// per-frame hook so we can identify which EXE code paths fire it.
static constexpr int kMaxRetAddrSlots = 8;
static uintptr_t g_retAddrSlots[kMaxRetAddrSlots] = {};
static int       g_retAddrSlotCount = 0;

// Session number: incremented each time ResetGameModeValidation is called
// (i.e. each new session).  Logged in every SPEED_DIAG line so we can
// immediately tell which session produced a given log entry.
static uint32_t g_sessionNumber = 0;

// Guard for per-frame SPEED_DIAG logging.  Disabled by default to avoid
// flooding the log in tournament mode.  Enable when debugging speed issues.
static bool g_speedDiagEnabled = false;

// Timer baseline snapshot: captured on the first SPEED_DIAG read of each
// session.  If timerScalar or timerInterval change later, we log an alert.
static double   g_baselineTimerScalar   = 0.0;
static double   g_baselineTimerInterval = 0.0;
static uintptr_t g_baselineTimerPtr     = 0;
static bool     g_timerBaselineCaptured = false;

// Init-once guard transition tracking: detect when the guard changes
// between frames (should only happen once during first-time global init).
static uint16_t g_lastInitOnceGuard     = 0;
static bool     g_initOnceGuardTracked  = false;

// Render context pointer tracking: detect if the EfzRender* global goes
// NULL or changes unexpectedly between frames (H2 stale context).
static uintptr_t g_lastRenderCtxPtr     = 0;
static bool     g_renderCtxTracked      = false;

// Session pointer tracking within a session: detect if dword_100A02CC
// is silently replaced mid-session (H1 double-init / leaked session).
static uintptr_t g_lastSessionPtrInTick = 0;
static bool     g_sessionPtrTracked     = false;

// Per-frame QPC delta for burst logging: log individual frame-to-frame
// intervals during the first 30 ticks to detect doubled rate from tick 1.
static LARGE_INTEGER g_prevFrameQpc     = {};
static bool     g_prevFrameQpcValid     = false;

// --- FPS drop detection & tick cost tracking ----------------------------
// Fires whenever the measured inter-frame interval exceeds 33.3ms (< 30 fps)
// or when the mod's own per-frame tick processing exceeds a budget.
static LARGE_INTEGER g_fpsDropPrevQpc       = {};   // QPC at start of previous tick
static bool     g_fpsDropPrevQpcValid       = false;
static uint32_t g_fpsDropCount              = 0;    // total drops in session
static uint32_t g_fpsDropLastLogTick        = 0;    // throttle: last frameTick logged
static uint32_t g_tickBudgetExceededCount   = 0;    // frames where mod cost > budget
static constexpr double kFpsDropThresholdMs = 33.33; // 30 fps
static constexpr double kTickBudgetMs       = 5.0;   // mod processing budget per frame
static LARGE_INTEGER g_qpcFreqCached        = {};    // cached once
static bool     g_qpcFreqValid              = false;

// Read the raw session pointer from dword_100A02CC without heuristic
// validation.  Used in the per-frame tick hot path.
static uintptr_t ReadSessionPtrRaw()
{
    if (g_activeRevival == nullptr || g_activeRevival->sessionPtrOffsetCount == 0)
        return 0;
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
        return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t sessionPtr = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[0]),
        &sessionPtr);
    return sessionPtr;
}

struct FpuControlSnapshot
{
    unsigned int crtControl = 0;
    uint16_t x87Control = 0;
    uint32_t mxcsr = 0;
    bool crtValid = false;
    bool x87Valid = false;
    bool mxcsrValid = false;
};

static constexpr unsigned int kRevivalCanonicalCrtControl = 0x0009001Fu;
static constexpr uint16_t kRevivalCanonicalX87Control = 0x027Fu;
static constexpr uint32_t kRevivalCanonicalMxcsr = 0x00001FA0u;

static FpuControlSnapshot CanonicalRevivalNetplayFpuSnapshot()
{
    FpuControlSnapshot snapshot = {};
    snapshot.crtControl = kRevivalCanonicalCrtControl;
    snapshot.x87Control = kRevivalCanonicalX87Control;
    snapshot.mxcsr = kRevivalCanonicalMxcsr;
    snapshot.crtValid = true;
    snapshot.x87Valid = true;
    snapshot.mxcsrValid = true;
    return snapshot;
}

static bool CaptureX87ControlWord(uint16_t* outValue)
{
    if (outValue == nullptr)
    {
        return false;
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    uint16_t value = 0;
    __try
    {
        __asm fnstcw value
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    *outValue = value;
    return true;
#else
    return false;
#endif
}

static bool ApplyX87ControlWord(uint16_t value)
{
#if defined(_MSC_VER) && defined(_M_IX86)
    __try
    {
        __asm fldcw value
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
#else
    (void)value;
    return false;
#endif
}

static bool CaptureMxcsr(uint32_t* outValue)
{
    if (outValue == nullptr)
    {
        return false;
    }

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    __try
    {
        *outValue = _mm_getcsr();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
#else
    return false;
#endif
}

static bool ApplyMxcsr(uint32_t value)
{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    __try
    {
        _mm_setcsr(value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
#else
    (void)value;
    return false;
#endif
}

static FpuControlSnapshot CaptureFpuControlSnapshot()
{
    FpuControlSnapshot snapshot = {};

    unsigned int crtControl = 0;
    if (_controlfp_s(&crtControl, 0, 0) == 0)
    {
        snapshot.crtControl = crtControl;
        snapshot.crtValid = true;
    }

    snapshot.x87Valid = CaptureX87ControlWord(&snapshot.x87Control);
    snapshot.mxcsrValid = CaptureMxcsr(&snapshot.mxcsr);
    return snapshot;
}

static bool FpuSnapshotHasAnyState(const FpuControlSnapshot& snapshot)
{
    return snapshot.crtValid || snapshot.x87Valid || snapshot.mxcsrValid;
}

static bool FpuSnapshotDiffersFromBaseline(
    const FpuControlSnapshot& current,
    const FpuControlSnapshot& baseline)
{
    if (baseline.crtValid
        && (!current.crtValid || current.crtControl != baseline.crtControl))
    {
        return true;
    }
    if (baseline.x87Valid
        && (!current.x87Valid || current.x87Control != baseline.x87Control))
    {
        return true;
    }
    if (baseline.mxcsrValid
        && (!current.mxcsrValid || current.mxcsr != baseline.mxcsr))
    {
        return true;
    }
    return false;
}

static bool ApplyFpuControlSnapshot(const FpuControlSnapshot& snapshot)
{
    bool appliedAny = false;
    if (snapshot.crtValid)
    {
        unsigned int ignored = 0;
        unsigned int mask = _MCW_EM | _MCW_PC | _MCW_RC;
#if defined(_MCW_IC)
        mask |= _MCW_IC;
#endif
#if defined(_MCW_DN)
        mask |= _MCW_DN;
#endif
        if (_controlfp_s(&ignored, snapshot.crtControl, mask) == 0)
        {
            appliedAny = true;
        }
    }
    if (snapshot.x87Valid)
    {
        appliedAny = ApplyX87ControlWord(snapshot.x87Control) || appliedAny;
    }
    if (snapshot.mxcsrValid)
    {
        appliedAny = ApplyMxcsr(snapshot.mxcsr) || appliedAny;
    }
    return appliedAny;
}

static FpuControlSnapshot g_revivalFpuBaseline = {};
static bool g_revivalFpuBaselineCaptured = false;
static uintptr_t g_revivalFpuBaselineSession = 0;
static uint32_t g_revivalFpuNormalizeCount = 0;

static bool g_syncDiagTrackerValid = false;
static uintptr_t g_syncDiagSession = 0;
static int g_syncDiagLastMatchId = -1;
static int g_syncDiagLastSyncFrame = -1;
static int g_syncDiagLastCurrentFrame = -1;

static bool ShouldManageRevivalFpuState(uintptr_t sessionPtr)
{
    return g_activeRevival != nullptr
        && sessionPtr != 0
        && g_localRoleFlag != kLocalRoleLocalPlay;
}

static void LogFpuSnapshotLine(
    const char* tag,
    const char* context,
    uintptr_t sessionPtr,
    const FpuControlSnapshot& snapshot)
{
    mod::Log(
        "%s[%s]: S#%u tick=%u session=0x%08lX "
        "crt=%s0x%08lX x87=%s0x%04X mxcsr=%s0x%08lX",
        tag != nullptr ? tag : "FPU",
        context != nullptr ? context : "unknown",
        g_sessionNumber,
        g_frameTick,
        static_cast<unsigned long>(sessionPtr),
        snapshot.crtValid ? "" : "invalid:",
        static_cast<unsigned long>(snapshot.crtControl),
        snapshot.x87Valid ? "" : "invalid:",
        static_cast<unsigned>(snapshot.x87Control),
        snapshot.mxcsrValid ? "" : "invalid:",
        static_cast<unsigned long>(snapshot.mxcsr));
}

static void CaptureRevivalFpuBaselineForSession(const char* context, uintptr_t sessionPtr)
{
    // Revival serializes _controlfp_s() into its sync packets.  1.02j's MinGW
    // online startup can leave host/client in different precision states
    // (observed host 0xA001F/x87 0x007F vs client 0x9001F/x87 0x027F), so a
    // per-process captured baseline preserves desync.  Use the stable Revival
    // netplay state instead for every non-local session.
    g_revivalFpuBaseline = ShouldManageRevivalFpuState(sessionPtr)
        ? CanonicalRevivalNetplayFpuSnapshot()
        : CaptureFpuControlSnapshot();
    g_revivalFpuBaselineCaptured = FpuSnapshotHasAnyState(g_revivalFpuBaseline);
    g_revivalFpuBaselineSession = sessionPtr;
    g_revivalFpuNormalizeCount = 0;

    if (SyncDiagnosticsEnabled())
    {
        LogFpuSnapshotLine(
            "FPU_SYNC_BASELINE",
            context,
            sessionPtr,
            g_revivalFpuBaseline);
    }
}

static void EnsureRevivalFpuBaselineForSession(const char* context, uintptr_t sessionPtr)
{
    if (!ShouldManageRevivalFpuState(sessionPtr))
    {
        return;
    }
    if (!g_revivalFpuBaselineCaptured || g_revivalFpuBaselineSession != sessionPtr)
    {
        CaptureRevivalFpuBaselineForSession(context, sessionPtr);
    }
}

static void NormalizeRevivalFpuState(const char* context, uintptr_t sessionPtr)
{
    if (!ShouldManageRevivalFpuState(sessionPtr))
    {
        return;
    }

    EnsureRevivalFpuBaselineForSession(context, sessionPtr);
    if (!g_revivalFpuBaselineCaptured)
    {
        return;
    }

    const FpuControlSnapshot before = CaptureFpuControlSnapshot();
    if (!FpuSnapshotDiffersFromBaseline(before, g_revivalFpuBaseline))
    {
        return;
    }

    const bool applied = ApplyFpuControlSnapshot(g_revivalFpuBaseline);
    const FpuControlSnapshot after = CaptureFpuControlSnapshot();
    ++g_revivalFpuNormalizeCount;

    if (SyncDiagnosticsEnabled())
    {
        LogFpuSnapshotLine("FPU_SYNC_NORMALIZE_BEFORE", context, sessionPtr, before);
        LogFpuSnapshotLine("FPU_SYNC_NORMALIZE_BASE", context, sessionPtr, g_revivalFpuBaseline);
        LogFpuSnapshotLine("FPU_SYNC_NORMALIZE_AFTER", context, sessionPtr, after);
        mod::Log(
            "FPU_SYNC_NORMALIZE[%s]: S#%u tick=%u session=0x%08lX "
            "applied=%d count=%u",
            context != nullptr ? context : "unknown",
            g_sessionNumber,
            g_frameTick,
            static_cast<unsigned long>(sessionPtr),
            applied ? 1 : 0,
            g_revivalFpuNormalizeCount);
    }
}

struct SyncDiagRingProbe
{
    const char* name = nullptr;
    DWORD head = 0;
    DWORD tail = 0;
    bool ok = false;
};

static void ProbeSyncDiagRing(SyncDiagRingProbe* probe)
{
    if (probe == nullptr || probe->name == nullptr)
    {
        return;
    }

    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, probe->name);
    if (hMap == nullptr)
    {
        return;
    }

    const volatile DWORD* view = static_cast<const volatile DWORD*>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 8));
    if (view != nullptr)
    {
        probe->head = view[0];
        probe->tail = view[1];
        probe->ok = true;
        UnmapViewOfFile(const_cast<DWORD*>(view));
    }
    CloseHandle(hMap);
}

static void ReadCoreSyncDiagFields(
    uintptr_t sessionPtr,
    int* outCurrentFrame,
    int* outMatchId,
    int* outSyncFrame)
{
    if (outCurrentFrame != nullptr)
    {
        *outCurrentFrame = -1;
    }
    if (outMatchId != nullptr)
    {
        *outMatchId = -1;
    }
    if (outSyncFrame != nullptr)
    {
        *outSyncFrame = -1;
    }
    if (g_activeRevival == nullptr || sessionPtr == 0)
    {
        return;
    }

    if (outCurrentFrame != nullptr)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
            outCurrentFrame);
    }
    if (outMatchId != nullptr)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetMatchId),
            outMatchId);
    }
    if (outSyncFrame != nullptr)
    {
        const uintptr_t gmBase =
            sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot;
        (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), outSyncFrame);
    }
}

static void LogRevivalSyncDiagnosticSnapshot(
    const char* context,
    uintptr_t sessionPtr,
    const char* reason)
{
    if (!SyncDiagnosticsEnabled())
    {
        return;
    }

    const char* ctx = context != nullptr ? context : "unknown";
    const char* why = reason != nullptr ? reason : "unknown";
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    const uintptr_t dllBase = revival ? reinterpret_cast<uintptr_t>(revival) : 0;
    const char* version =
        (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
            ? g_activeRevival->versionTag
            : "unknown";

    uint8_t screenIdx = 0xFF;
    __try
    {
        screenIdx = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    uintptr_t sessionVtable = 0;
    int inputDelay = -1;
    int activePlayer = -1;
    int queuePlayer = -1;
    int currentFrame = -1;
    int matchId = -1;
    int initComplete = -1;
    int pingMs = -1;
    uint32_t sentinel = 0;
    int prevGameMode = -1;
    int curGameMode = -1;
    int matchStartFrame = -1;
    int advanceCounter = -1;
    int syncFrame = -1;
    int highestFrame = -1;
    uint32_t pingStruct[4] = {
        0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu};

    if (g_activeRevival != nullptr && sessionPtr != 0)
    {
        (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &sessionVtable);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetInputDelay),
            &inputDelay);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
            &activePlayer);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetQueuePlayer),
            &queuePlayer);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
            &currentFrame);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetMatchId),
            &matchId);
        if (g_activeRevival->sessionOffsetInitComplete != 0)
        {
            (void)SafeReadInt(
                reinterpret_cast<const void*>(
                    sessionPtr + g_activeRevival->sessionOffsetInitComplete),
                &initComplete);
        }
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetPingMs),
            &pingMs);
        (void)SafeReadDword(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetSentinel),
            &sentinel);

        const uintptr_t gmBase =
            sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot;
        (void)SafeReadInt(reinterpret_cast<const void*>(gmBase), &prevGameMode);
        (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 4), &curGameMode);
        (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 8), &matchStartFrame);
        (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 12), &advanceCounter);
        (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), &syncFrame);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                sessionPtr + g_activeRevival->sessionOffsetSentinel + 4),
            &highestFrame);

        const uintptr_t pingBase =
            sessionPtr + g_activeRevival->sessionOffsetPingStructBase;
        for (int pi = 0; pi < 4; ++pi)
        {
            (void)SafeReadInt(
                reinterpret_cast<const void*>(pingBase + pi * 4),
                reinterpret_cast<int*>(&pingStruct[pi]));
        }
    }

    uintptr_t timerPtr = 0;
    uintptr_t renderCtx = 0;
    uintptr_t globalState = 0;
    int initFlag = -1;
    double timerInterval = 0.0;
    double timerScalar = 0.0;
    uint16_t initOnceGuard = 0;

    if (dllBase != 0 && g_activeRevival != nullptr)
    {
        (void)SafeReadWord(
            reinterpret_cast<const void*>(dllBase + g_activeRevival->initOnceGuardOffset),
            &initOnceGuard);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(dllBase + g_activeRevival->timerPtrOffset),
            &timerPtr);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(dllBase + g_activeRevival->renderContextGlobalOffset),
            &renderCtx);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(dllBase + g_activeRevival->globalStatePtrOffset),
            &globalState);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(dllBase + g_activeRevival->initFlagOffset),
            &initFlag);
        if (timerPtr != 0)
        {
            (void)SafeReadDouble(reinterpret_cast<const void*>(timerPtr + 16), &timerInterval);
            (void)SafeReadDouble(reinterpret_cast<const void*>(timerPtr + 32), &timerScalar);
        }
    }

    TwoByteVectorSnapshot localInputs = {};
    TwoByteVectorSnapshot remoteInputs = {};
    TwoByteVectorSnapshot predictedInputs = {};
    localInputs.length = -1;
    remoteInputs.length = -1;
    predictedInputs.length = -1;
    if (g_activeRevival != nullptr
        && sessionPtr != 0
        && g_activeRevival->sessionOffsetHistoryPrimaryVec != 0
        && g_activeRevival->sessionOffsetHistorySecondaryVec != 0)
    {
        localInputs = ReadTwoByteVectorSnapshot(
            sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryVec);
        remoteInputs = ReadTwoByteVectorSnapshot(
            sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryVec);
        if (IsRevival102jProfile())
        {
            predictedInputs = ReadTwoByteVectorSnapshot(
                sessionPtr + kRevival102jOffsetPredictedInputs);
        }
    }

    SyncDiagRingProbe rings[] = {
        {RevivalWireName("InputP1"), 0, 0, false},
        {RevivalWireName("InputP2"), 0, 0, false},
        {RevivalWireName("PaletteP1"), 0, 0, false},
        {RevivalWireName("PaletteP2"), 0, 0, false},
        {RevivalWireName("Sync"), 0, 0, false},
        {RevivalWireName("Net"), 0, 0, false},
        {RevivalWireName("Quit"), 0, 0, false},
        {RevivalWireName("LoadMatch"), 0, 0, false},
        {RevivalWireName("Init"), 0, 0, false},
    };
    for (size_t i = 0; i < sizeof(rings) / sizeof(rings[0]); ++i)
    {
        ProbeSyncDiagRing(&rings[i]);
    }

    const FpuControlSnapshot fpuNow = CaptureFpuControlSnapshot();
    const uintptr_t vtableRva =
        (dllBase != 0 && sessionVtable >= dllBase)
            ? sessionVtable - dllBase
            : 0;

    mod::Log(
        "SYNC_DIAG[%s]: reason=%s S#%u tick=%u version=%s role=%d netRole=%d "
        "screen=%u session=0x%08lX vtableRVA=0x%lX",
        ctx,
        why,
        g_sessionNumber,
        g_frameTick,
        version,
        g_localRoleFlag,
        g_netplayRole,
        static_cast<unsigned>(screenIdx),
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(vtableRva));

    mod::Log(
        "SYNC_DIAG[%s]: frame=%d matchId=%d prevMode=%d curMode=%d "
        "matchStart=%d advCtr=%d syncFrame=%d highFrame=%d "
        "delay=%d pingMs=%d active=%d queue=%d initComplete=%d sentinel=0x%08lX",
        ctx,
        currentFrame,
        matchId,
        prevGameMode,
        curGameMode,
        matchStartFrame,
        advanceCounter,
        syncFrame,
        highestFrame,
        inputDelay,
        pingMs,
        activePlayer,
        queuePlayer,
        initComplete,
        static_cast<unsigned long>(sentinel));

    mod::Log(
        "SYNC_DIAG[%s]: fpu current(crt=%s0x%08lX x87=%s0x%04X mxcsr=%s0x%08lX) "
        "baseline(session=0x%08lX crt=%s0x%08lX x87=%s0x%04X mxcsr=%s0x%08lX) "
        "normalizeCount=%u",
        ctx,
        fpuNow.crtValid ? "" : "invalid:",
        static_cast<unsigned long>(fpuNow.crtControl),
        fpuNow.x87Valid ? "" : "invalid:",
        static_cast<unsigned>(fpuNow.x87Control),
        fpuNow.mxcsrValid ? "" : "invalid:",
        static_cast<unsigned long>(fpuNow.mxcsr),
        static_cast<unsigned long>(g_revivalFpuBaselineSession),
        g_revivalFpuBaseline.crtValid ? "" : "invalid:",
        static_cast<unsigned long>(g_revivalFpuBaseline.crtControl),
        g_revivalFpuBaseline.x87Valid ? "" : "invalid:",
        static_cast<unsigned>(g_revivalFpuBaseline.x87Control),
        g_revivalFpuBaseline.mxcsrValid ? "" : "invalid:",
        static_cast<unsigned long>(g_revivalFpuBaseline.mxcsr),
        g_revivalFpuNormalizeCount);

    mod::Log(
        "SYNC_DIAG[%s]: ping=[%u,%u,%u,%u] timerPtr=0x%08lX "
        "timerScalar=%.6f timerInterval=%.6f initOnceGuard=0x%04X "
        "initFlag=%d renderCtx=0x%08lX globalState=0x%08lX",
        ctx,
        pingStruct[0],
        pingStruct[1],
        pingStruct[2],
        pingStruct[3],
        static_cast<unsigned long>(timerPtr),
        timerScalar,
        timerInterval,
        static_cast<unsigned>(initOnceGuard),
        initFlag,
        static_cast<unsigned long>(renderCtx),
        static_cast<unsigned long>(globalState));

    mod::Log(
        "SYNC_DIAG[%s]: inputVec local=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d "
        "remote=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d "
        "predicted=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d",
        ctx,
        static_cast<unsigned long>(localInputs.begin),
        static_cast<unsigned long>(localInputs.end),
        static_cast<unsigned long>(localInputs.capacity),
        localInputs.length,
        localInputs.valid ? 1 : 0,
        static_cast<unsigned long>(remoteInputs.begin),
        static_cast<unsigned long>(remoteInputs.end),
        static_cast<unsigned long>(remoteInputs.capacity),
        remoteInputs.length,
        remoteInputs.valid ? 1 : 0,
        static_cast<unsigned long>(predictedInputs.begin),
        static_cast<unsigned long>(predictedInputs.end),
        static_cast<unsigned long>(predictedInputs.capacity),
        predictedInputs.length,
        predictedInputs.valid ? 1 : 0);

    mod::Log(
        "SYNC_DIAG[%s]: rings "
        "InputP1(%s h=%lu t=%lu d=%ld) InputP2(%s h=%lu t=%lu d=%ld) "
        "PaletteP1(%s h=%lu t=%lu) PaletteP2(%s h=%lu t=%lu) "
        "Sync(%s h=%lu t=%lu) Net(%s h=%lu t=%lu) "
        "Quit(%s h=%lu t=%lu) LoadMatch(%s h=%lu t=%lu) Init(%s h=%lu t=%lu)",
        ctx,
        rings[0].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[0].head),
        static_cast<unsigned long>(rings[0].tail),
        static_cast<long>(rings[0].tail - rings[0].head),
        rings[1].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[1].head),
        static_cast<unsigned long>(rings[1].tail),
        static_cast<long>(rings[1].tail - rings[1].head),
        rings[2].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[2].head),
        static_cast<unsigned long>(rings[2].tail),
        rings[3].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[3].head),
        static_cast<unsigned long>(rings[3].tail),
        rings[4].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[4].head),
        static_cast<unsigned long>(rings[4].tail),
        rings[5].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[5].head),
        static_cast<unsigned long>(rings[5].tail),
        rings[6].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[6].head),
        static_cast<unsigned long>(rings[6].tail),
        rings[7].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[7].head),
        static_cast<unsigned long>(rings[7].tail),
        rings[8].ok ? "ok" : "NO",
        static_cast<unsigned long>(rings[8].head),
        static_cast<unsigned long>(rings[8].tail));
}

static void TrackRevivalSyncDiagnosticsAfterTick(
    const char* context,
    uintptr_t sessionPtr)
{
    if (!SyncDiagnosticsEnabled()
        || g_activeRevival == nullptr
        || sessionPtr == 0
        || g_localRoleFlag != kLocalRoleOnline)
    {
        return;
    }

    int currentFrame = -1;
    int matchId = -1;
    int syncFrame = -1;
    ReadCoreSyncDiagFields(sessionPtr, &currentFrame, &matchId, &syncFrame);

    if (!g_syncDiagTrackerValid || g_syncDiagSession != sessionPtr)
    {
        g_syncDiagTrackerValid = true;
        g_syncDiagSession = sessionPtr;
        g_syncDiagLastCurrentFrame = currentFrame;
        g_syncDiagLastMatchId = matchId;
        g_syncDiagLastSyncFrame = syncFrame;
        LogRevivalSyncDiagnosticSnapshot(context, sessionPtr, "session_observed");
        return;
    }

    if (matchId != g_syncDiagLastMatchId
        || (currentFrame >= 0
            && g_syncDiagLastCurrentFrame >= 0
            && currentFrame < g_syncDiagLastCurrentFrame))
    {
        g_syncDiagLastCurrentFrame = currentFrame;
        g_syncDiagLastMatchId = matchId;
        g_syncDiagLastSyncFrame = syncFrame;
        LogRevivalSyncDiagnosticSnapshot(context, sessionPtr, "new_game");
        return;
    }

    if (syncFrame != g_syncDiagLastSyncFrame)
    {
        g_syncDiagLastCurrentFrame = currentFrame;
        g_syncDiagLastSyncFrame = syncFrame;
        LogRevivalSyncDiagnosticSnapshot(context, sessionPtr, "sync_frame");
        return;
    }

    g_syncDiagLastCurrentFrame = currentFrame;
}

static void ResetRevivalFpuSyncDiagnostics()
{
    g_revivalFpuBaseline = FpuControlSnapshot();
    g_revivalFpuBaselineCaptured = false;
    g_revivalFpuBaselineSession = 0;
    g_revivalFpuNormalizeCount = 0;
    g_syncDiagTrackerValid = false;
    g_syncDiagSession = 0;
    g_syncDiagLastMatchId = -1;
    g_syncDiagLastSyncFrame = -1;
    g_syncDiagLastCurrentFrame = -1;
}

void MarkRevivalSyncDiagnosticsSessionStart(const char* context)
{
    uintptr_t sessionPtr = ReadSessionPtrRaw();
    if (sessionPtr == 0)
    {
        sessionPtr = ReadSessionPointerFromRevivalLoose();
    }

    if (sessionPtr != 0)
    {
        CaptureRevivalFpuBaselineForSession(context, sessionPtr);
        if (g_localRoleFlag == kLocalRoleOnline)
        {
            int currentFrame = -1;
            int matchId = -1;
            int syncFrame = -1;
            ReadCoreSyncDiagFields(sessionPtr, &currentFrame, &matchId, &syncFrame);
            g_syncDiagTrackerValid = true;
            g_syncDiagSession = sessionPtr;
            g_syncDiagLastCurrentFrame = currentFrame;
            g_syncDiagLastMatchId = matchId;
            g_syncDiagLastSyncFrame = syncFrame;
        }
    }
    LogRevivalSyncDiagnosticSnapshot(context, sessionPtr, "session_enter");
}

// Screen-index change monitor - logs every time byte_790148 transitions.
static uint8_t g_lastMonitoredScreenIndex = 0xFF;

// ---------------------------------------------------------------------------
// Screen-transition–based win tracking.
//
// Revival's internal win increment (session+1224/1228) does not fire in our
// takeover context because the vtable[2] → RollbackLoopTick →
// BuildMatchInfoAndHUD change-detection wrapper never triggers.  Instead of
// relying on Revival's session wins, we replicate the logic directly:
//
//   When the screen index transitions from 3 (battle) to 5 (results), we
//   read byte 4940 from the EFZ global-state object to determine the match
//   winner (0 = P1, 1 = P2) and increment our own counter.
//
// The global-state object is the same one accessed via gameSys (screenObj+0x1C)
// and via the Revival DLL's globalStatePtrOffset.  Offset 4940 is the
// EFZ_GlobalStruct_GetByte4940() return value used in BuildMatchInfoAndHUD's
// mode-5 branch.
// ---------------------------------------------------------------------------
static volatile LONG g_trackedP1Wins = 0;
static volatile LONG g_trackedP2Wins = 0;

void ResetTrackedWins()
{
    InterlockedExchange(&g_trackedP1Wins, 0);
    InterlockedExchange(&g_trackedP2Wins, 0);
    mod::Log("WIN_TRACK: reset tracked wins to 0-0");
}

static void MonitorScreenIndexChange()
{
    constexpr uintptr_t kScreenIndexAddr = 0x00790148;
    constexpr uintptr_t kScreenTableAddr = 0x00790110;
    constexpr uint32_t kOffsetGameSystem = 0x1C;
    constexpr uint32_t kModeOffset = 4964;
    constexpr uint32_t kWinnerByteOffset = 4940;

    uint8_t currentIdx = 0xFF;
    uint8_t gameMode = 0xFF;
    uint8_t secondaryMode = 0xFF;
    uint32_t screenObj = 0;
    uint8_t csInit = 0xFF, csExit = 0xFF;
    uint32_t csObj = 0;

    __try
    {
        currentIdx = *reinterpret_cast<const uint8_t*>(kScreenIndexAddr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (currentIdx == g_lastMonitoredScreenIndex)
        return;

    __try
    {
        if (currentIdx < 16)
        {
            screenObj = reinterpret_cast<const uint32_t*>(kScreenTableAddr)[currentIdx];
            if (screenObj != 0)
            {
                const uint32_t gameSys =
                    *reinterpret_cast<const uint32_t*>(screenObj + kOffsetGameSystem);
                if (gameSys != 0)
                {
                    gameMode = *reinterpret_cast<const uint8_t*>(gameSys + kModeOffset);
                    secondaryMode = *reinterpret_cast<const uint8_t*>(gameSys + kModeOffset + 1);
                }
            }
        }

        csObj = reinterpret_cast<const uint32_t*>(kScreenTableAddr)[1];
        if (csObj != 0)
        {
            csInit = *reinterpret_cast<const uint8_t*>(csObj + 44);
            csExit = *reinterpret_cast<const uint8_t*>(csObj + 45);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    mod::Log(
        "SCREEN_MONITOR: index %u -> %u tick=%u mode=%u/%u "
        "screenObj=0x%08lX charselect(init=%u exit=%u obj=0x%08lX)",
        static_cast<unsigned>(g_lastMonitoredScreenIndex),
        static_cast<unsigned>(currentIdx),
        g_frameTick,
        static_cast<unsigned>(gameMode),
        static_cast<unsigned>(secondaryMode),
        static_cast<unsigned long>(screenObj),
        static_cast<unsigned>(csInit),
        static_cast<unsigned>(csExit),
        static_cast<unsigned long>(csObj));

    // Session counter snapshot at every screen transition: transitions are
    // where the Sync-feed counter (gmBase+20) permanently freezes when they
    // land mid-prediction, so one line here captures the freeze moment
    // without needing VerboseSyncDiagnostics.
    if (g_localRoleFlag == kLocalRoleOnline
        && g_localInitAppliedForSession
        && g_lastValidatedSessionPtr != 0
        && g_activeRevival != nullptr)
    {
        int monFrame = -1;
        int monCommit = -1;
        int monSyncFeed = -1;
        const uintptr_t monGmBase =
            g_lastValidatedSessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot;
        (void)SafeReadInt(
            reinterpret_cast<const void*>(
                g_lastValidatedSessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
            &monFrame);
        (void)SafeReadInt(reinterpret_cast<const void*>(monGmBase + 16), &monCommit);
        (void)SafeReadInt(reinterpret_cast<const void*>(monGmBase + 20), &monSyncFeed);
        mod::Log(
            "SCREEN_MONITOR: netplay counters frame=%d commit=%d syncFeed=%d",
            monFrame,
            monCommit,
            monSyncFeed);
    }

    g_lastMonitoredScreenIndex = currentIdx;
}

// RunPerFrameTickDispatch - setjmp guard for the per-frame tick hook.
// Same pattern as RunFrameDispatch: isolates setjmp into a POD-only
// function so C++ recovery can run safely in OurPerFrameTickHook.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
static int RunPerFrameTickDispatch(void* fixedThis)
{
    g_netplayFrameJmpActive = true;
    if (setjmp(g_netplayFrameJmpBuf) != 0)
    {
        // longjmp path: ExitProcess was intercepted during this frame tick.
        g_netplayFrameJmpActive = false;
        g_insideFrameTick = false;
        g_tickRecoveryPending = true;
        return 0;
    }
    g_insideFrameTick = true;
    const int result = g_origPerFrameTick(fixedThis);
    g_insideFrameTick = false;
    g_netplayFrameJmpActive = false;
    return result;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Our per-frame tick hook.  Uses __fastcall to capture ECX (first arg) and
// EDX (second, unused). It supplies the live session as a conservative ABI
// argument before calling the original sub_1006E570.
static int __fastcall OurPerFrameTickHook(void* exeThis, void* /*edx*/)
{
    // NOTE: A "double-tick skip" heuristic used to live here. It compared
    // gameSys+4968 across consecutive calls and, when it saw the same value
    // twice, skipped ALL mod-side per-frame processing for that call (still
    // calling the original tick). It misfired on Revival's legitimate rollback
    // re-simulation - which validly re-enters this tick with the EXE toggle
    // unchanged - so it dropped needed per-frame work and itself caused
    // desyncs/crashes. Removed: every invocation now takes the normal path
    // below and calls the original exactly once via RunPerFrameTickDispatch.

    // Track distinct return addresses (call sites) for diagnostics.
    {
        const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
        bool known = false;
        for (int ri = 0; ri < g_retAddrSlotCount; ++ri)
        {
            if (g_retAddrSlots[ri] == ra) { known = true; break; }
        }
        if (!known && g_retAddrSlotCount < kMaxRetAddrSlots)
        {
            g_retAddrSlots[g_retAddrSlotCount++] = ra;
            mod::Log(
                "TICK_HOOK: new caller detected retAddr=0x%08lX "
                "(slot %d/%d) frameTick=%u",
                static_cast<unsigned long>(ra),
                g_retAddrSlotCount, kMaxRetAddrSlots,
                g_frameTick);
        }
    }

    ++g_frameTick;

    // --- FPS drop detection (measured at the tick entry point) ---------------
    // Capture QPC at the very start of per-frame processing.  Compare against
    // the previous frame's timestamp to detect genuine FPS drops (>33.3ms
    // between frames = below 30fps).  This catches stalls from ANY source:
    // game logic, Revival DLL, our mod, OS scheduling, disk I/O, etc.
    LARGE_INTEGER tickEntryQpc = {};
    QueryPerformanceCounter(&tickEntryQpc);
    if (!g_qpcFreqValid)
    {
        QueryPerformanceFrequency(&g_qpcFreqCached);
        g_qpcFreqValid = true;
    }
    if (g_fpsDropPrevQpcValid)
    {
        const double frameDeltaMs =
            static_cast<double>(tickEntryQpc.QuadPart - g_fpsDropPrevQpc.QuadPart)
            * 1000.0 / static_cast<double>(g_qpcFreqCached.QuadPart);
        if (frameDeltaMs > kFpsDropThresholdMs)
        {
            ++g_fpsDropCount;
            // Throttle: log at most once per 60 frames and always on the first.
            if (g_fpsDropCount <= 3
                || (g_frameTick - g_fpsDropLastLogTick > 60))
            {
                const double measuredFps = (frameDeltaMs > 0.0)
                    ? 1000.0 / frameDeltaMs : 0.0;
                mod::Log(
                    "FPS_WARN: *** FRAME DROP *** tick=%u deltaMs=%.1f "
                    "fps=%.1f drops=%u - game running below 30fps",
                    g_frameTick, frameDeltaMs, measuredFps, g_fpsDropCount);
                g_fpsDropLastLogTick = g_frameTick;
            }
        }
    }
    g_fpsDropPrevQpc = tickEntryQpc;
    g_fpsDropPrevQpcValid = true;

    MonitorScreenIndexChange();

    const uintptr_t exeThisAddr = reinterpret_cast<uintptr_t>(exeThis);
    const uintptr_t currentSession = ReadSessionPtrRaw();

    // Use the current DLL session if available; fall back to EXE's value.
    void* fixedThis = (currentSession != 0)
        ? reinterpret_cast<void*>(currentSession)
        : exeThis;

    EnsureRevivalFpuBaselineForSession("tick_entry", currentSession);

    // Record the first ECX difference. In verified 1.02h this is diagnostic:
    // the native wrapper reloads the global session before virtual dispatch.
    if (exeThisAddr != currentSession && !g_perFrameMismatchLogged)
    {
        g_perFrameMismatchLogged = true;
        mod::Log(
            "TICK_HOOK: *** ECX MISMATCH *** frameTick=%u "
            "exeECX=0x%08lX dllSession=0x%08lX - "
            "using current-session compatibility argument",
            g_frameTick,
            static_cast<unsigned long>(exeThisAddr),
            static_cast<unsigned long>(currentSession));

        // Dump session vtable for context.
        if (currentSession != 0)
        {
            uintptr_t vtable = 0;
            (void)SafeReadPtr(
                reinterpret_cast<const void*>(currentSession), &vtable);
            HMODULE revival = GetModuleHandleA("EfzRevival.dll");
            const uintptr_t revBase = revival
                ? reinterpret_cast<uintptr_t>(revival) : 0;
            mod::Log(
                "TICK_HOOK: current session vtable=0x%08lX (RVA=0x%lX)",
                static_cast<unsigned long>(vtable),
                static_cast<unsigned long>(
                    revBase != 0 ? vtable - revBase : 0));
        }

        LogSessionDiagnosticState("tick_ecx_mismatch");
    }

    // Periodic heartbeat every 600 frames (~10s at 60fps).
    // Extended to log continuously (removed the g_frameTick <= 6000 cap)
    // so we can observe FPS and toggleSame throughout long sessions.
    if (g_frameTick == 1
        || (g_frameTick % 600 == 0))
    {
        const DWORD nowMs = GetTickCount();
        DWORD elapsedMs = 0;
        double measuredFps = 0.0;
        if (g_lastHeartbeatTimeMs != 0 && g_lastHeartbeatFrameTick != 0)
        {
            elapsedMs = nowMs - g_lastHeartbeatTimeMs;
            const uint32_t elapsedFrames = g_frameTick - g_lastHeartbeatFrameTick;
            if (elapsedMs > 0)
                measuredFps = static_cast<double>(elapsedFrames) * 1000.0
                              / static_cast<double>(elapsedMs);
        }
        g_lastHeartbeatTimeMs = nowMs;
        g_lastHeartbeatFrameTick = g_frameTick;

        mod::Log(
            "TICK_HOOK: heartbeat S#%u frameTick=%u session=0x%08lX exeECX=0x%08lX match=%d "
            "elapsed=%lums fps=%.1f toggleSame=%u",
            g_sessionNumber,
            g_frameTick,
            static_cast<unsigned long>(currentSession),
            static_cast<unsigned long>(exeThisAddr),
            (exeThisAddr == currentSession) ? 1 : 0,
            static_cast<unsigned long>(elapsedMs),
            measuredFps,
            g_toggleSameCount);

        // --- Resource leak tracking (every heartbeat = ~10s) ----------------
        // Log the process's handle count and memory usage so we can spot
        // leaks over time.  A steadily growing handle count or working set
        // indicates the mod (or Revival) is leaking resources.
        //
        // GetProcessHandleCount requires XP SP1+; GetProcessMemoryInfo lives
        // in psapi.dll which may not be loaded on minimal Wine prefixes.
        // Resolve both at runtime to keep the DLL loadable everywhere.
        {
            typedef BOOL (WINAPI *PFN_GetProcessHandleCount)(HANDLE, PDWORD);
            typedef BOOL (WINAPI *PFN_GetProcessMemoryInfo)(
                HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);

            static PFN_GetProcessHandleCount s_pfnHandleCount = nullptr;
            static PFN_GetProcessMemoryInfo  s_pfnMemInfo = nullptr;
            static bool s_resolved = false;
            if (!s_resolved)
            {
                s_resolved = true;
                HMODULE k32 = GetModuleHandleA("kernel32.dll");
                if (k32 != nullptr)
                    s_pfnHandleCount = reinterpret_cast<PFN_GetProcessHandleCount>(
                        GetProcAddress(k32, "GetProcessHandleCount"));
                HMODULE psapi = LoadLibraryA("psapi.dll");
                if (psapi != nullptr)
                    s_pfnMemInfo = reinterpret_cast<PFN_GetProcessMemoryInfo>(
                        GetProcAddress(psapi, "GetProcessMemoryInfo"));
                // Intentionally leak the psapi HMODULE - we need it for the
                // lifetime of the process and it's tiny.
            }

            DWORD handleCount = 0;
            if (s_pfnHandleCount != nullptr)
                s_pfnHandleCount(GetCurrentProcess(), &handleCount);

            PROCESS_MEMORY_COUNTERS pmc = {};
            pmc.cb = sizeof(pmc);
            if (s_pfnMemInfo != nullptr)
                s_pfnMemInfo(GetCurrentProcess(), &pmc, sizeof(pmc));

            mod::Log(
                "RESOURCE_TRACK: handles=%lu workingSetMB=%.1f "
                "commitMB=%.1f peakWorkingSetMB=%.1f "
                "fpsDrops=%u tickOverBudget=%u",
                static_cast<unsigned long>(handleCount),
                static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0),
                static_cast<double>(pmc.PagefileUsage) / (1024.0 * 1024.0),
                static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0),
                g_fpsDropCount,
                g_tickBudgetExceededCount);
        }
    }

    // ---- Provisional desync warning watcher ------------------------------
    // Revival's "Desync detected" console line arrives on its own IPC
    // channel and is deliberately NON-fatal: stock Revival keeps the match
    // running after the one-shot warning.  An RNG-only split can persist
    // invisibly and become a gameplay desync when a later mechanic consumes
    // randomness (docs/NAYUKI_AWAKE_AIR_THROW_RNG_DESYNC.md).  Log a rich snapshot when
    // the warning first appears, then a once-per-second trail of the Revival
    // RNG engine state and EFZ effect-ring cursors so host/client logs can
    // be diffed for RNG phase and effect-cursor evolution.  Persistent
    // selected-field checksum windows are captured by the negotiated
    // desync_monitor stream; transport/protocol failures still use the
    // fatal consoleErrorSerial path below.
    {
        static uint32_t s_desyncWarnSeenSession = 0;
        static LONG s_desyncWarnSeenSerial = 0;
        static int s_desyncWarnTrailSamples = 0;
        static DWORD s_desyncWarnNextSampleTick = 0;

        // The shared warning serial is cleared when a session is torn down.
        // Reset the consumer on the explicit session generation as well: if
        // session N ended with serial 1, session N+1's first warning is also
        // serial 1 and must not be mistaken for an already-seen event.
        if (s_desyncWarnSeenSession != g_sessionNumber)
        {
            s_desyncWarnSeenSession = g_sessionNumber;
            s_desyncWarnSeenSerial = 0;
            s_desyncWarnTrailSamples = 0;
            s_desyncWarnNextSampleTick = 0;
        }

        const LONG desyncWarnSerial =
            g_hostBlock != nullptr
                ? InterlockedCompareExchange(&g_hostBlock->consoleDesyncWarnSerial, 0, 0)
                : 0;
        if (desyncWarnSerial < s_desyncWarnSeenSerial)
        {
            // Serial was externally cleared (session teardown/restart).
            s_desyncWarnSeenSerial = desyncWarnSerial;
            s_desyncWarnTrailSamples = 0;
        }

        const DWORD desyncWarnNowTick = GetTickCount();
        const bool newDesyncWarning = desyncWarnSerial > s_desyncWarnSeenSerial;
        const bool trailSampleDue =
            s_desyncWarnTrailSamples > 0
            && static_cast<int>(desyncWarnNowTick - s_desyncWarnNextSampleTick) >= 0;

        if (newDesyncWarning || trailSampleDue)
        {
            // EFZ gameSystem offsets (fixed across supported EFZ builds; the
            // same struct Revival's global-state pointer targets - offsets
            // 4964/4965/82563 in the profile address the same block).
            constexpr uintptr_t kEfzEffectAllocCursorOffset = 4992;
            constexpr uintptr_t kEfzEffectProcCursorOffset = 4994;
            constexpr uintptr_t kEfzEffectsSettingOffset = 4966;

            int rngEngineState = -1;
            int sessionFrame = -1;
            uint16_t effectAllocCursor = 0xFFFF;
            uint16_t effectProcCursor = 0xFFFF;
            uint8_t effectsSetting = 0xFF;
            uint8_t replayModeByte = 0xFF;
            uint8_t gameModeIndex = 0xFF;

            HMODULE revival = GetModuleHandleA("EfzRevival.dll");
            if (revival != nullptr && g_activeRevival != nullptr)
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
                if (g_activeRevival->rngEngineStateOffset != 0)
                {
                    (void)SafeReadInt(
                        reinterpret_cast<const void*>(
                            base + g_activeRevival->rngEngineStateOffset),
                        &rngEngineState);
                }
                if (g_activeRevival->sessionPtrOffsetCount > 0)
                {
                    uintptr_t sessionPtr = 0;
                    if (SafeReadPtr(
                            reinterpret_cast<const void*>(
                                base + g_activeRevival->sessionPtrOffsets[0]),
                            &sessionPtr)
                        && sessionPtr != 0
                        && g_activeRevival->sessionOffsetCurrentFrame != 0)
                    {
                        (void)SafeReadInt(
                            reinterpret_cast<const void*>(
                                sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
                            &sessionFrame);
                    }
                }
                uintptr_t globalStatePtr = 0;
                if (g_activeRevival->globalStatePtrOffset != 0
                    && SafeReadPtr(
                        reinterpret_cast<const void*>(
                            base + g_activeRevival->globalStatePtrOffset),
                        &globalStatePtr)
                    && globalStatePtr != 0)
                {
                    (void)SafeReadWord(
                        reinterpret_cast<const void*>(
                            globalStatePtr + kEfzEffectAllocCursorOffset),
                        &effectAllocCursor);
                    (void)SafeReadWord(
                        reinterpret_cast<const void*>(
                            globalStatePtr + kEfzEffectProcCursorOffset),
                        &effectProcCursor);
                    (void)SafeReadByte(
                        reinterpret_cast<const void*>(
                            globalStatePtr + kEfzEffectsSettingOffset),
                        &effectsSetting);
                    (void)SafeReadByte(
                        reinterpret_cast<const void*>(
                            globalStatePtr + g_activeRevival->globalStateOffsetSessionByte),
                        &replayModeByte);
                }
                if (g_activeRevival->addrGameModeCurrentIndex != 0)
                {
                    (void)SafeReadByte(
                        reinterpret_cast<const void*>(
                            g_activeRevival->addrGameModeCurrentIndex),
                        &gameModeIndex);
                }
            }

            if (newDesyncWarning)
            {
                char warnText[128] = {};
                ReadConsoleDesyncWarning(nullptr, warnText, sizeof(warnText));
                s_desyncWarnSeenSerial = desyncWarnSerial;
                s_desyncWarnTrailSamples = 10;
                s_desyncWarnNextSampleTick = desyncWarnNowTick + 1000;
                mod::Log(
                    "DESYNC_WARN_PROVISIONAL serial=%ld frameTick=%u sessionFrame=%d "
                    "gameMode=%u rngState=%d effectAlloc=%u effectProc=%u "
                    "effectsSetting=%u replayModeByte=%u text='%s' - session kept "
                    "alive (stock-parity); warning recorded for attribution",
                    static_cast<long>(desyncWarnSerial),
                    g_frameTick,
                    sessionFrame,
                    gameModeIndex,
                    rngEngineState,
                    effectAllocCursor,
                    effectProcCursor,
                    effectsSetting,
                    replayModeByte,
                    warnText);
            }
            else
            {
                --s_desyncWarnTrailSamples;
                // Schedule from the observation time.  Advancing an old
                // deadline after a stall creates a burst of catch-up samples
                // on consecutive game ticks and can itself perturb timing.
                s_desyncWarnNextSampleTick = desyncWarnNowTick + 1000;
                mod::Log(
                    "DESYNC_WARN_TRAIL serial=%ld remaining=%d frameTick=%u "
                    "sessionFrame=%d gameMode=%u rngState=%d effectAlloc=%u "
                    "effectProc=%u",
                    static_cast<long>(s_desyncWarnSeenSerial),
                    s_desyncWarnTrailSamples,
                    g_frameTick,
                    sessionFrame,
                    gameModeIndex,
                    rngEngineState,
                    effectAllocCursor,
                    effectProcCursor);
            }
        }
    }

    // ---- Pre-tick graceful-end / disconnect detection --------------------
    // Replace Revival's patched-out quitMem -> ExitProcess path on the host
    // side, and keep the existing console-error short-circuit as well.
    bool preTickDisconnect = false;
    bool preTickGracefulQuit = false;
    LONG preTickQuitHead = 0;
    LONG preTickQuitTail = 0;
    if (g_dllExitProcessPatchesSaved
        && InterlockedCompareExchange(&g_scheduledGracefulQuitTeardownActive, 0, 0) == 0
        && InterlockedCompareExchange(&g_onlineMatchEscGracefulQuitArmed, 0, 0) == 0)
    {
        if (ConsumeGracefulQuitRingSignal(&preTickQuitHead, &preTickQuitTail))
        {
            if (!ConsumeLocalBattleEscQuitRingIgnore(
                    "PRE-TICK",
                    preTickQuitHead,
                    preTickQuitTail))
            {
                mod::Log(
                    "TICK_HOOK: *** PRE-TICK GRACEFUL SESSION END *** frameTick=%u "
                    "quitHead=%ld quitTail=%ld - skipping DLL tick",
                    g_frameTick,
                    static_cast<long>(preTickQuitHead),
                    static_cast<long>(preTickQuitTail));
                preTickDisconnect = true;
                preTickGracefulQuit = true;
            }
        }

        if (!preTickDisconnect && g_hostBlock != nullptr)
        {
            const LONG preTickErrSerial =
                InterlockedCompareExchange(&g_hostBlock->consoleErrorSerial, 0, 0);
            if (preTickErrSerial > 0)
            {
                mod::Log(
                    "TICK_HOOK: *** PRE-TICK DISCONNECT *** frameTick=%u "
                    "consoleErrorSerial=%ld - skipping DLL tick to prevent "
                    "corrupted render",
                    g_frameTick,
                    static_cast<long>(preTickErrSerial));
                preTickDisconnect = true;
            }
        }
    }

    // ---- Online match ESC battle-return tracking -------------------------
    // Native EFZ/Revival battle ESC returns to charselect rather than leaving
    // netplay entirely. Keep a short marker so if the helper publishes a Quit
    // ring entry for that local ESC, we consume it without promoting the
    // session into the title/netplay-menu teardown path.
    {
        static bool s_onlineMatchEscWasDown = false;
        const bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        uint8_t escScreen = 0xFF;
        __try {
            escScreen = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        const bool onlineBattleActive =
            g_localRoleFlag == kLocalRoleOnline
            && g_localInitAppliedForSession
            && g_dllExitProcessPatchesSaved
            && currentSession != 0
            && escScreen == 3;
        const bool escRisingEdge =
            onlineBattleActive && escDown && !s_onlineMatchEscWasDown;
        s_onlineMatchEscWasDown = escDown;

        if (escRisingEdge)
        {
            ArmLocalBattleEscQuitRingIgnore();
            mod::Log(
                "TICK_HOOK: online match ESC detected frameTick=%u screen=%u "
                "session=0x%08lX role=%d netRole=%d helperPid=%lu "
                "dllExitPatched=%d battleReturnOnly=1 ignoreQuitRingWindowMs=%lu",
                g_frameTick,
                static_cast<unsigned>(escScreen),
                static_cast<unsigned long>(currentSession),
                g_localRoleFlag,
                g_netplayRole,
                static_cast<unsigned long>(g_revivalProcessId),
                g_dllExitProcessPatchesSaved ? 1 : 0,
                static_cast<unsigned long>(kLocalBattleEscQuitRingIgnoreWindowMs));
        }
    }

    // Call the original sub_1006E570 with the compatibility ECX unless a
    // pre-tick disconnect was detected.
    int result = 0;
    LARGE_INTEGER tickQpcBefore = {}, tickQpcAfter = {};
    RevivalRemoteInputDiagSnapshot revivalTickBefore = {};
    RevivalRemoteInputDiagSnapshot revivalTickAfter = {};
    bool revivalRemoteDiagActive = false;
    bool revivalBatchZeroFrameDiag = false;
    const bool eagerZeroFrameExperiment =
        netplay::mod_settings::IsEagerZeroFrameGraphicsRestoreEnabled();
    static int s_lastZeroFrameLeftAloneFrame = -1;
    static uint32_t s_lastZeroFrameLeftAloneTick = 0;

    if (!preTickDisconnect)
    {
        // The remote-input snapshot describes RollbackSession.  In
        // SpectatorSession the same offsets are different MinGW objects and
        // wire queues, so reading them only creates misleading diagnostics.
        if (g_localRoleFlag == kLocalRoleOnline
            && (kLogRevivalTickDiag || eagerZeroFrameExperiment))
        {
            CaptureRevivalRemoteInputDiag(currentSession, &revivalTickBefore);
            revivalRemoteDiagActive =
                ShouldLogRevivalRemoteInputDiag(revivalTickBefore);
        }
        if (revivalRemoteDiagActive)
        {
            mod::Log(
                "REVIVAL_TICK_ENTER state=%d screen=%u currentFrame=%d "
                "inputSizes=local:%d remote:%d delay=%d activePlayer=%d "
                "patchState=%d",
                revivalTickBefore.state,
                static_cast<unsigned>(revivalTickBefore.screen),
                revivalTickBefore.currentFrame,
                revivalTickBefore.localLen,
                revivalTickBefore.remoteLen,
                revivalTickBefore.inputDelay,
                revivalTickBefore.activePlayer,
                revivalTickBefore.patchState);
        }

        if (revivalTickBefore.pauseRemoteInput)
        {
            revivalBatchZeroFrameDiag = true;
            if (kLogRevivalTickDiag || eagerZeroFrameExperiment)
            {
                mod::Log(
                    "REVIVAL_PAUSE_REMOTE_INPUT ping=%u delay=%d localLen=%d "
                    "remoteLen=%d returnIterations=0",
                    static_cast<unsigned>(revivalTickBefore.pingStruct[3]),
                    revivalTickBefore.waitDelay,
                    revivalTickBefore.localLen,
                    revivalTickBefore.remoteLen);
                mod::Log(
                    "REVIVAL_BATCH_RENDER_ENTER frameCount=0 patchStateBefore=%d",
                    revivalTickBefore.patchState);
            }
        }

        // Wrapped in RunPerFrameTickDispatch which sets up a setjmp recovery
        // point so NeutralizeExitProcess can longjmp back if ExitProcess fires
        // during the DLL's session tick (vtable[2] → RollbackLoopTick).
        NormalizeRevivalFpuState("pre_orig_tick", currentSession);
        QueryPerformanceCounter(&tickQpcBefore);
        result = RunPerFrameTickDispatch(fixedThis);
        QueryPerformanceCounter(&tickQpcAfter);
        NormalizeRevivalFpuState("post_orig_tick", currentSession);
        TrackRevivalSyncDiagnosticsAfterTick("post_orig_tick", currentSession);

        if (revivalRemoteDiagActive || revivalBatchZeroFrameDiag)
        {
            CaptureRevivalRemoteInputDiag(currentSession, &revivalTickAfter);
            int iterations = result;
            if (revivalTickBefore.currentFrame >= 0
                && revivalTickAfter.currentFrame >= 0)
            {
                iterations =
                    revivalTickAfter.currentFrame - revivalTickBefore.currentFrame;
            }
            mod::Log(
                "REVIVAL_TICK_EXIT iterations=%d state=%d "
                "currentFrameBefore=%d currentFrameAfter=%d rawResult=%d "
                "patchState=%d",
                iterations,
                revivalTickAfter.state,
                revivalTickBefore.currentFrame,
                revivalTickAfter.currentFrame,
                result,
                revivalTickAfter.patchState);
        }

        if (revivalBatchZeroFrameDiag)
        {
            mod::Log(
                "REVIVAL_BATCH_RENDER_EXIT frameCount=0 patchStateAfter=%d",
                revivalTickAfter.patchState);
            if (revivalTickBefore.state == 3
                && revivalTickBefore.patchState == 1
                && revivalTickAfter.patchState == 0)
            {
                mod::Log(
                    "REVIVAL_ZERO_FRAME_RENDER_SUPPRESSION_OBSERVED state=3 frameCount=0 before=1 after=0 currentFrame=%d",
                    revivalTickBefore.currentFrame);
                // Stock Revival leaves the patch set disabled across
                // zero-iteration batches (final-frame-only rendering); the
                // eager re-enable is a mod-specific difference and a desync
                // A/B axis. [Others]
                // ExperimentalEagerZeroFrameGraphicsRestore=0 gives
                // stock render-patch policy; terminal-recovery restores are separate and
                // unaffected.
                if (eagerZeroFrameExperiment)
                {
                    const bool restoreOk =
                        EnsureRevivalGraphicsPatchSetEnabled(
                            "zero_frame_render_override_experiment");
                    mod::Log(
                        "REVIVAL_ZERO_FRAME_RENDER_OVERRIDE_APPLIED result=%d",
                        restoreOk ? 1 : 0);
                }
                else
                {
                    mod::Log(
                        "REVIVAL_ZERO_FRAME_RENDER_SUPPRESSION_PRESERVED reason=stock_parity_default "
                        "currentFrame=%d",
                        revivalTickBefore.currentFrame);
                }
            }
            else if (revivalTickAfter.patchState == 0)
            {
                const bool shouldLogLeftAlone =
                    revivalTickBefore.currentFrame != s_lastZeroFrameLeftAloneFrame
                    || (g_frameTick - s_lastZeroFrameLeftAloneTick) >= 60u;
                if (shouldLogLeftAlone)
                {
                    s_lastZeroFrameLeftAloneFrame = revivalTickBefore.currentFrame;
                    s_lastZeroFrameLeftAloneTick = g_frameTick;
                    mod::Log(
                        "REVIVAL_ZERO_FRAME_RENDER_REMAINS_SUPPRESSED reason=native_zero_frame_batch state=%d frameCount=0 before=%d after=%d currentFrame=%d",
                        revivalTickBefore.state,
                        revivalTickBefore.patchState,
                        revivalTickAfter.patchState,
                        revivalTickBefore.currentFrame);
                }
            }
        }
    }

    if (!preTickGracefulQuit)
    {
        const bool originalTickSkipped = preTickDisconnect;
        const bool originalTickRan = !originalTickSkipped;
        if (UpdateGameplayStallTracker(
                preTickDisconnect
                    ? "pre_tick_disconnect_skip"
                    : "normal_tick",
                currentSession,
                false,
                originalTickSkipped,
                originalTickRan))
        {
            return 0;
        }
    }

    // ====================================================================
    // Comprehensive per-frame diagnostics (double-speed bug)
    // ====================================================================
    // Log EVERYTHING that could explain why the game runs at double FPS
    // on the second (and subsequent) netplay sessions.
    //
    // Fires: every frame for first 30 ticks (burst), every 30 frames after
    // (2x/sec at 60fps), and ALWAYS when result > 1 (multi-iteration).
    // ====================================================================
    if (g_speedDiagEnabled
        && g_activeRevival != nullptr
        && currentSession != 0
        && g_dllExitProcessPatchesSaved
        && g_localRoleFlag != kLocalRoleLocalPlay)
    {
        const bool isBurst = (g_frameTick <= 30);
        const bool isPeriodic = (g_frameTick % 30 == 0);
        const bool isMultiIter = (result > 1);
        if (isBurst || isPeriodic || isMultiIter)
        {
            HMODULE revival = GetModuleHandleA("EfzRevival.dll");
            const uintptr_t dllBase = revival
                ? reinterpret_cast<uintptr_t>(revival) : 0;

            // ---- 1. Session object fields ----
            int inputDelay = -1, activePlayer = -1, queuePlayer = -1;
            int currentFrame = -1, matchId = -1, initComplete = -1;
            uint32_t pingStruct[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
            uint8_t screenIdx = 0xFF;
            // Fields at known fixed offsets relative to sessionOffsetGameModeSnapshot:
            //   +716 = previousGameMode,  +720 = currentGameMode
            //   +724 = matchStartFrame,   +728 = advanceCounter
            //   +736 = syncFrameCounter
            int prevGameMode = -1, curGameMode = -1, matchStartFrame = -1;
            int advanceCounter = -1, syncFrameCounter = -1;
            int windowBaseDelay = -1;  // +692 = prediction enabled / windowBaseDelayOffset
            uint32_t sentinelVal = 0;
            uintptr_t sessionVtable = 0;
            // +1236 = highestFrameReached (DWORD[309])
            int highestFrame = -1;

            (void)SafeReadPtr(reinterpret_cast<const void*>(currentSession), &sessionVtable);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetInputDelay), &inputDelay);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetActivePlayer), &activePlayer);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetQueuePlayer), &queuePlayer);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetCurrentFrame), &currentFrame);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetMatchId), &matchId);
            if (g_activeRevival->sessionOffsetInitComplete != 0)
            {
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    currentSession + g_activeRevival->sessionOffsetInitComplete), &initComplete);
            }
            (void)SafeReadDword(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetSentinel), &sentinelVal);

            // Game mode fields - always 4 bytes after sessionOffsetGameModeSnapshot.
            const uintptr_t gmBase = currentSession + g_activeRevival->sessionOffsetGameModeSnapshot;
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase), &prevGameMode);       // +716
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 4), &curGameMode);    // +720
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 8), &matchStartFrame);// +724
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 12), &advanceCounter);// +728
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), &syncFrameCounter);// +736

            // windowBaseDelayOffset = sessionOffsetInputDelay + 4 (byte offset +692)
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetInputDelay + 4), &windowBaseDelay);

            // highestFrameReached = sentinel offset + 4 (byte offset +1236)
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetSentinel + 4), &highestFrame);

            // Read the 4-DWORD ping struct (AdjustPrediction / WaitLoop fields).
            const uintptr_t pingBase = currentSession + g_activeRevival->sessionOffsetPingStructBase;
            for (int pi = 0; pi < 4; ++pi)
            {
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(pingBase + pi * 4),
                    reinterpret_cast<int*>(&pingStruct[pi]));
            }

            __try {
                screenIdx = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            // ---- 2. DLL global variables ----
            uint16_t initOnceGuard = 0;
            uintptr_t timerPtr = 0, renderCtxPtr = 0, globalStatePtr = 0;
            uintptr_t subStructBase = 0;
            int initFlag = -1;
            double timerScalar = 0.0;   // timerPtr+32: 1000.0/interval
            double timerInterval = 0.0; // timerPtr+16: frame interval in ms

            if (dllBase != 0)
            {
                (void)SafeReadWord(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->initOnceGuardOffset),
                    &initOnceGuard);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->timerPtrOffset),
                    &timerPtr);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->renderContextGlobalOffset),
                    &renderCtxPtr);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->globalStatePtrOffset),
                    &globalStatePtr);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->renderContextBaseOffset),
                    &subStructBase);
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->initFlagOffset),
                    &initFlag);

                // Read timer context: [+16]=interval(double), [+32]=scalar(double)
                if (timerPtr != 0)
                {
                    (void)SafeReadDouble(
                        reinterpret_cast<const void*>(timerPtr + 16), &timerInterval);
                    (void)SafeReadDouble(
                        reinterpret_cast<const void*>(timerPtr + 32), &timerScalar);
                }
            }

            // ---- 3. Compute what AdjustPrediction would compute ----
            // Replicate the math so we can see the intermediate values:
            //   halfPeriod = pingTicks / (2 * timerScalar)
            //   threshold = floor(halfPeriod) - 1
            // If (localFrames - remoteFrames) < threshold → "Add frame"
            double halfPeriodFloat = 0.0;
            int predThreshold = -9999;
            if (timerScalar > 0.0)
            {
                halfPeriodFloat = static_cast<double>(pingStruct[0])
                                  / (2.0 * timerScalar);
                predThreshold = static_cast<int>(floor(halfPeriodFloat)) - 1;
            }

            // ---- 4. DLL tick timing ----
            LARGE_INTEGER qpcFreq;
            QueryPerformanceFrequency(&qpcFreq);
            double tickDurationUs = 0.0;
            if (tickQpcAfter.QuadPart > tickQpcBefore.QuadPart)
            {
                tickDurationUs = static_cast<double>(
                    tickQpcAfter.QuadPart - tickQpcBefore.QuadPart)
                    * 1000000.0 / static_cast<double>(qpcFreq.QuadPart);
            }

            // ---- 5. Vtable RVA for session type identification ----
            uintptr_t vtableRva = (dllBase != 0 && sessionVtable >= dllBase)
                ? (sessionVtable - dllBase) : 0;

            // ---- LOG LINE 1: core session state ----
            mod::Log(
                "SPEED_DIAG[1]: S#%u tick=%u result=%d screen=%u role=%d "
                "session=0x%08lX vtableRVA=0x%lX "
                "frame=%d matchId=%d initComp=%d sentinel=0x%08lX "
                "tickUs=%.0f%s",
                g_sessionNumber, g_frameTick, result,
                static_cast<unsigned>(screenIdx), g_localRoleFlag,
                static_cast<unsigned long>(currentSession),
                static_cast<unsigned long>(vtableRva),
                currentFrame, matchId, initComplete,
                static_cast<unsigned long>(sentinelVal),
                tickDurationUs,
                isMultiIter ? " *** MULTI-ITER ***" : "");

            // ---- LOG LINE 2: game mode / timing fields ----
            mod::Log(
                "SPEED_DIAG[2]: prevMode=%d curMode=%d matchStart=%d "
                "advCtr=%d syncFrame=%d highFrame=%d "
                "inputDelay=%d wndBaseDelay=%d "
                "active=%d queue=%d",
                prevGameMode, curGameMode, matchStartFrame,
                advanceCounter, syncFrameCounter, highestFrame,
                inputDelay, windowBaseDelay,
                activePlayer, queuePlayer);

            // ---- LOG LINE 3: ping struct + prediction math ----
            mod::Log(
                "SPEED_DIAG[3]: ping[0]=%u [1]=%u [2]=%u [3]=%u "
                "timerScalar=%.4f timerInterval=%.4f "
                "halfPeriod=%.6f predThreshold=%d",
                pingStruct[0], pingStruct[1], pingStruct[2], pingStruct[3],
                timerScalar, timerInterval,
                halfPeriodFloat, predThreshold);

            // ---- LOG LINE 4: DLL globals ----
            mod::Log(
                "SPEED_DIAG[4]: initOnceGuard=0x%04X initFlag=%d "
                "timerPtr=0x%08lX renderCtx=0x%08lX "
                "globalState=0x%08lX subStructBase=0x%08lX",
                static_cast<unsigned>(initOnceGuard), initFlag,
                static_cast<unsigned long>(timerPtr),
                static_cast<unsigned long>(renderCtxPtr),
                static_cast<unsigned long>(globalStatePtr),
                static_cast<unsigned long>(subStructBase));

            // ---- LOG LINE 5: EXE hook bytes at 0x401582 and 0x401642 ----
            // 0x401582 = frame-hook dispatcher (sub_1006E590 entry point)
            // 0x401642 = per-frame tick (sub_1006E570 dispatch site)
            if (isBurst || isMultiIter)
            {
                uint8_t exeHookA[12] = {}, exeHookB[12] = {};
                __try {
                    for (int bi = 0; bi < 12; ++bi)
                    {
                        exeHookA[bi] = reinterpret_cast<const volatile uint8_t*>(0x401582u)[bi];
                        exeHookB[bi] = reinterpret_cast<const volatile uint8_t*>(0x401642u)[bi];
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}

                mod::Log(
                    "SPEED_DIAG[5a]: EXE@0x401582: "
                    "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    exeHookA[0], exeHookA[1], exeHookA[2], exeHookA[3],
                    exeHookA[4], exeHookA[5], exeHookA[6], exeHookA[7],
                    exeHookA[8], exeHookA[9], exeHookA[10], exeHookA[11]);
                mod::Log(
                    "SPEED_DIAG[5b]: EXE@0x401642: "
                    "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    exeHookB[0], exeHookB[1], exeHookB[2], exeHookB[3],
                    exeHookB[4], exeHookB[5], exeHookB[6], exeHookB[7],
                    exeHookB[8], exeHookB[9], exeHookB[10], exeHookB[11]);

                // Also dump DLL-side trampoline bytes (perFrameTickRva)
                if (dllBase != 0)
                {
                    const uintptr_t hookAddr = dllBase + g_activeRevival->perFrameTickRva;
                    uint8_t dllHook[12] = {};
                    __try {
                        for (int bi = 0; bi < 12; ++bi)
                            dllHook[bi] = reinterpret_cast<const volatile uint8_t*>(hookAddr)[bi];
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}

                    mod::Log(
                        "SPEED_DIAG[5c]: DLL perFrameTick @0x%08lX: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                        static_cast<unsigned long>(hookAddr),
                        dllHook[0], dllHook[1], dllHook[2], dllHook[3],
                        dllHook[4], dllHook[5], dllHook[6], dllHook[7],
                        dllHook[8], dllHook[9], dllHook[10], dllHook[11]);

                    // Also dump frameHookRva (sub_1006E590) to see if it's been
                    // re-hooked or clobbered.
                    const uintptr_t fhAddr = dllBase + g_activeRevival->frameHookRva;
                    uint8_t fhHook[12] = {};
                    __try {
                        for (int bi = 0; bi < 12; ++bi)
                            fhHook[bi] = reinterpret_cast<const volatile uint8_t*>(fhAddr)[bi];
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}

                    mod::Log(
                        "SPEED_DIAG[5d]: DLL frameHook @0x%08lX: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                        static_cast<unsigned long>(fhAddr),
                        fhHook[0], fhHook[1], fhHook[2], fhHook[3],
                        fhHook[4], fhHook[5], fhHook[6], fhHook[7],
                        fhHook[8], fhHook[9], fhHook[10], fhHook[11]);
                }
            }

            // ---- LOG LINE 6: Ring buffer head/tail for InputP1/InputP2 ----
            // These are shared-memory regions read by the DLL session.
            // Head/tail at DWORD[0] and DWORD[1] of each mapping.
            if (isBurst || isPeriodic || isMultiIter)
            {
                struct RingProbe {
                    const char* name;
                    DWORD head, tail;
                    bool ok;
                };
                RingProbe probes[] = {
                    {RevivalWireName("InputP1"), 0, 0, false},
                    {RevivalWireName("InputP2"), 0, 0, false},
                    {RevivalWireName("Sync"),    0, 0, false},
                    {RevivalWireName("Net"),     0, 0, false},
                };
                constexpr int kProbeCount = 4;

                for (int pi = 0; pi < kProbeCount; ++pi)
                {
                    HANDLE hMap = OpenFileMappingA(
                        FILE_MAP_READ, FALSE, probes[pi].name);
                    if (hMap != nullptr)
                    {
                        const volatile DWORD* view = static_cast<const volatile DWORD*>(
                            MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 8));
                        if (view != nullptr)
                        {
                            probes[pi].head = view[0];
                            probes[pi].tail = view[1];
                            probes[pi].ok = true;
                            UnmapViewOfFile(const_cast<DWORD*>(view));
                        }
                        CloseHandle(hMap);
                    }
                }

                mod::Log(
                    "SPEED_DIAG[6]: ring InputP1(%s h=%lu t=%lu d=%ld) "
                    "InputP2(%s h=%lu t=%lu d=%ld) "
                    "Sync(%s h=%lu t=%lu) Net(%s h=%lu t=%lu)",
                    probes[0].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[0].head),
                    static_cast<unsigned long>(probes[0].tail),
                    static_cast<long>(probes[0].tail - probes[0].head),
                    probes[1].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[1].head),
                    static_cast<unsigned long>(probes[1].tail),
                    static_cast<long>(probes[1].tail - probes[1].head),
                    probes[2].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[2].head),
                    static_cast<unsigned long>(probes[2].tail),
                    probes[3].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[3].head),
                    static_cast<unsigned long>(probes[3].tail));
            }

            // ---- LOG LINE 7: gameSys state (mode bytes, toggle) ----
            if (isBurst || isPeriodic)
            {
                uint32_t gameSys = 0;
                uint8_t gameSysMode = 0xFF, gameSysMode2 = 0xFF;
                uint32_t gameSysToggle = 0xFFFFFFFF;
                __try {
                    gameSys = *reinterpret_cast<const volatile uint32_t*>(0x0079010Cu);
                    if (gameSys != 0)
                    {
                        gameSysMode = *reinterpret_cast<const volatile uint8_t*>(gameSys + 4964);
                        gameSysMode2 = *reinterpret_cast<const volatile uint8_t*>(gameSys + 4965);
                        gameSysToggle = *reinterpret_cast<const volatile uint32_t*>(gameSys + 4968);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}

                mod::Log(
                    "SPEED_DIAG[7]: gameSys=0x%08lX mode=%u/%u toggle=%u "
                    "toggleSameCount=%u exeECX=0x%08lX dllSession=0x%08lX "
                    "ecxMatch=%d",
                    static_cast<unsigned long>(gameSys),
                    static_cast<unsigned>(gameSysMode),
                    static_cast<unsigned>(gameSysMode2),
                    gameSysToggle,
                    g_toggleSameCount,
                    static_cast<unsigned long>(exeThisAddr),
                    static_cast<unsigned long>(currentSession),
                    (exeThisAddr == currentSession) ? 1 : 0);
            }

            // ---- QPC-based precise FPS measurement (every 30 frames) ----
            if (isPeriodic)
            {
                static LARGE_INTEGER s_lastQpc = {};
                static uint32_t s_lastQpcTick = 0;
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                if (s_lastQpcTick != 0 && s_lastQpcTick < g_frameTick)
                {
                    const double elapsed =
                        static_cast<double>(qpcNow.QuadPart - s_lastQpc.QuadPart)
                        / static_cast<double>(qpcFreq.QuadPart);
                    const uint32_t dFrames = g_frameTick - s_lastQpcTick;
                    const double qpcFps = (elapsed > 0.0)
                        ? static_cast<double>(dFrames) / elapsed : 0.0;
                    mod::Log(
                        "SPEED_DIAG[8]: QPC fps=%.2f elapsed=%.4fs frames=%u "
                        "qpcFreq=%lld",
                        qpcFps, elapsed, dFrames,
                        static_cast<long long>(qpcFreq.QuadPart));
                }
                s_lastQpc = qpcNow;
                s_lastQpcTick = g_frameTick;
            }

            // ---- LOG LINE 9: cross-frame change detection ----
            // Detects silent changes to timer, init-once guard, render
            // context, and session pointer between frames.  Fires every
            // time the SPEED_DIAG block fires (burst + periodic + multi).
            {
                // 9a: Timer baseline capture and drift detection.
                if (!g_timerBaselineCaptured && timerPtr != 0
                    && timerScalar != 0.0)
                {
                    g_baselineTimerScalar   = timerScalar;
                    g_baselineTimerInterval = timerInterval;
                    g_baselineTimerPtr      = timerPtr;
                    g_timerBaselineCaptured = true;
                    mod::Log(
                        "SPEED_DIAG[9a]: S#%u timer baseline captured "
                        "ptr=0x%08lX scalar=%.4f interval=%.4f",
                        g_sessionNumber,
                        static_cast<unsigned long>(timerPtr),
                        timerScalar, timerInterval);
                }
                else if (g_timerBaselineCaptured)
                {
                    const bool ptrChanged = (timerPtr != g_baselineTimerPtr);
                    const bool scalarChanged =
                        (fabs(timerScalar - g_baselineTimerScalar) > 0.001);
                    const bool intervalChanged =
                        (fabs(timerInterval - g_baselineTimerInterval) > 0.001);
                    if (ptrChanged || scalarChanged || intervalChanged)
                    {
                        mod::Log(
                            "SPEED_DIAG[9a]: S#%u *** TIMER CHANGED *** "
                            "ptr 0x%08lX->0x%08lX "
                            "scalar %.4f->%.4f interval %.4f->%.4f",
                            g_sessionNumber,
                            static_cast<unsigned long>(g_baselineTimerPtr),
                            static_cast<unsigned long>(timerPtr),
                            g_baselineTimerScalar, timerScalar,
                            g_baselineTimerInterval, timerInterval);
                        // Update baseline so we don't spam.
                        g_baselineTimerScalar   = timerScalar;
                        g_baselineTimerInterval = timerInterval;
                        g_baselineTimerPtr      = timerPtr;
                    }
                }

                // 9b: Init-once guard transition detection.
                if (!g_initOnceGuardTracked)
                {
                    g_lastInitOnceGuard    = initOnceGuard;
                    g_initOnceGuardTracked = true;
                }
                else if (initOnceGuard != g_lastInitOnceGuard)
                {
                    mod::Log(
                        "SPEED_DIAG[9b]: S#%u *** INIT-ONCE GUARD CHANGED *** "
                        "0x%04X -> 0x%04X (low byte: %s)",
                        g_sessionNumber,
                        static_cast<unsigned>(g_lastInitOnceGuard),
                        static_cast<unsigned>(initOnceGuard),
                        (initOnceGuard & 0xFF) != 0
                            ? "SET - global init SKIPPED"
                            : "CLEAR - global init WILL RUN");
                    g_lastInitOnceGuard = initOnceGuard;
                }

                // 9c: Render context pointer change detection (H2).
                if (!g_renderCtxTracked)
                {
                    g_lastRenderCtxPtr  = renderCtxPtr;
                    g_renderCtxTracked  = true;
                }
                else if (renderCtxPtr != g_lastRenderCtxPtr)
                {
                    mod::Log(
                        "SPEED_DIAG[9c]: S#%u *** RENDER CTX CHANGED *** "
                        "0x%08lX -> 0x%08lX%s",
                        g_sessionNumber,
                        static_cast<unsigned long>(g_lastRenderCtxPtr),
                        static_cast<unsigned long>(renderCtxPtr),
                        renderCtxPtr == 0
                            ? " - NOW NULL (H2 stale context!)"
                            : "");
                    g_lastRenderCtxPtr = renderCtxPtr;
                }

                // 9d: Session pointer change detection (H1 double-init).
                if (!g_sessionPtrTracked)
                {
                    g_lastSessionPtrInTick = currentSession;
                    g_sessionPtrTracked    = true;
                }
                else if (currentSession != g_lastSessionPtrInTick)
                {
                    mod::Log(
                        "SPEED_DIAG[9d]: S#%u *** SESSION PTR CHANGED *** "
                        "0x%08lX -> 0x%08lX (tick=%u)",
                        g_sessionNumber,
                        static_cast<unsigned long>(g_lastSessionPtrInTick),
                        static_cast<unsigned long>(currentSession),
                        g_frameTick);
                    g_lastSessionPtrInTick = currentSession;
                }
            }

            // ---- LOG LINE 10: per-frame QPC delta (burst only) ----
            // During the first 30 ticks, log the exact wall-clock interval
            // between consecutive frames.  At 64fps each delta should be
            // ~15.6ms; at 128fps (double-speed bug) each delta is ~7.8ms.
            if (isBurst)
            {
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                if (g_prevFrameQpcValid)
                {
                    const double deltaMs =
                        static_cast<double>(
                            qpcNow.QuadPart - g_prevFrameQpc.QuadPart)
                        * 1000.0
                        / static_cast<double>(qpcFreq.QuadPart);
                    mod::Log(
                        "SPEED_DIAG[10]: S#%u tick=%u frameDeltaMs=%.3f "
                        "(expect ~15.6 at 64fps, ~7.8 at 128fps)",
                        g_sessionNumber, g_frameTick, deltaMs);
                }
                g_prevFrameQpc = qpcNow;
                g_prevFrameQpcValid = true;
            }

            // ---- LOG LINE 11: timer object deep dump (burst only) ----
            // Read additional timer object fields beyond +16/+32 to
            // capture the full timer state on session start.
            if (isBurst && timerPtr != 0 && g_frameTick <= 5)
            {
                double timerField0 = 0.0, timerField8 = 0.0;
                double timerField24 = 0.0, timerField40 = 0.0;
                int32_t timerField48 = 0;
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 0),
                    &timerField0);
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 8),
                    &timerField8);
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 24),
                    &timerField24);
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 40),
                    &timerField40);
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(timerPtr + 48),
                    &timerField48);
                mod::Log(
                    "SPEED_DIAG[11]: S#%u timerDump "
                    "+0=%.6f +8=%.6f +16=%.6f +24=%.6f "
                    "+32=%.6f +40=%.6f +48=%d",
                    g_sessionNumber,
                    timerField0, timerField8, timerInterval,
                    timerField24, timerScalar, timerField40,
                    timerField48);
            }
        }
    }

    // ====================================================================
    // Desync detection - periodic game-state snapshot logging
    // ====================================================================
    // During active online matches, periodically log key game state values
    // that BOTH peers should agree on.  If logs from both sides are compared
    // and these values diverge, the exact frame of desync can be identified.
    //
    // Fires every 120 frames (~2s at 60fps) during online matches.  Also
    // fires on the first frame of each match and whenever the frame counter
    // crosses a round boundary (every 3600 frames = ~60s).
    // ====================================================================
    if (g_localRoleFlag == kLocalRoleOnline
        && g_localInitAppliedForSession
        && currentSession != 0
        && g_activeRevival != nullptr
        && !preTickDisconnect)
    {
        // Experimental tracer: one lightweight frame observation during
        // negotiation, then selected post-tick state only after both peers
        // accept the same future start (no hot-path file/socket I/O).
        if (netplay::bridge::desync_monitor::IsSessionTracing())
        {
            int dmFrame = -1;
            (void)SafeReadInt(
                reinterpret_cast<const void*>(
                    currentSession + g_activeRevival->sessionOffsetCurrentFrame),
                &dmFrame);
            netplay::bridge::desync_monitor::ObserveFrame(dmFrame);
            if (netplay::bridge::desync_monitor::IsCaptureArmed())
            {
                int dmCommit = -1;
                (void)ReadGameplaySyncFrameForRecovery(currentSession, &dmCommit);
                netplay::bridge::desync_monitor::RecordFrameTick(
                    currentSession, dmFrame, dmCommit);
            }
        }

        const bool isDesyncCheckFrame =
            (g_frameTick == 1)
            || (g_frameTick % 120 == 0)
            || (g_frameTick % 3600 == 0);

        if (isDesyncCheckFrame)
        {
            // Read the critical game state that must be in sync between peers.
            int dsCurrentFrame = -1;
            int dsInputDelay = -1;
            int dsMatchId = -1;
            int dsActivePlayer = -1;
            int dsAdvanceCounter = -1;
            int dsSyncFrame = -1;
            int dsPingMs = -1;
            uint32_t dsSentinel = 0;
            uint8_t dsScreen = 0xFF;

            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetCurrentFrame), &dsCurrentFrame);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetInputDelay), &dsInputDelay);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetMatchId), &dsMatchId);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetActivePlayer), &dsActivePlayer);
            (void)SafeReadDword(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetSentinel), &dsSentinel);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetPingMs), &dsPingMs);

            // Game mode fields
            int dsCommitFrame = -1;
            const uintptr_t dsGmBase = currentSession + g_activeRevival->sessionOffsetGameModeSnapshot;
            (void)SafeReadInt(reinterpret_cast<const void*>(dsGmBase + 12), &dsAdvanceCounter);
            (void)SafeReadInt(reinterpret_cast<const void*>(dsGmBase + 16), &dsCommitFrame);
            (void)SafeReadInt(reinterpret_cast<const void*>(dsGmBase + 20), &dsSyncFrame);

            __try {
                dsScreen = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            // Compute a lightweight checksum of the state snapshot.
            // Both peers should produce the same checksum if in sync.
            const uint32_t stateChecksum =
                static_cast<uint32_t>(dsCurrentFrame)
                ^ (static_cast<uint32_t>(dsAdvanceCounter) * 2654435761u)
                ^ (static_cast<uint32_t>(dsSyncFrame) * 40503u)
                ^ (static_cast<uint32_t>(dsMatchId) << 16)
                ^ dsSentinel;

            if (SyncDiagnosticsEnabled())
            {
                const FpuControlSnapshot dsFpu = CaptureFpuControlSnapshot();
                mod::Log(
                    "DESYNC_CHECK: S#%u tick=%u screen=%u frame=%d advCtr=%d "
                    "commit=%d syncFrame=%d matchId=%d delay=%d ping=%d active=%d "
                    "sentinel=0x%08lX chk=0x%08lX "
                    "fpuCrt=%s0x%08lX x87=%s0x%04X mxcsr=%s0x%08lX",
                    g_sessionNumber, g_frameTick,
                    static_cast<unsigned>(dsScreen),
                    dsCurrentFrame, dsAdvanceCounter,
                    dsCommitFrame, dsSyncFrame, dsMatchId,
                    dsInputDelay, dsPingMs, dsActivePlayer,
                    static_cast<unsigned long>(dsSentinel),
                    static_cast<unsigned long>(stateChecksum),
                    dsFpu.crtValid ? "" : "invalid:",
                    static_cast<unsigned long>(dsFpu.crtControl),
                    dsFpu.x87Valid ? "" : "invalid:",
                    static_cast<unsigned>(dsFpu.x87Control),
                    dsFpu.mxcsrValid ? "" : "invalid:",
                    static_cast<unsigned long>(dsFpu.mxcsr));
            }
            else
            {
                mod::Log(
                    "DESYNC_CHECK: S#%u tick=%u screen=%u frame=%d advCtr=%d "
                    "commit=%d syncFrame=%d matchId=%d delay=%d ping=%d active=%d "
                    "sentinel=0x%08lX chk=0x%08lX",
                    g_sessionNumber, g_frameTick,
                    static_cast<unsigned>(dsScreen),
                    dsCurrentFrame, dsAdvanceCounter,
                    dsCommitFrame, dsSyncFrame, dsMatchId,
                    dsInputDelay, dsPingMs, dsActivePlayer,
                    static_cast<unsigned long>(dsSentinel),
                    static_cast<unsigned long>(stateChecksum));
            }
        }
    }

    // Session creation/destruction is unsafe while the old role's virtual
    // tick is still using its `this` pointer.  Title/menu hooks only request
    // lifecycle work; perform it here after g_origPerFrameTick has returned
    // and after all diagnostics finished reading the old object.
    if (!g_tickRecoveryPending
        && InterlockedExchange(&g_deferredLifecycleWorkRequested, 0) != 0)
    {
        const LONG deferredSelection =
            InterlockedExchange(&g_deferredTitleSelection, -1);
        mod::Log(
            "TICK_HOOK: processing deferred lifecycle work post-tick "
            "selection=%ld tournamentPending=%d",
            static_cast<long>(deferredSelection),
            g_tournamentReturnCleanupPending ? 1 : 0);
        LogRevival102jDeepSnapshot("FrameHook.80.deferred_lifecycle_pre");

        if (deferredSelection >= 0)
        {
            netplay::bridge::OnTitleSelectionConfirmed(
                static_cast<int>(deferredSelection));
        }
        (void)netplay::bridge::CompletePendingTournamentReturnCleanup();
        netplay::bridge::Tick();
        LogRevival102jDeepSnapshot("FrameHook.81.deferred_lifecycle_post");
    }

    // ---- ExitProcess recovery path -----------------------------------------
    // If ExitProcess fired during the per-frame tick, NeutralizeExitProcess
    // longjmp'd back through RunPerFrameTickDispatch, which set
    // g_tickRecoveryPending.  Perform full cleanup now that we're outside
    // the setjmp scope and can safely use C++ constructs.
    if (g_tickRecoveryPending)
    {
        g_tickRecoveryPending = false;
        InterlockedExchange(&g_deferredLifecycleWorkRequested, 0);
        InterlockedExchange(&g_deferredTitleSelection, -1);

        const int recoveredRole = g_localRoleFlag;
        const DWORD recoveredPid = g_revivalProcessId;
        const uint8_t recoveryScreen = ReadCurrentScreenIndexForRecovery();
        LogSessionDiagnosticState("TickHook_recovery_entry");
        LogRevival102jDeepSnapshot("FrameHookRecovery.01.entry");
        mod::Log(
            "TICK_HOOK: ExitProcess intercepted during per-frame tick "
            "(role=%d pid=%lu screen=%u) - performing full cleanup",
            recoveredRole,
            static_cast<unsigned long>(recoveredPid),
            static_cast<unsigned>(recoveryScreen));

        if (ShouldSuppressOldGameplayExitTeardown("tick_exitprocess"))
        {
            LogSessionDiagnosticState("TickHook_recovery_suppressed");
            return 0;
        }

        if (IsGameplayExitRecoveryScreen(recoveryScreen))
        {
            (void)netplay::bridge::recovery::BeginGameplayExitRecovery(
                netplay::bridge::recovery::GameplayExitOrigin::TickExitProcess);
            LogSessionDiagnosticState("TickHook_recovery_shared_exit");
            return 0;
        }

        // Step 1: Reinstate a live local-play session.
        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "TICK_HOOK: recovery step 1 ForceLocalPlayInit result=%d",
            initOk ? 1 : 0);

        // Step 2: Terminate the dead helper process.
        if (g_revivalProcess != nullptr)
        {
            const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
            const DWORD termErr = termOk ? 0 : GetLastError();
            CloseHandle(g_revivalProcess);
            g_revivalProcess = nullptr;
            g_revivalProcessId = 0;
            mod::Log(
                "TICK_HOOK: recovery step 2 helper terminated "
                "(pid=%lu termOk=%d err=%lu)",
                static_cast<unsigned long>(recoveredPid),
                termOk ? 1 : 0,
                static_cast<unsigned long>(termErr));
        }
        else
        {
            mod::Log("TICK_HOOK: recovery step 2 skipped (no helper handle)");
        }

        // Step 3: Restore DLL Jcc patches.
        const bool patchOk = RestoreDllExitProcessPatches();
        mod::Log(
            "TICK_HOOK: recovery step 3 RestoreDllExitProcessPatches result=%d",
            patchOk ? 1 : 0);

        // Step 4: Reset stale text renderer state without leaving 1.02j disabled.
        const bool textOk = ResetRevivalTextRenderingAfterCleanup(
            "tick_exitprocess_recovery");
        mod::Log(
            "TICK_HOOK: recovery step 4 ResetRevivalTextRenderingAfterCleanup result=%d",
            textOk ? 1 : 0);

        // Step 5: Reset crash/validation state.
        mod::ResetCrashRecoveryState();
        ResetGameModeValidation();
        mod::Log("TICK_HOOK: recovery step 5 crash/validation state reset");

        // Step 6: Force game mode to title screen.
        const bool modeOk = ForceGameModeToTitle();
        mod::Log(
            "TICK_HOOK: recovery step 6 ForceGameModeToTitle result=%d",
            modeOk ? 1 : 0);

        g_localInitAppliedForSession = false;
        mod::Log(
            "TICK_HOOK: full recovery complete (was role=%d), "
            "next title-screen frame will consume exit interception",
            recoveredRole);
        LogSessionDiagnosticState("TickHook_recovery_exit");
        LogRevival102jDeepSnapshot("FrameHookRecovery.99.exit");

        return 0;
    }

    if (InterlockedCompareExchange(&g_scheduledGracefulQuitTeardownActive, 0, 0) != 0)
    {
        const DWORD elapsedMs = GetTickCount() - g_scheduledGracefulQuitTeardownStartMs;
        const bool helperAlive = g_revivalProcess != nullptr && IsPeerProcessAlive();
        if (helperAlive && elapsedMs < kScheduledGracefulQuitTeardownDelayMs)
        {
            return 0;
        }

        char phaseTag[sizeof(g_scheduledGracefulQuitPhase)] = {};
        std::memcpy(phaseTag, g_scheduledGracefulQuitPhase, sizeof(phaseTag));
        const LONG quitHeadBefore = g_scheduledGracefulQuitHead;
        const LONG quitTailBefore = g_scheduledGracefulQuitTail;
        const DWORD scheduledPid = g_scheduledGracefulQuitHelperPid;
        ResetScheduledGracefulQuitTeardown();
        mod::Log(
            "TICK_HOOK: graceful-quit scheduled teardown ready elapsedMs=%lu helperAlive=%d scheduledPid=%lu phase=%s",
            static_cast<unsigned long>(elapsedMs),
            helperAlive ? 1 : 0,
            static_cast<unsigned long>(scheduledPid),
            phaseTag[0] != '\0' ? phaseTag : "POST-TICK");
        return FinalizeGracefulQuitTeardown(
            phaseTag[0] != '\0' ? phaseTag : "POST-TICK",
            quitHeadBefore,
            quitTailBefore,
            helperAlive ? "delay_elapsed" : "helper_exited");
    }

    if (preTickGracefulQuit)
    {
        return RecoverFromQuitRingSignal("PRE-TICK", preTickQuitHead, preTickQuitTail);
    }

    // ---- Proactive graceful session-end detection -------------------------
    // Also catch Quit-ring signals that were published during the current
    // DLL tick, not just between frames.
    if (g_dllExitProcessPatchesSaved
        && InterlockedCompareExchange(&g_onlineMatchEscGracefulQuitArmed, 0, 0) == 0)
    {
        LONG quitHeadAfter = 0;
        LONG quitTailAfter = 0;
        if (ConsumeGracefulQuitRingSignal(&quitHeadAfter, &quitTailAfter))
        {
            if (!ConsumeLocalBattleEscQuitRingIgnore(
                    "POST-TICK",
                    quitHeadAfter,
                    quitTailAfter))
            {
                return RecoverFromQuitRingSignal("POST-TICK", quitHeadAfter, quitTailAfter);
            }
        }
    }

    // ---- Proactive network-disconnect detection ---------------------------
    // The DLL exit-process Jcc patches make ExitProcess unreachable, which
    // is a problem during charselect/loading: the DLL's session tick doesn't
    // actively drive rollback and never triggers ExitProcess even after the
    // remote opponent disconnects.  The helper process (EfzRevival.exe)
    // stays alive because ExitProcess is patched out, but its console
    // capture *does* detect the disconnect and publishes an error string
    // to the IPC shared block. Detected messages include:
    //   - "Connection timed out"      (initial handshake timeout)
    //   - "Source quit or timed out"  (connected source peer quit/timed out)
    //   - "Host timed out"            (host peer timed out)
    //   - "Remote timed out"          (remote peer timed out, general)
    //   - "Socket error"              (low-level network failure)
    //
    // Check every frame (not just every ~60) to minimise the window where
    // the DLL could render corrupted frames with missing opponent data.
    // This post-tick check catches errors set DURING the current tick
    // (complementing the pre-tick check which catches errors from BETWEEN
    // frames).
    // -----------------------------------------------------------------------
    if (g_dllExitProcessPatchesSaved
        && g_hostBlock != nullptr)
    {
        const LONG consoleErrSerial =
            InterlockedCompareExchange(&g_hostBlock->consoleErrorSerial, 0, 0);
        if (consoleErrSerial > 0)
        {
            // Read the error text for logging before recovery clears it.
            char consoleErrText[128] = {};
            ReadConsoleError(nullptr, consoleErrText, sizeof(consoleErrText));

            const int deadRole = g_localRoleFlag;
            const DWORD deadPid = g_revivalProcessId;
            LogSessionDiagnosticState("TickHook_disconnectDetected_entry");
            LogRevival102jDeepSnapshot("FrameHookDisconnect.01.entry");
            mod::Log(
                "TICK_HOOK: *** NETWORK DISCONNECT *** frameTick=%u "
                "role=%d pid=%lu consoleError='%s' - synthesizing exit interception",
                g_frameTick,
                deadRole,
                static_cast<unsigned long>(deadPid),
                consoleErrText);

            (void)netplay::bridge::recovery::BeginGameplayExitRecovery(
                netplay::bridge::recovery::GameplayExitOrigin::ConsoleErrorDisconnect);
            LogSessionDiagnosticState("TickHook_disconnectDetected_exit");
            LogRevival102jDeepSnapshot("FrameHookDisconnect.99.exit");

            return 0;
        }
    }

    // -----------------------------------------------------------------------
    // Spectator ESC exit
    // -----------------------------------------------------------------------
    // While spectating, the Revival DLL's input-replay system consumes all
    // local keyboard input, so the game's own Esc handler never fires.
    // We detect Esc ourselves with GetAsyncKeyState and synthesize the same
    // exit interception that the disconnect path uses, which routes the
    // player back through the title screen into the netplay menu.
    // -----------------------------------------------------------------------
    if (g_localRoleFlag == kLocalRoleSpectate
        && g_dllExitProcessPatchesSaved)
    {
        static bool s_spectateEscWasDown = false;
        const bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool escRisingEdge = escDown && !s_spectateEscWasDown;
        s_spectateEscWasDown = escDown;

        if (escRisingEdge)
        {
            const int deadRole = g_localRoleFlag;
            const DWORD deadPid = g_revivalProcessId;
            LogSessionDiagnosticState("TickHook_spectatorEsc_entry");
            LogRevival102jDeepSnapshot("FrameHookSpectatorEsc.01.entry");
            mod::Log(
                "TICK_HOOK: *** SPECTATOR ESC EXIT *** frameTick=%u "
                "role=%d pid=%lu - user requested spectate disconnect",
                g_frameTick,
                deadRole,
                static_cast<unsigned long>(deadPid));

            (void)netplay::bridge::recovery::BeginGameplayExitRecovery(
                netplay::bridge::recovery::GameplayExitOrigin::SpectatorEsc);
            LogSessionDiagnosticState("TickHook_spectatorEsc_exit");
            LogRevival102jDeepSnapshot("FrameHookSpectatorEsc.99.exit");

            return 0;
        }
    }

    // ---- Hard-fallback watchdog -------------------------------------------
    // Detect Revival child process death that slipped past ExitProcess
    // interception and consoleErrorSerial detection.  This catches external
    // process kills (Task Manager, OS termination), crashes in non-DLL code
    // (unhandled SEH in the helper), and any scenario where the process
    // dies without going through our hooked ExitProcess or publishing a
    // console error.
    //
    // Uses a grace-period counter to avoid racing with the existing recovery
    // mechanisms (which fire synchronously within the same frame).  Only
    // triggers after kWatchdogGraceFrames consecutive frames of sustained
    // dead-process detection with no other recovery path having fired.
    // -----------------------------------------------------------------------
    if (g_dllExitProcessPatchesSaved
        && g_localRoleFlag != kLocalRoleLocalPlay
        && !g_tickRecoveryPending
        && !g_frameRecoveryPending
        && !g_deferredCancelCleanup)
    {
        const bool processWasCreated = (g_revivalProcess != nullptr);
        const bool processAlive = processWasCreated && IsPeerProcessAlive();
        const bool exitAlreadyPending =
            (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) != 0);

        if (!processAlive && processWasCreated && !exitAlreadyPending)
        {
            // Read screen index - only trigger on non-title screens.
            uint8_t wdScreen = 0;
            __try {
                wdScreen = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            if (wdScreen != 0)
            {
                ++g_watchdogDeadFrameCount;
                if (g_watchdogDeadFrameCount == 1)
                {
                    mod::Log(
                        "TICK_HOOK: WATCHDOG: Revival process dead, grace period "
                        "started (frameTick=%u screen=%u role=%d pid=%lu)",
                        g_frameTick,
                        static_cast<unsigned int>(wdScreen),
                        g_localRoleFlag,
                        static_cast<unsigned long>(g_revivalProcessId));
                }

                if (g_watchdogDeadFrameCount >= kWatchdogGraceFrames)
                {
                    const int deadRole = g_localRoleFlag;
                    const DWORD deadPid = g_revivalProcessId;
                    LogSessionDiagnosticState("TickHook_hardFallback_entry");
                    mod::Log(
                        "TICK_HOOK: *** HARD FALLBACK *** frameTick=%u "
                        "role=%d pid=%lu screen=%u - Revival process died "
                        "without ExitProcess/consoleError, performing "
                        "emergency cleanup",
                        g_frameTick,
                        deadRole,
                        static_cast<unsigned long>(deadPid),
                        static_cast<unsigned int>(wdScreen));

                    if (ShouldSuppressOldGameplayExitTeardown("helper_death_watchdog"))
                    {
                        g_watchdogDeadFrameCount = 0;
                        LogSessionDiagnosticState("TickHook_hardFallback_suppressed");
                        return 0;
                    }

                    if (IsGameplayExitRecoveryScreen(wdScreen))
                    {
                        (void)netplay::bridge::recovery::BeginGameplayExitRecovery(
                            netplay::bridge::recovery::GameplayExitOrigin::HelperDeathWatchdog);
                        g_watchdogDeadFrameCount = 0;
                        LogSessionDiagnosticState("TickHook_hardFallback_shared_exit");
                        return 0;
                    }

                    // Synthesize exit interception so ConsumeRevivalExitInterception
                    // fires on the next title-screen frame and routes to the
                    // netplay menu (with lobby NotifyEndMatch if applicable).
                    InterlockedExchange(&g_revivalExitMode,
                                        static_cast<LONG>(g_localRoleFlag));
                    InterlockedExchange(&g_revivalExitIntercepted, 1);

                    NeutralizeRevivalSessionVtable();

                    // Step 1: Reinstate a live local-play session.
                    const bool initOk = ForceLocalPlayInit();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 1 ForceLocalPlayInit "
                        "result=%d",
                        initOk ? 1 : 0);

                    // Step 2: Terminate / close the dead helper process.
                    if (g_revivalProcess != nullptr)
                    {
                        const BOOL termOk =
                            TerminateProcess(g_revivalProcess, 0);
                        const DWORD termErr = termOk ? 0 : GetLastError();
                        CloseHandle(g_revivalProcess);
                        g_revivalProcess = nullptr;
                        g_revivalProcessId = 0;
                        mod::Log(
                            "TICK_HOOK: hard-fallback step 2 helper terminated "
                            "(pid=%lu termOk=%d err=%lu)",
                            static_cast<unsigned long>(deadPid),
                            termOk ? 1 : 0,
                            static_cast<unsigned long>(termErr));
                    }
                    else
                    {
                        mod::Log(
                            "TICK_HOOK: hard-fallback step 2 skipped "
                            "(no helper handle)");
                    }

                    // Step 3: Restore DLL Jcc patches.
                    const bool patchOk = RestoreDllExitProcessPatches();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 3 "
                        "RestoreDllExitProcessPatches result=%d",
                        patchOk ? 1 : 0);

                    // Step 4: Reset stale text renderer state without leaving 1.02j
                    // rendering disabled.
                    const bool textOk = ResetRevivalTextRenderingAfterCleanup(
                        "tick_hard_fallback");
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 4 "
                        "ResetRevivalTextRenderingAfterCleanup result=%d",
                        textOk ? 1 : 0);

                    // Step 5: Reset crash/validation state.
                    mod::ResetCrashRecoveryState();
                    ResetGameModeValidation();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 5 "
                        "crash/validation state reset");

                    // Step 6: Force game mode to title screen.
                    const bool modeOk = ForceGameModeToTitle();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 6 "
                        "ForceGameModeToTitle result=%d",
                        modeOk ? 1 : 0);

                    g_localInitAppliedForSession = false;
                    g_watchdogDeadFrameCount = 0;
                    mod::Log(
                        "TICK_HOOK: hard-fallback recovery complete "
                        "(was role=%d), next title-screen frame will "
                        "consume exit interception",
                        deadRole);
                    LogSessionDiagnosticState("TickHook_hardFallback_exit");

                    return 0;
                }
            }
            else
            {
                // On title screen - existing title-hook mechanisms handle it.
                g_watchdogDeadFrameCount = 0;
            }
        }
        else
        {
            // Process alive, no process, or exit already pending.
            g_watchdogDeadFrameCount = 0;
        }
    }
    else
    {
        // Not in an active session or recovery already in progress.
        g_watchdogDeadFrameCount = 0;
    }

    // If CancelSession tried to run ForceLocalPlayInit while we were inside
    // the tick (which would destroy the session that RollbackLoopTick was
    // using as 'this'), it deferred the work.  Execute it now that the tick
    // has completed safely.
    if (g_deferredCancelCleanup)
    {
        if (netplay::bridge::recovery::ShouldSuppressLegacyGameplayExitCleanup())
        {
            mod::Log(
                "GAMEPLAY_EXIT_SUPPRESS_DEFERRED_CLEANUP_AFTER_RECOVERY state=%s frontendReturnState=%s pending=%d sourceReason=%s sourceScreen=%u",
                netplay::bridge::recovery::CurrentGameplayExitRecoveryStateName(),
                netplay::bridge::frontend_return::CurrentStateName(),
                g_deferredCancelCleanup ? 1 : 0,
                CurrentDeferredCancelCleanupReason(),
                static_cast<unsigned>(CurrentDeferredCancelCleanupSourceScreen()));
            if (netplay::bridge::recovery::HasGameplayExitMenuEntryStarted()
                || netplay::bridge::recovery::HasGameplayExitMenuEntryBeenConsumed()
                || netplay::bridge::recovery::WasGameplayExitRecoveryCompleted()
                || netplay::bridge::frontend_return::HasConsumedNetplayMenuContinuation())
            {
                mod::Log("GAMEPLAY_EXIT_INVARIANT_VIOLATION name=deferred_cleanup_after_menu_entry");
            }
            (void)ClearDeferredCancelCleanupForRecovery("tick_suppressed_after_recovery");
            return result;
        }
        g_deferredCancelCleanup = false;
        g_deferredCancelCleanupReason[0] = '\0';
        g_deferredCancelCleanupSourceScreen = 0xFF;
        g_deferredCancelCleanupSourceRole = -1;
        g_deferredCancelCleanupSourceFrame = 0;
        mod::Log(
            "TICK_HOOK: executing deferred cancel cleanup "
            "(ForceLocalPlayInit was unsafe mid-tick)");

        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "TICK_HOOK: deferred ForceLocalPlayInit result=%d",
            initOk ? 1 : 0);

        (void)ClearRevivalText();
        (void)RestoreRenderContext();
        (void)ResetRevivalTextRenderingAfterCleanup("deferred_cancel_cleanup");
        mod::ResetCrashRecoveryState();
        ResetGameModeValidation();

        mod::Log("TICK_HOOK: deferred cancel cleanup complete");
    }

    netplay::bridge::recovery::ObserveGameplayExitRecoveryProgress(
        ReadCurrentScreenIndexForRecovery(),
        -1,
        g_localRoleFlag);

    // Pulse a lightweight export tick every frame so that activityPhase,
    // inNetplayMenu, stateSeq, and all other shared-memory fields remain
    // current during loading (screenIdx=2) and battle (screenIdx=3) - screens
    // that have no title/charselect hook calling the full session_bridge::Tick().
    // TickExportOnly() only calls state_export::Update(g_status) under the
    // bridge mutex; it does NOT call takeover::Tick() and is safe here.
    netplay::bridge::TickExportOnly();

    // --- Tick cost measurement (mod overhead budget) -------------------------
    // Measure total time our hook spent AFTER entering the tick.  Warn when
    // the mod's own processing exceeds the per-frame budget, which could be
    // the cause of stalls that slow the game.
    {
        LARGE_INTEGER tickExitQpc = {};
        QueryPerformanceCounter(&tickExitQpc);
        if (g_qpcFreqValid)
        {
            const double tickCostMs =
                static_cast<double>(tickExitQpc.QuadPart - tickEntryQpc.QuadPart)
                * 1000.0 / static_cast<double>(g_qpcFreqCached.QuadPart);
            if (tickCostMs > kTickBudgetMs)
            {
                ++g_tickBudgetExceededCount;
                // Log first 5 and then every 300th to avoid spamming.
                if (g_tickBudgetExceededCount <= 5
                    || (g_tickBudgetExceededCount % 300 == 0))
                {
                    mod::Log(
                        "PERF_WARN: *** TICK OVER BUDGET *** tick=%u "
                        "costMs=%.2f budget=%.1fms exceeded=%u - mod "
                        "processing is stalling the game",
                        g_frameTick, tickCostMs, kTickBudgetMs,
                        g_tickBudgetExceededCount);
                }
            }
        }
    }

    return result;
}

// Returns true while inside the per-frame tick (g_origPerFrameTick running).
bool IsInsideFrameTick()
{
    return g_insideFrameTick;
}

// Request deferred cancel cleanup after the frame tick returns.
void RequestDeferredCancelCleanup(const char* reason)
{
    g_deferredCancelCleanup = true;
    CopyString(
        g_deferredCancelCleanupReason,
        sizeof(g_deferredCancelCleanupReason),
        reason != nullptr ? reason : "unknown");
    g_deferredCancelCleanupSourceScreen = ReadCurrentScreenIndexForRecovery();
    g_deferredCancelCleanupSourceRole = g_localRoleFlag;
    g_deferredCancelCleanupSourceFrame = g_frameTick;
    mod::Log(
        "GAMEPLAY_EXIT_DEFERRED_CANCEL_CLEANUP_REQUEST reason=%s sourceScreen=%u sourceRole=%d sourceFrame=%u",
        g_deferredCancelCleanupReason,
        static_cast<unsigned>(g_deferredCancelCleanupSourceScreen),
        g_deferredCancelCleanupSourceRole,
        g_deferredCancelCleanupSourceFrame);
}

void ArmOnlineMatchEscGracefulQuit()
{
    InterlockedExchange(&g_onlineMatchEscGracefulQuitArmed, 1);
}

bool ConsumeOnlineMatchEscGracefulQuit()
{
    return InterlockedExchange(&g_onlineMatchEscGracefulQuitArmed, 0) != 0;
}

void ResetOnlineMatchEscGracefulQuit()
{
    InterlockedExchange(&g_onlineMatchEscGracefulQuitArmed, 0);
}

void ConfirmCharacterSelectEscToMenu()
{
    const bool battleIgnoreWasArmed =
        InterlockedCompareExchange(&g_localBattleEscQuitRingIgnoreArmed, 0, 0) != 0;
    ResetLocalBattleEscQuitRingIgnore();
    mod::Log(
        "TICK_HOOK: character-select ESC-to-menu confirmed; "
        "battle Quit-ring ignore cleared wasArmed=%d",
        battleIgnoreWasArmed ? 1 : 0);
}

static void ResetLocalBattleEscQuitRingIgnore()
{
    InterlockedExchange(&g_localBattleEscQuitRingIgnoreArmed, 0);
    g_localBattleEscQuitRingIgnoreStartMs = 0;
}

static void ArmLocalBattleEscQuitRingIgnore()
{
    g_localBattleEscQuitRingIgnoreStartMs = GetTickCount();
    InterlockedExchange(&g_localBattleEscQuitRingIgnoreArmed, 1);
}

static bool ConsumeLocalBattleEscQuitRingIgnore(
    const char* phaseTag,
    LONG quitHeadBefore,
    LONG quitTailBefore)
{
    if (InterlockedCompareExchange(&g_localBattleEscQuitRingIgnoreArmed, 0, 0) == 0)
    {
        return false;
    }

    const DWORD elapsedMs = GetTickCount() - g_localBattleEscQuitRingIgnoreStartMs;
    if (elapsedMs > kLocalBattleEscQuitRingIgnoreWindowMs)
    {
        ResetLocalBattleEscQuitRingIgnore();
        mod::Log(
            "TICK_HOOK: Quit ring entry arrived after battle ESC ignore window expired "
            "phase=%s frameTick=%u quitHead=%ld quitTail=%ld elapsedMs=%lu - treating as real session end",
            phaseTag != nullptr ? phaseTag : "POST-TICK",
            g_frameTick,
            static_cast<long>(quitHeadBefore),
            static_cast<long>(quitTailBefore),
            static_cast<unsigned long>(elapsedMs));
        return false;
    }

    ResetLocalBattleEscQuitRingIgnore();
    mod::Log(
        "TICK_HOOK: local battle ESC consumed Quit ring without session teardown "
        "phase=%s frameTick=%u quitHead=%ld quitTail=%ld elapsedMs=%lu",
        phaseTag != nullptr ? phaseTag : "POST-TICK",
        g_frameTick,
        static_cast<long>(quitHeadBefore),
        static_cast<long>(quitTailBefore),
        static_cast<unsigned long>(elapsedMs));
    return true;
}

static void ResetScheduledGracefulQuitTeardown()
{
    InterlockedExchange(&g_scheduledGracefulQuitTeardownActive, 0);
    g_scheduledGracefulQuitTeardownStartMs = 0;
    g_scheduledGracefulQuitHead = 0;
    g_scheduledGracefulQuitTail = 0;
    g_scheduledGracefulQuitHelperPid = 0;
    std::memset(g_scheduledGracefulQuitPhase, 0, sizeof(g_scheduledGracefulQuitPhase));
}

static void ScheduleGracefulQuitTeardown(const char* phaseTag, LONG quitHeadBefore, LONG quitTailBefore)
{
    g_scheduledGracefulQuitTeardownStartMs = GetTickCount();
    g_scheduledGracefulQuitHead = quitHeadBefore;
    g_scheduledGracefulQuitTail = quitTailBefore;
    g_scheduledGracefulQuitHelperPid = g_revivalProcessId;
    std::memset(g_scheduledGracefulQuitPhase, 0, sizeof(g_scheduledGracefulQuitPhase));
    if (phaseTag != nullptr)
    {
        strncpy_s(
            g_scheduledGracefulQuitPhase,
            sizeof(g_scheduledGracefulQuitPhase),
            phaseTag,
            _TRUNCATE);
    }
    InterlockedExchange(&g_scheduledGracefulQuitTeardownActive, 1);
}

static bool EnsureQuitRingHeader()
{
    if (g_quitRingHeader != nullptr)
    {
        return true;
    }

    HANDLE hMap = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        RevivalWireName("Quit"));
    if (hMap == nullptr)
    {
        return false;
    }

    auto* header = static_cast<volatile LONG*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 8));
    if (header == nullptr)
    {
        CloseHandle(hMap);
        return false;
    }

    g_quitRingHeader = header;
    CloseHandle(hMap);
    return true;
}

static void ReleaseQuitRingHeader()
{
    if (g_quitRingHeader != nullptr)
    {
        UnmapViewOfFile(const_cast<LONG*>(g_quitRingHeader));
        g_quitRingHeader = nullptr;
    }
}

static bool ConsumeGracefulQuitRingSignal(LONG* outHeadBefore, LONG* outTailBefore)
{
    if (outHeadBefore != nullptr)
    {
        *outHeadBefore = 0;
    }
    if (outTailBefore != nullptr)
    {
        *outTailBefore = 0;
    }

    if (!EnsureQuitRingHeader())
    {
        return false;
    }

    auto* headPtr = const_cast<LONG*>(&g_quitRingHeader[0]);
    auto* tailPtr = const_cast<LONG*>(&g_quitRingHeader[1]);
    const LONG headBefore = InterlockedCompareExchange(headPtr, 0, 0);
    const LONG tailBefore = InterlockedCompareExchange(tailPtr, 0, 0);

    if (outHeadBefore != nullptr)
    {
        *outHeadBefore = headBefore;
    }
    if (outTailBefore != nullptr)
    {
        *outTailBefore = tailBefore;
    }

    if (headBefore == tailBefore)
    {
        return false;
    }

    // Consume the pending graceful-quit signal once we decide to replace
    // Revival's patched-out quitMem -> ExitProcess path on the host side.
    InterlockedExchange(headPtr, tailBefore);
    MemoryBarrier();
    return true;
}

static char FinalizeGracefulQuitTeardown(
    const char* phaseTag,
    LONG quitHeadBefore,
    LONG quitTailBefore,
    const char* originTag)
{
    const int deadRole = g_localRoleFlag;
    const DWORD deadPid = g_revivalProcessId;
    LogSessionDiagnosticState("TickHook_gracefulQuit_finalize");
    LogRevival102jDeepSnapshot("FrameHookGracefulQuit.finalize");
    mod::Log(
        "TICK_HOOK: graceful-quit teardown begin origin=%s phase=%s role=%d pid=%lu quitHead=%ld quitTail=%ld",
        originTag != nullptr ? originTag : "immediate",
        phaseTag != nullptr ? phaseTag : "POST-TICK",
        deadRole,
        static_cast<unsigned long>(deadPid),
        static_cast<long>(quitHeadBefore),
        static_cast<long>(quitTailBefore));

    const uint8_t teardownScreen = ReadCurrentScreenIndexForRecovery();
    if (ShouldSuppressOldGameplayExitTeardown("quit_ring_teardown"))
    {
        LogSessionDiagnosticState("TickHook_gracefulQuit_suppressed");
        return 0;
    }

    if (IsGameplayExitRecoveryScreen(teardownScreen))
    {
        (void)netplay::bridge::recovery::BeginGameplayExitRecovery(
            netplay::bridge::recovery::GameplayExitOrigin::QuitRing);
        LogSessionDiagnosticState("TickHook_gracefulQuit_shared_exit");
        return 0;
    }

    NeutralizeRevivalSessionVtable();

    const bool initOk = ForceLocalPlayInit();
    mod::Log(
        "TICK_HOOK: graceful-quit step 1 ForceLocalPlayInit result=%d",
        initOk ? 1 : 0);

    if (g_revivalProcess != nullptr)
    {
        const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
        const DWORD termErr = termOk ? 0 : GetLastError();
        CloseHandle(g_revivalProcess);
        g_revivalProcess = nullptr;
        g_revivalProcessId = 0;
        mod::Log(
            "TICK_HOOK: graceful-quit step 2 helper terminated "
            "(pid=%lu termOk=%d err=%lu)",
            static_cast<unsigned long>(deadPid),
            termOk ? 1 : 0,
            static_cast<unsigned long>(termErr));
    }

    const bool patchOk = RestoreDllExitProcessPatches();
    mod::Log(
        "TICK_HOOK: graceful-quit step 3 RestoreDllExitProcessPatches result=%d",
        patchOk ? 1 : 0);

    const bool textOk = ResetRevivalTextRenderingAfterCleanup(
        "quit_ring_graceful_teardown");
    mod::Log(
        "TICK_HOOK: graceful-quit step 4 ResetRevivalTextRenderingAfterCleanup result=%d",
        textOk ? 1 : 0);

    mod::ResetCrashRecoveryState();
    ResetGameModeValidation();
    mod::Log("TICK_HOOK: graceful-quit step 5 crash/validation state reset");

    const bool modeOk = ForceGameModeToTitle();
    mod::Log(
        "TICK_HOOK: graceful-quit step 6 ForceGameModeToTitle result=%d",
        modeOk ? 1 : 0);

    g_localInitAppliedForSession = false;
    mod::Log(
        "TICK_HOOK: graceful-quit recovery complete (was role=%d), "
        "next title-screen frame will consume exit interception",
        deadRole);
    LogSessionDiagnosticState("TickHook_gracefulQuit_exit");

    return 0;
}

static char RecoverFromQuitRingSignal(const char* phaseTag, LONG quitHeadBefore, LONG quitTailBefore)
{
    const int deadRole = g_localRoleFlag;
    const DWORD deadPid = g_revivalProcessId;
    const uint8_t quitScreen = ReadCurrentScreenIndexForRecovery();
    LogSessionDiagnosticState("TickHook_gracefulQuit_entry");
    LogRevival102jDeepSnapshot("FrameHookGracefulQuit.entry");
    mod::Log(
        "TICK_HOOK: *** %s GRACEFUL SESSION END *** frameTick=%u "
        "role=%d pid=%lu quitHead=%ld quitTail=%ld - synthesizing exit interception",
        phaseTag != nullptr ? phaseTag : "POST-TICK",
        g_frameTick,
        deadRole,
        static_cast<unsigned long>(deadPid),
        static_cast<long>(quitHeadBefore),
        static_cast<long>(quitTailBefore));

    if (!IsGameplayExitRecoveryScreen(quitScreen))
    {
        InterlockedExchange(&g_revivalExitMode, static_cast<LONG>(g_localRoleFlag));
        InterlockedExchange(&g_revivalExitIntercepted, 1);
    }

    if (deadRole == kLocalRoleOnline)
    {
        // Revival can publish the Quit-ring signal before our later
        // ExitProcess interception path sees the local ESC. If we convert the
        // session back to local play and tear the helper down first, the peer
        // never receives the native MessageQuit packet and has to wait for the
        // network timeout instead. Request that packet here, then defer the
        // actual local teardown to a later frame so the helper gets a grace
        // window to finish its native quit path before we destroy it.
        const bool peerQuitSent =
            RequestInjectedPeerQuitBroadcast("quit_ring_pre_teardown", 300u);
        NeutralizeRevivalSessionVtable();
        if (g_revivalProcess != nullptr)
        {
            ScheduleGracefulQuitTeardown(phaseTag, quitHeadBefore, quitTailBefore);
            mod::Log(
                "TICK_HOOK: graceful-quit teardown scheduled delayMs=%lu "
                "phase=%s role=%d pid=%lu peerQuitSent=%d",
                static_cast<unsigned long>(kScheduledGracefulQuitTeardownDelayMs),
                phaseTag != nullptr ? phaseTag : "POST-TICK",
                deadRole,
                static_cast<unsigned long>(deadPid),
                peerQuitSent ? 1 : 0);
            mod::Log(
                "TICK_HOOK: graceful-quit pre-teardown peer-quit broadcast=%d "
                "phase=%s role=%d pid=%lu",
                peerQuitSent ? 1 : 0,
                phaseTag != nullptr ? phaseTag : "POST-TICK",
                deadRole,
                static_cast<unsigned long>(deadPid));
            return 0;
        }
        mod::Log(
            "TICK_HOOK: graceful-quit pre-teardown peer-quit broadcast=%d "
            "phase=%s role=%d pid=%lu",
            peerQuitSent ? 1 : 0,
            phaseTag != nullptr ? phaseTag : "POST-TICK",
            deadRole,
            static_cast<unsigned long>(deadPid));
    }

    return FinalizeGracefulQuitTeardown(
        phaseTag,
        quitHeadBefore,
        quitTailBefore,
        "immediate");
}

// Reset the per-frame validator state.  Called when a session ends so the
// next session gets fresh validation.
void ResetGameModeValidation()
{
    g_frameTick = 0;
    g_perFrameMismatchLogged = false;

    // Reset double-speed diagnostic state for the new session.
    g_lastHeartbeatTimeMs = 0;
    g_lastHeartbeatFrameTick = 0;
    g_lastToggleValue = 0xFFFFFFFFu;
    g_toggleSameCount = 0;
    g_toggleDiagLogged = false;
    g_retAddrSlotCount = 0;
    std::memset(g_retAddrSlots, 0, sizeof(g_retAddrSlots));
    ResetOnlineMatchEscGracefulQuit();
    ResetLocalBattleEscQuitRingIgnore();
    ResetScheduledGracefulQuitTeardown();
    ReleaseQuitRingHeader();
    if (g_gameplayStall.active)
    {
        GameplayStallSample sample = {};
        sample.screen = ReadCurrentScreenIndexForRecovery();
        sample.mode = ReadCurrentGameModeForRecovery();
        sample.frameTick = g_frameTick;
        sample.nowMs = GetTickCount();
        sample.role = g_localRoleFlag;
        (void)ReadGameplaySyncFrameForRecovery(ReadSessionPtrRaw(), &sample.syncFrame);
        sample.syncFrameValid = sample.syncFrame >= 0;
        ResetGameplayStallTracker("game_mode_validation_reset", sample, true);
    }
    else
    {
        g_gameplayStall = GameplayStallTrackerState();
    }

    // Increment session number and reset cross-session change-detection state.
    ++g_sessionNumber;
    g_timerBaselineCaptured = false;
    g_baselineTimerScalar   = 0.0;
    g_baselineTimerInterval = 0.0;
    g_baselineTimerPtr      = 0;
    g_initOnceGuardTracked  = false;
    g_lastInitOnceGuard     = 0;
    g_renderCtxTracked      = false;
    g_lastRenderCtxPtr      = 0;
    g_sessionPtrTracked     = false;
    g_lastSessionPtrInTick  = 0;
    g_prevFrameQpcValid     = false;
    memset(&g_prevFrameQpc, 0, sizeof(g_prevFrameQpc));
    ResetRevivalFpuSyncDiagnostics();

    // Reset FPS drop / tick cost tracking for the new session.
    g_fpsDropPrevQpcValid       = false;
    memset(&g_fpsDropPrevQpc, 0, sizeof(g_fpsDropPrevQpc));
    g_fpsDropCount              = 0;
    g_fpsDropLastLogTick        = 0;
    g_tickBudgetExceededCount   = 0;

    mod::Log("ResetGameModeValidation: session #%u starting", g_sessionNumber);

    // Clear stale deferred-cleanup flags from a previous session.
    // If g_deferredCancelCleanup persists into session 2, the first
    // frame tick would run ForceLocalPlayInit and destroy the new session.
    // If g_frameRecoveryPending persists, OurFrameDispatch would run
    // the full ExitProcess recovery path on the wrong session.
    if (g_deferredCancelCleanup)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_deferredCancelCleanup");
        (void)ClearDeferredCancelCleanupForRecovery("reset_game_mode_validation");
    }
    if (g_frameRecoveryPending)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_frameRecoveryPending");
        g_frameRecoveryPending = false;
    }
    if (g_tickRecoveryPending)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_tickRecoveryPending");
        g_tickRecoveryPending = false;
    }
    if (g_watchdogDeadFrameCount != 0)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_watchdogDeadFrameCount=%u",
                 g_watchdogDeadFrameCount);
        g_watchdogDeadFrameCount = 0;
    }
}

// OurFrameDispatch - entry point patched over sub_1006E590's prologue.
//
// On the normal path, delegates to RunFrameDispatch (→ trampoline → original).
//
// On the ExitProcess interception path (peer process died during rollback tick):
//   1. RunFrameDispatch returns with g_frameRecoveryPending = true.
//   2. Full reverse-init cleanup: reinstate local-play session, terminate the
//      dead helper process, restore DLL patches, disable text overlays, and
//      reset the VEH crash-recovery guard.
//   3. Force game mode to 0 (title screen) so HookedTitleUpdateImplBody can
//      consume the exit-interception flag and re-enter the netplay menu
//      immediately instead of waiting for the match to end naturally.
static void OurFrameDispatch()
{
    RunFrameDispatch();

    if (g_frameRecoveryPending)
    {
        g_frameRecoveryPending = false;

        // ---- Full reverse-init recovery ------------------------------------
        // The online/spectate session tick fired ExitProcess (peer died).
        // NeutralizeExitProcess has already:
        //   - captured g_revivalExitMode = g_localRoleFlag
        //   - set g_revivalExitIntercepted = 1
        //   - neutralised the session vtable (all slots → no-op stubs)
        //   - longjmp'd back here via g_netplayFrameJmpBuf
        //
        // Step 1: Reinstate a live local-play session immediately so the
        // game loop gets a valid vtable for subsequent frame dispatches.
        // --------------------------------------------------------------------
        const int recoveredRole = g_localRoleFlag;
        const DWORD recoveredPid = g_revivalProcessId;
        const uint8_t recoveryScreen = ReadCurrentScreenIndexForRecovery();
        LogSessionDiagnosticState("OurFrameDispatch_recovery_entry");
        LogRevival102jDeepSnapshot("FrameDispatchRecovery.01.entry");
        mod::Log(
            "OurFrameDispatch: ExitProcess intercepted during frame tick "
            "(role=%d pid=%lu screen=%u) - performing full cleanup",
            recoveredRole,
            static_cast<unsigned long>(recoveredPid),
            static_cast<unsigned>(recoveryScreen));

        if (ShouldSuppressOldGameplayExitTeardown("frame_exitprocess"))
        {
            LogSessionDiagnosticState("OurFrameDispatch_recovery_suppressed");
            return;
        }

        if (IsGameplayExitRecoveryScreen(recoveryScreen))
        {
            (void)netplay::bridge::recovery::BeginGameplayExitRecovery(
                netplay::bridge::recovery::GameplayExitOrigin::FrameExitProcess);
            LogSessionDiagnosticState("OurFrameDispatch_recovery_shared_exit");
            return;
        }

        // Step 1: Reinstate a live local-play session.
        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "OurFrameDispatch: step 1 ForceLocalPlayInit result=%d",
            initOk ? 1 : 0);

        // Step 2: Terminate the dead helper process and close its handle.
        // The session is already neutralised; terminating ensures OS
        // resources are released immediately.
        if (g_revivalProcess != nullptr)
        {
            const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
            const DWORD termErr = termOk ? 0 : GetLastError();
            CloseHandle(g_revivalProcess);
            g_revivalProcess = nullptr;
            g_revivalProcessId = 0;
            mod::Log(
                "OurFrameDispatch: step 2 helper process terminated "
                "(pid=%lu termOk=%d err=%lu) and handle closed",
                static_cast<unsigned long>(recoveredPid),
                termOk ? 1 : 0,
                static_cast<unsigned long>(termErr));
        }
        else
        {
            mod::Log("OurFrameDispatch: step 2 skipped (no helper process handle)");
        }

        // Step 3: Restore DLL Jcc patches that made ExitProcess call-sites
        // unreachable.  No longer needed now that the session is local-play.
        const bool patchOk = RestoreDllExitProcessPatches();
        mod::Log(
            "OurFrameDispatch: step 3 RestoreDllExitProcessPatches result=%d",
            patchOk ? 1 : 0);

        // Step 4: Reset stale text overlay state left by the online session.
        // 1.02j needs the renderer enabled again after cleanup; older builds
        // keep the historical disable.
        const bool textOk = ResetRevivalTextRenderingAfterCleanup(
            "frame_dispatch_exitprocess_recovery");
        mod::Log(
            "OurFrameDispatch: step 4 ResetRevivalTextRenderingAfterCleanup result=%d",
            textOk ? 1 : 0);

        // Step 5: Reset the one-shot VEH TOCTOU recovery guard so a
        // subsequent session can still be recovered if needed.
        mod::ResetCrashRecoveryState();
        ResetGameModeValidation();
        mod::Log("OurFrameDispatch: step 5 crash/validation state reset");

        // Step 6: Force game mode to 0 (title screen).  On the next main-
        // loop iteration, HookedTitleUpdateImplBody runs, detects
        // g_revivalExitIntercepted, calls ConsumeRevivalExitInterception,
        // and re-enters the netplay menu - skipping the rest of the match.
        const bool modeOk = ForceGameModeToTitle();
        mod::Log(
            "OurFrameDispatch: step 6 ForceGameModeToTitle result=%d",
            modeOk ? 1 : 0);

        g_localInitAppliedForSession = false;
        mod::Log(
            "OurFrameDispatch: full recovery complete (was role=%d), "
            "next title-screen frame will consume exit interception",
            recoveredRole);
        LogSessionDiagnosticState("OurFrameDispatch_recovery_exit");
        LogRevival102jDeepSnapshot("FrameDispatchRecovery.99.exit");
    }
}

bool InstallNetplayFrameHook()
{
    if (g_frameHookInstalled)
    {
        if (IsRevival102jProfile())
        {
            (void)InstallRevival102jSafeInputReadPatch(
                "InstallNetplayFrameHook_already");
        }
        return true;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("InstallNetplayFrameHook: EfzRevival.dll not loaded");
        return false;
    }

    const uintptr_t base    = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t frameHookRva = g_activeRevival->frameHookRva;
    uint8_t* const  target  = reinterpret_cast<uint8_t*>(base + frameHookRva);

    // --- Dump bytes at EXE 0x401642 for double-speed investigation ----------
    // Revival's init-time handler patches 8 bytes at 0x401642 in the EXE's
    // main loop via a REPLACEMENT hook (sub_1006FAA0).  If these bytes are
    // corrupted or restored to original, the main loop's screen update would
    // run in addition to Revival's tick → doubled game speed.
    {
        constexpr uintptr_t kHookAddr = 0x00401642;
        uint8_t hookBytes[16] = {};
        __try {
            memcpy(hookBytes, reinterpret_cast<const void*>(kHookAddr), 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(hookBytes, 0xCC, sizeof(hookBytes));
        }
        mod::Log(
            "InstallNetplayFrameHook: EXE 0x401642 bytes (16): "
            "[%02X %02X %02X %02X %02X %02X %02X %02X "
            " %02X %02X %02X %02X %02X %02X %02X %02X]",
            hookBytes[0], hookBytes[1], hookBytes[2], hookBytes[3],
            hookBytes[4], hookBytes[5], hookBytes[6], hookBytes[7],
            hookBytes[8], hookBytes[9], hookBytes[10], hookBytes[11],
            hookBytes[12], hookBytes[13], hookBytes[14], hookBytes[15]);
    }

    uint8_t framePrologue[6] = {};
    bool framePrologueRead = true;
    __try
    {
        std::memcpy(framePrologue, target, sizeof(framePrologue));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        framePrologueRead = false;
    }

    constexpr uint8_t kMsvcFrameHookPrologue[6] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0,
    };
    constexpr uint8_t kMingw102jFrameHookPrologue[6] = {
        0x55, 0x89, 0xE5, 0x57, 0x56, 0x53,
    };
    const bool msvcFrameHook =
        framePrologueRead
        && std::memcmp(framePrologue, kMsvcFrameHookPrologue, sizeof(framePrologue)) == 0;
    const bool mingw102jFrameHook =
        framePrologueRead
        && std::memcmp(framePrologue, kMingw102jFrameHookPrologue, sizeof(framePrologue)) == 0;
    const bool isRevival102j =
        g_activeRevival != nullptr
        && g_activeRevival->versionTag != nullptr
        && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0;
    const bool legacyPushEbpFallback =
        framePrologueRead && framePrologue[0] == 0x55 && !isRevival102j;
    if (!msvcFrameHook && !mingw102jFrameHook && !legacyPushEbpFallback)
    {
        mod::Log(
            "InstallNetplayFrameHook: unsupported frame-hook prologue at RVA 0x%lX "
            "[%02X %02X %02X %02X %02X %02X] - skipping",
            static_cast<unsigned long>(frameHookRva),
            framePrologue[0], framePrologue[1], framePrologue[2],
            framePrologue[3], framePrologue[4], framePrologue[5]);
        return false;
    }
    if (legacyPushEbpFallback && !msvcFrameHook)
    {
        mod::Log(
            "InstallNetplayFrameHook: legacy frame-hook fallback at RVA 0x%lX "
            "[%02X %02X %02X %02X %02X %02X]",
            static_cast<unsigned long>(frameHookRva),
            framePrologue[0], framePrologue[1], framePrologue[2],
            framePrologue[3], framePrologue[4], framePrologue[5]);
    }

    // Build trampoline: 6 original bytes + JMP-near back to original+6.
    memcpy(g_frameHookTrampoline, target, 6);
    const uintptr_t origContinue = base + frameHookRva + 6;
    g_frameHookTrampoline[6]     = 0xE9; // JMP near rel32
    const uintptr_t jmpFrom      = reinterpret_cast<uintptr_t>(&g_frameHookTrampoline[6]) + 5;
    *reinterpret_cast<int32_t*>(&g_frameHookTrampoline[7]) =
        static_cast<int32_t>(origContinue - jmpFrom);
    g_frameHookTrampoline[11] = 0x90; // padding NOP

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            g_frameHookTrampoline,
            sizeof(g_frameHookTrampoline),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        mod::Log("InstallNetplayFrameHook: VirtualProtect(trampoline) failed");
        return false;
    }

    g_origFrameDispatch = reinterpret_cast<FrameDispatchFn>(
        reinterpret_cast<void*>(g_frameHookTrampoline));

    // Patch the first 6 bytes of sub_1006E590:
    //   E9 rel32 (5-byte JMP to OurFrameDispatch) + 90 (NOP).
    uint8_t patch[6];
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(&patch[1]) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&OurFrameDispatch)
            - (reinterpret_cast<uintptr_t>(target) + 5));
    patch[5] = 0x90;

    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        mod::Log("InstallNetplayFrameHook: VirtualProtect(target) failed");
        return false;
    }
    memcpy(target, patch, 6);
    VirtualProtect(target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 6);

    g_frameHookInstalled = true;
    mod::Log(
        "InstallNetplayFrameHook: installed at DLL RVA 0x%lX, trampoline at %p",
        static_cast<unsigned long>(frameHookRva),
        static_cast<void*>(g_frameHookTrampoline));

    // -----------------------------------------------------------------------
    // Per-frame tick hook on sub_1006E570 (RVA 0x6E570).
    //
    // sub_1006E570 is the ACTUAL per-frame entry point.  sub_1006E590 (hooked
    // above) runs once during init and installs an EXE patch at 0x401642 that
    // calls sub_1006E570 every frame. We hook sub_1006E570 to install the
    // recovery boundary and run per-frame diagnostics. Verified 1.02h reloads
    // the global session itself; the ECX compatibility argument is not an RNG
    // scheduling fix.
    //
    // sub_1006E570 prologue - may start with:
    //   55                 push ebp       (MSVC frame-pointer prologue)
    //   51 E8 xx xx xx xx  push ecx; call (MSVC thiscall-saving prologue)
    //   83 EC 28 E8 ...    sub esp, 0x28; call (1.02j MinGW prologue)
    // We overwrite a complete stolen-instruction span with JMP + NOP padding.
    // -----------------------------------------------------------------------
    if (!g_perFrameTickInstalled)
    {
        const uintptr_t perFrameTickRva = g_activeRevival->perFrameTickRva;
        uint8_t* const tickTarget =
            reinterpret_cast<uint8_t*>(base + perFrameTickRva);

        uint8_t tickPrologue[8] = {};
        bool tickPrologueRead = true;
        __try
        {
            std::memcpy(tickPrologue, tickTarget, sizeof(tickPrologue));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tickPrologueRead = false;
        }

        size_t tickStealSize = 0;
        size_t tickRelInsnOffset = static_cast<size_t>(-1);
        if (tickPrologueRead && (tickPrologue[0] == 0x55 || tickPrologue[0] == 0x51))
        {
            tickStealSize = 6;
            if (tickPrologue[1] == 0xE8 || tickPrologue[1] == 0xE9)
            {
                tickRelInsnOffset = 1;
            }
        }
        else if (tickPrologueRead
            && tickPrologue[0] == 0x83
            && tickPrologue[1] == 0xEC
            && tickPrologue[2] == 0x28
            && (tickPrologue[3] == 0xE8 || tickPrologue[3] == 0xE9))
        {
            tickStealSize = 8;
            tickRelInsnOffset = 3;
        }

        if (tickStealSize == 0)
        {
            mod::Log(
                "InstallNetplayFrameHook: per-frame tick unexpected byte "
                "0x%02X at RVA 0x%lX - skipping",
                static_cast<unsigned>(tickPrologueRead ? tickPrologue[0] : 0xFF),
                static_cast<unsigned long>(perFrameTickRva));
        }
        else
        {
            // Build trampoline: stolen original bytes + JMP-near back.
            //
            // IMPORTANT: some stolen spans contain a relative E8/E9. A raw
            // memcpy would preserve a displacement that was correct at the
            // original address but wrong at the trampoline address.
            std::memset(g_perFrameTickTrampoline, 0x90, sizeof(g_perFrameTickTrampoline));
            memcpy(g_perFrameTickTrampoline, tickTarget, tickStealSize);

            if (tickRelInsnOffset != static_cast<size_t>(-1))
            {
                // Original call/jmp displacement (little-endian int32).
                int32_t origDisp = 0;
                memcpy(&origDisp, tickTarget + tickRelInsnOffset + 1, sizeof(origDisp));

                // Absolute target the original instruction reached.
                const uintptr_t origInsnAddr =
                    reinterpret_cast<uintptr_t>(tickTarget) + tickRelInsnOffset;
                const uintptr_t absTarget = origInsnAddr + 5 + origDisp;

                // New displacement from trampoline location.
                const uintptr_t newInsnAddr =
                    reinterpret_cast<uintptr_t>(&g_perFrameTickTrampoline[tickRelInsnOffset]);
                const int32_t newDisp =
                    static_cast<int32_t>(absTarget - (newInsnAddr + 5));
                memcpy(&g_perFrameTickTrampoline[tickRelInsnOffset + 1], &newDisp, sizeof(newDisp));

                mod::Log(
                    "InstallNetplayFrameHook: fixed up trampoline E8/E9 at "
                    "+%lu: origDisp=0x%08X newDisp=0x%08X absTarget=%p",
                    static_cast<unsigned long>(tickRelInsnOffset),
                    static_cast<unsigned>(origDisp),
                    static_cast<unsigned>(newDisp),
                    reinterpret_cast<void*>(absTarget));
            }

            const uintptr_t tickContinue = base + perFrameTickRva + tickStealSize;
            g_perFrameTickTrampoline[tickStealSize] = 0xE9;
            const uintptr_t tickJmpFrom =
                reinterpret_cast<uintptr_t>(&g_perFrameTickTrampoline[tickStealSize]) + 5;
            *reinterpret_cast<int32_t*>(&g_perFrameTickTrampoline[tickStealSize + 1]) =
                static_cast<int32_t>(tickContinue - tickJmpFrom);

            DWORD tickTrampolineProtect = 0;
            if (!VirtualProtect(
                    g_perFrameTickTrampoline,
                    sizeof(g_perFrameTickTrampoline),
                    PAGE_EXECUTE_READWRITE,
                    &tickTrampolineProtect))
            {
                mod::Log(
                    "InstallNetplayFrameHook: VirtualProtect(tick trampoline) failed");
            }
            else
            {
                g_origPerFrameTick = reinterpret_cast<PerFrameTickFn>(
                    reinterpret_cast<void*>(g_perFrameTickTrampoline));

                // Patch sub_1006E570 prologue: JMP to OurPerFrameTickHook.
                uint8_t tickPatch[8] = {};
                std::memset(tickPatch, 0x90, sizeof(tickPatch));
                tickPatch[0] = 0xE9;
                *reinterpret_cast<int32_t*>(&tickPatch[1]) =
                    static_cast<int32_t>(
                        reinterpret_cast<uintptr_t>(&OurPerFrameTickHook)
                        - (reinterpret_cast<uintptr_t>(tickTarget) + 5));

                DWORD tickTargetProtect = 0;
                if (!VirtualProtect(
                        tickTarget, tickStealSize, PAGE_EXECUTE_READWRITE, &tickTargetProtect))
                {
                    mod::Log(
                        "InstallNetplayFrameHook: VirtualProtect(tick target) failed");
                }
                else
                {
                    memcpy(tickTarget, tickPatch, tickStealSize);
                    VirtualProtect(tickTarget, tickStealSize, tickTargetProtect, &tickTargetProtect);
                    FlushInstructionCache(GetCurrentProcess(), tickTarget, tickStealSize);

                    g_perFrameTickInstalled = true;
                    mod::Log(
                        "InstallNetplayFrameHook: per-frame tick hook installed "
                        "at DLL RVA 0x%lX steal=%lu trampoline at %p",
                        static_cast<unsigned long>(perFrameTickRva),
                        static_cast<unsigned long>(tickStealSize),
                        static_cast<void*>(g_perFrameTickTrampoline));
                }
            }
        }
    }

    if (IsRevival102jProfile())
    {
        (void)InstallRevival102jSafeInputReadPatch("InstallNetplayFrameHook");
    }

    return true;
}

// ---------------------------------------------------------------------------
// ForceGameModeToTitle - write 0 to the EFZ.exe game-mode index so the
// next main-loop iteration dispatches to the title-screen update, where
// HookedTitleUpdateImplBody can run ConsumeRevivalExitInterception and
// re-enter the netplay menu.
//
// Deliberately minimal: called from the VEH crash handler where complex
// operations (allocations, locks, deep call chains) are unsafe.
// ---------------------------------------------------------------------------
bool ForceGameModeToTitle()
{
    if (g_activeRevival == nullptr || g_activeRevival->addrGameModeCurrentIndex == 0)
    {
        mod::Log("ForceGameModeToTitle: no active Revival profile or game mode address");
        return false;
    }

    int currentGameMode = -1;
    if (!SafeReadInt(
            reinterpret_cast<const void*>(g_activeRevival->addrGameModeCurrentIndex),
            &currentGameMode))
    {
        mod::Log(
            "ForceGameModeToTitle: failed to read game mode at 0x%08lX",
            static_cast<unsigned long>(g_activeRevival->addrGameModeCurrentIndex));
        return false;
    }

    if (currentGameMode == 0)
    {
        mod::Log("ForceGameModeToTitle: already on title screen (mode=0), no-op");
        return true;
    }

    auto* const modePtr = reinterpret_cast<int*>(
        g_activeRevival->addrGameModeCurrentIndex);

    DWORD oldProtect = 0;
    if (!VirtualProtect(modePtr, sizeof(int), PAGE_READWRITE, &oldProtect))
    {
        mod::Log(
            "ForceGameModeToTitle: VirtualProtect failed addr=0x%08lX err=%lu",
            static_cast<unsigned long>(g_activeRevival->addrGameModeCurrentIndex),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    *modePtr = 0;
    DWORD ignored = 0;
    (void)VirtualProtect(modePtr, sizeof(int), oldProtect, &ignored);

    mod::Log(
        "ForceGameModeToTitle: game mode %d -> 0 (title screen)",
        currentGameMode);

    // -----------------------------------------------------------------------
    // Battle resource cleanup - replicate the EFZ battle screen's exit
    // cleanup that we bypass when force-transitioning mid-match.
    //
    // Verified against the EXE's own replay exit path in
    // updateBattleScreenLogic (0x763C20), byte[45]==2 replay branch:
    //   1. safelyCloseFileHandle(gameSys+82564) - close replay file
    //   2. gameSys+82563 = 0                   - clear replay I/O state
    //   3. stopBackgroundMusic(gameSys)         - stop battle BGM
    //   4. cleanupPlayerObject(P1, 1)           - release surfaces/sounds/free
    //   5. null P1 slot
    //   6. cleanupPlayerObject(P2, 1)
    //   7. null P2 slot
    //   8. free(gameSys+4988)                   - free animated stage-bg
    //   9. gameSys+4988 = 0
    //
    // We additionally reset:
    //   - battleObj+0x578 (game speed) back to 3 (default)
    //   - gameSys+82540 (fade controller byte) to 0
    //   - gameSys+82556 (speed override flag) to 0
    //   - battleObj byte[44]=1, byte[45]=0 (screen reinit flags)
    //
    // All addresses verified via capstone disassembly of efz.exe.
    // -----------------------------------------------------------------------
    __try
    {
        if (currentGameMode == 2  // loading screen
            || currentGameMode == 3  // battle screen
            || currentGameMode == 5) // results screen
        {
            constexpr uintptr_t kScreenTable   = 0x00790110;
            constexpr uintptr_t kGameSystemPtr = 0x0079010C;

            const uint32_t battleObj =
                reinterpret_cast<const uint32_t*>(kScreenTable)[3];
            const uint32_t gameSys =
                *reinterpret_cast<const uint32_t*>(kGameSystemPtr);

            if (battleObj != 0 && gameSys != 0)
            {
                // ---- Close replay file if active ---------------------------
                // safelyCloseFileHandle: __thiscall at 0x405F50.
                // Checks [this+4] for a valid handle, calls CloseHandle, nulls.
                const int8_t replayState =
                    *reinterpret_cast<const int8_t*>(gameSys + 82563);
                if (replayState > 0)
                {
                    using CloseFileFn = void*(__thiscall*)(void* fileStruct);
                    constexpr uintptr_t kCloseFileAddr = 0x00405F50;
                    auto const closeFile =
                        reinterpret_cast<CloseFileFn>(kCloseFileAddr);
                    closeFile(reinterpret_cast<void*>(gameSys + 82564));
                    *reinterpret_cast<int8_t*>(gameSys + 82563) = 0;
                    mod::Log(
                        "ForceGameModeToTitle: closed replay file "
                        "(replayState was %d)",
                        static_cast<int>(replayState));
                }

                // ---- Stop battle BGM ---------------------------------------
                // stopBackgroundMusic: __thiscall at 0x406A10.
                using StopBgmFn = int(__thiscall*)(void* gameSys);
                constexpr uintptr_t kStopBgmAddr = 0x00406A10;
                auto const stopBgm =
                    reinterpret_cast<StopBgmFn>(kStopBgmAddr);
                stopBgm(reinterpret_cast<void*>(gameSys));

                // ---- Clean up character objects via the EXE's own function --
                // cleanupPlayerObject: __thiscall at 0x401920.
                //   Calls cleanupCharacterObject (releases DD surfaces, 50
                //   sound buffers, resource arrays), then j__free(this) when
                //   freeMemory & 1.
                using CleanupPlayerFn =
                    void(__thiscall*)(void* playerObj, char freeMemory);
                constexpr uintptr_t kCleanupPlayerAddr = 0x00401920;
                auto const cleanupPlayer =
                    reinterpret_cast<CleanupPlayerFn>(kCleanupPlayerAddr);

                // battleObj+0x14 (20) = P1 slot ptr, +0x18 (24) = P2 slot ptr
                for (int pIdx = 0; pIdx < 2; ++pIdx)
                {
                    const uint32_t slotAddr =
                        *reinterpret_cast<const uint32_t*>(
                            battleObj + 0x14 + static_cast<uint32_t>(pIdx) * 4);
                    if (slotAddr != 0)
                    {
                        const uint32_t playerObj =
                            *reinterpret_cast<const uint32_t*>(slotAddr);
                        if (playerObj != 0)
                        {
                            cleanupPlayer(
                                reinterpret_cast<void*>(playerObj), 1);
                            *reinterpret_cast<uint32_t*>(slotAddr) = 0;
                            mod::Log(
                                "ForceGameModeToTitle: cleaned up P%d "
                                "obj=0x%08lX (slot=0x%08lX)",
                                pIdx + 1,
                                static_cast<unsigned long>(playerObj),
                                static_cast<unsigned long>(slotAddr));
                        }
                    }
                }

                // ---- Free the animated stage-bg handler --------------------
                // Uses j__free at 0x777E20 (the EXE's own CRT free), matching
                // the EXE's own cleanup at 0x763FE4.
                auto* const bgObjPtr =
                    reinterpret_cast<uint32_t*>(gameSys + 4988);
                if (*bgObjPtr != 0)
                {
                    using ExeFreeFn = void(__cdecl*)(void* ptr);
                    constexpr uintptr_t kExeFreeAddr = 0x00777E20;
                    auto const exeFree =
                        reinterpret_cast<ExeFreeFn>(kExeFreeAddr);
                    const uint32_t bgObj = *bgObjPtr;
                    exeFree(reinterpret_cast<void*>(bgObj));
                    *bgObjPtr = 0;
                    mod::Log(
                        "ForceGameModeToTitle: freed animated bg "
                        "obj=0x%08lX via EXE j__free",
                        static_cast<unsigned long>(bgObj));
                }

                // ---- Reset game speed to default ---------------------------
                // battleObj+0x578 (1400) = game speed (frames per update).
                // Default is 3; spectating/practice can change it.
                *reinterpret_cast<uint8_t*>(battleObj + 0x578) = 3;

                // ---- Reset fade & speed-override controllers ---------------
                // gameSys+82540 (byte): 1=lighten, -1=darken, 0=off.
                // gameSys+82556 (dword): speed override pending flag.
                *reinterpret_cast<uint8_t*>(gameSys + 82540) = 0;
                *reinterpret_cast<uint32_t*>(gameSys + 82556) = 0;

                // ---- Reset battle screen init/exit flags -------------------
                *reinterpret_cast<uint8_t*>(battleObj + 44) = 1;  // reinit
                *reinterpret_cast<uint8_t*>(battleObj + 45) = 0;  // clear exit

                mod::Log(
                    "ForceGameModeToTitle: battle resources cleaned up "
                    "(screen=%d battleObj=0x%08lX gameSys=0x%08lX)",
                    currentGameMode,
                    static_cast<unsigned long>(battleObj),
                    static_cast<unsigned long>(gameSys));

                // ---- Black out the hardware palette immediately ------------
                // Between this function returning and EnterNetplayMenu firing
                // on the next title-screen update frame, the EXE's main loop
                // renders at least one frame via HookedTitleRenderImpl.
                // Without blacking out the palette here, that frame displays
                // the stale battle back-buffer with the battle's 8-bit palette
                // → garbled blue/black/white corruption visible to the user.
                //
                // Fix: zero the title screen's software palette buffer (256
                // BGRA entries at screenObj+46) and call the EXE's own
                // setPalette to upload it to the hardware IDirectDrawPalette.
                // This makes any intermediate frame render as solid black.
                const uint32_t titleObj =
                    reinterpret_cast<const uint32_t*>(kScreenTable)[0];
                if (titleObj != 0)
                {
                    // kOffsetPalette = 46, palette is 256 * 4 = 1024 bytes
                    memset(reinterpret_cast<void*>(titleObj + 46), 0, 1024);

                    // kOffsetGraphicsContext = 0x20
                    void** gfxCtx =
                        *reinterpret_cast<void***>(titleObj + 0x20);
                    if (gfxCtx != nullptr)
                    {
                        using SetPaletteFn =
                            int(__thiscall*)(void** ctx, int paletteData);
                        constexpr uintptr_t kSetPaletteAddr = 0x0040BD30;
                        auto const setPal =
                            reinterpret_cast<SetPaletteFn>(kSetPaletteAddr);
                        setPal(gfxCtx, static_cast<int>(titleObj + 46));
                        mod::Log(
                            "ForceGameModeToTitle: blacked out hardware "
                            "palette via title screen obj=0x%08lX gfx=0x%08lX",
                            static_cast<unsigned long>(titleObj),
                            reinterpret_cast<unsigned long>(gfxCtx));
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "ForceGameModeToTitle: SEH exception during battle resource "
            "cleanup (screen=%d)", currentGameMode);
    }

    // Reset the charselect screen's init/exit flags so it properly
    // re-initializes on next entry.  The DLL's init(mode=2) does NOT
    // restore these EXE-side screen flags, so without this the charselect
    // screen skips its initializeCharacterSelectScreen call and renders
    // incorrectly when the user enters any local game mode (e.g. practice)
    // after a netplay disconnect recovery.
    //
    // Screen table at 0x00790110; index 1 = charselect object.
    // Offset +44 = init-required flag (1 = re-init on next frame).
    // Offset +45 = exit flag (0 = cleared, prevents premature exit).
    __try
    {
        constexpr uintptr_t kScreenTable = 0x00790110;
        const uint32_t csObj =
            reinterpret_cast<const uint32_t*>(kScreenTable)[1];
        if (csObj != 0)
        {
            const uint8_t oldInit = *reinterpret_cast<const uint8_t*>(csObj + 44);
            const uint8_t oldExit = *reinterpret_cast<const uint8_t*>(csObj + 45);
            *reinterpret_cast<uint8_t*>(csObj + 44) = 1;  // init required
            *reinterpret_cast<uint8_t*>(csObj + 45) = 0;  // exit cleared
            mod::Log(
                "ForceGameModeToTitle: charselect screen reset "
                "init %u->1 exit %u->0 (obj=0x%08lX)",
                static_cast<unsigned>(oldInit),
                static_cast<unsigned>(oldExit),
                static_cast<unsigned long>(csObj));
        }

        // Also reset the title screen's init byte so the original EFZ
        // update handler (case 1) re-applies setPalette on re-entry.
        // Without this, stale init state from the pre-match title screen
        // can cause palette mismatches after disconnect recovery.
        const uint32_t titleObj =
            reinterpret_cast<const uint32_t*>(kScreenTable)[0];
        if (titleObj != 0)
        {
            const uint8_t oldTitleInit = *reinterpret_cast<const uint8_t*>(titleObj + 44);
            *reinterpret_cast<uint8_t*>(titleObj + 44) = 1;  // init required
            *reinterpret_cast<uint8_t*>(titleObj + 45) = 0;  // exit cleared
            mod::Log(
                "ForceGameModeToTitle: title screen reset "
                "init %u->1 (obj=0x%08lX)",
                static_cast<unsigned>(oldTitleInit),
                static_cast<unsigned long>(titleObj));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("ForceGameModeToTitle: SEH exception resetting screen flags");
    }

    // Reset game mode bytes (gameSystem + 4964/4965) from battle values
    // (e.g. mode=5, secondary=1) back to safe defaults.  Without this,
    // EFZ subsystems that read the game mode during title-screen updates
    // may behave incorrectly (e.g. the charselect intro sequence, BGM
    // selection, or input routing).
    __try
    {
        constexpr uintptr_t kGameSystemPtr = 0x0079010C;
        const uint32_t gameSys =
            *reinterpret_cast<const uint32_t*>(kGameSystemPtr);
        if (gameSys != 0)
        {
            auto* primaryMode = reinterpret_cast<uint8_t*>(gameSys + 4964);
            auto* secondaryMode = reinterpret_cast<uint8_t*>(gameSys + 4965);
            const uint8_t oldPrimary = *primaryMode;
            const uint8_t oldSecondary = *secondaryMode;
            *primaryMode = 0;    // reset to Arcade/default
            *secondaryMode = 0;  // reset secondary
            mod::Log(
                "ForceGameModeToTitle: game mode reset "
                "primary %u->0 secondary %u->0 (gameSys=0x%08lX)",
                static_cast<unsigned>(oldPrimary),
                static_cast<unsigned>(oldSecondary),
                static_cast<unsigned long>(gameSys));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("ForceGameModeToTitle: SEH exception resetting game mode bytes");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Diagnostic logging - session lifecycle state dump
// ---------------------------------------------------------------------------

static int g_forceLocalPlayInitCount = 0;

void IncrementForceLocalPlayInitCount()
{
    ++g_forceLocalPlayInitCount;
}

int GetForceLocalPlayInitCount()
{
    return g_forceLocalPlayInitCount;
}

void ResetForceLocalPlayInitCount()
{
    g_forceLocalPlayInitCount = 0;
}

static void LogSessionDiagnosticStateImpl(const char* context, bool force)
{
    if (!force && !DetailedDiagnosticReportsEnabled())
    {
        return;
    }

    const char* ctx = (context != nullptr) ? context : "unknown";

    // --- DLL patch state ---
    mod::Log(
        "DIAG[%s]: S#%u dllExitPatchesSaved=%d tournamentPatchesSaved=%d "
        "renderContextSaved=%d savedRenderCtx=0x%08lX",
        ctx,
        g_sessionNumber,
        g_dllExitProcessPatchesSaved ? 1 : 0,
        g_tournamentPatchesSaved ? 1 : 0,
        g_renderContextSaved ? 1 : 0,
        static_cast<unsigned long>(g_savedRenderContext));

    // --- Revival DLL globals ---
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr && g_activeRevival != nullptr)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(revival);

        // Role flag
        int roleFlag = -1;
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->roleFlagOffsets[0]),
            &roleFlag);

        // Session pointer
        uintptr_t sessionPtr = 0;
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[0]),
            &sessionPtr);

        // Render context
        uintptr_t renderCtx = 0;
        if (g_activeRevival->renderContextGlobalOffset != 0)
        {
            (void)SafeReadPtr(
                reinterpret_cast<const void*>(base + g_activeRevival->renderContextGlobalOffset),
                &renderCtx);
        }

        // Global state
        uintptr_t globalState = 0;
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->globalStatePtrOffset),
            &globalState);

        mod::Log(
            "DIAG[%s]: dllBase=0x%08lX roleFlag=%d sessionPtr=0x%08lX "
            "renderCtx=0x%08lX globalState=0x%08lX",
            ctx,
            static_cast<unsigned long>(base),
            roleFlag,
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(renderCtx),
            static_cast<unsigned long>(globalState));

        // Init-once guard, timer pointer, timer scalar - persistent across sessions
        uint16_t initOnceGuard = 0;
        uintptr_t timerPtr = 0;
        int initFlag = -1;
        double timerScalar = 0.0, timerInterval = 0.0;

        (void)SafeReadWord(
            reinterpret_cast<const void*>(base + g_activeRevival->initOnceGuardOffset),
            &initOnceGuard);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->timerPtrOffset),
            &timerPtr);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initFlagOffset),
            &initFlag);
        if (timerPtr != 0)
        {
            (void)SafeReadDouble(reinterpret_cast<const void*>(timerPtr + 16), &timerInterval);
            (void)SafeReadDouble(reinterpret_cast<const void*>(timerPtr + 32), &timerScalar);
        }
        mod::Log(
            "DIAG[%s]: initOnceGuard=0x%04X initFlag=%d timerPtr=0x%08lX "
            "timerScalar=%.4f timerInterval=%.4f",
            ctx,
            static_cast<unsigned>(initOnceGuard), initFlag,
            static_cast<unsigned long>(timerPtr),
            timerScalar, timerInterval);

        // Session vtable validation
        if (sessionPtr != 0 && sessionPtr >= 0x00100000u)
        {
            uintptr_t vtable = 0;
            int initComplete = -1;
            int activePlayer = -1;
            (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable);
            if (g_activeRevival->sessionOffsetInitComplete != 0)
            {
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInitComplete),
                    &initComplete);
            }
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
                &activePlayer);

            mod::Log(
                "DIAG[%s]: session vtable=0x%08lX vtableRVA=0x%lX initComplete=%d activePlayer=%d",
                ctx,
                static_cast<unsigned long>(vtable),
                (base != 0 && vtable >= base) ? static_cast<unsigned long>(vtable - base) : 0UL,
                initComplete,
                activePlayer);

            // Game mode fields and sentinel
            int prevGameMode = -1, curGameMode = -1, matchStartFrame = -1;
            int advanceCounter = -1, syncFrameCounter = -1;
            uint32_t sentinelVal = 0;
            const uintptr_t gmBase = sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot;
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase), &prevGameMode);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 4), &curGameMode);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 8), &matchStartFrame);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 12), &advanceCounter);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), &syncFrameCounter);
            (void)SafeReadDword(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetSentinel),
                &sentinelVal);

            mod::Log(
                "DIAG[%s]: prevMode=%d curMode=%d matchStart=%d advCtr=%d syncFrame=%d "
                "sentinel=0x%08lX",
                ctx,
                prevGameMode, curGameMode, matchStartFrame,
                advanceCounter, syncFrameCounter,
                static_cast<unsigned long>(sentinelVal));

            // Ping struct + session fields relevant to double-speed bug.
            int inputDelay = -1, queuePlayer = -1, currentFrame = -1, matchId = -1;
            uint32_t pingStruct[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay),
                &inputDelay);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer),
                &queuePlayer);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
                &currentFrame);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetMatchId),
                &matchId);
            const uintptr_t pingBase = sessionPtr + g_activeRevival->sessionOffsetPingStructBase;
            for (int pi = 0; pi < 4; ++pi)
            {
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(pingBase + pi * 4),
                    reinterpret_cast<int*>(&pingStruct[pi]));
            }
            mod::Log(
                "DIAG[%s]: session frame=%d matchId=%d inputDelay=%d queuePlayer=%d "
                "ping[0]=%u ping[1]=%u ping[2]=%u ping[3]=%u",
                ctx,
                currentFrame, matchId, inputDelay, queuePlayer,
                pingStruct[0], pingStruct[1], pingStruct[2], pingStruct[3]);

            if (g_activeRevival->sessionOffsetHistoryPrimaryVec != 0
                && g_activeRevival->sessionOffsetHistorySecondaryVec != 0)
            {
                const TwoByteVectorSnapshot localInputs =
                    ReadTwoByteVectorSnapshot(
                        sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryVec);
                const TwoByteVectorSnapshot remoteInputs =
                    ReadTwoByteVectorSnapshot(
                        sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryVec);
                TwoByteVectorSnapshot predictedInputs = {};
                predictedInputs.length = -1;
                if (IsRevival102jProfile())
                {
                    predictedInputs = ReadTwoByteVectorSnapshot(
                        sessionPtr + kRevival102jOffsetPredictedInputs);
                }

                mod::Log(
                    "DIAG[%s]: inputVec local=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d "
                    "remote=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d "
                    "predicted=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d",
                    ctx,
                    static_cast<unsigned long>(localInputs.begin),
                    static_cast<unsigned long>(localInputs.end),
                    static_cast<unsigned long>(localInputs.capacity),
                    localInputs.length,
                    localInputs.valid ? 1 : 0,
                    static_cast<unsigned long>(remoteInputs.begin),
                    static_cast<unsigned long>(remoteInputs.end),
                    static_cast<unsigned long>(remoteInputs.capacity),
                    remoteInputs.length,
                    remoteInputs.valid ? 1 : 0,
                    static_cast<unsigned long>(predictedInputs.begin),
                    static_cast<unsigned long>(predictedInputs.end),
                    static_cast<unsigned long>(predictedInputs.capacity),
                    predictedInputs.length,
                    predictedInputs.valid ? 1 : 0);
            }
        }

        // ExitProcess patch site byte values (verify actual DLL code state)
        for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
        {
            const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
            if (rva == 0) continue;
            uint8_t currentByte = 0;
            (void)SafeReadByte(reinterpret_cast<const void*>(base + rva), &currentByte);
            mod::Log(
                "DIAG[%s]: exitPatch[%zu] RVA=0x%lX byte=0x%02X expected=0x%02X",
                ctx,
                i,
                static_cast<unsigned long>(rva),
                static_cast<unsigned>(currentByte),
                static_cast<unsigned>(g_activeRevival->exitProcessPatchOriginal[i]));
        }
        for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
        {
            const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
            if (rva == 0) continue;
            uint8_t b0 = 0, b1 = 0;
            (void)SafeReadByte(reinterpret_cast<const void*>(base + rva), &b0);
            (void)SafeReadByte(reinterpret_cast<const void*>(base + rva + 1), &b1);
            mod::Log(
                "DIAG[%s]: nearJccPatch[%zu] RVA=0x%lX bytes=%02X %02X",
                ctx,
                i,
                static_cast<unsigned long>(rva),
                static_cast<unsigned>(b0),
                static_cast<unsigned>(b1));
        }
    }
    else
    {
        mod::Log(
            "DIAG[%s]: EfzRevival.dll not loaded or no active profile",
            ctx);
    }

    // --- Takeover state ---
    mod::Log(
        "DIAG[%s]: localRoleFlag=%d localInitApplied=%d "
        "exitIntercepted=%ld exitMode=%ld frameHookInstalled=%d "
        "frameRecoveryPending=%d forceLocalPlayInitCount=%d",
        ctx,
        g_localRoleFlag,
        g_localInitAppliedForSession ? 1 : 0,
        static_cast<long>(InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0)),
        static_cast<long>(InterlockedCompareExchange(&g_revivalExitMode, 0, 0)),
        g_frameHookInstalled ? 1 : 0,
        g_frameRecoveryPending ? 1 : 0,
        g_forceLocalPlayInitCount);

    // --- Process state ---
    mod::Log(
        "DIAG[%s]: revivalProcess=0x%p revivalPid=%lu lastValidatedSession=0x%08lX "
        "lastSessionPtrOffset=0x%lX",
        ctx,
        static_cast<void*>(g_revivalProcess),
        static_cast<unsigned long>(g_revivalProcessId),
        static_cast<unsigned long>(g_lastValidatedSessionPtr),
        static_cast<unsigned long>(g_lastSessionPtrOffset));
}

void LogSessionDiagnosticState(const char* context)
{
    LogSessionDiagnosticStateImpl(context, false);
}

void LogSessionDiagnosticStateForced(const char* context)
{
    LogSessionDiagnosticStateImpl(context, true);
}

// ---------------------------------------------------------------------------
// LogInitWriteSnapshot - comprehensive snapshot of every value init() writes.
//
// Captures ALL DLL globals and session object fields that init() modifies,
// in a format designed for before/after diff comparison.  Call this
// immediately before AND after every g_localInitFn() call to produce a
// complete write trace.  This covers all 5 crash hypotheses:
//
//   H1 (double init leak):  sessionPtr changes → old object leaked
//   H2 (stale render ctx):  initOnceGuard, renderCtx, renderCtxBase
//   H3 (patch state machine): exitPatch bytes (already in LogSessionDiagnosticState)
//   H4 (saved flag stale):  g_dllExitProcessPatchesSaved
//   H5 (history binding):   historyPrimary/SecondaryPtr, activePlayer
// ---------------------------------------------------------------------------
void LogInitWriteSnapshot(const char* context)
{
    if (!DetailedDiagnosticReportsEnabled())
    {
        return;
    }

    const char* ctx = (context != nullptr) ? context : "unknown";

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr || g_activeRevival == nullptr)
    {
        mod::Log("INIT_SNAP[%s]: EfzRevival.dll not loaded or no profile", ctx);
        return;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);

    // ---- DLL globals that init() writes directly ----

    // Role flag (dword_100A05D0)
    int roleFlag = -1;
    (void)SafeReadInt(
        reinterpret_cast<const void*>(base + g_activeRevival->roleFlagOffsets[0]),
        &roleFlag);

    // Session pointer (dword_100A02CC)
    uintptr_t sessionPtr = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[0]),
        &sessionPtr);

    // Init flag (dword_100A05D4) - zeroed at top of init()
    int initFlag = -1;
    if (g_activeRevival->initFlagOffset != 0)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initFlagOffset),
            &initFlag);
    }

    // Init byte (byte_100A0289) - zeroed after version check
    uint8_t initByte = 0xFF;
    if (g_activeRevival->initByteOffset != 0)
    {
        (void)SafeReadByte(
            reinterpret_cast<const void*>(base + g_activeRevival->initByteOffset),
            &initByte);
    }

    // Render context (dword_100A0778 = EfzRender*)
    uintptr_t renderCtx = 0;
    if (g_activeRevival->renderContextGlobalOffset != 0)
    {
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->renderContextGlobalOffset),
            &renderCtx);
    }

    // Render context base (dword_100A0760)
    uintptr_t renderCtxBase = 0;
    if (g_activeRevival->renderContextBaseOffset != 0)
    {
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->renderContextBaseOffset),
            &renderCtxBase);
    }

    // Init-once guard (word_100A0774) - critical for H2
    int initOnceGuard = -1;
    if (g_activeRevival->initOnceGuardOffset != 0)
    {
        // Read as int (the low byte is the guard; full word gives more context)
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initOnceGuardOffset),
            &initOnceGuard);
    }

    // Timer pointer (dword_100A0764)
    uintptr_t timerPtr = 0;
    if (g_activeRevival->timerPtrOffset != 0)
    {
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->timerPtrOffset),
            &timerPtr);
    }

    // Global state pointer (dword_100A07B8)
    uintptr_t globalStatePtr = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->globalStatePtrOffset),
        &globalStatePtr);

    mod::Log(
        "INIT_SNAP[%s]: S#%u roleFlag=%d sessionPtr=0x%08lX initFlag=%d initByte=0x%02X "
        "renderCtx=0x%08lX renderCtxBase=0x%08lX initOnceGuard=0x%08X "
        "timerPtr=0x%08lX globalStatePtr=0x%08lX",
        ctx,
        g_sessionNumber,
        roleFlag,
        static_cast<unsigned long>(sessionPtr),
        initFlag,
        static_cast<unsigned>(initByte),
        static_cast<unsigned long>(renderCtx),
        static_cast<unsigned long>(renderCtxBase),
        static_cast<unsigned>(initOnceGuard),
        static_cast<unsigned long>(timerPtr),
        static_cast<unsigned long>(globalStatePtr));

    // ---- Timer scalar/interval (critical for double-speed bug) ----
    // If init() corrupts or re-initializes the timer, this before/after
    // diff will show the change.
    if (timerPtr != 0)
    {
        double snapInterval = 0.0, snapScalar = 0.0;
        (void)SafeReadDouble(
            reinterpret_cast<const void*>(timerPtr + 16), &snapInterval);
        (void)SafeReadDouble(
            reinterpret_cast<const void*>(timerPtr + 32), &snapScalar);
        mod::Log(
            "INIT_SNAP[%s]: S#%u timerScalar=%.6f timerInterval=%.6f "
            "(expect ~64/~15.6 at 64fps)",
            ctx, g_sessionNumber, snapScalar, snapInterval);
    }

    // ---- Secondary role flag / session pointer globals (detect stale copies) ----
    for (size_t i = 1; i < g_activeRevival->roleFlagOffsetCount; ++i)
    {
        int rf = -1;
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->roleFlagOffsets[i]),
            &rf);
        mod::Log(
            "INIT_SNAP[%s]: roleFlag[%zu]=0x%lX val=%d",
            ctx, i,
            static_cast<unsigned long>(g_activeRevival->roleFlagOffsets[i]),
            rf);
    }
    for (size_t i = 1; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        uintptr_t sp = 0;
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[i]),
            &sp);
        mod::Log(
            "INIT_SNAP[%s]: sessionPtr[%zu]=0x%lX val=0x%08lX",
            ctx, i,
            static_cast<unsigned long>(g_activeRevival->sessionPtrOffsets[i]),
            static_cast<unsigned long>(sp));
    }

    // ---- Session object fields (only if session pointer is valid) ----
    if (sessionPtr != 0 && sessionPtr >= 0x00100000u)
    {
        uintptr_t vtable = 0;
        int initComplete = -1;
        int activePlayer = -1;
        int queuePlayer = -1;
        int inputDelay = -1;
        int currentFrame = -1;
        int gameModeSnapshot = -1;
        int matchId = -1;
        int sentinel = -1;
        uintptr_t helperHandle = 0;
        uintptr_t histPrimary = 0;
        uintptr_t histSecondary = 0;

        (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable);
        if (g_activeRevival->sessionOffsetInitComplete != 0)
        {
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInitComplete),
                &initComplete);
        }
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
            &activePlayer);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer),
            &queuePlayer);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay),
            &inputDelay);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
            &currentFrame);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot),
            &gameModeSnapshot);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetMatchId),
            &matchId);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetSentinel),
            &sentinel);
        // Guard helperHandle read: mode 2 sessions are only 0x2B0 (688) bytes,
        // and sessionOffsetHelperHandle (700) is out of bounds.  Only read it
        // for modes where the session object is large enough.
        const bool helperHandleInBounds =
            (g_localRoleFlag != kLocalRoleLocalPlay);
        if (helperHandleInBounds)
        {
            (void)SafeReadPtr(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle),
                &helperHandle);
        }
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryPtr),
            &histPrimary);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryPtr),
            &histSecondary);

        mod::Log(
            "INIT_SNAP[%s]: session vtable=0x%08lX initComplete=%d "
            "activePlayer=%d queuePlayer=%d inputDelay=%d",
            ctx,
            static_cast<unsigned long>(vtable),
            initComplete,
            activePlayer,
            queuePlayer,
            inputDelay);

        mod::Log(
            "INIT_SNAP[%s]: session currentFrame=%d gameModeSnap=%d "
            "matchId=%d sentinel=%d helperHandle=0x%08lX",
            ctx,
            currentFrame,
            gameModeSnapshot,
            matchId,
            sentinel,
            static_cast<unsigned long>(helperHandle));

        mod::Log(
            "INIT_SNAP[%s]: session histPrimary=0x%08lX histSecondary=0x%08lX "
            "histPrimaryVec=0x%08lX histSecondaryVec=0x%08lX",
            ctx,
            static_cast<unsigned long>(histPrimary),
            static_cast<unsigned long>(histSecondary),
            static_cast<unsigned long>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryVec),
            static_cast<unsigned long>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryVec));

        if (g_activeRevival->sessionOffsetHistoryPrimaryVec != 0
            && g_activeRevival->sessionOffsetHistorySecondaryVec != 0)
        {
            const TwoByteVectorSnapshot localInputs =
                ReadTwoByteVectorSnapshot(
                    sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryVec);
            const TwoByteVectorSnapshot remoteInputs =
                ReadTwoByteVectorSnapshot(
                    sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryVec);
            TwoByteVectorSnapshot predictedInputs = {};
            predictedInputs.length = -1;
            if (IsRevival102jProfile())
            {
                predictedInputs = ReadTwoByteVectorSnapshot(
                    sessionPtr + kRevival102jOffsetPredictedInputs);
            }

            mod::Log(
                "INIT_SNAP[%s]: inputVec local=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d "
                "remote=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d "
                "predicted=0x%08lX/0x%08lX/0x%08lX len=%d valid=%d",
                ctx,
                static_cast<unsigned long>(localInputs.begin),
                static_cast<unsigned long>(localInputs.end),
                static_cast<unsigned long>(localInputs.capacity),
                localInputs.length,
                localInputs.valid ? 1 : 0,
                static_cast<unsigned long>(remoteInputs.begin),
                static_cast<unsigned long>(remoteInputs.end),
                static_cast<unsigned long>(remoteInputs.capacity),
                remoteInputs.length,
                remoteInputs.valid ? 1 : 0,
                static_cast<unsigned long>(predictedInputs.begin),
                static_cast<unsigned long>(predictedInputs.end),
                static_cast<unsigned long>(predictedInputs.capacity),
                predictedInputs.length,
                predictedInputs.valid ? 1 : 0);
        }

        // Read first few bytes of session for mode-specific field detection.
        // Offset +4/+8/+12/+16 are zeroed by StartInitPlayer; +9 (dword) is
        // set to 1 by the local-play constructor.
        int field4 = -1, field8 = -1, field12 = -1, field16 = -1;
        int field36 = -1;  // this[9] = 1 for local play
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 4), &field4);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 8), &field8);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 12), &field12);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 16), &field16);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 36), &field36);

        mod::Log(
            "INIT_SNAP[%s]: session +4=%d +8=%d +12=%d +16=%d +36=%d",
            ctx,
            field4, field8, field12, field16, field36);
    }
    else
    {
        mod::Log("INIT_SNAP[%s]: sessionPtr=0x%08lX (invalid/NULL, no field dump)",
                 ctx, static_cast<unsigned long>(sessionPtr));
    }

    // ---- Our module state (hypothesis flags) ----
    mod::Log(
        "INIT_SNAP[%s]: g_localRoleFlag=%d g_localInitApplied=%d "
        "g_dllExitPatchesSaved=%d g_tournamentPatchesSaved=%d "
        "g_renderContextSaved=%d g_savedRenderCtx=0x%08lX "
        "g_frameHookInstalled=%d initCount=%d",
        ctx,
        g_localRoleFlag,
        g_localInitAppliedForSession ? 1 : 0,
        g_dllExitProcessPatchesSaved ? 1 : 0,
        g_tournamentPatchesSaved ? 1 : 0,
        g_renderContextSaved ? 1 : 0,
        static_cast<unsigned long>(g_savedRenderContext),
        g_frameHookInstalled ? 1 : 0,
        g_forceLocalPlayInitCount);
}

} // namespace netplay::bridge::takeover
