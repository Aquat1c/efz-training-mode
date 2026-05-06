#include "../include/utils/crash_handler.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <eh.h>
#include <exception>
#include <fstream>
#include <intrin.h>
#include <stdlib.h>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace CrashHandler {

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE,
                                          DWORD,
                                          HANDLE,
                                          MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

LONG g_installed = 0;
LONG g_handlingCrash = 0;
HMODULE g_selfModule = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
PVOID g_vectoredHandler = nullptr;
char g_debugLogPath[MAX_PATH] = {0};
char g_crashLogPath[MAX_PATH] = {0};
char g_dumpDirectory[MAX_PATH] = {0};
LONG g_efzSymbolMapLoadState = 0;
LONG g_dbgHelpSymbolState = 0;
char g_dbgHelpSearchPath[2048] = {0};

constexpr uintptr_t kEfzImageBase = 0x00400000;
constexpr unsigned char kEfzSymbolPathXorKey = 0x5A;
constexpr LONG kEfzSymbolMapStateNotStarted = 0;
constexpr LONG kEfzSymbolMapStateLoading = 1;
constexpr LONG kEfzSymbolMapStateReady = 2;
constexpr LONG kEfzSymbolMapStateUnavailable = 3;
constexpr LONG kDbgHelpStateNotStarted = 0;
constexpr LONG kDbgHelpStateReady = 1;
constexpr LONG kDbgHelpStateUnavailable = 2;
constexpr DWORD kSyntheticInvalidParameterCode = 0xE0001001;
constexpr DWORD kSyntheticPureCallCode = 0xE0001002;
constexpr DWORD kSyntheticTerminateCode = 0xE0001003;
constexpr DWORD kSyntheticSignalBaseCode = 0xE0001100;
const unsigned char kEfzSymbolMapPathBytes[] = {
    0x3E, 0x60, 0x06, 0x3E, 0x3F, 0x2C, 0x06, 0x1F, 0x1C, 0x00, 0x05, 0x37,
    0x35, 0x3E, 0x3E, 0x33, 0x34, 0x3D, 0x06, 0x37, 0x35, 0x3E, 0x05, 0x2A,
    0x28, 0x35, 0x30, 0x3F, 0x39, 0x2E, 0x29, 0x06, 0x3F, 0x3C, 0x20, 0x77,
    0x2E, 0x28, 0x3B, 0x33, 0x34, 0x33, 0x34, 0x3D, 0x77, 0x37, 0x35, 0x3E,
    0x3F, 0x06, 0x3E, 0x3F, 0x39, 0x35, 0x37, 0x2A, 0x33, 0x36, 0x3B, 0x2E,
    0x33, 0x35, 0x34, 0x29, 0x06, 0x3F, 0x3C, 0x20, 0x06, 0x3F, 0x3C, 0x20,
    0x05, 0x37, 0x3F, 0x37, 0x35, 0x28, 0x33, 0x3B, 0x36, 0x05, 0x36, 0x3B,
    0x2E, 0x3F, 0x29, 0x2E, 0x74, 0x39,
};

struct EfzFunctionEntry {
    uintptr_t rva;
    std::string name;
};

std::vector<EfzFunctionEntry> g_efzFunctionEntries;

LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* exceptionPointers);
LONG CALLBACK VectoredCrashHandler(EXCEPTION_POINTERS* exceptionPointers);

bool LoadEfzSymbolMapBestEffort();
bool LookupEfzFunction(uintptr_t address, char* output, size_t outputSize);
bool TryLookupDebugSymbol(uintptr_t address, char* output, size_t outputSize);
bool DecodeEfzSymbolMapPath(char* output, size_t outputSize);
void FormatAddress(char* output, size_t outputSize, uintptr_t address);
void WriteCrashReport(EXCEPTION_POINTERS* exceptionPointers, const char* source, const char* detail);
void ReportSyntheticCrash(const char* source, DWORD code, uintptr_t preferredIp, const char* detail);
void __cdecl InvalidParameterCrashHandler(const wchar_t* expression,
                                          const wchar_t* function,
                                          const wchar_t* file,
                                          unsigned int line,
                                          uintptr_t reserved);
void __cdecl PureCallCrashHandler();
void __cdecl TerminateCrashHandler();
void __cdecl SignalCrashHandler(int signalNumber);
DWORD WINAPI SymbolWarmupThreadProc(LPVOID param);

bool IsCrashCandidateException(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case kSyntheticInvalidParameterCode:
    case kSyntheticPureCallCode:
    case kSyntheticTerminateCode:
        return true;
    default:
        return false;
    }
}

bool DirectoryExists(const char* path) {
    if (!path || !*path) {
        return false;
    }

    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

const char* BaseName(const char* path) {
    if (!path || !*path) {
        return "<unknown>";
    }

    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    return base;
}

void InitializePaths(HMODULE module) {
    char modulePath[MAX_PATH] = {0};
    HMODULE resolvedModule = module;
    if (!resolvedModule) {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&InitializePaths),
                           &resolvedModule);
    }

    if (!resolvedModule || !GetModuleFileNameA(resolvedModule, modulePath, MAX_PATH)) {
        lstrcpynA(g_debugLogPath, "efz_training_debug.log", MAX_PATH);
        lstrcpynA(g_crashLogPath, "efz_training_crash.log", MAX_PATH);
        g_dumpDirectory[0] = '\0';
        return;
    }

    char* slash = modulePath + lstrlenA(modulePath);
    while (slash > modulePath && *slash != '\\' && *slash != '/') {
        --slash;
    }
    if (*slash == '\\' || *slash == '/') {
        *slash = '\0';
    }

    lstrcpynA(g_dumpDirectory, modulePath, MAX_PATH);
    _snprintf_s(g_debugLogPath, MAX_PATH, _TRUNCATE, "%s\\efz_training_debug.log", modulePath);
    _snprintf_s(g_crashLogPath, MAX_PATH, _TRUNCATE, "%s\\efz_training_crash.log", modulePath);
}

void AppendLineToFile(const char* path, const char* line) {
    if (!path || !*path || !line) {
        return;
    }

    HANDLE file = CreateFileA(path,
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(lstrlenA(line)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void AppendLine(const char* line) {
    AppendLineToFile(g_crashLogPath, line);
    AppendLineToFile(g_debugLogPath, line);
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

void AppendFormat(const char* format, ...) {
    char buffer[2048] = {0};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    AppendLine(buffer);
}

const char* ExceptionCodeName(DWORD code) {
    switch (code) {
    case kSyntheticInvalidParameterCode: return "CRT_INVALID_PARAMETER";
    case kSyntheticPureCallCode: return "CRT_PURECALL";
    case kSyntheticTerminateCode: return "CPP_TERMINATE";
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP: return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    default: return "UNKNOWN_EXCEPTION";
    }
}

const char* SignalName(int signalNumber) {
    switch (signalNumber) {
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGINT: return "SIGINT";
    case SIGSEGV: return "SIGSEGV";
    case SIGTERM: return "SIGTERM";
    default: return "SIGUNKNOWN";
    }
}

bool TryReadMemory(const void* address, void* buffer, size_t size) {
    if (!address || !buffer || size == 0) {
        return false;
    }

    __try {
        memcpy(buffer, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsPathSeparator(char ch) {
    return ch == '\\' || ch == '/';
}

const char* MemoryStateName(DWORD state) {
    switch (state) {
    case MEM_COMMIT: return "MEM_COMMIT";
    case MEM_FREE: return "MEM_FREE";
    case MEM_RESERVE: return "MEM_RESERVE";
    default: return "MEM_UNKNOWN";
    }
}

const char* MemoryTypeName(DWORD type) {
    switch (type) {
    case MEM_IMAGE: return "MEM_IMAGE";
    case MEM_MAPPED: return "MEM_MAPPED";
    case MEM_PRIVATE: return "MEM_PRIVATE";
    case 0: return "MEM_NONE";
    default: return "MEM_OTHER";
    }
}

bool IsExecutableProtection(DWORD protection) {
    switch (protection & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

void FormatProtectionFlags(char* output, size_t outputSize, DWORD protection) {
    if (!output || outputSize == 0) {
        return;
    }

    const char* base = "UNKNOWN";
    switch (protection & 0xFF) {
    case 0: base = "NONE"; break;
    case PAGE_NOACCESS: base = "NOACCESS"; break;
    case PAGE_READONLY: base = "READONLY"; break;
    case PAGE_READWRITE: base = "READWRITE"; break;
    case PAGE_WRITECOPY: base = "WRITECOPY"; break;
    case PAGE_EXECUTE: base = "EXECUTE"; break;
    case PAGE_EXECUTE_READ: base = "EXECUTE_READ"; break;
    case PAGE_EXECUTE_READWRITE: base = "EXECUTE_READWRITE"; break;
    case PAGE_EXECUTE_WRITECOPY: base = "EXECUTE_WRITECOPY"; break;
    }

    _snprintf_s(output, outputSize, _TRUNCATE, "%s", base);
    const size_t currentLength = lstrlenA(output);
    if ((protection & PAGE_GUARD) && currentLength < outputSize) {
        _snprintf_s(output + currentLength, outputSize - currentLength, _TRUNCATE, "|GUARD");
    }
    if (protection & PAGE_NOCACHE) {
        const size_t len = lstrlenA(output);
        if (len < outputSize) {
            _snprintf_s(output + len, outputSize - len, _TRUNCATE, "|NOCACHE");
        }
    }
    if (protection & PAGE_WRITECOMBINE) {
        const size_t len = lstrlenA(output);
        if (len < outputSize) {
            _snprintf_s(output + len, outputSize - len, _TRUNCATE, "|WRITECOMBINE");
        }
    }
}

void WideToAnsiLossy(const wchar_t* value, char* output, size_t outputSize) {
    if (!output || outputSize == 0) {
        return;
    }

    output[0] = '\0';
    if (!value || !*value) {
        return;
    }

    if (WideCharToMultiByte(CP_ACP, 0, value, -1, output, static_cast<int>(outputSize), "?", nullptr) == 0) {
        _snprintf_s(output, outputSize, _TRUNCATE, "<wide-conversion-failed>");
    }
}

bool FileExists(const char* path) {
    if (!path || !*path) {
        return false;
    }

    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool CopyDirectoryName(char* output, size_t outputSize, const char* path) {
    if (!output || outputSize == 0 || !path || !*path) {
        return false;
    }

    output[0] = '\0';
    _snprintf_s(output, outputSize, _TRUNCATE, "%s", path);
    char* slash = output + lstrlenA(output);
    while (slash > output && *slash != '\\' && *slash != '/') {
        --slash;
    }
    if (*slash != '\\' && *slash != '/') {
        output[0] = '\0';
        return false;
    }
    *slash = '\0';
    return output[0] != '\0';
}

bool CopyProjectRootFromEfzSymbolMap(char* output, size_t outputSize) {
    char symbolPath[MAX_PATH] = {0};
    char level1[MAX_PATH] = {0};
    char level2[MAX_PATH] = {0};
    char level3[MAX_PATH] = {0};
    if (!DecodeEfzSymbolMapPath(symbolPath, sizeof(symbolPath))
        || !CopyDirectoryName(level1, sizeof(level1), symbolPath)
        || !CopyDirectoryName(level2, sizeof(level2), level1)
        || !CopyDirectoryName(level3, sizeof(level3), level2)) {
        return false;
    }

    _snprintf_s(output, outputSize, _TRUNCATE, "%s", level3);
    return DirectoryExists(output);
}

void AppendSymbolSearchDirectory(char* searchPath, size_t searchPathSize, const char* candidate) {
    if (!searchPath || searchPathSize == 0 || !candidate || !*candidate || !DirectoryExists(candidate)) {
        return;
    }

    if (searchPath[0] != '\0') {
        _snprintf_s(searchPath + lstrlenA(searchPath),
                    searchPathSize - lstrlenA(searchPath),
                    _TRUNCATE,
                    ";");
    }
    _snprintf_s(searchPath + lstrlenA(searchPath),
                searchPathSize - lstrlenA(searchPath),
                _TRUNCATE,
                "%s",
                candidate);
}

bool EnsureDbgHelpSymbolsReady() {
    const LONG state = InterlockedCompareExchange(&g_dbgHelpSymbolState, 0, 0);
    if (state == kDbgHelpStateReady) {
        return true;
    }
    if (state == kDbgHelpStateUnavailable) {
        return false;
    }

    char searchPath[2048] = {0};
    if (g_dumpDirectory[0]) {
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), g_dumpDirectory);
    }

    char cwd[MAX_PATH] = {0};
    if (GetCurrentDirectoryA(MAX_PATH, cwd) != 0) {
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), cwd);
    }

    char repoRoot[MAX_PATH] = {0};
    if (CopyProjectRootFromEfzSymbolMap(repoRoot, sizeof(repoRoot))) {
        char candidate[MAX_PATH] = {0};
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\build_xp_win32", repoRoot);
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), candidate);
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\build_xp_win32\\bin\\Release", repoRoot);
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), candidate);
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\build_xp_win32\\Release", repoRoot);
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), candidate);
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\build", repoRoot);
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), candidate);
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\build\\bin\\Release", repoRoot);
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), candidate);
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE, "%s\\build\\Release", repoRoot);
        AppendSymbolSearchDirectory(searchPath, sizeof(searchPath), candidate);
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitialize(GetCurrentProcess(), searchPath[0] ? searchPath : nullptr, TRUE)) {
        InterlockedExchange(&g_dbgHelpSymbolState, kDbgHelpStateUnavailable);
        return false;
    }

    lstrcpynA(g_dbgHelpSearchPath, searchPath, static_cast<int>(sizeof(g_dbgHelpSearchPath)));
    InterlockedExchange(&g_dbgHelpSymbolState, kDbgHelpStateReady);
    return true;
}

bool TryLookupDebugSymbol(uintptr_t address, char* output, size_t outputSize) {
    if (!output || outputSize == 0) {
        return false;
    }

    output[0] = '\0';
    if (!EnsureDbgHelpSymbolsReady()) {
        return false;
    }

    unsigned char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (!SymFromAddr(GetCurrentProcess(), static_cast<DWORD64>(address), &displacement, symbol)) {
        return false;
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    const BOOL haveLine = SymGetLineFromAddr64(GetCurrentProcess(),
                                               static_cast<DWORD64>(address),
                                               &lineDisplacement,
                                               &line) != FALSE;

    char undecorated[MAX_SYM_NAME] = {0};
    const DWORD undecoratedLength = UnDecorateSymbolName(symbol->Name,
                                                         undecorated,
                                                         MAX_SYM_NAME,
                                                         UNDNAME_COMPLETE);
    const char* name = undecoratedLength != 0 ? undecorated : symbol->Name;

    if (haveLine && line.FileName && *line.FileName) {
        _snprintf_s(output,
                    outputSize,
                    _TRUNCATE,
                    "%s+0x%08lX @ %s:%lu",
                    name,
                    static_cast<unsigned long>(displacement),
                    BaseName(line.FileName),
                    static_cast<unsigned long>(line.LineNumber));
    } else {
        _snprintf_s(output,
                    outputSize,
                    _TRUNCATE,
                    "%s+0x%08lX",
                    name,
                    static_cast<unsigned long>(displacement));
    }

    return true;
}

bool DecodeEfzSymbolMapPath(char* output, size_t outputSize) {
    if (!output || outputSize <= sizeof(kEfzSymbolMapPathBytes)) {
        return false;
    }

    for (size_t i = 0; i < sizeof(kEfzSymbolMapPathBytes); ++i) {
        output[i] = static_cast<char>(kEfzSymbolMapPathBytes[i] ^ kEfzSymbolPathXorKey);
    }
    output[sizeof(kEfzSymbolMapPathBytes)] = '\0';
    return true;
}

bool ParseHexAddressLine(const std::string& line, uintptr_t* addressOut) {
    if (!addressOut) {
        return false;
    }

    const size_t open = line.find('(');
    const size_t close = line.find(')', open == std::string::npos ? 0 : open + 1);
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return false;
    }

    const std::string token = line.substr(open + 1, close - open - 1);
    if (token.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(token.c_str(), &end, 16);
    if (!end || *end != '\0') {
        return false;
    }

    *addressOut = static_cast<uintptr_t>(parsed);
    return true;
}

bool ExtractCommentLabel(const std::string& line, const char* prefix, std::string* out) {
    if (!prefix || !out) {
        return false;
    }

    const size_t position = line.find(prefix);
    if (position == std::string::npos) {
        return false;
    }

    std::string value = line.substr(position + std::strlen(prefix));
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    if (value.empty()) {
        return false;
    }

    *out = value;
    return true;
}

bool IsGenericFunctionName(const std::string& value) {
    return value.rfind("sub_", 0) == 0
        || value.rfind("nullsub_", 0) == 0
        || value == "StartAddress";
}

std::string ExtractSignatureFunctionName(const std::string& line) {
    if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '/' || line[0] == '*' || line[0] == '#') {
        return std::string();
    }

    if (line.find(';') != std::string::npos || line.find('=') != std::string::npos) {
        return std::string();
    }

    const size_t openParen = line.find('(');
    if (openParen == std::string::npos || openParen == 0) {
        return std::string();
    }

    const std::string leadingToken = line.substr(0, openParen);
    if (leadingToken == "if " || leadingToken == "while " || leadingToken == "switch " || leadingToken == "for ") {
        return std::string();
    }

    size_t nameEnd = openParen;
    while (nameEnd > 0 && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t' || line[nameEnd - 1] == '*')) {
        --nameEnd;
    }
    if (nameEnd == 0) {
        return std::string();
    }

    size_t nameStart = nameEnd;
    while (nameStart > 0) {
        const char ch = line[nameStart - 1];
        const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
        if (!valid) {
            break;
        }
        --nameStart;
    }
    if (nameStart == nameEnd) {
        return std::string();
    }

    return line.substr(nameStart, nameEnd - nameStart);
}

void MaybeUpdateBestFunctionName(std::string* bestName, int* bestPriority, const std::string& candidate, int priority) {
    if (!bestName || !bestPriority || candidate.empty()) {
        return;
    }

    if (priority > *bestPriority) {
        *bestName = candidate;
        *bestPriority = priority;
    }
}

bool LoadEfzFunctionMapFromFile(const char* path) {
    if (!path || !*path) {
        return false;
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    std::vector<EfzFunctionEntry> parsedEntries;
    parsedEntries.reserve(1536);

    uintptr_t currentAddress = 0;
    std::string bestName;
    int bestPriority = 0;
    bool hasCurrentAddress = false;

    auto finalizeCurrent = [&]() {
        if (!hasCurrentAddress || currentAddress < kEfzImageBase) {
            hasCurrentAddress = false;
            currentAddress = 0;
            bestName.clear();
            bestPriority = 0;
            return;
        }

        if (bestName.empty()) {
            char fallback[32] = {0};
            _snprintf_s(fallback, sizeof(fallback), _TRUNCATE, "sub_%08lX", static_cast<unsigned long>(currentAddress));
            bestName = fallback;
        }

        parsedEntries.push_back({currentAddress - kEfzImageBase, bestName});
        hasCurrentAddress = false;
        currentAddress = 0;
        bestName.clear();
        bestPriority = 0;
    };

    std::string line;
    while (std::getline(input, line)) {
        uintptr_t parsedAddress = 0;
        if (line.rfind("//----- (", 0) == 0 && ParseHexAddressLine(line, &parsedAddress)) {
            finalizeCurrent();
            hasCurrentAddress = true;
            currentAddress = parsedAddress;

            std::string inlineComment;
            if (ExtractCommentLabel(line, "// Original name:", &inlineComment)
                || ExtractCommentLabel(line, "// Function name:", &inlineComment)
                || ExtractCommentLabel(line, "// New name:", &inlineComment)
                || ExtractCommentLabel(line, "// Old name:", &inlineComment)) {
                const int priority = IsGenericFunctionName(inlineComment) ? 1 : 3;
                MaybeUpdateBestFunctionName(&bestName, &bestPriority, inlineComment, priority);
            }
            continue;
        }

        if (!hasCurrentAddress) {
            continue;
        }

        std::string commentName;
        if (ExtractCommentLabel(line, "// New name:", &commentName) || ExtractCommentLabel(line, "// Old name:", &commentName)) {
            MaybeUpdateBestFunctionName(&bestName, &bestPriority, commentName, 4);
            continue;
        }
        if (ExtractCommentLabel(line, "// Function name:", &commentName) || ExtractCommentLabel(line, "// Original name:", &commentName)) {
            const int priority = IsGenericFunctionName(commentName) ? 1 : 2;
            MaybeUpdateBestFunctionName(&bestName, &bestPriority, commentName, priority);
            continue;
        }

        const std::string signatureName = ExtractSignatureFunctionName(line);
        if (!signatureName.empty()) {
            const int priority = IsGenericFunctionName(signatureName) ? 1 : 5;
            MaybeUpdateBestFunctionName(&bestName, &bestPriority, signatureName, priority);
        }
    }

    finalizeCurrent();

    if (parsedEntries.empty()) {
        return false;
    }

    std::sort(parsedEntries.begin(), parsedEntries.end(), [](const EfzFunctionEntry& left, const EfzFunctionEntry& right) {
        return left.rva < right.rva;
    });

    g_efzFunctionEntries.swap(parsedEntries);
    return true;
}

bool LoadEfzSymbolMapBestEffort() {
    bool loaded = false;
    char resolvedPath[MAX_PATH] = {0};
    if (DecodeEfzSymbolMapPath(resolvedPath, sizeof(resolvedPath)) && FileExists(resolvedPath)) {
        loaded = LoadEfzFunctionMapFromFile(resolvedPath);
    }

    if (loaded) {
        if (InterlockedCompareExchange(&g_handlingCrash, 0, 0) != 0) {
            AppendFormat("[CRASH] Loaded EFZ symbol map (%lu entries)",
                         static_cast<unsigned long>(g_efzFunctionEntries.size()));
        }
    } else {
        if (InterlockedCompareExchange(&g_handlingCrash, 0, 0) != 0) {
            AppendLine("[CRASH] EFZ symbol map unavailable; crash logs will use module+offset only");
        }
    }
    return loaded;
}

bool LookupEfzFunction(uintptr_t address, char* output, size_t outputSize) {
    if (!output || outputSize == 0) {
        return false;
    }

    output[0] = '\0';
    if (InterlockedCompareExchange(&g_efzSymbolMapLoadState, 0, 0) != kEfzSymbolMapStateReady || g_efzFunctionEntries.empty()) {
        return false;
    }

    HMODULE exeModule = GetModuleHandleA(nullptr);
    if (!exeModule) {
        return false;
    }

    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(exeModule);
    if (address < exeBase) {
        return false;
    }

    const uintptr_t rva = address - exeBase;
    auto it = std::upper_bound(g_efzFunctionEntries.begin(), g_efzFunctionEntries.end(), rva,
                               [](uintptr_t value, const EfzFunctionEntry& entry) {
                                   return value < entry.rva;
                               });
    if (it == g_efzFunctionEntries.begin()) {
        return false;
    }

    --it;
    const uintptr_t delta = rva - it->rva;
    if (delta == 0) {
        _snprintf_s(output, outputSize, _TRUNCATE, "%s", it->name.c_str());
    } else {
        _snprintf_s(output, outputSize, _TRUNCATE, "%s+0x%08lX", it->name.c_str(), static_cast<unsigned long>(delta));
    }
    return true;
}

DWORD WINAPI SymbolWarmupThreadProc(LPVOID param) {
    (void)param;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const bool loaded = LoadEfzSymbolMapBestEffort();
    InterlockedExchange(&g_efzSymbolMapLoadState,
                        loaded ? kEfzSymbolMapStateReady : kEfzSymbolMapStateUnavailable);
    return 0;
}

void LogAddressMemoryDetails(const char* label, uintptr_t address) {
    char addressInfo[256] = {0};
    FormatAddress(addressInfo, sizeof(addressInfo), address);

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!address || !VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) {
        AppendFormat("[CRASH] %s: %s region=<unavailable>", label, addressInfo);
        return;
    }

    char protect[64] = {0};
    char allocProtect[64] = {0};
    char modulePath[MAX_PATH] = {0};
    FormatProtectionFlags(protect, sizeof(protect), mbi.Protect);
    FormatProtectionFlags(allocProtect, sizeof(allocProtect), mbi.AllocationProtect);
    if (!mbi.AllocationBase || !GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modulePath, MAX_PATH)) {
        lstrcpynA(modulePath, "<no-module>", MAX_PATH);
    }

    AppendFormat("[CRASH] %s: %s regionBase=0x%08lX allocBase=0x%08lX size=0x%08lX state=%s protect=%s allocProtect=%s type=%s module=%s",
                 label,
                 addressInfo,
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)),
                 static_cast<unsigned long>(mbi.RegionSize),
                 MemoryStateName(mbi.State),
                 protect,
                 allocProtect,
                 MemoryTypeName(mbi.Type),
                 modulePath);
}

bool QueryAddressModule(uintptr_t address, MEMORY_BASIC_INFORMATION* mbiOut, char* modulePath, size_t modulePathSize) {
    if (mbiOut) {
        std::memset(mbiOut, 0, sizeof(*mbiOut));
    }
    if (modulePath && modulePathSize > 0) {
        modulePath[0] = '\0';
    }
    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) {
        return false;
    }

    if (mbiOut) {
        *mbiOut = mbi;
    }
    if (modulePath && modulePathSize > 0 && mbi.AllocationBase) {
        GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modulePath, static_cast<DWORD>(modulePathSize));
    }
    return true;
}

bool IsWindowsSystemPath(const char* path) {
    if (!path || !*path) {
        return false;
    }
    return _strnicmp(path, "C:\\Windows\\", 11) == 0;
}

const char* FaultTargetSummary(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State == MEM_FREE) {
        return "free memory";
    }
    if (mbi.State == MEM_RESERVE) {
        return "reserved but uncommitted memory";
    }
    if (mbi.State != MEM_COMMIT) {
        return "non-committed memory";
    }
    if ((mbi.Protect & 0xFF) == PAGE_NOACCESS) {
        return "committed no-access memory";
    }
    if (mbi.Protect & PAGE_GUARD) {
        return "guard page memory";
    }
    if (mbi.Type == MEM_IMAGE) {
        return "image-backed memory";
    }
    if (mbi.Type == MEM_PRIVATE) {
        return "private heap/stack memory";
    }
    if (mbi.Type == MEM_MAPPED) {
        return "mapped memory";
    }
    return "memory region";
}

bool DescribeFirstNonSystemFrameFromChain(uintptr_t framePointer, char* output, size_t outputSize) {
    if (!output || outputSize == 0) {
        return false;
    }

    output[0] = '\0';
    uintptr_t current = framePointer;
    for (int depth = 0; depth < 32 && current; ++depth) {
        uintptr_t next = 0;
        uintptr_t ret = 0;
        if (!TryReadMemory(reinterpret_cast<const void*>(current), &next, sizeof(next))
            || !TryReadMemory(reinterpret_cast<const void*>(current + sizeof(uintptr_t)), &ret, sizeof(ret))) {
            break;
        }

        MEMORY_BASIC_INFORMATION mbi = {};
        char modulePath[MAX_PATH] = {0};
        if (QueryAddressModule(ret, &mbi, modulePath, sizeof(modulePath))
            && mbi.AllocationBase
            && modulePath[0]
            && !IsWindowsSystemPath(modulePath)) {
            char addressInfo[256] = {0};
            FormatAddress(addressInfo, sizeof(addressInfo), ret);
            _snprintf_s(output,
                        outputSize,
                        _TRUNCATE,
                        "frame #%02d %s",
                        depth,
                        addressInfo);
            return true;
        }

        if (next <= current || (next - current) > 0x100000) {
            break;
        }
        current = next;
    }

    return false;
}

void LogCrashAnalysisSummary(const EXCEPTION_POINTERS* exceptionPointers) {
    if (!exceptionPointers || !exceptionPointers->ExceptionRecord) {
        return;
    }

    const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
    char exceptionAddressInfo[256] = {0};
    FormatAddress(exceptionAddressInfo,
                  sizeof(exceptionAddressInfo),
                  reinterpret_cast<uintptr_t>(record->ExceptionAddress));
    AppendFormat("[CRASH] Analysis: exception raised at %s", exceptionAddressInfo);

    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
        && record->NumberParameters >= 2) {
        const char* op = "access";
        if (record->ExceptionInformation[0] == 0) op = "read";
        else if (record->ExceptionInformation[0] == 1) op = "write";
        else if (record->ExceptionInformation[0] == 8) op = "execute";

        MEMORY_BASIC_INFORMATION mbi = {};
        char targetInfo[256] = {0};
        FormatAddress(targetInfo,
                      sizeof(targetInfo),
                      static_cast<uintptr_t>(record->ExceptionInformation[1]));
        if (QueryAddressModule(static_cast<uintptr_t>(record->ExceptionInformation[1]), &mbi, nullptr, 0)) {
            AppendFormat("[CRASH] Analysis: %s touched %s at %s",
                         op,
                         FaultTargetSummary(mbi),
                         targetInfo);
        } else {
            AppendFormat("[CRASH] Analysis: %s touched unknown memory at %s", op, targetInfo);
        }
    }

    char probableFrame[256] = {0};
#if defined(_M_IX86)
    if (exceptionPointers->ContextRecord
        && DescribeFirstNonSystemFrameFromChain(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Ebp), probableFrame, sizeof(probableFrame))) {
        AppendFormat("[CRASH] Analysis: first non-system frame = %s", probableFrame);
    }
#elif defined(_M_X64)
    if (exceptionPointers->ContextRecord
        && DescribeFirstNonSystemFrameFromChain(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rbp), probableFrame, sizeof(probableFrame))) {
        AppendFormat("[CRASH] Analysis: first non-system frame = %s", probableFrame);
    }
#endif
}

bool IsModuleBackedExecutableAddress(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!address || !VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || !IsExecutableProtection(mbi.Protect) || !mbi.AllocationBase) {
        return false;
    }

    char modulePath[MAX_PATH] = {0};
    return GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modulePath, MAX_PATH) != 0;
}

void LogRegisterPointerHint(const char* name, uintptr_t value) {
    if (!value) {
        return;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(value), &mbi, sizeof(mbi))) {
        return;
    }

    char addressInfo[256] = {0};
    FormatAddress(addressInfo, sizeof(addressInfo), value);
    AppendFormat("[CRASH] Register %s -> %s", name, addressInfo);
}

void LogRegisterPointerHints(const CONTEXT* context) {
    if (!context) {
        return;
    }

    AppendLine("[CRASH] Register address hints:");
#if defined(_M_IX86)
    LogRegisterPointerHint("EAX", static_cast<uintptr_t>(context->Eax));
    LogRegisterPointerHint("EBX", static_cast<uintptr_t>(context->Ebx));
    LogRegisterPointerHint("ECX", static_cast<uintptr_t>(context->Ecx));
    LogRegisterPointerHint("EDX", static_cast<uintptr_t>(context->Edx));
    LogRegisterPointerHint("ESI", static_cast<uintptr_t>(context->Esi));
    LogRegisterPointerHint("EDI", static_cast<uintptr_t>(context->Edi));
    LogRegisterPointerHint("EBP", static_cast<uintptr_t>(context->Ebp));
    LogRegisterPointerHint("ESP", static_cast<uintptr_t>(context->Esp));
    LogRegisterPointerHint("EIP", static_cast<uintptr_t>(context->Eip));
#elif defined(_M_X64)
    LogRegisterPointerHint("RAX", static_cast<uintptr_t>(context->Rax));
    LogRegisterPointerHint("RBX", static_cast<uintptr_t>(context->Rbx));
    LogRegisterPointerHint("RCX", static_cast<uintptr_t>(context->Rcx));
    LogRegisterPointerHint("RDX", static_cast<uintptr_t>(context->Rdx));
    LogRegisterPointerHint("RSI", static_cast<uintptr_t>(context->Rsi));
    LogRegisterPointerHint("RDI", static_cast<uintptr_t>(context->Rdi));
    LogRegisterPointerHint("RBP", static_cast<uintptr_t>(context->Rbp));
    LogRegisterPointerHint("RSP", static_cast<uintptr_t>(context->Rsp));
    LogRegisterPointerHint("RIP", static_cast<uintptr_t>(context->Rip));
#endif
}

void LogStackCandidates(uintptr_t stackPointer) {
    if (!stackPointer) {
        return;
    }

    AppendLine("[CRASH] Stack return-address candidates:");
    int found = 0;
    for (int i = 0; i < 128 && found < 32; ++i) {
        const uintptr_t slot = stackPointer + static_cast<uintptr_t>(i * sizeof(uintptr_t));
        uintptr_t value = 0;
        if (!TryReadMemory(reinterpret_cast<const void*>(slot), &value, sizeof(value))) {
            break;
        }
        if (!IsModuleBackedExecutableAddress(value)) {
            continue;
        }

        char valueInfo[256] = {0};
        FormatAddress(valueInfo, sizeof(valueInfo), value);
        AppendFormat("[CRASH]   candidate[%02d] SP+0x%03X @0x%08lX -> %s",
                     found,
                     static_cast<unsigned int>(i * sizeof(uintptr_t)),
                     static_cast<unsigned long>(slot),
                     valueInfo);
        ++found;
    }

    if (found == 0) {
        AppendLine("[CRASH]   <no module-backed executable addresses found in scan window>");
    }
}

void LogLoadedModules() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        AppendFormat("[CRASH] Module snapshot failed (error=%lu)", static_cast<unsigned long>(GetLastError()));
        return;
    }

    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    AppendLine("[CRASH] Loaded modules:");
    if (!Module32First(snapshot, &entry)) {
        AppendFormat("[CRASH] Module32First failed (error=%lu)", static_cast<unsigned long>(GetLastError()));
        CloseHandle(snapshot);
        return;
    }

    do {
        const uintptr_t base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        AppendFormat("[CRASH]   %s base=0x%08lX end=0x%08lX size=0x%08lX path=%s",
                     entry.szModule,
                     static_cast<unsigned long>(base),
                     static_cast<unsigned long>(base + entry.modBaseSize),
                     static_cast<unsigned long>(entry.modBaseSize),
                     entry.szExePath);
    } while (Module32Next(snapshot, &entry));

    CloseHandle(snapshot);
}

void CaptureCurrentContext(CONTEXT* context, uintptr_t preferredIp) {
    if (!context) {
        return;
    }

    std::memset(context, 0, sizeof(*context));
    RtlCaptureContext(context);
    context->ContextFlags = CONTEXT_FULL;
#if defined(_M_IX86)
    if (preferredIp) {
        context->Eip = static_cast<DWORD>(preferredIp);
    }
#elif defined(_M_X64)
    if (preferredIp) {
        context->Rip = static_cast<DWORD64>(preferredIp);
    }
#endif
}

void ReportSyntheticCrash(const char* source, DWORD code, uintptr_t preferredIp, const char* detail) {
    if (InterlockedCompareExchange(&g_handlingCrash, 1, 0) != 0) {
        return;
    }

    EXCEPTION_RECORD record = {};
    CONTEXT context = {};
    CaptureCurrentContext(&context, preferredIp);

    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_IX86)
    record.ExceptionAddress = reinterpret_cast<PVOID>(preferredIp ? preferredIp : static_cast<uintptr_t>(context.Eip));
#elif defined(_M_X64)
    record.ExceptionAddress = reinterpret_cast<PVOID>(preferredIp ? preferredIp : static_cast<uintptr_t>(context.Rip));
#else
    record.ExceptionAddress = reinterpret_cast<PVOID>(preferredIp);
#endif

    EXCEPTION_POINTERS pointers = {};
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &context;
    WriteCrashReport(&pointers, source, detail);
}

void __cdecl InvalidParameterCrashHandler(const wchar_t* expression,
                                          const wchar_t* function,
                                          const wchar_t* file,
                                          unsigned int line,
                                          uintptr_t reserved) {
    (void)reserved;

    char expressionText[128] = {0};
    char functionText[128] = {0};
    char fileText[256] = {0};
    char detail[640] = {0};
    WideToAnsiLossy(expression, expressionText, sizeof(expressionText));
    WideToAnsiLossy(function, functionText, sizeof(functionText));
    WideToAnsiLossy(file, fileText, sizeof(fileText));
    _snprintf_s(detail,
                sizeof(detail),
                _TRUNCATE,
                "CRT invalid parameter function=%s file=%s line=%u expression=%s",
                functionText[0] ? functionText : "<unknown>",
                fileText[0] ? fileText : "<unknown>",
                line,
                expressionText[0] ? expressionText : "<unknown>");
    ReportSyntheticCrash("CRT invalid parameter", kSyntheticInvalidParameterCode, reinterpret_cast<uintptr_t>(_ReturnAddress()), detail);
    TerminateProcess(GetCurrentProcess(), kSyntheticInvalidParameterCode);
}

void __cdecl PureCallCrashHandler() {
    ReportSyntheticCrash("CRT pure virtual call", kSyntheticPureCallCode, reinterpret_cast<uintptr_t>(_ReturnAddress()), "Pure virtual function call reached runtime purecall handler");
    TerminateProcess(GetCurrentProcess(), kSyntheticPureCallCode);
}

void __cdecl TerminateCrashHandler() {
    ReportSyntheticCrash("std::terminate", kSyntheticTerminateCode, reinterpret_cast<uintptr_t>(_ReturnAddress()), "Unhandled C++ termination path invoked std::terminate");
    TerminateProcess(GetCurrentProcess(), kSyntheticTerminateCode);
}

void __cdecl SignalCrashHandler(int signalNumber) {
    char detail[160] = {0};
    _snprintf_s(detail,
                sizeof(detail),
                _TRUNCATE,
                "C runtime signal %s (%d)",
                SignalName(signalNumber),
                signalNumber);
    ReportSyntheticCrash("CRT signal", kSyntheticSignalBaseCode + static_cast<DWORD>(signalNumber & 0xFF), reinterpret_cast<uintptr_t>(_ReturnAddress()), detail);
    TerminateProcess(GetCurrentProcess(), kSyntheticSignalBaseCode + static_cast<DWORD>(signalNumber & 0xFF));
}

void FormatAddress(char* output, size_t outputSize, uintptr_t address) {
    if (!output || outputSize == 0) {
        return;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!address || !VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        _snprintf_s(output, outputSize, _TRUNCATE, "0x%08lX", static_cast<unsigned long>(address));
        return;
    }

    char modulePath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modulePath, MAX_PATH)) {
        _snprintf_s(output,
                    outputSize,
                    _TRUNCATE,
                    "0x%08lX (base=0x%08lX)",
                    static_cast<unsigned long>(address),
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
        return;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    char functionInfo[256] = {0};
    const bool haveEfzFunction = mbi.AllocationBase == GetModuleHandleA(nullptr)
        && LookupEfzFunction(address, functionInfo, sizeof(functionInfo));
    const bool haveDebugSymbol = !haveEfzFunction
        && TryLookupDebugSymbol(address, functionInfo, sizeof(functionInfo));
    _snprintf_s(output,
                outputSize,
                _TRUNCATE,
                (haveEfzFunction || haveDebugSymbol)
                    ? "0x%08lX (%s+0x%08lX base=0x%08lX func=%s)"
                    : "0x%08lX (%s+0x%08lX base=0x%08lX)",
                static_cast<unsigned long>(address),
                BaseName(modulePath),
                static_cast<unsigned long>(address - base),
                static_cast<unsigned long>(base),
                functionInfo);
}

void LogExceptionRecord(const EXCEPTION_RECORD* record) {
    int depth = 0;
    while (record && depth < 4) {
        char addressInfo[256] = {0};
        FormatAddress(addressInfo, sizeof(addressInfo), reinterpret_cast<uintptr_t>(record->ExceptionAddress));
        AppendFormat("[CRASH] Exception[%d] code=0x%08lX (%s) flags=0x%08lX address=%s",
                     depth,
                     static_cast<unsigned long>(record->ExceptionCode),
                     ExceptionCodeName(record->ExceptionCode),
                     static_cast<unsigned long>(record->ExceptionFlags),
                     addressInfo);

        if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
            && record->NumberParameters >= 2) {
            const char* op = "unknown";
            if (record->ExceptionInformation[0] == 0) op = "read";
            else if (record->ExceptionInformation[0] == 1) op = "write";
            else if (record->ExceptionInformation[0] == 8) op = "execute";

            char targetInfo[256] = {0};
            FormatAddress(targetInfo,
                          sizeof(targetInfo),
                          static_cast<uintptr_t>(record->ExceptionInformation[1]));
            AppendFormat("[CRASH] Access violation operation=%s target=%s", op, targetInfo);
        }

        for (DWORD i = 0; i < record->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
            AppendFormat("[CRASH] Exception[%d] info[%lu]=0x%08lX",
                         depth,
                         static_cast<unsigned long>(i),
                         static_cast<unsigned long>(record->ExceptionInformation[i]));
        }

        record = record->ExceptionRecord;
        ++depth;
    }
}

void LogRegisters(const CONTEXT* context) {
    if (!context) {
        AppendLine("[CRASH] No CPU context available");
        return;
    }

#if defined(_M_IX86)
    char eipInfo[256] = {0};
    FormatAddress(eipInfo, sizeof(eipInfo), static_cast<uintptr_t>(context->Eip));
    AppendFormat("[CRASH] Registers: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
                 static_cast<unsigned long>(context->Eax),
                 static_cast<unsigned long>(context->Ebx),
                 static_cast<unsigned long>(context->Ecx),
                 static_cast<unsigned long>(context->Edx));
    AppendFormat("[CRASH]            ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
                 static_cast<unsigned long>(context->Esi),
                 static_cast<unsigned long>(context->Edi),
                 static_cast<unsigned long>(context->Ebp),
                 static_cast<unsigned long>(context->Esp));
    AppendFormat("[CRASH]            EIP=%08lX EFLAGS=%08lX CS=%04lX SS=%04lX",
                 static_cast<unsigned long>(context->Eip),
                 static_cast<unsigned long>(context->EFlags),
                 static_cast<unsigned long>(context->SegCs),
                 static_cast<unsigned long>(context->SegSs));
    AppendFormat("[CRASH] Fault location: %s", eipInfo);
#elif defined(_M_X64)
    char ripInfo[256] = {0};
    FormatAddress(ripInfo, sizeof(ripInfo), static_cast<uintptr_t>(context->Rip));
    AppendFormat("[CRASH] Registers: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
                 static_cast<unsigned long long>(context->Rax),
                 static_cast<unsigned long long>(context->Rbx),
                 static_cast<unsigned long long>(context->Rcx),
                 static_cast<unsigned long long>(context->Rdx));
    AppendFormat("[CRASH]            RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX",
                 static_cast<unsigned long long>(context->Rsi),
                 static_cast<unsigned long long>(context->Rdi),
                 static_cast<unsigned long long>(context->Rbp),
                 static_cast<unsigned long long>(context->Rsp));
    AppendFormat("[CRASH]            RIP=%016llX RFLAGS=%016llX", 
                 static_cast<unsigned long long>(context->Rip),
                 static_cast<unsigned long long>(context->EFlags));
    AppendFormat("[CRASH] Fault location: %s", ripInfo);
#else
    AppendLine("[CRASH] Register logging is not implemented for this architecture");
#endif
}

void LogCodeBytes(uintptr_t instructionPointer) {
    if (!instructionPointer) {
        return;
    }

    const uintptr_t start = instructionPointer > 8 ? instructionPointer - 8 : instructionPointer;
    unsigned char bytes[32] = {0};
    if (!TryReadMemory(reinterpret_cast<const void*>(start), bytes, sizeof(bytes))) {
        AppendFormat("[CRASH] Failed to read code bytes near 0x%08lX", static_cast<unsigned long>(instructionPointer));
        return;
    }

    for (int row = 0; row < 2; ++row) {
        char line[512] = {0};
        char* cursor = line;
        const uintptr_t rowAddress = start + static_cast<uintptr_t>(row * 16);
        cursor += _snprintf_s(cursor,
                              sizeof(line) - static_cast<size_t>(cursor - line),
                              _TRUNCATE,
                              "[CRASH] Code %08lX:",
                              static_cast<unsigned long>(rowAddress));
        for (int col = 0; col < 16; ++col) {
            const int index = row * 16 + col;
            cursor += _snprintf_s(cursor,
                                  sizeof(line) - static_cast<size_t>(cursor - line),
                                  _TRUNCATE,
                                  " %02X",
                                  static_cast<unsigned int>(bytes[index]));
        }
        AppendLine(line);
    }
}

void LogStackWords(uintptr_t stackPointer) {
    if (!stackPointer) {
        return;
    }

    AppendLine("[CRASH] Stack dump:");
    for (int i = 0; i < 32; ++i) {
        uintptr_t value = 0;
        const uintptr_t slot = stackPointer + static_cast<uintptr_t>(i * sizeof(uintptr_t));
        if (!TryReadMemory(reinterpret_cast<const void*>(slot), &value, sizeof(value))) {
            AppendFormat("[CRASH]   SP+0x%02X @0x%08lX = <unreadable>",
                         static_cast<unsigned int>(i * sizeof(uintptr_t)),
                         static_cast<unsigned long>(slot));
            break;
        }

        char valueInfo[256] = {0};
        FormatAddress(valueInfo, sizeof(valueInfo), value);
        AppendFormat("[CRASH]   SP+0x%02X @0x%08lX = 0x%08lX -> %s",
                     static_cast<unsigned int>(i * sizeof(uintptr_t)),
                     static_cast<unsigned long>(slot),
                     static_cast<unsigned long>(value),
                     valueInfo);
    }
}

void LogFrameChain(uintptr_t framePointer) {
    if (!framePointer) {
        return;
    }

    AppendLine("[CRASH] Frame chain:");
    uintptr_t current = framePointer;
    for (int depth = 0; depth < 32 && current; ++depth) {
        uintptr_t next = 0;
        uintptr_t ret = 0;
        if (!TryReadMemory(reinterpret_cast<const void*>(current), &next, sizeof(next))
            || !TryReadMemory(reinterpret_cast<const void*>(current + sizeof(uintptr_t)), &ret, sizeof(ret))) {
            AppendFormat("[CRASH]   #%02d EBP=0x%08lX <unreadable>", depth, static_cast<unsigned long>(current));
            break;
        }

        char retInfo[256] = {0};
        FormatAddress(retInfo, sizeof(retInfo), ret);
        AppendFormat("[CRASH]   #%02d FP=0x%08lX RET=%s NEXT=0x%08lX",
                     depth,
                     static_cast<unsigned long>(current),
                     retInfo,
                     static_cast<unsigned long>(next));

        if (next <= current || (next - current) > 0x100000) {
            break;
        }
        current = next;
    }
}

void TryWriteMiniDump(EXCEPTION_POINTERS* exceptionPointers) {
    if (!g_dumpDirectory[0]) {
        AppendLine("[CRASH] Minidump skipped because dump directory is unavailable");
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char dumpPath[MAX_PATH] = {0};
    _snprintf_s(dumpPath,
                sizeof(dumpPath),
                _TRUNCATE,
                "%s\\efz_training_crash_%04u%02u%02u_%02u%02u%02u_%03u.dmp",
                g_dumpDirectory,
                static_cast<unsigned int>(st.wYear),
                static_cast<unsigned int>(st.wMonth),
                static_cast<unsigned int>(st.wDay),
                static_cast<unsigned int>(st.wHour),
                static_cast<unsigned int>(st.wMinute),
                static_cast<unsigned int>(st.wSecond),
                static_cast<unsigned int>(st.wMilliseconds));

    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (!dbghelp) {
        AppendFormat("[CRASH] Failed to load dbghelp.dll (error=%lu)", static_cast<unsigned long>(GetLastError()));
        return;
    }

    auto miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!miniDumpWriteDump) {
        AppendLine("[CRASH] dbghelp.dll does not export MiniDumpWriteDump");
        FreeLibrary(dbghelp);
        return;
    }

    HANDLE dumpFile = CreateFileA(dumpPath,
                                  GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE) {
        AppendFormat("[CRASH] Failed to create minidump file %s (error=%lu)",
                     dumpPath,
                     static_cast<unsigned long>(GetLastError()));
        FreeLibrary(dbghelp);
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;

    const MINIDUMP_TYPE detailedType = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal |
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData |
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpScanMemory |
        MiniDumpWithProcessThreadData |
        MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules);

    BOOL ok = miniDumpWriteDump(GetCurrentProcess(),
                                GetCurrentProcessId(),
                                dumpFile,
                                detailedType,
                                exceptionPointers ? &exceptionInfo : nullptr,
                                nullptr,
                                nullptr);
    DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok) {
        ok = miniDumpWriteDump(GetCurrentProcess(),
                               GetCurrentProcessId(),
                               dumpFile,
                               MiniDumpNormal,
                               exceptionPointers ? &exceptionInfo : nullptr,
                               nullptr,
                               nullptr);
        if (ok) {
            AppendLine("[CRASH] Detailed minidump flags failed; wrote fallback MiniDumpNormal");
        } else {
            lastError = GetLastError();
        }
    }
    CloseHandle(dumpFile);
    FreeLibrary(dbghelp);

    if (ok) {
        AppendFormat("[CRASH] Minidump written to %s", dumpPath);
    } else {
        DeleteFileA(dumpPath);
        AppendFormat("[CRASH] MiniDumpWriteDump failed for %s (error=%lu)",
                     dumpPath,
                     static_cast<unsigned long>(lastError));
    }
}

void WriteCrashReport(EXCEPTION_POINTERS* exceptionPointers, const char* source, const char* detail) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    AppendLine("========================================");
    AppendFormat("[CRASH] EFZ Training Mode crash report at %04u-%02u-%02u %02u:%02u:%02u.%03u",
                 static_cast<unsigned int>(st.wYear),
                 static_cast<unsigned int>(st.wMonth),
                 static_cast<unsigned int>(st.wDay),
                 static_cast<unsigned int>(st.wHour),
                 static_cast<unsigned int>(st.wMinute),
                 static_cast<unsigned int>(st.wSecond),
                 static_cast<unsigned int>(st.wMilliseconds));
    AppendFormat("[CRASH] ProcessId=%lu ThreadId=%lu",
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(GetCurrentThreadId()));
    AppendFormat("[CRASH] Source=%s", source && *source ? source : "UnhandledExceptionFilter");
    if (detail && *detail) {
        AppendFormat("[CRASH] Detail=%s", detail);
    }
    AppendFormat("[CRASH] DebugLogPath=%s", g_debugLogPath[0] ? g_debugLogPath : "<unavailable>");
    AppendFormat("[CRASH] CrashLogPath=%s", g_crashLogPath[0] ? g_crashLogPath : "<unavailable>");

    if (!exceptionPointers) {
        AppendLine("[CRASH] No EXCEPTION_POINTERS were provided");
        AppendLine("========================================");
        return;
    }

    LogCrashAnalysisSummary(exceptionPointers);
    LogExceptionRecord(exceptionPointers->ExceptionRecord);
    if (exceptionPointers->ExceptionRecord) {
        LogAddressMemoryDetails("Exception address detail",
                                reinterpret_cast<uintptr_t>(exceptionPointers->ExceptionRecord->ExceptionAddress));
        if ((exceptionPointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
             || exceptionPointers->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
            && exceptionPointers->ExceptionRecord->NumberParameters >= 2) {
            LogAddressMemoryDetails("Fault target detail",
                                    static_cast<uintptr_t>(exceptionPointers->ExceptionRecord->ExceptionInformation[1]));
        }
    }
    LogRegisters(exceptionPointers->ContextRecord);
    LogRegisterPointerHints(exceptionPointers->ContextRecord);

#if defined(_M_IX86)
    if (exceptionPointers->ContextRecord) {
        LogCodeBytes(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Eip));
        LogStackWords(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Esp));
        LogStackCandidates(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Esp));
        LogFrameChain(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Ebp));
    }
#elif defined(_M_X64)
    if (exceptionPointers->ContextRecord) {
        LogCodeBytes(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rip));
        LogStackWords(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rsp));
        LogStackCandidates(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rsp));
        LogFrameChain(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rbp));
    }
#endif

    LogLoadedModules();
    TryWriteMiniDump(exceptionPointers);
    AppendLine("========================================");
}

LONG CallPreviousFilter(EXCEPTION_POINTERS* exceptionPointers) {
    if (!g_previousFilter || g_previousFilter == &UnhandledCrashFilter) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    __try {
        return g_previousFilter(exceptionPointers);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        AppendLine("[CRASH] Previous unhandled exception filter raised an exception");
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* exceptionPointers) {
    if (InterlockedCompareExchange(&g_handlingCrash, 1, 0) == 0) {
        WriteCrashReport(exceptionPointers, "UnhandledExceptionFilter", nullptr);
    }
    return CallPreviousFilter(exceptionPointers);
}

LONG CALLBACK VectoredCrashHandler(EXCEPTION_POINTERS* exceptionPointers) {
    if (!exceptionPointers || !exceptionPointers->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (!IsCrashCandidateException(exceptionPointers->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedCompareExchange(&g_handlingCrash, 1, 0) == 0) {
        WriteCrashReport(exceptionPointers,
                         "VectoredExceptionHandler",
                         "Fatal SEH candidate captured before the top-level filter; another layer may still intercept termination");
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Install(HMODULE selfModule) {
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) {
        return;
    }

    g_selfModule = selfModule;
    InitializePaths(g_selfModule);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_invalid_parameter_handler(&InvalidParameterCrashHandler);
    _set_purecall_handler(&PureCallCrashHandler);
    std::set_terminate(&TerminateCrashHandler);
    signal(SIGABRT, &SignalCrashHandler);
    signal(SIGFPE, &SignalCrashHandler);
    signal(SIGILL, &SignalCrashHandler);
    signal(SIGSEGV, &SignalCrashHandler);
    signal(SIGTERM, &SignalCrashHandler);
    g_vectoredHandler = AddVectoredExceptionHandler(1, &VectoredCrashHandler);
    g_previousFilter = SetUnhandledExceptionFilter(&UnhandledCrashFilter);
}

void WarmupSymbolMaps() {
    if (InterlockedCompareExchange(&g_efzSymbolMapLoadState,
                                   kEfzSymbolMapStateLoading,
                                   kEfzSymbolMapStateNotStarted) != kEfzSymbolMapStateNotStarted) {
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, &SymbolWarmupThreadProc, nullptr, 0, nullptr);
    if (!thread) {
        InterlockedExchange(&g_efzSymbolMapLoadState, kEfzSymbolMapStateNotStarted);
        return;
    }

    CloseHandle(thread);
}

} // namespace CrashHandler