// IPC shared memory, config loading, module resolution, and session status helpers.

#include "netplay/bridge/takeover_internal.h"
#include <array>
#include <cstdio>
#include <cstring>

#include <windows.h>

namespace netplay::bridge::takeover
{

namespace
{
std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return std::string();
    }

    const int bytes = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (bytes <= 0)
    {
        return std::string();
    }

    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            static_cast<int>(wide.size()),
            utf8.data(),
            bytes,
            nullptr,
            nullptr)
        <= 0)
    {
        return std::string();
    }

    return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
    {
        return std::wstring();
    }

    const int chars = MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (chars <= 0)
    {
        return std::wstring();
    }

    std::wstring wide(static_cast<size_t>(chars), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8.data(),
            static_cast<int>(utf8.size()),
            wide.data(),
            chars)
        <= 0)
    {
        return std::wstring();
    }

    return wide;
}

bool IsActiveRevivalVersionTag(const char* versionTag)
{
    return versionTag != nullptr
        && g_activeRevival != nullptr
        && g_activeRevival->versionTag != nullptr
        && std::strcmp(g_activeRevival->versionTag, versionTag) == 0;
}

std::wstring ModuleDirectoryWide(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return std::wstring();
    }

    std::wstring dir(path, path + n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return std::wstring();
    }

    dir.resize(slash);
    return dir;
}

bool RemoveDirectoryTreeBestEffortW(const std::wstring& directory, DWORD* outError)
{
    DWORD lastError = ERROR_SUCCESS;
    const std::wstring searchPath = directory + L"\\*";
    WIN32_FIND_DATAW findData = {};
    HANDLE find = FindFirstFileW(searchPath.c_str(), &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (lstrcmpW(findData.cFileName, L".") == 0
                || lstrcmpW(findData.cFileName, L"..") == 0)
            {
                continue;
            }

            const std::wstring childPath = directory + L"\\" + findData.cFileName;
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                if (!RemoveDirectoryTreeBestEffortW(childPath, &lastError))
                {
                    FindClose(find);
                    if (outError != nullptr)
                    {
                        *outError = lastError;
                    }
                    return false;
                }
                continue;
            }

            (void)SetFileAttributesW(childPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(childPath.c_str()))
            {
                lastError = GetLastError();
                if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
                {
                    FindClose(find);
                    if (outError != nullptr)
                    {
                        *outError = lastError;
                    }
                    return false;
                }
            }
        }
        while (FindNextFileW(find, &findData) != FALSE);

        lastError = GetLastError();
        FindClose(find);
        if (lastError != ERROR_NO_MORE_FILES)
        {
            if (outError != nullptr)
            {
                *outError = lastError;
            }
            return false;
        }
    }
    else
    {
        lastError = GetLastError();
        if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
        {
            if (outError != nullptr)
            {
                *outError = lastError;
            }
            return false;
        }
    }

    if (!RemoveDirectoryW(directory.c_str()))
    {
        lastError = GetLastError();
        if (lastError != ERROR_PATH_NOT_FOUND)
        {
            if (outError != nullptr)
            {
                *outError = lastError;
            }
            return false;
        }
    }

    if (outError != nullptr)
    {
        *outError = ERROR_SUCCESS;
    }
    return true;
}

void ClearPeerQuitDiagnosticBlock(SharedBlock* block)
{
    if (block == nullptr)
    {
        return;
    }

    block->peerQuitDiagnosticText[0] = '\0';
    InterlockedIncrement(&block->peerQuitDiagnosticSerial);
}

void AppendPeerQuitDiagnosticBlock(SharedBlock* block, const char* text)
{
    if (block == nullptr || text == nullptr || text[0] == '\0')
    {
        return;
    }

    const size_t capacity = sizeof(block->peerQuitDiagnosticText);
    size_t used = std::strlen(block->peerQuitDiagnosticText);
    if (used >= capacity - 1u)
    {
        return;
    }

    if (used > 0 && block->peerQuitDiagnosticText[used - 1] != '\n')
    {
        block->peerQuitDiagnosticText[used++] = '\n';
        block->peerQuitDiagnosticText[used] = '\0';
    }

    strncpy_s(
        block->peerQuitDiagnosticText + used,
        capacity - used,
        text,
        _TRUNCATE);
    InterlockedIncrement(&block->peerQuitDiagnosticSerial);
}
} // namespace

void* EnsureRevivalErrorCodeNullGuardStub()
{
    if (g_revivalErrorCodeNullGuardStub != nullptr)
    {
        return g_revivalErrorCodeNullGuardStub;
    }

    void* stub = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (stub == nullptr)
    {
        mod::Log(
            "Takeover: failed to allocate null-guard stub err=%lu",
            static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }

    // if ((ecx & 0xFFFF0000) == 0) return true; else return (*ecx == 0);
    constexpr std::array<uint8_t, 17> kStubBytes = {
        0xB0, 0x01,                         // mov al, 1
        0xF7, 0xC1, 0x00, 0x00, 0xFF, 0xFF, // test ecx, 0xFFFF0000
        0x74, 0x06,                         // jz +6
        0x83, 0x39, 0x00,                   // cmp dword ptr [ecx], 0
        0x0F, 0x94, 0xC0,                   // sete al
        0xC3,                               // ret
    };

    std::memcpy(stub, kStubBytes.data(), kStubBytes.size());
    (void)FlushInstructionCache(GetCurrentProcess(), stub, kStubBytes.size());
    g_revivalErrorCodeNullGuardStub = stub;
    return stub;
}

bool OpenTempIpcContext(TempIpcContext* ctx, bool needInitEvent, bool needConsoleEvent)
{
    if (ctx == nullptr)
    {
        return false;
    }

    ctx->mapHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kSharedBlockName);
    if (ctx->mapHandle == nullptr)
    {
        return false;
    }

    ctx->block = static_cast<SharedBlock*>(MapViewOfFile(ctx->mapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
    if (ctx->block == nullptr)
    {
        CloseHandle(ctx->mapHandle);
        ctx->mapHandle = nullptr;
        return false;
    }

    if (needInitEvent)
    {
        ctx->initEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kInitReadyEventName);
    }
    if (needConsoleEvent)
    {
        ctx->consoleEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kConsoleReadyEventName);
    }

    return true;
}

void CloseTempIpcContext(TempIpcContext* ctx)
{
    if (ctx == nullptr)
    {
        return;
    }

    if (ctx->block != nullptr)
    {
        UnmapViewOfFile(ctx->block);
        ctx->block = nullptr;
    }
    if (ctx->mapHandle != nullptr)
    {
        CloseHandle(ctx->mapHandle);
        ctx->mapHandle = nullptr;
    }
    if (ctx->initEvent != nullptr)
    {
        CloseHandle(ctx->initEvent);
        ctx->initEvent = nullptr;
    }
    if (ctx->consoleEvent != nullptr)
    {
        CloseHandle(ctx->consoleEvent);
        ctx->consoleEvent = nullptr;
    }
}

void ClearDelayPromptState(const char* reason)
{
    const LONG oldPrompt =
        InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    const LONG oldServed =
        InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    const LONG oldConnected =
        InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    g_injectedDelayPromptWaitStartTick = 0;

    LONG oldSharedPrompt = 0;
    LONG oldSharedServed = 0;
    LONG oldMetricsSerial = 0;
    LONG oldInputSerial = 0;
    LONG oldInputServed = 0;
    if (g_hostBlock != nullptr)
    {
        oldSharedPrompt = InterlockedExchange(&g_hostBlock->delayPromptSerial, 0);
        oldSharedServed = InterlockedExchange(&g_hostBlock->delayPromptServedSerial, 0);
        oldMetricsSerial = InterlockedExchange(&g_hostBlock->delayMetricsSerial, 0);
        oldInputSerial = InterlockedExchange(&g_hostBlock->delayInputSerial, 0);
        oldInputServed = InterlockedExchange(&g_hostBlock->delayInputServedSerial, 0);
        g_hostBlock->delayInputValue = -1;
        g_hostBlock->delayAveragePingMs = -1;
        g_hostBlock->delayMinPingMs = -1;
        g_hostBlock->delayMaxPingMs = -1;
        g_hostBlock->delayRecommended = -1;
        g_hostBlock->delayRangeMin = 0;
        g_hostBlock->delayRangeMax = 20;
    }

    g_delayPromptMetrics = {};

    mod::Log(
        "DelayPromptState: cleared reason=%s oldPrompt=%ld oldServed=%ld oldConnected=%ld "
        "oldSharedPrompt=%ld oldSharedServed=%ld oldMetrics=%ld oldInput=%ld/%ld",
        reason != nullptr ? reason : "",
        static_cast<long>(oldPrompt),
        static_cast<long>(oldServed),
        static_cast<long>(oldConnected),
        static_cast<long>(oldSharedPrompt),
        static_cast<long>(oldSharedServed),
        static_cast<long>(oldMetricsSerial),
        static_cast<long>(oldInputSerial),
        static_cast<long>(oldInputServed));
}

void PublishDelayPromptSerial(LONG serial)
{
    if (serial <= 0)
    {
        return;
    }

    if (g_injectedBlock != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&g_injectedBlock->delayPromptSerial, 0, 0);
        if (serial > current)
        {
            InterlockedExchange(&g_injectedBlock->delayPromptSerial, serial);
        }
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&temp.block->delayPromptSerial, 0, 0);
        if (serial > current)
        {
            InterlockedExchange(&temp.block->delayPromptSerial, serial);
        }
    }
    CloseTempIpcContext(&temp);
}

void PublishSpectateConfirmPromptSerial(LONG serial, int promptKind)
{
    if (serial <= 0)
    {
        return;
    }

    if (g_injectedBlock != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&g_injectedBlock->spectateConfirmPromptSerial, 0, 0);
        if (serial > current)
        {
            g_injectedBlock->spectateConfirmPromptKind = promptKind;
            InterlockedExchange(&g_injectedBlock->spectateConfirmPromptSerial, serial);
        }
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&temp.block->spectateConfirmPromptSerial, 0, 0);
        if (serial > current)
        {
            temp.block->spectateConfirmPromptKind = promptKind;
            InterlockedExchange(&temp.block->spectateConfirmPromptSerial, serial);
        }
    }
    CloseTempIpcContext(&temp);
}

void ReadSpectateConfirmPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial, int* outPromptKind)
{
    LONG promptSerial = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptSerial, 0, 0);
    LONG promptServedSerial = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptServedSerial, 0, 0);
    int promptKind = static_cast<int>(netplay::bridge::NetbridgeSpectatePromptKind::None);

    if (g_hostBlock != nullptr)
    {
        const LONG sharedPromptSerial = InterlockedCompareExchange(&g_hostBlock->spectateConfirmPromptSerial, 0, 0);
        const LONG sharedPromptServedSerial = InterlockedCompareExchange(&g_hostBlock->spectateConfirmPromptServedSerial, 0, 0);
        if (sharedPromptSerial > promptSerial)
        {
            promptSerial = sharedPromptSerial;
            promptKind = g_hostBlock->spectateConfirmPromptKind;
        }
        if (sharedPromptServedSerial > promptServedSerial)
        {
            promptServedSerial = sharedPromptServedSerial;
        }
    }

    if (outPromptSerial != nullptr)
    {
        *outPromptSerial = promptSerial;
    }
    if (outPromptServedSerial != nullptr)
    {
        *outPromptServedSerial = promptServedSerial;
    }
    if (outPromptKind != nullptr)
    {
        *outPromptKind = promptKind;
    }
}

void PublishConsoleError(const char* errorText)
{
    if (errorText == nullptr || errorText[0] == '\0')
    {
        return;
    }

    auto writeError = [&](SharedBlock* block)
    {
        CopyString(block->consoleErrorText, sizeof(block->consoleErrorText), errorText);
        InterlockedIncrement(&block->consoleErrorSerial);
    };

    if (g_injectedBlock != nullptr)
    {
        writeError(g_injectedBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        writeError(temp.block);
    }
    CloseTempIpcContext(&temp);
}

void PublishConsoleDesyncWarning(const char* warnText)
{
    if (warnText == nullptr || warnText[0] == '\0')
    {
        return;
    }

    auto writeWarning = [&](SharedBlock* block)
    {
        CopyString(
            block->consoleDesyncWarnText,
            sizeof(block->consoleDesyncWarnText),
            warnText);
        InterlockedIncrement(&block->consoleDesyncWarnSerial);
    };

    if (g_injectedBlock != nullptr)
    {
        writeWarning(g_injectedBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        writeWarning(temp.block);
    }
    CloseTempIpcContext(&temp);
}

void ReadConsoleDesyncWarning(LONG* outSerial, char* outText, int outTextSize)
{
    LONG serial = 0;
    const char* text = nullptr;

    if (g_hostBlock != nullptr)
    {
        serial = InterlockedCompareExchange(&g_hostBlock->consoleDesyncWarnSerial, 0, 0);
        text = g_hostBlock->consoleDesyncWarnText;
    }

    if (outSerial != nullptr)
    {
        *outSerial = serial;
    }
    if (outText != nullptr && outTextSize > 0 && text != nullptr && serial > 0)
    {
        CopyString(outText, outTextSize, text);
    }
    else if (outText != nullptr && outTextSize > 0)
    {
        outText[0] = '\0';
    }
}

void ReadConsoleError(LONG* outSerial, char* outText, int outTextSize)
{
    LONG serial = 0;
    const char* text = nullptr;

    if (g_hostBlock != nullptr)
    {
        serial = InterlockedCompareExchange(&g_hostBlock->consoleErrorSerial, 0, 0);
        text = g_hostBlock->consoleErrorText;
    }

    if (outSerial != nullptr)
    {
        *outSerial = serial;
    }
    if (outText != nullptr && outTextSize > 0 && text != nullptr && serial > 0)
    {
        CopyString(outText, outTextSize, text);
    }
    else if (outText != nullptr && outTextSize > 0)
    {
        outText[0] = '\0';
    }
}

void ClearPeerQuitDiagnostic()
{
    if (g_injectedBlock != nullptr)
    {
        ClearPeerQuitDiagnosticBlock(g_injectedBlock);
        return;
    }
    if (g_hostBlock != nullptr)
    {
        ClearPeerQuitDiagnosticBlock(g_hostBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        ClearPeerQuitDiagnosticBlock(temp.block);
    }
    CloseTempIpcContext(&temp);
}

void AppendPeerQuitDiagnostic(const char* text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return;
    }

    if (g_injectedBlock != nullptr)
    {
        AppendPeerQuitDiagnosticBlock(g_injectedBlock, text);
        return;
    }
    if (g_hostBlock != nullptr)
    {
        AppendPeerQuitDiagnosticBlock(g_hostBlock, text);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        AppendPeerQuitDiagnosticBlock(temp.block, text);
    }
    CloseTempIpcContext(&temp);
}

void ReadPeerQuitDiagnostic(LONG* outSerial, char* outText, int outTextSize)
{
    LONG serial = 0;
    const char* text = nullptr;

    if (g_hostBlock != nullptr)
    {
        serial = InterlockedCompareExchange(&g_hostBlock->peerQuitDiagnosticSerial, 0, 0);
        text = g_hostBlock->peerQuitDiagnosticText;
    }
    else if (g_injectedBlock != nullptr)
    {
        serial = InterlockedCompareExchange(&g_injectedBlock->peerQuitDiagnosticSerial, 0, 0);
        text = g_injectedBlock->peerQuitDiagnosticText;
    }

    if (outSerial != nullptr)
    {
        *outSerial = serial;
    }
    if (outText != nullptr && outTextSize > 0 && text != nullptr && text[0] != '\0')
    {
        CopyString(outText, static_cast<size_t>(outTextSize), text);
    }
    else if (outText != nullptr && outTextSize > 0)
    {
        outText[0] = '\0';
    }
}

HMODULE SelfModule()
{
    HMODULE module = nullptr;
    (void)GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&SelfModule),
        &module);
    return module;
}

std::string ModulePath(HMODULE module)
{
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return std::string();
    }
    return std::string(path);
}

void CleanupNativeHostShadowLogDirectory(const char* reason)
{
    if (IsCurrentProcessRevival())
    {
        return;
    }

    const std::wstring moduleDir = ModuleDirectoryWide(SelfModule());
    if (moduleDir.empty())
    {
        mod::Log(
            "CAPTURE_LOG: native_host cleanup skipped reason='%s' err=module_dir_unresolved",
            reason != nullptr ? reason : "");
        return;
    }

    // Pre-logs-folder builds used <mod>\native_host; sweep the legacy
    // location too so upgrades don't leave a stale copy behind.
    const std::wstring legacyShadowDir = moduleDir + L"\\native_host";
    if (GetFileAttributesW(legacyShadowDir.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DWORD legacyErr = ERROR_SUCCESS;
        (void)RemoveDirectoryTreeBestEffortW(legacyShadowDir, &legacyErr);
    }

    const std::wstring shadowDir = moduleDir + L"\\logs\\native_host";
    const DWORD attrs = GetFileAttributesW(shadowDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
        {
            mod::Log(
                "CAPTURE_LOG: native_host cleanup stat failed reason='%s' err=%lu",
                reason != nullptr ? reason : "",
                static_cast<unsigned long>(err));
        }
        return;
    }

    DWORD cleanupErr = ERROR_SUCCESS;
    if (!RemoveDirectoryTreeBestEffortW(shadowDir, &cleanupErr))
    {
        mod::Log(
            "CAPTURE_LOG: native_host cleanup incomplete reason='%s' err=%lu",
            reason != nullptr ? reason : "",
            static_cast<unsigned long>(cleanupErr));
        return;
    }

    mod::Log(
        "CAPTURE_LOG: native_host cleanup complete reason='%s'",
        reason != nullptr ? reason : "");
}

bool TryReadCaptureRevivalNativeLogsConfig(bool* outEnabled, std::string* outSourceTag)
{
    if (outEnabled == nullptr)
    {
        return false;
    }

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        return false;
    }
    char* slash = std::strrchr(exePath, '\\');
    if (slash == nullptr)
    {
        slash = std::strrchr(exePath, '/');
    }
    if (slash == nullptr)
    {
        return false;
    }
    slash[1] = '\0';
    std::string iniPath = std::string(exePath) + "EfzRevival.ini";

    const UINT debugValue = GetPrivateProfileIntA("Global", "Debug", 2, iniPath.c_str());
    if (debugValue > 1)
    {
        return false;
    }

    *outEnabled = (debugValue != 0);
    if (outSourceTag != nullptr)
    {
        *outSourceTag = std::string("ini:") + iniPath;
    }
    return true;
}

bool CaptureRevivalNativeLogsEnabled()
{
    if (g_captureRevivalNativeLogsConfigured)
    {
        return g_captureRevivalNativeLogs;
    }

    bool enabled = false;
    std::string sourceTag = "default_off";
    if (TryReadCaptureRevivalNativeLogsConfig(&enabled, &sourceTag))
    {
        g_captureRevivalNativeLogs = enabled;
    }
    else
    {
        g_captureRevivalNativeLogs = enabled;
    }
    g_captureRevivalNativeLogsConfigured = true;

    mod::Log(
        "Takeover: capture revival native logs=%d source=%s",
        g_captureRevivalNativeLogs ? 1 : 0,
        sourceTag.c_str());

    return g_captureRevivalNativeLogs;
}

std::string GameDirectory()
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
    {
        return std::string();
    }
    char* slash = std::strrchr(path, '\\');
    if (slash == nullptr)
    {
        return std::string();
    }
    *slash = '\0';
    return std::string(path);
}

std::wstring GameDirectoryWide()
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
    {
        return std::wstring();
    }

    std::wstring directory = path;
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return std::wstring();
    }

    directory.resize(slash);
    return directory;
}

bool TryWriteClipboardAscii(const char* text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return false;
    }

    const size_t bytes = std::strlen(text) + 1;
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (OpenClipboard(nullptr) == FALSE)
        {
            Sleep(10);
            continue;
        }

        bool success = false;
        if (EmptyClipboard() != FALSE)
        {
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory != nullptr)
            {
                void* const locked = GlobalLock(memory);
                if (locked != nullptr)
                {
                    std::memcpy(locked, text, bytes);
                    GlobalUnlock(memory);
                    if (SetClipboardData(CF_TEXT, memory) != nullptr)
                    {
                        // Clipboard owns the handle after success.
                        memory = nullptr;
                        success = true;
                    }
                }
                if (memory != nullptr)
                {
                    GlobalFree(memory);
                }
            }
        }

        CloseClipboard();
        if (success)
        {
            return true;
        }

        Sleep(10);
    }

    return false;
}

void SetPhase(NetbridgeStatus* status, NetbridgePhase phase, const char* error)
{
    if (status == nullptr)
    {
        return;
    }

    const NetbridgePhase oldPhase = static_cast<NetbridgePhase>(status->phase);
    char oldError[sizeof(status->errorMsg)] = {};
    std::memcpy(oldError, status->errorMsg, sizeof(status->errorMsg));

    status->phase = static_cast<int>(phase);
    status->phaseTick = GetTickCount();
    if (error != nullptr)
    {
        CopyString(status->errorMsg, sizeof(status->errorMsg), error);
    }
    else if (phase != NetbridgePhase::Failed)
    {
        status->errorMsg[0] = '\0';
    }

    if (oldPhase != phase || std::strncmp(oldError, status->errorMsg, sizeof(status->errorMsg)) != 0)
    {
        mod::Log(
            "Takeover: phase %s -> %s reason='%s'",
            netplay::bridge::PhaseToString(oldPhase),
            netplay::bridge::PhaseToString(phase),
            status->errorMsg[0] != '\0' ? status->errorMsg : "");
        LogRevival102jDeepStep("Phase.takeover_transition", status);
    }
}

void CloseProcessHandle(NetbridgeStatus* status)
{
    ResetInjectedPeerQuitBroadcastState();
    if (g_revivalProcess != nullptr)
    {
        CloseHandle(g_revivalProcess);
        g_revivalProcess = nullptr;
    }
    g_revivalProcessId = 0;
    g_remoteInjectedSelfBase = 0;
    g_lastLatePatchRetryTick = 0;
    g_lastLatePatchRetryLogTick = 0;
    g_latePatchRetryAttempts = 0;
    g_latePatchRetrySuccesses = 0;
    g_lastLatePatchRetryResultValid = false;
    g_lastLatePatchRetryResult = false;
    g_observedTakeoverCreatePath = false;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    if (status != nullptr)
    {
        status->processId = 0;
    }
}

bool ProcessAlive(NetbridgeStatus* status)
{
    if (g_revivalProcess == nullptr)
    {
        return false;
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(g_revivalProcess, &exitCode) == FALSE || exitCode != STILL_ACTIVE)
    {
        CloseProcessHandle(status);
        return false;
    }
    return true;
}

bool IsSyncReadyForVsHuman(const NetbridgeStatus* status)
{
    if (status == nullptr)
    {
        return false;
    }

    // Player sync ready: EFZ game mode 3 (online play), connection flag 4,
    // rollback session byte in a known-good state.
    if (status->syncGameMode == 3
        && status->syncMode0Flag1084 == 4
        && (status->syncSessionByte == 0 || status->syncSessionByte == 1 || status->syncSessionByte == 2))
    {
        return true;
    }

    // Spectator sync ready: EFZ game mode 8 (spectate), connection flag 4.
    // Spectators don't use the same session object layout so we don't check
    // sessionByte here - the lightweight spectator wrapper has different
    // offsets and a smaller object (160 bytes vs 688 for players).
    if (status->syncGameMode == 8 && status->syncMode0Flag1084 == 4)
    {
        return true;
    }

    return false;
}

bool RequiresNativeVsHumanSyncForHandoff(const NetbridgeStatus* status)
{
    // 1.02j's native "sync ready" fields describe the post-handoff EFZ VS
    // state: game mode 3 plus the mode-0 flag set by PrepareVsHumanGameState.
    // The infinite-sync capture showed those fields stay 0 while both native
    // rollback sessions already exist and wait at frame 0 for remote input.
    // Waiting for them before handoff deadlocks the startup path; the safe
    // input hook protects the early remote-empty window instead.
    if (IsActiveRevivalVersionTag("1.02j")
        && (g_localRoleFlag == kLocalRoleOnline
            || (status != nullptr && status->roleFlag == kLocalRoleOnline)))
    {
        return false;
    }

    return false;
}

void ReadDelayPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial)
{
    LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
    LONG promptServedSerial = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);

    if (g_hostBlock != nullptr)
    {
        const LONG sharedPromptSerial = InterlockedCompareExchange(&g_hostBlock->delayPromptSerial, 0, 0);
        const LONG sharedPromptServedSerial = InterlockedCompareExchange(&g_hostBlock->delayPromptServedSerial, 0, 0);
        if (sharedPromptSerial > promptSerial)
        {
            promptSerial = sharedPromptSerial;
        }
        if (sharedPromptServedSerial > promptServedSerial)
        {
            promptServedSerial = sharedPromptServedSerial;
        }
    }

    if (outPromptSerial != nullptr)
    {
        *outPromptSerial = promptSerial;
    }
    if (outPromptServedSerial != nullptr)
    {
        *outPromptServedSerial = promptServedSerial;
    }
}

RuntimeReadyProbe EvaluateRuntimeReadyProbe(const NetbridgeStatus* status)
{
    RuntimeReadyProbe probe = {};
    probe.nativeSyncReady = IsSyncReadyForVsHuman(status);
    if (probe.nativeSyncReady)
    {
        probe.ready = true;
        probe.source = "native_sync";
        return probe;
    }

    probe.localInitApplied = g_localInitAppliedForSession;
    if (!probe.localInitApplied)
    {
        return probe;
    }

    LONG promptSerial = 0;
    LONG promptServedSerial = 0;
    ReadDelayPromptSignal(&promptSerial, &promptServedSerial);
    probe.delayPromptSeen = promptSerial > 0;
    if (!probe.delayPromptSeen)
    {
        return probe;
    }

    LONG inputSerial = 0;
    LONG inputServedSerial = 0;
    if (g_hostBlock != nullptr)
    {
        inputSerial = InterlockedCompareExchange(&g_hostBlock->delayInputSerial, 0, 0);
        inputServedSerial = InterlockedCompareExchange(&g_hostBlock->delayInputServedSerial, 0, 0);
    }

    probe.delayInputApplied =
        (inputSerial > 0 && inputServedSerial >= inputSerial)
        || (inputSerial <= 0 && promptServedSerial >= promptSerial);
    if (!probe.delayInputApplied)
    {
        return probe;
    }

    if (RequiresNativeVsHumanSyncForHandoff(status))
    {
        probe.source = "await_native_sync_102j";
        return probe;
    }

    if (g_revivalProcessId == 0)
    {
        return probe;
    }

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    probe.sessionPointerValid = sessionPtr != 0;
    if (!probe.sessionPointerValid)
    {
        return probe;
    }

    int helperPidField = -1;
    uintptr_t helperHandleFieldRaw = 0;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid), &helperPidField);
    (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle), &helperHandleFieldRaw);

    probe.helperPidMatches = helperPidField == static_cast<int>(g_revivalProcessId);
    const HANDLE helperHandle = reinterpret_cast<HANDLE>(helperHandleFieldRaw);
    if (helperHandle != nullptr && helperHandle != INVALID_HANDLE_VALUE)
    {
        probe.helperHandleMatches = GetProcessId(helperHandle) == g_revivalProcessId;
    }

    probe.helperBindingReady = probe.helperPidMatches || probe.helperHandleMatches;
    if (probe.helperBindingReady)
    {
        probe.ready = true;
        probe.source = "post_delay_binding";
    }

    return probe;
}

bool HasRuntimeReadySignal(const NetbridgeStatus* status)
{
    return EvaluateRuntimeReadyProbe(status).ready;
}

uint32_t BuildRuntimeReadyProbeMask(const RuntimeReadyProbe& probe)
{
    uint32_t mask = 0;
    if (probe.nativeSyncReady)
    {
        mask |= (1u << 0);
    }
    if (probe.localInitApplied)
    {
        mask |= (1u << 1);
    }
    if (probe.delayPromptSeen)
    {
        mask |= (1u << 2);
    }
    if (probe.delayInputApplied)
    {
        mask |= (1u << 3);
    }
    if (probe.sessionPointerValid)
    {
        mask |= (1u << 4);
    }
    if (probe.helperPidMatches)
    {
        mask |= (1u << 5);
    }
    if (probe.helperHandleMatches)
    {
        mask |= (1u << 6);
    }
    if (probe.ready)
    {
        mask |= (1u << 7);
    }
    return mask;
}

bool EnsureHostIpc()
{
    if (g_hostBlock != nullptr && g_hostInitEvent != nullptr && g_hostConsoleEvent != nullptr)
    {
        return true;
    }

    if (g_hostMapHandle == nullptr)
    {
        g_hostMapHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBlock), kSharedBlockName);
        if (g_hostMapHandle == nullptr)
        {
            mod::Log("Takeover: CreateFileMapping failed: %s", ErrorString(GetLastError()).c_str());
            return false;
        }
    }
    if (g_hostBlock == nullptr)
    {
        g_hostBlock = static_cast<SharedBlock*>(MapViewOfFile(g_hostMapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
        if (g_hostBlock == nullptr)
        {
            mod::Log("Takeover: MapViewOfFile failed: %s", ErrorString(GetLastError()).c_str());
            return false;
        }
    }

    if (g_hostInitEvent == nullptr)
    {
        g_hostInitEvent = CreateEventA(nullptr, FALSE, FALSE, kInitReadyEventName);
        if (g_hostInitEvent == nullptr)
        {
            return false;
        }
    }
    if (g_hostConsoleEvent == nullptr)
    {
        g_hostConsoleEvent = CreateEventA(nullptr, FALSE, FALSE, kConsoleReadyEventName);
        if (g_hostConsoleEvent == nullptr)
        {
            return false;
        }
    }

    g_hostBlock->magic = kIpcMagic;
    g_hostBlock->version = kIpcVersion;
    g_hostBlock->hostPid = GetCurrentProcessId();
    g_hostBlock->hostRevivalBase = static_cast<uint32_t>(g_hostRevivalBase);
    g_hostBlock->hostRevivalTimestamp =
        (g_hostRevivalBase != 0 && g_activeRevival != nullptr)
            ? g_activeRevival->peTimestamp
            : 0;
    LogRevival102jDeepStep("HostIpc.ensure_complete");
    return true;
}

void CloseHostIpc()
{
    LogRevival102jDeepStep("HostIpc.close_begin");
    if (g_hostBlock != nullptr)
    {
        UnmapViewOfFile(g_hostBlock);
        g_hostBlock = nullptr;
    }
    if (g_hostMapHandle != nullptr)
    {
        CloseHandle(g_hostMapHandle);
        g_hostMapHandle = nullptr;
    }
    if (g_hostInitEvent != nullptr)
    {
        CloseHandle(g_hostInitEvent);
        g_hostInitEvent = nullptr;
    }
    if (g_hostConsoleEvent != nullptr)
    {
        CloseHandle(g_hostConsoleEvent);
        g_hostConsoleEvent = nullptr;
    }
    g_hostRevivalBase = 0;
}

bool EnsureLocalRevivalLoaded()
{
    if (g_localInitFn != nullptr)
    {
        EnsureHostLogEfzIatPatched(true);
        PublishHostRevivalBase();
        if (!PatchRevivalErrorCodeNullGuard())
        {
            mod::Log("Takeover: warning failed to verify EfzRevival null-guard");
        }
        if (g_localRoleFlag < 0)
        {
            g_localRoleFlag = kLocalRoleLocalPlay;
        }
        return true;
    }

    if (g_localRevivalModule == nullptr)
    {
        g_localRevivalModule = GetModuleHandleA("EfzRevival.dll");
        if (g_localRevivalModule == nullptr)
        {
            g_localRevivalModule = LoadLibraryA("EfzRevival.dll");
        }
    }
    if (g_localRevivalModule == nullptr)
    {
        mod::Log("Takeover: LoadLibrary(EfzRevival.dll) failed");
        return false;
    }

    // Detect the DLL version BEFORE any profile-dependent operations.
    // Without this, InstallNetplayFrameHook() (called below) would use
    // the default 1.02e profile addresses which are wrong for 1.02g+.
    DetectRevivalVersion();

    PublishHostRevivalBase();
    if (!PatchRevivalErrorCodeNullGuard())
    {
        mod::Log("Takeover: warning failed to patch EfzRevival null-guard");
    }

    g_localInitFn = reinterpret_cast<RevivalInitFn>(GetProcAddress(g_localRevivalModule, "init"));
    if (g_localInitFn == nullptr)
    {
        mod::Log("Takeover: GetProcAddress(init) failed");
        return false;
    }

    // Patch logEfz interception before the first Revival init() call.
    // All supported Revival versions open logEfz.txt inside init(), so any
    // later patch point leaves behind a real-file handle we can no longer
    // safely take away from native code.
    EnsureHostLogEfzIatPatched(true);
    // Each Revival init() should start a fresh logical session section in
    // the managed root logEfz.txt. Closing here lets the first native log
    // line from this init reopen the file and emit a new SESSION header.
    CloseMirrorLogFiles();

    int localParams[2] = {2, 102};
    const int initResult = g_localInitFn(localParams);
    g_localRoleFlag = kLocalRoleLocalPlay;
    mod::Log("Takeover: local init(2,102) result=%d", initResult);

    if (!PatchRevivalDllExitProcess())
    {
        mod::Log("Takeover: warning - failed to patch EfzRevival ExitProcess IAT");
    }

    // Install a setjmp recovery wrapper around sub_1006E590 (the DLL's per-
    // frame dispatcher) so that NeutralizeExitProcess can longjmp back to
    // safety instead of freezing the main game thread during netplay exit.
    if (!InstallNetplayFrameHook())
    {
        mod::Log("Takeover: warning - failed to install netplay frame hook");
    }

    return true;
}

void ReinitLocalPlay()
{
    if (g_localRoleFlag == kLocalRoleLocalPlay)
    {
        mod::Log("Takeover: local re-init skipped (already local play)");
        return;
    }

    // Calling Revival init() during teardown/timeout paths can race with
    // in-flight audio/network cleanup and crash. Keep local role state in
    // local play mode without forcing another immediate init() call.
    // Also restore the DLL flag globals so other mods see local play.
    mod::Log(
        "Takeover: local re-init deferred (from role=%d -> local play, no init call)",
        g_localRoleFlag);
    SetRoleFlagDirect(kLocalRoleLocalPlay, "reinit_local_play");
    const bool titleDispatchOk =
        RestoreExeDispatchHookForTitle("ReinitLocalPlay");
    if (titleDispatchOk)
    {
        mod::Log("Takeover: local re-init restored 1.02j title dispatch");
    }
}

uintptr_t ResolveHostRevivalBase()
{
    if (g_localRevivalModule != nullptr)
    {
        return reinterpret_cast<uintptr_t>(g_localRevivalModule);
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr)
    {
        return reinterpret_cast<uintptr_t>(revival);
    }

    return 0;
}

bool PatchRevivalErrorCodeNullGuard()
{
    const uintptr_t base = ResolveHostRevivalBase();
    if (base == 0)
    {
        return false;
    }

    if (g_activeRevival == nullptr
        || g_activeRevival->errorCodeIsZeroRva == 0
        || g_activeRevival->errorCodeIsZeroPatchSize == 0)
    {
        mod::Log(
            "Takeover: null-guard patch not required for Revival version=%s",
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown");
        return true;
    }

    void* const stub = EnsureRevivalErrorCodeNullGuardStub();
    if (stub == nullptr)
    {
        return false;
    }

    uint8_t* const target = reinterpret_cast<uint8_t*>(base + g_activeRevival->errorCodeIsZeroRva);
    std::array<uint8_t, kMaxErrorCodePatchBytes> patchBytes = {};
    patchBytes[0] = 0xE9;
    const intptr_t delta = reinterpret_cast<uint8_t*>(stub) - (target + 5);
    const int32_t rel = static_cast<int32_t>(delta);
    std::memcpy(&patchBytes[1], &rel, sizeof(rel));
    patchBytes[5] = 0x90;
    patchBytes[6] = 0x90;
    patchBytes[7] = 0x90;

    if (g_revivalErrorCodeNullGuardPatched && g_revivalErrorCodeNullGuardPatchedBase == base)
    {
        uint8_t verify[kMaxErrorCodePatchBytes] = {};
        std::memcpy(verify, target, sizeof(verify));
        if (std::memcmp(verify, patchBytes.data(), sizeof(verify)) == 0)
        {
            return true;
        }
        mod::Log("Takeover: null-guard bytes changed, reapplying");
        g_revivalErrorCodeNullGuardPatched = false;
    }

    constexpr std::array<uint8_t, 8> kExpectedPrefix = {
        0x33, 0xC0, 0x39, 0x01, 0x0F, 0x94, 0xC0, 0xC3,
    };
    constexpr std::array<uint8_t, 8> kLegacyPatchedPrefix = {
        0x85, 0xC9, 0x74, 0x06, 0x83, 0x39, 0x00, 0x0F,
    };

    uint8_t current[kMaxErrorCodePatchBytes] = {};
    std::memcpy(current, target, sizeof(current));
    if (std::memcmp(current, patchBytes.data(), sizeof(current)) == 0)
    {
        g_revivalErrorCodeNullGuardPatched = true;
        g_revivalErrorCodeNullGuardPatchedBase = base;
        return true;
    }

    if (std::memcmp(current, kExpectedPrefix.data(), kExpectedPrefix.size()) != 0
        && std::memcmp(current, kLegacyPatchedPrefix.data(), kLegacyPatchedPrefix.size()) != 0)
    {
        mod::Log(
            "Takeover: skip null-guard patch at +0x%04lX (unexpected bytes %02X %02X %02X %02X)",
            static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva),
            static_cast<unsigned>(current[0]),
            static_cast<unsigned>(current[1]),
            static_cast<unsigned>(current[2]),
            static_cast<unsigned>(current[3]));
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(patchBytes), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        mod::Log(
            "Takeover: failed null-guard patch protect at +0x%04lX err=%s",
            static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva),
            ErrorString(GetLastError()).c_str());
        return false;
    }

    std::memcpy(target, patchBytes.data(), sizeof(patchBytes));
    (void)FlushInstructionCache(GetCurrentProcess(), target, sizeof(patchBytes));

    DWORD restoredProtect = 0;
    if (!VirtualProtect(target, sizeof(patchBytes), oldProtect, &restoredProtect))
    {
        mod::Log(
            "Takeover: null-guard patch restore protect failed at +0x%04lX err=%s",
            static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva),
            ErrorString(GetLastError()).c_str());
    }

    g_revivalErrorCodeNullGuardPatched = true;
    g_revivalErrorCodeNullGuardPatchedBase = base;
    mod::Log(
        "Takeover: patched EfzRevival.dll null-guard at +0x%04lX",
        static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva));
    return true;
}

void PublishHostRevivalBase()
{
    const uintptr_t base = ResolveHostRevivalBase();
    const uint32_t timestamp =
        (base != 0 && g_activeRevival != nullptr)
            ? g_activeRevival->peTimestamp
            : 0;
    g_hostRevivalBase = base;
    if (g_hostBlock != nullptr)
    {
        g_hostBlock->hostRevivalBase = static_cast<uint32_t>(base);
        g_hostBlock->hostRevivalTimestamp = timestamp;
    }
    if (base != 0)
    {
        mod::Log(
            "Takeover: host revival base=0x%08lX version=%s timestamp=0x%08X",
            static_cast<unsigned long>(base),
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown",
            static_cast<unsigned>(timestamp));
    }
}

uintptr_t ResolveInjectedExpectedRevivalBase()
{
    if (g_injectedBlock != nullptr && g_injectedBlock->hostRevivalBase != 0)
    {
        return static_cast<uintptr_t>(g_injectedBlock->hostRevivalBase);
    }

    TempIpcContext temp = {};
    uintptr_t base = 0;
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        base = static_cast<uintptr_t>(temp.block->hostRevivalBase);
    }
    CloseTempIpcContext(&temp);
    return base;
}

bool WriteIni(
    int role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni)
{
    std::wstring wideIniPath = GameDirectoryWide();
    if (!wideIniPath.empty())
    {
        wideIniPath += L"\\";
    }
    wideIniPath += L"EfzRevival.ini";

    std::string iniPath = WideToUtf8(wideIniPath);
    if (iniPath.empty())
    {
        iniPath = "EfzRevival.ini";
    }

    const DWORD existingAttrs = GetFileAttributesW(wideIniPath.c_str());
    const bool existed = (existingAttrs != INVALID_FILE_ATTRIBUTES);
    const char* safeAddress = (address != nullptr) ? address : "";
    const char* safeNickname = (nickname != nullptr) ? nickname : "";
    const bool hasNickname = (safeNickname[0] != '\0');
    bool wroteNickname = false;

    char portText[16] = {};
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    auto writeIniKeyUtf8 = [&wideIniPath, &iniPath](
                               const wchar_t* sectionW,
                               const wchar_t* keyW,
                               const char* sectionA,
                               const char* keyA,
                               const std::string& valueUtf8) -> bool {
        const std::wstring wideValue = Utf8ToWide(valueUtf8);
        if (!valueUtf8.empty() && wideValue.empty())
        {
            mod::Log(
                "Takeover: WriteIni key conversion failed section='%s' key='%s' value='%s' path='%s'",
                sectionA != nullptr ? sectionA : "",
                keyA != nullptr ? keyA : "",
                valueUtf8.c_str(),
                iniPath.c_str());
            return false;
        }

        if (WritePrivateProfileStringW(
                sectionW,
                keyW,
                wideValue.c_str(),
                wideIniPath.c_str())
            == FALSE)
        {
            mod::Log(
                "Takeover: WriteIni key failed section='%s' key='%s' value='%s' err=%s",
                sectionA != nullptr ? sectionA : "",
                keyA != nullptr ? keyA : "",
                valueUtf8.c_str(),
                ErrorString(GetLastError()).c_str());
            return false;
        }
        return true;
    };
    bool ok = true;

    if (writeNicknameToIni && hasNickname)
    {
        const bool nameWriteOk =
            writeIniKeyUtf8(L"Network", L"Name", "Network", "Name", safeNickname);
        wroteNickname = nameWriteOk;
        ok = nameWriteOk && ok;
    }
    else if (writeNicknameToIni)
    {
        mod::Log("Takeover: WriteIni preserving existing Network.Name (empty nickname supplied)");
    }
    else
    {
        mod::Log("Takeover: WriteIni preserving existing Network.Name (caller disabled nickname sync)");
    }

    // Keep user INI intact. Update only the settings currently supported by
    // InGameNetplay menu integration.
    bool wroteAddress = false;
    if (safeAddress[0] != '\0')
    {
        wroteAddress = writeIniKeyUtf8(
            L"Network",
            L"Address",
            "Network",
            "Address",
            safeAddress);
        ok = wroteAddress && ok;
    }
    ok = writeIniKeyUtf8(L"Network", L"Port", "Network", "Port", portText) && ok;

    mod::Log(
        "Takeover: WriteIni path='%s' existed=%d role=%d port=%u nickname='%s' address='%s' result=%d writeNicknameToIni=%d wroteNickname=%d wroteAddress=%d wrotePort=1",
        iniPath.c_str(),
        existed ? 1 : 0,
        role,
        static_cast<unsigned>(port),
        safeNickname,
        safeAddress,
        ok ? 1 : 0,
        writeNicknameToIni ? 1 : 0,
        wroteNickname ? 1 : 0,
        wroteAddress ? 1 : 0);
    return ok;
}

bool IsCurrentProcessRevival()
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
    {
        return false;
    }
    return BaseLower(path) == "efzrevival.exe";
}

bool IsRunningUnderWine()
{
    // Wine exposes wine_get_version() from ntdll.dll.  Probing for this
    // export is the canonical way to detect Wine at runtime.  This does
    // not exist on native Windows, so GetProcAddress returns nullptr.
    static int cached = -1;
    if (cached >= 0)
    {
        return cached != 0;
    }
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    cached = (ntdll != nullptr
              && GetProcAddress(ntdll, "wine_get_version") != nullptr) ? 1 : 0;
    if (cached != 0)
    {
        mod::Log("Platform: running under Wine/Proton");
    }
    return cached != 0;
}

void InitializeInjected()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    DetectRevivalVersion();
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (g_injectedMapHandle == nullptr)
        {
            g_injectedMapHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kSharedBlockName);
        }
        if (g_injectedMapHandle != nullptr && g_injectedBlock == nullptr)
        {
            g_injectedBlock = static_cast<SharedBlock*>(MapViewOfFile(g_injectedMapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
        }
        if (g_injectedInitEvent == nullptr)
        {
            g_injectedInitEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kInitReadyEventName);
        }
        if (g_injectedConsoleEvent == nullptr)
        {
            g_injectedConsoleEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kConsoleReadyEventName);
        }

        if (g_injectedBlock != nullptr && g_injectedInitEvent != nullptr && g_injectedConsoleEvent != nullptr)
        {
            break;
        }
        Sleep(10);
    }

    g_remoteThreadCallIndex = 0;
    g_startAbortRequested = 0;
    g_injectedLastConsoleSerialServed = 0;
    g_injectedLastConsoleAuxSerialServed = 0;
    g_injectedActiveConsoleAuxSerial = 0;
    g_injectedConsoleAuxScriptOffset = 0;
    g_injectedAutoConsoleFallbackCount = 0;
    g_injectedConsoleOutputHits = 0;
    g_injectedDelayPromptWaitStartTick = 0;
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_fakeProcessThreadHandle = nullptr;
    g_initCapturedFromWrite = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_injectedInitAddress = 0;
    g_injectedLazyBound = false;
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    ClearFakeThreads();
    ClearRedirectAllocations();
    g_redirectWriteBlockedHits = 0;
    g_injectedReady = (g_injectedBlock != nullptr && g_injectedInitEvent != nullptr && g_injectedConsoleEvent != nullptr);
    if (g_injectedReady)
    {
        DetectRevivalVersion();
        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival != nullptr)
        {
            const FARPROC initProc = GetProcAddress(revival, "init");
            g_injectedInitAddress = reinterpret_cast<uintptr_t>(initProc);
        }
    }
    mod::Log(
        "Takeover: injected initialized ready=%d block=0x%p init=0x%p console=0x%p initAddr=0x%p hostRevivalBase=0x%08lX hostRevivalTimestamp=0x%08X version=%s",
        g_injectedReady ? 1 : 0,
        g_injectedBlock,
        g_injectedInitEvent,
        g_injectedConsoleEvent,
        reinterpret_cast<void*>(g_injectedInitAddress),
        static_cast<unsigned long>(g_injectedBlock != nullptr ? g_injectedBlock->hostRevivalBase : 0),
        static_cast<unsigned>(g_injectedBlock != nullptr ? g_injectedBlock->hostRevivalTimestamp : 0),
        (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
            ? g_activeRevival->versionTag
            : "unknown");
    LogRevival102jDeepStep("HelperIpc.initialize_complete");
    InterlockedExchange(&g_injectedLazyBootstrapState, g_injectedReady ? 2 : 0);
}

void ShutdownInjected()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepStep("HelperIpc.shutdown_begin");
    ClearFakeThreads();
    ClearRedirectAllocations();

    if (g_injectedBlock != nullptr)
    {
        UnmapViewOfFile(g_injectedBlock);
        g_injectedBlock = nullptr;
    }
    if (g_injectedMapHandle != nullptr)
    {
        CloseHandle(g_injectedMapHandle);
        g_injectedMapHandle = nullptr;
    }
    if (g_injectedInitEvent != nullptr)
    {
        CloseHandle(g_injectedInitEvent);
        g_injectedInitEvent = nullptr;
    }
    if (g_injectedConsoleEvent != nullptr)
    {
        CloseHandle(g_injectedConsoleEvent);
        g_injectedConsoleEvent = nullptr;
    }
    g_injectedReady = false;
    g_injectedInitAddress = 0;
    g_injectedLazyBound = false;
    g_startAbortRequested = 0;
    g_injectedLastConsoleSerialServed = 0;
    g_injectedLastConsoleAuxSerialServed = 0;
    g_injectedActiveConsoleAuxSerial = 0;
    g_injectedConsoleAuxScriptOffset = 0;
    g_injectedAutoConsoleFallbackCount = 0;
    g_injectedConsoleOutputHits = 0;
    g_injectedDelayPromptWaitStartTick = 0;
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_fakeProcessThreadHandle = nullptr;
    g_initCapturedFromWrite = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_localRoleFlag = -1;
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    InterlockedExchange(&g_injectedLazyBootstrapState, 0);
    mod::Log("Takeover: injected shutdown");
}

} // namespace netplay::bridge::takeover
