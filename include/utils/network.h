#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include "efz_netplay_state.h"
#include "efz_revival_profile.h"

extern std::atomic<bool> isOnlineMatch;

enum class NetplayStateSource : int {
	None = 0,
	ExportSharedMemory,
	ExportDll,
	LegacyRevival
};

// Minimal EfzRevival version enum detected from the game's window title
enum class EfzRevivalVersion : int {
	Unknown = 0,
	Vanilla,     // No "-Revival-" marker or no version tag
	Revival102f, // Eternal Fighter Zero -Revival- 1.02f
	Revival102e, // Eternal Fighter Zero -Revival- 1.02e
	Revival102g, // Eternal Fighter Zero -Revival- 1.02g
	Revival102h, // Eternal Fighter Zero -Revival- 1.02h!!! (supported; shares RVAs/semantics with 1.02i where noted)
	Revival102i, // Eternal Fighter Zero -Revival- 1.02i!!! (treated like 1.02h for RVAs except where explicitly different)
	Other,       // Some other Revival build (keep persisted numeric value stable)
	Revival102j  // Eternal Fighter Zero -Revival- 1.02j (MinGW build; uses its own verified profile)
};

enum class EfzRevivalDllFlavor : int {
	Unknown = 0,
	Standard,
	Revival102fClassic,
	Revival102fSubframe
};

// Online state reported by EfzRevival.dll flag (0=netplay, 1=spectating, 2=offline)
enum class OnlineState : int {
	Netplay = 0,
	Spectating = 1,
	Offline = 2,
	Tournament = 3,
	Unknown = -1
};

struct NetplayRuntimeState {
	DWORD refreshTick;
	bool exportAvailable;
	bool sessionActive;
	bool suspendTraining;
	bool inNetplayMenu;
	bool inNetplayFlow;
	bool inNetplayCharacterSelect;
	bool inNetplayMatch;
	NetplayStateSource source;
	OnlineState legacyOnlineState;
	EFZNetplayState exportState;
};

// Compatibility wrapper: pure query only, no side effects.
bool DetectOnlineMatch();

// Detect EfzRevival version by parsing the EFZ window title. Stable Revival results are
// cached; a cached Vanilla result is rechecked if EfzRevival.dll loads later.
EfzRevivalVersion GetEfzRevivalVersion();
// Refines versions where the title string is not enough, especially split 1.02f builds.
EfzRevivalDllFlavor GetEfzRevivalDllFlavor();
bool IsEfzRevival102fSubframeBuild();
bool IsEfzRevival102fClassicBuild();
// Identifies which verified 1.02j compilation supplies the active RVAs.
EfzRevival102jBuild GetEfzRevival102jBuild();
// True only for a supported exact 1.02j PE profile. J moved critical code,
// globals, vtable slots, and Practice fields, so title detection alone is not
// sufficient permission to use its RVAs.
bool IsEfzRevival102jVerifiedBuild();
// Human-readable name for EfzRevivalVersion
const char* EfzRevivalVersionName(EfzRevivalVersion v);
const char* EfzRevivalDllFlavorName(EfzRevivalDllFlavor flavor);
const char* NetplayStateSourceName(NetplayStateSource source);
// Whether this build of the training mode supports the detected Revival version
bool IsEfzRevivalVersionSupported(EfzRevivalVersion v = (EfzRevivalVersion)0 /*use detected*/);


// Optional: read state directly from EfzRevival.dll if available
OnlineState ReadEfzRevivalOnlineState();
// Helper: get human-readable name for OnlineState
const char* OnlineStateName(OnlineState st);

void RefreshNetplayRuntimeState();
NetplayRuntimeState GetNetplayRuntimeState();
bool IsNetplaySuspendActive();
bool IsNetplaySessionActive();
bool IsNetplayMenuActive();
bool IsNetplayFlowActive();
bool IsNetplayExportAvailable();

// Reason for last online detection (best-effort; for diagnostics/logging)
std::string GetLastOnlineDetectionReason();
