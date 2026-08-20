#pragma once
// Internal shared header for the revival_takeover module decomposition.
// Not part of the public API - only included by src/netplay/bridge/*.cpp files.

#include "netplay/bridge/revival_addresses.h"
#include "netplay/bridge/session_bridge.h"
#include "logger.h"

#include <csetjmp>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace netplay::bridge::takeover
{
// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint32_t kIpcMagic = 0x4E425247;
constexpr uint32_t kIpcVersion = 2;
constexpr char kSharedBlockName[] = "EFZNetbridge_Shared";
constexpr char kInitReadyEventName[] = "EFZNetbridge_InitReady";
constexpr char kConsoleReadyEventName[] = "EFZNetbridge_ConsoleReady";
constexpr DWORD kStartTimeoutMs = 15000;
constexpr DWORD kLatePatchRetryLogIntervalMs = 3000;
constexpr DWORD kPromptDelayInputWaitTimeoutMs = 30000;
constexpr DWORD kPromptSpectateConfirmWaitTimeoutMs = 30000;

// Version-specific Revival addresses and offsets live in
// revival_addresses.h (RevivalAddressProfile).  The active profile is
// selected at runtime through g_activeRevival (see global state below).

// Revival init() roleFlag semantics (from decompiled EfzRevival.dll):
// 0 = Online session (host AND join)
// 1 = Spectator session
// 2 = Local play (offline practice)
// 3 = Offline tournament
constexpr int kLocalRoleOnline = 0;
constexpr int kLocalRoleSpectate = 1;
constexpr int kLocalRoleLocalPlay = 2;
constexpr int kLocalRoleTournament = 3;

// Netplay connection role - distinguishes host from client (joiner) within
// the kLocalRoleOnline umbrella.  Tracked by the mod so we know whether
// the P1/P2 input-config swap needs to be reversed on disconnect.
constexpr int kNetplayRoleNone      = 0;  // not in a netplay session
constexpr int kNetplayRoleHost      = 1;  // hosting (P1 side)
constexpr int kNetplayRoleClient    = 2;  // joined (P2 side - inputs swapped)
constexpr int kNetplayRoleSpectator = 3;  // spectating

// ---------------------------------------------------------------------------
// Shared structures
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct SharedBlock
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t hostPid = 0;
    uint32_t hostRevivalBase = 0;
    uint32_t hostRevivalTimestamp = 0;
    volatile LONG initSerial = 0;
    int initParams[2] = {0, 0};
    volatile LONG consoleSerial = 0;
    char consoleInput[64] = {};
    volatile LONG consoleAuxSerial = 0;
    char consoleInputAux[128] = {};
    volatile LONG delayPromptSerial = 0;
    volatile LONG delayPromptServedSerial = 0;
    volatile LONG delayMetricsSerial = 0;
    int delayAveragePingMs = -1;
    int delayMinPingMs = -1;
    int delayMaxPingMs = -1;
    int delayRecommended = -1;
    int delayRangeMin = 0;
    int delayRangeMax = 20;
    volatile LONG delayInputSerial = 0;
    volatile LONG delayInputServedSerial = 0;
    int delayInputValue = -1;
    volatile LONG spectateConfirmPromptSerial = 0;
    volatile LONG spectateConfirmPromptServedSerial = 0;
    int spectateConfirmPromptKind = 0;
    volatile LONG spectateConfirmInputSerial = 0;
    volatile LONG spectateConfirmInputServedSerial = 0;
    int spectateConfirmInputValue = 0; // 0 = not set, otherwise raw Revival menu choice
    volatile LONG dbgReadConsoleHits = 0;
    volatile LONG dbgReadConsoleAutoHits = 0;
    volatile LONG dbgCreateProcessHits = 0;
    volatile LONG dbgWriteProcessHits = 0;
    volatile LONG dbgCreateRemoteThreadHits = 0;
    volatile LONG consoleErrorSerial = 0;
    char consoleErrorText[128] = {};
    volatile LONG peerQuitDiagnosticSerial = 0;
    char peerQuitDiagnosticText[8192] = {};
    // Provisional desync warning channel.  Revival's "Desync detected"
    // console line is a one-shot Sync-record inequality warning that stock
    // Revival treats as non-fatal (it keeps comparing and the match keeps
    // running).  It must NOT flow through consoleErrorSerial, which the
    // tick hook treats as an immediate disconnect.  RNG-only transients
    // (e.g. a visual particle bouncing on different prediction frames)
    // reconverge within a few frames and would otherwise tear the match
    // down.  See docs/NAYUKI_AWAKE_AIR_THROW_RNG_DESYNC.md.
    volatile LONG consoleDesyncWarnSerial = 0;
    char consoleDesyncWarnText[128] = {};
};
#pragma pack(pop)

using RevivalInitFn = int(__cdecl*)(int*);

struct FakeThreadInfo
{
    HANDLE handle = nullptr;
    DWORD exitCode = 0;
};

struct RedirectAllocationInfo
{
    uintptr_t base = 0;
    SIZE_T size = 0;
};

struct RevivalSyncFlags
{
    int gameMode = -1;
    int mode0Flag1084 = -1;
    int sessionByte = -1;
    int globalFlag4964 = -1;
    int globalFlag4965 = -1;
    bool inRollbackSyncState = false;
    bool inRollbackActiveState = false;
};

struct TempIpcContext
{
    HANDLE mapHandle = nullptr;
    SharedBlock* block = nullptr;
    HANDLE initEvent = nullptr;
    HANDLE consoleEvent = nullptr;
};

struct RuntimeReadyProbe
{
    bool nativeSyncReady = false;
    bool localInitApplied = false;
    bool delayPromptSeen = false;
    bool delayInputApplied = false;
    bool sessionPointerValid = false;
    bool helperPidMatches = false;
    bool helperHandleMatches = false;
    bool helperBindingReady = false;
    bool ready = false;
    const char* source = "none";
};

// ---------------------------------------------------------------------------
// Global state (defined in revival_takeover.cpp)
// ---------------------------------------------------------------------------

extern std::mutex g_mutex;
extern const RevivalAddressProfile* g_activeRevival;

// Runtime version detection - reads PE TimeDateStamp, sets g_activeRevival.
void DetectRevivalVersion();
void EnsureActiveRevivalProfile();
bool ActiveRevivalProfileSupportsSessionStart();

// Resolve a legacy Revival shared-memory channel name to the name used by
// the active binary.  The MinGW 1.02j build suffixes every native wire with
// "_Spec"; older MSVC builds use the unsuffixed names.
const char* RevivalWireName(const char* legacyName);

extern HMODULE g_localRevivalModule;
extern RevivalInitFn g_localInitFn;
extern HANDLE g_revivalProcess;
extern DWORD g_revivalProcessId;
extern int g_localRoleFlag;
extern int g_netplayRole;
extern uintptr_t g_hostRevivalBase;

extern HANDLE g_hostMapHandle;
extern SharedBlock* g_hostBlock;
extern HANDLE g_hostInitEvent;
extern HANDLE g_hostConsoleEvent;

extern bool g_injectedReady;
extern HANDLE g_injectedMapHandle;
extern SharedBlock* g_injectedBlock;
extern HANDLE g_injectedInitEvent;
extern HANDLE g_injectedConsoleEvent;
extern volatile LONG g_injectedLastConsoleSerialServed;
extern volatile LONG g_injectedLastConsoleAuxSerialServed;
extern volatile LONG g_injectedActiveConsoleAuxSerial;
extern volatile LONG g_injectedConsoleAuxScriptOffset;
extern volatile LONG g_injectedAutoConsoleFallbackCount;
extern volatile LONG g_injectedConsoleOutputHits;
extern volatile LONG g_injectedTerminateUnknownPidHits;
extern volatile LONG g_injectedDelayPromptSerial;
extern volatile LONG g_injectedDelayPromptServedSerial;
extern volatile LONG g_injectedConnectedFromDelayPromptSerial;
extern DWORD g_injectedDelayPromptWaitStartTick;
extern volatile LONG g_injectedSpectateConfirmPromptSerial;
extern volatile LONG g_injectedSpectateConfirmPromptServedSerial;
extern DWORD g_injectedSpectateConfirmPromptWaitStartTick;
extern volatile LONG g_injectedFingerprintReadHits;
extern uintptr_t g_injectedInitAddress;
extern bool g_injectedLazyBound;
extern volatile LONG g_injectedLazyBootstrapState;
extern volatile LONG g_remoteThreadCallIndex;
extern volatile LONG g_startAbortRequested;
extern HANDLE g_fakeProcessThreadHandle;
extern bool g_initCapturedFromWrite;
extern DWORD g_lastConnectingDiagnosticTick;
extern uintptr_t g_lastSessionPtrOffset;
extern uintptr_t g_lastValidatedSessionPtr;
extern DWORD g_lastSessionPointerMismatchTick;
extern DWORD g_lastRuntimeReadyProbeLogTick;
extern uint32_t g_lastRuntimeReadyProbeMask;
extern bool g_lastRuntimeReadyProbeMaskValid;
extern bool g_localInitAppliedForSession;
extern bool g_spectatorPostInitAttemptedForSession;
extern bool g_spectatorPostInitSucceededForSession;
extern volatile LONG g_deferredLifecycleWorkRequested;
extern volatile LONG g_deferredTitleSelection;
extern bool g_tournamentReturnCleanupPending;
extern uintptr_t g_remoteInjectedSelfBase;
extern DWORD g_lastLatePatchRetryTick;
extern DWORD g_lastLatePatchRetryLogTick;
extern DWORD g_latePatchRetryAttempts;
extern DWORD g_latePatchRetrySuccesses;
extern bool g_lastLatePatchRetryResultValid;
extern bool g_lastLatePatchRetryResult;
extern bool g_observedTakeoverCreatePath;
extern std::mutex g_fakeThreadMutex;
extern std::vector<FakeThreadInfo> g_fakeThreads;
extern std::mutex g_redirectAllocMutex;
extern std::vector<RedirectAllocationInfo> g_redirectAllocations;
extern volatile LONG g_redirectWriteBlockedHits;
extern volatile LONG g_sessionHistoryRepairHits;
extern std::mutex g_consoleLogMutex;
extern std::string g_consolePendingWriteFile;
extern std::string g_consolePendingWriteFileDisk;
extern std::string g_consolePendingWriteConsoleA;
extern std::string g_consolePendingWriteConsoleW;
extern std::string g_consolePendingWriteConsoleOutputCharacterA;
extern std::string g_consolePendingWriteConsoleOutputCharacterW;
extern std::string g_consolePendingOutputDebugStringA;
extern std::string g_consolePendingOutputDebugStringW;
extern std::unordered_map<std::string, LONG> g_diskCapturePathHits;
extern bool g_captureRevivalNativeLogsConfigured;
extern bool g_captureRevivalNativeLogs;
extern bool g_revivalErrorCodeNullGuardPatched;
extern uintptr_t g_revivalErrorCodeNullGuardPatchedBase;
extern void* g_revivalErrorCodeNullGuardStub;
extern DelayPromptMetrics g_delayPromptMetrics;
extern volatile LONG g_revivalExitIntercepted;
extern volatile LONG g_revivalExitMode;
extern bool g_nativeWorkflowLoadedSeen;
extern bool g_nativeWorkflowMatchLoopSeen;
extern bool g_nativeWorkflowTournamentSeen;
extern bool g_nativeWorkflowPeerDiedSeen;
extern bool g_nativeWorkflowHolePunchDiedSeen;
extern bool g_holePunchServerConfigLoaded;
extern std::string g_configuredHolePunchServer;

// ---------------------------------------------------------------------------
// Utility functions (console_capture.cpp)
// ---------------------------------------------------------------------------

void CopyString(char* dst, size_t dstSize, const char* src);
bool ExtractConsoleScriptLine(const char* script, LONG* inOutOffset, char* outLine, size_t outLineSize, bool* outHasMore);
bool IsLikelyTextChunk(const char* text, size_t length);
std::string TrimAscii(const std::string& text);
std::string ToLowerAscii(std::string text);
bool TryExtractDiedEndpoint(const std::string& text, std::string* outEndpoint);
std::string LoadConfiguredHolePunchServer();
bool ContainsCaseInsensitive(const std::string& text, const char* needle);
bool ParseIntAt(const std::string& text, size_t start, int* outValue, size_t* outEnd);
bool ExtractIntAfterToken(const std::string& text, const char* token, int* outValue);
bool ExtractDelayRange(const std::string& text, int* outMin, int* outMax);
void EnsureHostLogEfzIatPatched(bool verboseLogs);
DelayPromptMetrics ParseDelayPromptMetricsFromText(const std::string& text, bool* outHasMetrics);
void PublishDelayPromptMetrics(const DelayPromptMetrics& metrics, LONG serial);
bool TryGetDiskFilePathFromHandle(HANDLE hFile, std::string* outPath);
bool TryGetLogEfzDiskPath(HANDLE hFile, std::string* outPath);
void PrimeManagedLogEfzHistory();
void BeginManagedLogEfzWrite();
void EndManagedLogEfzWrite();
bool IsManagedLogEfzWriteActive();
void ResetNativeWorkflowFlags();
void NoteConsolePromptLine(const std::string& text);
std::string* SelectPendingConsoleLine(const char* sourceTag);
bool IsLikelyRevivalDiskLogPath(const std::string& path);
void LogConsoleTextChunk(const char* sourceTag, const char* text, size_t length);
void FlushPendingConsoleOutput(const char* reason);
void MaybeLogConsoleOutputChunk(HANDLE hFile, LPCVOID lpBuffer, DWORD nBytes);
void CloseMirrorLogFiles();
void MaybeLogConsoleWriteAChunk(const VOID* lpBuffer, DWORD nChars);
void MaybeLogConsoleWriteWChunk(const VOID* lpBuffer, DWORD nChars);
void MaybeLogConsoleOutputCharacterAChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord);
void MaybeLogConsoleOutputCharacterWChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord);
void MaybeLogOutputDebugStringA(LPCSTR lpOutputString);
void MaybeLogOutputDebugStringW(LPCWSTR lpOutputString);
std::string ErrorString(DWORD code);
std::string BaseLower(const std::string& path);

// ---------------------------------------------------------------------------
// Memory introspection (revival_memory.cpp)
// ---------------------------------------------------------------------------

bool IsReadableRange(const void* address, size_t size);
bool IsWritableRange(void* address, size_t size);
bool SafeReadInt(const void* address, int* outValue);
bool SafeReadPtr(const void* address, uintptr_t* outValue);
bool SafeReadByte(const void* address, uint8_t* outValue);
bool ReadModuleImageRange(HMODULE module, uintptr_t* outBase, uintptr_t* outEnd);
int ReadRoleFlagFromRevival();
bool IsSessionPointerByVtable(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd);
bool IsLikelySessionPointer(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd);
uintptr_t ReadSessionPointerFromRevival();
uintptr_t ReadSessionPointerFromRevivalLoose();
uintptr_t ReadSessionPointerForMutation(bool* outUsedCached);
bool ReadRevivalSyncFlags(RevivalSyncFlags* outFlags);
void RefreshRuntimeStatus(NetbridgeStatus* ioStatus);
bool SetLocalRoleFlag(int roleFlag, const char* reason);
bool SetRoleFlagDirect(int roleFlag, const char* reason);
bool NeutralizeTournamentAutoNav();
bool SaveTournamentExePatches();
bool RestoreTournamentExePatches();
bool SaveAndApplyDllExitProcessPatches();
bool RestoreDllExitProcessPatches();
bool AreDllExitPatchesSaved();
bool DestroyCurrentSession(const char* caller);
bool ForceLocalPlayInit();
bool InvokeSessionVtableInit(const char* caller);
bool IsRevival102jSpectatorPostInitReady(const char* caller);
bool InvokeRevival102jSpectatorPostInit(const char* caller);
bool SaveRenderContext();
bool RestoreRenderContext();
bool ClearRevivalText();
bool SetRevivalTextRenderingEnabled(bool enable, const char* reason);
bool DisableRevivalTextRendering();
bool ResetRevivalTextRenderingAfterCleanup(const char* reason);
bool ShouldRepeatPostExitTextCleanup();
bool RestoreRenderContextForGameplayExitCleanup();
void MarkRenderContextConsumedForGameplayExitCleanup();
bool ClearRevivalTextWithCurrentRenderContext();
bool SetRevivalTextRenderingEnabledWithCurrentRenderContext(bool enable, const char* reason);
bool DisableRevivalTextRenderingWithCurrentRenderContext();
bool ResetRevivalTextRenderingAfterCleanupWithCurrentRenderContext(const char* reason);
int GetRevivalGraphicsPatchState();
bool EnsureRevivalGraphicsPatchSetEnabled(const char* reason);

// Reverse the P1/P2 input-config swap that Revival applied when we joined
// as client (P2).  No-op unless g_netplayRole == kNetplayRoleClient.
bool ReverseInputSwapIfClient();

void ResetDebugCounters(SharedBlock* block);
bool InvokeStartInitPlayer(int initMode);
void StabilizeOnlineSessionBindingAfterInit(int initMode);
void RepairRollbackHistoryBindingsIfNeeded();
bool InstallRevival102jSafeInputReadPatch(const char* caller);
void MarkRevivalSyncDiagnosticsSessionStart(const char* context);
bool InstallNetplayFrameHook();
// Force the game mode index to 0 (title screen).
// Safe to call from the crash handler VEH where minimal code should run.
bool ForceGameModeToTitle();

// ExitProcess recovery temporarily replaces the live session vtable.  Keep
// and restore the original identity before destructor selection so cleanup
// can still invoke the correct version-specific deleting destructor.
bool RestoreNeutralizedSessionVtableForCleanup(
    uintptr_t sessionPtr,
    uintptr_t* outOriginalVtable);

// Save / restore the 10 bytes at EXE address 0x401582 before and after
// every g_localInitFn() call.  Prevents Revival's init() from chaining
// trampolines whose unrelocated E9 displacement causes wild-EIP crashes.
void SaveExeFrameHookBytes();
void RestoreExeFrameHookBytes();

// Save / restore the 8 bytes at EXE address 0x401642 before and after
// every g_localInitFn() call.  Prevents init() from leaking malloc'd
// trampolines and changing the JMP target between sessions.
void SaveExeDispatchHookBytes();
void RestoreExeDispatchHookBytes();
void RestoreExeDispatchHookBytesAfterSessionInit(int initMode);
bool RestoreExeDispatchHookForTitle(const char* caller);

// Save / restore the 7 bytes at EXE addresses 0x763E50 and 0x763F04
// before and after every g_localInitFn() call.  Prevents trampoline
// chain growth at the mode-constructor hook sites (same pattern as
// SaveExeFrameHookBytes for 0x401582).
void SaveModeCtorHookBytes();
void RestoreModeCtorHookBytes();
void DiscardModeCtorHookBytes();
void RestoreModeCtorOriginalBytes();

// Fix up relative instructions (E8/E9/0F 8x) inside the trampolines
// created by Revival's mode constructors at EXE addresses 0x763E50 and
// 0x763F04.  Must be called AFTER every init() to prevent wild-EIP crashes
// caused by unrelocated displacements in the copied original bytes.
void FixupModeConstructorTrampolines(const char* caller);

// Reset the g_lastFixedTrampoline[] cache so that newly allocated
// trampolines at reused heap addresses are properly fixed up.
// Called from DestroyCurrentSession when the old trampolines are freed.
void ResetModeConstructorTrampolineCache();

// Diagnostic logging for second-session crash investigation.
// Dumps all critical session lifecycle state to the log file.
void LogSessionDiagnosticState(const char* context);
void LogSessionDiagnosticStateForced(const char* context);

// Comprehensive snapshot of ALL values that init() writes to.
// Call before and after every init() invocation to capture a complete
// before/after diff for tracing corruption across sessions.
// Logs: DLL globals (role flag, session ptr, init flag, init byte,
// render context, render context base, timer ptr, init-once guard,
// global state ptr), session object fields (vtable, initComplete,
// activePlayer, queuePlayer, inputDelay, currentFrame,
// gameModeSnapshot, matchId, sentinel, helperHandle, historyPtrs),
// and our module's patch/flag state.
void LogInitWriteSnapshot(const char* context);

// 1.02j-only diagnostic layer. Every function is a no-op unless the active
// profile is 1.02j and [Others] VerboseRevival102jLifecycleLogging is enabled.
// Step logging is intentionally compact; Snapshot logging includes raw IPC,
// wire, hook, global, vtable, and complete role-specific session-object dumps.
bool IsRevival102jDeepDiagnosticsEnabled();
void LogRevival102jDeepStep(
    const char* context,
    const NetbridgeStatus* status = nullptr);
void LogRevival102jDeepSnapshot(
    const char* context,
    const NetbridgeStatus* status = nullptr);
void LogRevival102jDeepBytes(
    const char* context,
    const char* label,
    uintptr_t address,
    size_t size);

// Track ForceLocalPlayInit call count for diagnostic purposes.
void IncrementForceLocalPlayInitCount();
int GetForceLocalPlayInitCount();
void ResetForceLocalPlayInitCount();

// Reset the per-frame game mode vtable validator state so the next session
// gets fresh validation.  Call when a session starts or is cancelled.
void ResetGameModeValidation();
uint32_t GetGameplayExitRecoveryFrameTick();
bool IsGameplayExitRecoveryInsideFrameTick();
bool ClearDeferredCancelCleanupForRecovery(const char* reason = nullptr);
bool SuppressDeferredCancelCleanupAfterGameplayRecovery(const char* origin);
bool IsDeferredCancelCleanupPending();
bool IsDeferredCancelCleanupGameplaySource();
const char* CurrentDeferredCancelCleanupReason();
uint8_t CurrentDeferredCancelCleanupSourceScreen();
void NotifyLocalProcessCloseForGameplayStall();
void ClearLocalProcessCloseForGameplayStall();
// True once the user has initiated a window close (WM_CLOSE/DESTROY etc.) and
// before the next session start. Used by the ExitProcess neutralizer to tell a
// genuine quit apart from a peer-death interception.
bool IsLocalProcessCloseForGameplayStallActive();

// Returns true while the per-frame tick hook (OurPerFrameTickHook) is
// executing the original sub_1006E570.  Used by CancelSessionUnlocked to
// defer ForceLocalPlayInit (which destroys the session object) until after
// the frame tick completes - destroying it mid-tick would cause
// RollbackLoopTick to use a freed 'this' pointer.
bool IsInsideFrameTick();

// Request that ForceLocalPlayInit + associated cleanup run after the
// current frame tick completes instead of immediately.
void RequestDeferredCancelCleanup(const char* reason = nullptr);

// Arm/consume the one-shot "online match ESC should trigger a native peer
// quit broadcast" marker. The per-frame tick sets it when it detects a local
// Esc edge on the live battle screen, and ExitProcess interception consumes it
// to ask the injected helper to send MessageQuit before teardown.
void ArmOnlineMatchEscGracefulQuit();
bool ConsumeOnlineMatchEscGracefulQuit();
void ResetOnlineMatchEscGracefulQuit();
// A fresh ESC from character select is a real session exit, not the held
// battle-return ESC. Do not let the short battle Quit-ring suppression window
// consume the new menu-exit signal.
void ConfirmCharacterSelectEscToMenu();

// Ask the injected EfzRevival.exe helper to broadcast its native MessageQuit
// packet to connected peers before the host tears the helper down locally.
// This is used for the "press ESC but don't actually exit EFZ.exe" path.
bool RequestInjectedPeerQuitBroadcast(const char* reason, DWORD waitMs);
// Clear the successful-send coalescing state when a helper session closes.
void ResetInjectedPeerQuitBroadcastState();

// Helper-process entry point invoked inside EfzRevival.exe. Resolves the live
// peer manager object and calls the native "send quit to every peer" routine.
DWORD RunInjectedPeerQuitBroadcast();

// Classify a native helper quit-packet sender using Revival's own peer-role
// helpers. Returns true when classification ran successfully; the output flags
// then indicate whether the endpoint is the active remote peer and/or a
// spectator endpoint.
bool TryClassifyRevivalQuitEndpoint(
    const char* endpointText,
    bool* outIsActivePeer,
    bool* outIsSpectator);

// Advisory peer-process liveness check. No lock held; result is TOCTOU.
bool IsPeerProcessAlive();

// setjmp buffer and active flag used by the netplay frame-hook recovery
// mechanism.  Defined in revival_memory.cpp; read by iat_stubs.cpp.
extern jmp_buf       g_netplayFrameJmpBuf;
extern volatile bool g_netplayFrameJmpActive;

// Secondary setjmp buffer used by title/menu update hooks as a fallback
// recovery path when ExitProcess fires outside OurFrameDispatch.
// Defined in revival_memory.cpp; armed in HookedTitleUpdateImpl;
// read by iat_stubs.cpp.
extern jmp_buf       g_netplayUiJmpBuf;
extern volatile bool g_netplayUiJmpActive;

// ---------------------------------------------------------------------------
// IPC, config, module loading (ipc_shared.cpp)
// ---------------------------------------------------------------------------

void* EnsureRevivalErrorCodeNullGuardStub();
bool OpenTempIpcContext(TempIpcContext* ctx, bool needInitEvent, bool needConsoleEvent);
void CloseTempIpcContext(TempIpcContext* ctx);
void ClearDelayPromptState(const char* reason);
void PublishDelayPromptSerial(LONG serial);
void ReadDelayPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial);
void PublishSpectateConfirmPromptSerial(LONG serial, int promptKind);
void ReadSpectateConfirmPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial, int* outPromptKind);
void PublishConsoleError(const char* errorText);
void ReadConsoleError(LONG* outSerial, char* outText, int outTextSize);
void PublishConsoleDesyncWarning(const char* warnText);
void ReadConsoleDesyncWarning(LONG* outSerial, char* outText, int outTextSize);
void ClearPeerQuitDiagnostic();
void AppendPeerQuitDiagnostic(const char* text);
void ReadPeerQuitDiagnostic(LONG* outSerial, char* outText, int outTextSize);
HMODULE SelfModule();
std::string ModulePath(HMODULE module);
void CleanupNativeHostShadowLogDirectory(const char* reason);
bool TryReadCaptureRevivalNativeLogsConfig(bool* outEnabled, std::string* outSourceTag);
bool CaptureRevivalNativeLogsEnabled();
std::string GameDirectory();
std::wstring GameDirectoryWide();
bool TryWriteClipboardAscii(const char* text);
void SetPhase(NetbridgeStatus* status, NetbridgePhase phase, const char* error);
void CloseProcessHandle(NetbridgeStatus* status);
bool ProcessAlive(NetbridgeStatus* status);
bool IsSyncReadyForVsHuman(const NetbridgeStatus* status);
bool RequiresNativeVsHumanSyncForHandoff(const NetbridgeStatus* status);
RuntimeReadyProbe EvaluateRuntimeReadyProbe(const NetbridgeStatus* status);
uint32_t BuildRuntimeReadyProbeMask(const RuntimeReadyProbe& probe);
bool HasRuntimeReadySignal(const NetbridgeStatus* status);
bool EnsureHostIpc();
void CloseHostIpc();
bool EnsureLocalRevivalLoaded();
void ReinitLocalPlay();
// Custom exception code historically used for ExitProcess interception.
// Retained for diagnostic purposes in crash handler log output.
static constexpr DWORD kExitProcessInterceptedException = 0xE0EF0001u;

bool PatchRevivalDllExitProcess();
bool SignalGracefulQuitRing(const char* contextTag, uintptr_t callerRva);
void NeutralizeRevivalSessionVtable();
uintptr_t ResolveHostRevivalBase();
bool PatchRevivalErrorCodeNullGuard();
void PublishHostRevivalBase();
uintptr_t ResolveInjectedExpectedRevivalBase();
bool WriteIni(
    int role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni);
bool IsCurrentProcessRevival();
bool IsRunningUnderWine();
void InitializeInjected();
void ShutdownInjected();

// ---------------------------------------------------------------------------
// Process injection & IAT patching (process_inject.cpp)
// ---------------------------------------------------------------------------

struct RemoteModuleRecord
{
    uintptr_t base = 0;
    std::string moduleLower;
};

std::vector<RemoteModuleRecord> EnumerateRemoteModules(DWORD processId);
bool ReadRemoteString(HANDLE process, uintptr_t address, char* out, size_t outSize);
bool InjectSelf(HANDLE process, uintptr_t* outRemoteBase);
bool WaitForPreloadedSelf(DWORD processId, uintptr_t* outRemoteBase, DWORD timeoutMs);
bool PatchIatModule(
    HANDLE process,
    uintptr_t moduleBase,
    const char* moduleName,
    const std::unordered_map<std::string, uint32_t>& patchMap,
    int* outPatched,
    bool verboseLogs);
std::unordered_map<std::string, uint32_t> BuildPatchMap(uintptr_t remoteBase);
bool PatchIat(HANDLE process, DWORD processId, const std::unordered_map<std::string, uint32_t>& patchMap, bool verboseLogs);
// In-process IAT patching for Wine - safe to call from DllMain.
// Returns number of entries patched, or -1 on error.
int SelfPatchIat();
HANDLE CreateFakeThread(DWORD exitCode);
bool LookupFakeThread(HANDLE handle, DWORD* outExitCode);
void ClearFakeThreads();
void RegisterRedirectAllocation(void* base, SIZE_T size);
void ForgetRedirectAllocation(void* base);
bool IsWithinRedirectAllocation(const void* address, SIZE_T size);
void ClearRedirectAllocations();
bool HasInjectedContext();
void TryLazyBootstrapInjected();
bool EnsureInjectedContextFast();

// ---------------------------------------------------------------------------
// IAT stub implementations (iat_stubs.cpp)
// ---------------------------------------------------------------------------

BOOL StubCreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
BOOL StubReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead);
LPVOID StubVirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL StubVirtualFreeEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
BOOL StubWriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten);
HANDLE StubCreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
BOOL StubTerminateProcess(HANDLE hProcess, UINT uExitCode);
HANDLE StubOpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
BOOL StubReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl);
BOOL StubReadConsoleW(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl);
BOOL StubWriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL StubWriteConsoleA(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved);
BOOL StubWriteConsoleW(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved);
BOOL StubWriteConsoleOutputCharacterA(HANDLE hConsoleOutput, LPCSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten);
BOOL StubWriteConsoleOutputCharacterW(HANDLE hConsoleOutput, LPCWSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten);
VOID StubOutputDebugStringA(LPCSTR lpOutputString);
VOID StubOutputDebugStringW(LPCWSTR lpOutputString);
DWORD StubWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
BOOL StubGetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
DWORD StubResumeThread(HANDLE hThread);

} // namespace netplay::bridge::takeover
