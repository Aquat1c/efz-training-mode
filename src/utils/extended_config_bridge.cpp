#include "../include/utils/extended_config_bridge.h"

#include "../include/core/logger.h"
#include "../include/utils/config.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace ExtendedConfigBridge {
namespace {

constexpr DWORD kRefreshMs = 1000;

Status g_status{};
DWORD g_lastRefreshTick = 0;
FILETIME g_lastConfigWriteTime{};
bool g_haveConfigWriteTime = false;
bool g_loggedDiscovery = false;

int ClampPercent(int value) {
    return std::clamp(value, 0, 100);
}

bool FileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
}

bool TryGetWriteTime(const std::string& path, FILETIME& outTime) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    outTime = data.ftLastWriteTime;
    return true;
}

bool SameFileTime(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

std::string ModulePath(HMODULE module) {
    char path[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return path;
}

bool FindLoadedModule(Status& status) {
    static const char* const kModuleNames[] = {
        "EFZConfigExtension.dll",
        "efz_config_extension.dll",
        "EFZ_Config_Extension.dll",
        "EFZExtendedConfig.dll",
        "ExtendedConfig.dll",
        "InGameControlsRebind.dll",
    };

    for (const char* name : kModuleNames) {
        if (HMODULE module = GetModuleHandleA(name)) {
            status.modLoaded = true;
            status.moduleName = name;
            status.modulePath = ModulePath(module);
            return true;
        }
    }
    return false;
}

void AddPath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    for (const auto& existing : paths) {
        if (_stricmp(existing.string().c_str(), path.string().c_str()) == 0) {
            return;
        }
    }
    paths.push_back(path);
}

std::filesystem::path DefaultSharedPathForModule(const Status& status) {
    if (!status.modulePath.empty()) {
        return std::filesystem::path(status.modulePath).parent_path() / "efz_extended_config.ini";
    }
    return {};
}

bool FindSharedConfigPath(Status& status) {
    static const char* const kFileNames[] = {
        "efz_extended_config.ini",
        "EFZExtendedConfig.ini",
        "EFZConfigExtension.ini",
        "extended_config.ini",
        "InGameControlsRebind.ini",
    };

    std::vector<std::filesystem::path> dirs;
    if (!status.modulePath.empty()) {
        AddPath(dirs, std::filesystem::path(status.modulePath).parent_path());
    }

    char currentDir[MAX_PATH] = {0};
    if (GetCurrentDirectoryA(MAX_PATH, currentDir) > 0) {
        const std::filesystem::path gameDir(currentDir);
        AddPath(dirs, gameDir);
        AddPath(dirs, gameDir / "mods" / "EFZConfigExtension");
        AddPath(dirs, gameDir / "mods" / "efz_config_extension");
        AddPath(dirs, gameDir / "mods" / "ExtendedConfig");
        AddPath(dirs, gameDir / "mods" / "InGameControlsRebind");
    }

    for (const auto& dir : dirs) {
        for (const char* name : kFileNames) {
            const auto candidate = dir / name;
            if (FileExists(candidate)) {
                status.sharedConfigFound = true;
                status.sharedConfigPath = candidate.string();
                return true;
            }
        }
    }

    if (status.modLoaded) {
        const auto defaultPath = DefaultSharedPathForModule(status);
        if (!defaultPath.empty()) {
            status.sharedConfigPath = defaultPath.string();
        }
    }
    return false;
}

int ReadIniInt(const char* section, const char* key, int fallback, const std::string& path) {
    return GetPrivateProfileIntA(section, key, fallback, path.c_str());
}

void ReadSharedConfig(Status& status) {
    if (status.sharedConfigPath.empty() || !FileExists(status.sharedConfigPath)) {
        return;
    }

    status.sharedConfigFound = true;
    status.version = ReadIniInt("Shared", "version", 0, status.sharedConfigPath);

    const bool sharedEnabled = ReadIniInt("Shared", "enabled", 1, status.sharedConfigPath) != 0;
    const bool audioEnabled =
        ReadIniInt("Shared", "audio", ReadIniInt("Audio", "enabled", 0, status.sharedConfigPath), status.sharedConfigPath) != 0;
    const bool controlsEnabled =
        ReadIniInt("Shared", "controls", ReadIniInt("Controls", "enabled", 0, status.sharedConfigPath), status.sharedConfigPath) != 0;

    const int bgm = ReadIniInt("Audio", "bgmVolumePercent", -1, status.sharedConfigPath);
    const int se = ReadIniInt("Audio", "seVolumePercent", -1, status.sharedConfigPath);

    status.bgmVolumeAvailable = bgm >= 0 && bgm <= 100;
    status.seVolumeAvailable = se >= 0 && se <= 100;
    if (status.bgmVolumeAvailable) {
        status.bgmVolumePercent = ClampPercent(bgm);
    }
    if (status.seVolumeAvailable) {
        status.seVolumePercent = ClampPercent(se);
    }

    status.audioRevision = ReadIniInt("Audio", "revision", 0, status.sharedConfigPath);
    status.controlsRevision = ReadIniInt("Controls", "revision", 0, status.sharedConfigPath);
    status.audioShared = sharedEnabled && audioEnabled && (status.bgmVolumeAvailable || status.seVolumeAvailable);
    status.controlsShared = sharedEnabled && controlsEnabled;
}

void LogDiscoveryIfChanged(const Status& previous, const Status& current) {
    const bool changed = !g_loggedDiscovery
        || previous.modLoaded != current.modLoaded
        || previous.sharedConfigFound != current.sharedConfigFound
        || previous.audioShared != current.audioShared
        || previous.controlsShared != current.controlsShared
        || previous.sharedConfigPath != current.sharedConfigPath
        || previous.audioRevision != current.audioRevision
        || previous.controlsRevision != current.controlsRevision;

    if (!changed) {
        return;
    }

    g_loggedDiscovery = true;
    std::ostringstream oss;
    oss << "[EXTCFG] status mod=" << (current.modLoaded ? "1" : "0")
        << " sharedFile=" << (current.sharedConfigFound ? "1" : "0")
        << " audio=" << (current.audioShared ? "1" : "0")
        << " controls=" << (current.controlsShared ? "1" : "0");
    if (!current.moduleName.empty()) {
        oss << " module=" << current.moduleName;
    }
    if (!current.sharedConfigPath.empty()) {
        oss << " path=" << current.sharedConfigPath;
    }
    if (current.audioShared) {
        oss << " bgm=" << current.bgmVolumePercent
            << " se=" << current.seVolumePercent
            << " audioRev=" << current.audioRevision;
    }
    if (current.controlsShared) {
        oss << " controlsRev=" << current.controlsRevision;
    }
    LogOut(oss.str(), true);
}

bool WriteIniInt(const char* section, const char* key, int value, const std::string& path) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", value);
    return WritePrivateProfileStringA(section, key, buf, path.c_str()) != FALSE;
}

} // namespace

bool Refresh(bool force) {
    const DWORD now = GetTickCount();
    if (!force && g_lastRefreshTick != 0 && (now - g_lastRefreshTick) < kRefreshMs) {
        return g_status.sharedConfigFound || g_status.modLoaded;
    }
    g_lastRefreshTick = now;

    Status next{};
    FindLoadedModule(next);
    FindSharedConfigPath(next);

    FILETIME writeTime{};
    const bool haveWriteTime = !next.sharedConfigPath.empty() && TryGetWriteTime(next.sharedConfigPath, writeTime);
    if (!force
        && haveWriteTime
        && g_haveConfigWriteTime
        && SameFileTime(writeTime, g_lastConfigWriteTime)
        && next.modLoaded == g_status.modLoaded
        && next.sharedConfigPath == g_status.sharedConfigPath) {
        return g_status.sharedConfigFound || g_status.modLoaded;
    }

    if (haveWriteTime) {
        g_lastConfigWriteTime = writeTime;
        g_haveConfigWriteTime = true;
    } else {
        g_haveConfigWriteTime = false;
    }

    ReadSharedConfig(next);
    LogDiscoveryIfChanged(g_status, next);
    g_status = next;
    return g_status.sharedConfigFound || g_status.modLoaded;
}

bool ImportAudioSettingsIfAvailable(bool force) {
    Refresh(force);
    if (!g_status.audioShared) {
        return false;
    }

    bool changed = false;
    if (g_status.bgmVolumeAvailable && Config::GetSettings().bgmVolumePercent != g_status.bgmVolumePercent) {
        Config::SetSetting("General", "bgmVolumePercent", std::to_string(g_status.bgmVolumePercent));
        changed = true;
    }
    if (g_status.seVolumeAvailable && Config::GetSettings().seVolumePercent != g_status.seVolumePercent) {
        Config::SetSetting("General", "seVolumePercent", std::to_string(g_status.seVolumePercent));
        changed = true;
    }

    if (changed) {
        std::ostringstream oss;
        oss << "[AUDIO][SHARED] imported ExtendedConfig audio bgm="
            << Config::GetSettings().bgmVolumePercent
            << " se=" << Config::GetSettings().seVolumePercent
            << " path=" << g_status.sharedConfigPath;
        LogOut(oss.str(), true);
    }
    return changed;
}

bool PublishAudioSettings(int bgmPercent, int sePercent) {
    Refresh(false);
    if (g_status.sharedConfigPath.empty()) {
        return false;
    }
    if (!g_status.modLoaded && !g_status.sharedConfigFound) {
        return false;
    }

    const int bgm = ClampPercent(bgmPercent);
    const int se = ClampPercent(sePercent);
    const int revision = (g_status.audioRevision > 0 ? g_status.audioRevision : 0) + 1;

    bool ok = true;
    ok = WritePrivateProfileStringA("Shared", "version", "1", g_status.sharedConfigPath.c_str()) != FALSE && ok;
    ok = WritePrivateProfileStringA("Shared", "enabled", "1", g_status.sharedConfigPath.c_str()) != FALSE && ok;
    ok = WritePrivateProfileStringA("Shared", "audio", "1", g_status.sharedConfigPath.c_str()) != FALSE && ok;
    ok = WritePrivateProfileStringA("Shared", "lastWriter", "efz-training-mode", g_status.sharedConfigPath.c_str()) != FALSE && ok;
    ok = WritePrivateProfileStringA("Audio", "enabled", "1", g_status.sharedConfigPath.c_str()) != FALSE && ok;
    ok = WriteIniInt("Audio", "bgmVolumePercent", bgm, g_status.sharedConfigPath) && ok;
    ok = WriteIniInt("Audio", "seVolumePercent", se, g_status.sharedConfigPath) && ok;
    ok = WriteIniInt("Audio", "revision", revision, g_status.sharedConfigPath) && ok;

    if (ok) {
        g_lastRefreshTick = 0;
        Refresh(true);
        std::ostringstream oss;
        oss << "[AUDIO][SHARED] published ExtendedConfig audio bgm=" << bgm
            << " se=" << se
            << " path=" << g_status.sharedConfigPath;
        LogOut(oss.str(), true);
    } else {
        LogOut("[AUDIO][SHARED] failed to publish ExtendedConfig audio", true);
    }
    return ok;
}

const Status& GetStatus() {
    Refresh(false);
    return g_status;
}

bool IsSharedAudioActive() {
    Refresh(false);
    return g_status.audioShared;
}

} // namespace ExtendedConfigBridge
