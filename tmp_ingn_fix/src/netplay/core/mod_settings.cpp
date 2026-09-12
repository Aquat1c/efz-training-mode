#include "netplay/core/mod_settings.h"

#include <windows.h>

#include <cwchar>
#include <string>

namespace netplay::mod_settings
{
namespace
{
Settings g_settings = {};

std::wstring ResolveRevivalIniPathWide()
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
    {
        return L"EfzRevival.ini";
    }

    std::wstring iniPath = exePath;
    const size_t sep = iniPath.find_last_of(L"\\/");
    if (sep == std::wstring::npos)
    {
        return L"EfzRevival.ini";
    }

    iniPath.resize(sep + 1);
    iniPath += L"EfzRevival.ini";

    const DWORD exeAttrs = GetFileAttributesW(iniPath.c_str());
    if (exeAttrs != INVALID_FILE_ATTRIBUTES && (exeAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return iniPath;
    }

    const DWORD localAttrs = GetFileAttributesW(L"EfzRevival.ini");
    if (localAttrs != INVALID_FILE_ATTRIBUTES && (localAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return L"EfzRevival.ini";
    }

    return iniPath;
}

std::string ReadStringValue(
    const wchar_t* sectionName,
    const wchar_t* keyName,
    const wchar_t* defaultValue,
    const std::wstring& iniPath)
{
    wchar_t buffer[128] = {};
    GetPrivateProfileStringW(
        sectionName,
        keyName,
        defaultValue,
        buffer,
        static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])),
        iniPath.c_str());

    char utf8[256] = {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8,
        0,
        buffer,
        -1,
        utf8,
        static_cast<int>(std::size(utf8)),
        nullptr,
        nullptr);
    if (bytes <= 0)
    {
        return {};
    }

    return utf8;
}

bool ReadBoolValue(
    const wchar_t* sectionName,
    const wchar_t* keyName,
    bool defaultValue,
    const std::wstring& iniPath)
{
    return GetPrivateProfileIntW(sectionName, keyName, defaultValue ? 1 : 0, iniPath.c_str()) != 0;
}

bool TryReadBoolValue(
    const wchar_t* sectionName,
    const wchar_t* keyName,
    bool* outValue,
    const std::wstring& iniPath)
{
    if (outValue == nullptr)
    {
        return false;
    }

    wchar_t buffer[16] = {};
    static constexpr wchar_t kMissing[] = L"__missing__";
    GetPrivateProfileStringW(
        sectionName,
        keyName,
        kMissing,
        buffer,
        static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])),
        iniPath.c_str());

    if (std::wcscmp(buffer, kMissing) == 0)
    {
        return false;
    }

    *outValue = ReadBoolValue(sectionName, keyName, false, iniPath);
    return true;
}
} // namespace

void Reload()
{
    const std::wstring iniPath = ResolveRevivalIniPathWide();

    Settings loaded = {};
    loaded.offlineVsHumanMode =
        ReadStringValue(L"Others", L"OfflineVsHumanMode", L"Tournament", iniPath);
    if (loaded.offlineVsHumanMode != "VS Human")
    {
        loaded.offlineVsHumanMode = "Tournament";
    }
    loaded.writeLogFile =
        ReadBoolValue(L"Others", L"WriteLogFile", true, iniPath);
    loaded.preserveModLogAcrossLaunches =
        ReadBoolValue(L"Others", L"PreserveModLogAcrossLaunches", false, iniPath);
    if (!TryReadBoolValue(
            L"Others",
            L"PreserveRevivalLogsAcrossLaunches",
            &loaded.preserveRevivalLogsAcrossLaunches,
            iniPath))
    {
        loaded.preserveRevivalLogsAcrossLaunches =
            ReadBoolValue(L"Others", L"PreserveLogEfzAcrossLaunches", false, iniPath);
    }
    loaded.enableConsole =
        ReadBoolValue(L"Others", L"EnableConsole", false, iniPath);
    loaded.enableDebugMenu =
        ReadBoolValue(L"Others", L"EnableDebugMenu", false, iniPath);
    if (!TryReadBoolValue(
            L"Others",
            L"VerboseBridgePatchLogging",
            &loaded.verboseBridgePatchLogging,
            iniPath))
    {
        loaded.verboseBridgePatchLogging =
            ReadBoolValue(L"Others", L"EnableBridgePatchDiagnostics", false, iniPath);
    }
    if (!TryReadBoolValue(
            L"Others",
            L"VerboseSyncDiagnostics",
            &loaded.verboseSyncDiagnostics,
            iniPath))
    {
        loaded.verboseSyncDiagnostics =
            ReadBoolValue(L"Others", L"EnableSyncDiagnostics", false, iniPath);
    }
    loaded.verboseRevival102jLifecycleLogging =
        ReadBoolValue(
            L"Others",
            L"VerboseRevival102jLifecycleLogging",
            false,
            iniPath);
    loaded.hideEmptySetsInBattleLog =
        ReadBoolValue(L"Others", L"HideEmptySetsInBattleLog", true, iniPath);
    // Do not inherit former default-on experimental keys. Builds that
    // auto-created them must not silently reactivate either the old tracer or
    // the active-battle zero-frame graphics override after an upgrade.
    loaded.desyncDetection =
        ReadBoolValue(L"Others", L"ExperimentalDesyncMonitor", false, iniPath);
    loaded.eagerZeroFrameGraphicsRestore =
        ReadBoolValue(
            L"Others",
            L"ExperimentalEagerZeroFrameGraphicsRestore",
            false,
            iniPath);
    loaded.menuTtfText =
        ReadBoolValue(L"Others", L"MenuTtfText", true, iniPath);
    loaded.menuTtfFontFace =
        ReadStringValue(L"Others", L"MenuTtfFont", L"Yu Gothic", iniPath);
    if (loaded.menuTtfFontFace.empty())
    {
        loaded.menuTtfFontFace = "Yu Gothic";
    }
    loaded.hostingTipFontFace =
        ReadStringValue(L"Others", L"HostingTipFont", L"Yu Gothic", iniPath);
    if (loaded.hostingTipFontFace.empty())
    {
        loaded.hostingTipFontFace = "Yu Gothic";
    }
    loaded.asyncHostReturnKey =
        ReadStringValue(L"Others", L"AsyncHostReturnKey", L"DIK_F1", iniPath);
    if (loaded.asyncHostReturnKey.empty())
    {
        loaded.asyncHostReturnKey = "DIK_F1";
    }

    g_settings = loaded;
}

const Settings& Get()
{
    return g_settings;
}

bool UseTournamentModeForOfflineVsHuman()
{
    return g_settings.offlineVsHumanMode != "VS Human";
}

bool IsFileLoggingEnabled()
{
    return g_settings.writeLogFile;
}

bool PreserveModLogAcrossLaunches()
{
    return g_settings.preserveModLogAcrossLaunches;
}

bool PreserveRevivalLogsAcrossLaunches()
{
    return g_settings.preserveRevivalLogsAcrossLaunches;
}

bool IsConsoleEnabled()
{
    return g_settings.enableConsole;
}

bool IsDebugMenuEnabled()
{
    return g_settings.enableDebugMenu;
}

bool IsDesyncDetectionEnabled()
{
    return g_settings.desyncDetection;
}

bool IsVerboseBridgePatchLoggingEnabled()
{
    return g_settings.verboseBridgePatchLogging;
}

bool IsVerboseSyncDiagnosticsEnabled()
{
    return g_settings.verboseSyncDiagnostics;
}

bool IsVerboseRevival102jLifecycleLoggingEnabled()
{
    return g_settings.verboseRevival102jLifecycleLogging;
}

bool AreAllVerboseLogsEnabled()
{
    return g_settings.verboseBridgePatchLogging
        && g_settings.verboseSyncDiagnostics
        && g_settings.verboseRevival102jLifecycleLogging;
}

bool HideEmptySetsInBattleLogByDefault()
{
    return g_settings.hideEmptySetsInBattleLog;
}

bool IsEagerZeroFrameGraphicsRestoreEnabled()
{
    return g_settings.eagerZeroFrameGraphicsRestore;
}

bool IsMenuTtfTextEnabled()
{
    return g_settings.menuTtfText;
}

const std::string& MenuTtfFontFace()
{
    return g_settings.menuTtfFontFace;
}

const std::string& HostingTipFontFace()
{
    return g_settings.hostingTipFontFace;
}

const std::string& AsyncHostReturnKeyBinding()
{
    return g_settings.asyncHostReturnKey;
}
} // namespace netplay::mod_settings
