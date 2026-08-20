#pragma once

#include <string>

namespace netplay::mod_settings
{
struct Settings
{
    std::string offlineVsHumanMode = "Tournament";
    bool writeLogFile = true;
    bool preserveModLogAcrossLaunches = false;
    bool preserveRevivalLogsAcrossLaunches = false;
    bool enableConsole = false;
    bool enableDebugMenu = false;
    bool verboseBridgePatchLogging = false;
    bool verboseSyncDiagnostics = false;
    bool verboseRevival102jLifecycleLogging = false;
    bool hideEmptySetsInBattleLog = true;
    // Experimental two-peer rollback tracer. It is deliberately opt-in:
    // it must never add full per-frame work unless the opposite peer
    // completes the matching tracer handshake.
    bool desyncDetection = false;
    // Eagerly re-enable Revival's suppressed graphics primitives after an
    // ordinary zero-frame online battle tick (remote-input starvation).
    // Stock Revival leaves them disabled until the next positive rollback
    // batch. Render-patch policy parity and the low-overhead path are the
    // production default; terminal/frontend recovery restores are unaffected. See
    // Explicit opt-in key: ExperimentalEagerZeroFrameGraphicsRestore.
    // docs/NAYUKI_AWAKE_AIR_THROW_RNG_DESYNC.md.
    bool eagerZeroFrameGraphicsRestore = false;
    // Menu text via the TTF game-RT overlay (crisp badge font) instead of
    // the 5x7 indexed-surface font, where a producer supports it (battle
    // log menu, footer tooltip). Falls back to 5x7 automatically when the
    // D3D9/ImGui overlay is unavailable.
    bool menuTtfText = true;
    // TTF face for the menu text overlay. Known values: Yu Gothic, Meiryo,
    // MS Gothic, Noto Sans JP / Noto Sans Mono (bundled mod assets),
    // Segoe UI, Arial, ITC Bolt (mod asset). Yu Gothic is the default: it
    // natively covers Latin + Cyrillic + Japanese AND ships with every
    // Windows 10/11 base install (Meiryo is an optional feature there); the
    // bundled Noto faces guarantee coverage everywhere (incl. Wine) - the
    // Microsoft faces cannot legally be redistributed with the mod.
    std::string menuTtfFontFace = "Yu Gothic";
    // Font face for the in-game hosting-overlay badge ("Hosting... Press F1...").
    // Independent of the menu face so the tip can stand out; ASCII-only text.
    std::string hostingTipFontFace = "Yu Gothic";
    // Keyboard binding (DIK_* form) for the async-hosting "return / rehost"
    // hotkey used while the hosting overlay is minimized in-game.
    std::string asyncHostReturnKey = "DIK_F1";
};

void Reload();
const Settings& Get();

bool UseTournamentModeForOfflineVsHuman();
bool IsFileLoggingEnabled();
bool PreserveModLogAcrossLaunches();
bool PreserveRevivalLogsAcrossLaunches();
bool IsConsoleEnabled();
bool IsDebugMenuEnabled();
bool IsVerboseBridgePatchLoggingEnabled();
bool IsVerboseSyncDiagnosticsEnabled();
bool IsVerboseRevival102jLifecycleLoggingEnabled();
bool AreAllVerboseLogsEnabled();
bool HideEmptySetsInBattleLogByDefault();
bool IsDesyncDetectionEnabled();
bool IsEagerZeroFrameGraphicsRestoreEnabled();
bool IsMenuTtfTextEnabled();
const std::string& MenuTtfFontFace();
const std::string& HostingTipFontFace();
// Async-hosting return/rehost hotkey as a DIK_* binding value (e.g. "DIK_F1").
const std::string& AsyncHostReturnKeyBinding();
} // namespace netplay::mod_settings
