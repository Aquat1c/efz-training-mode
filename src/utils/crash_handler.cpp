#include "../include/utils/crash_handler.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

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
char g_debugLogPath[MAX_PATH] = {0};
char g_crashLogPath[MAX_PATH] = {0};
char g_dumpDirectory[MAX_PATH] = {0};

LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* exceptionPointers);

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
    _snprintf_s(output,
                outputSize,
                _TRUNCATE,
                "0x%08lX (%s+0x%08lX base=0x%08lX)",
                static_cast<unsigned long>(address),
                BaseName(modulePath),
                static_cast<unsigned long>(address - base),
                static_cast<unsigned long>(base));
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
    for (int i = 0; i < 16; ++i) {
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
    for (int depth = 0; depth < 16 && current; ++depth) {
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

    const BOOL ok = miniDumpWriteDump(GetCurrentProcess(),
                                      GetCurrentProcessId(),
                                      dumpFile,
                                      MiniDumpNormal,
                                      exceptionPointers ? &exceptionInfo : nullptr,
                                      nullptr,
                                      nullptr);
    const DWORD lastError = GetLastError();
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

void WriteCrashReport(EXCEPTION_POINTERS* exceptionPointers) {
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
    AppendFormat("[CRASH] DebugLogPath=%s", g_debugLogPath[0] ? g_debugLogPath : "<unavailable>");
    AppendFormat("[CRASH] CrashLogPath=%s", g_crashLogPath[0] ? g_crashLogPath : "<unavailable>");

    if (!exceptionPointers) {
        AppendLine("[CRASH] No EXCEPTION_POINTERS were provided");
        AppendLine("========================================");
        return;
    }

    LogExceptionRecord(exceptionPointers->ExceptionRecord);
    LogRegisters(exceptionPointers->ContextRecord);

#if defined(_M_IX86)
    if (exceptionPointers->ContextRecord) {
        LogCodeBytes(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Eip));
        LogStackWords(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Esp));
        LogFrameChain(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Ebp));
    }
#elif defined(_M_X64)
    if (exceptionPointers->ContextRecord) {
        LogCodeBytes(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rip));
        LogStackWords(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rsp));
        LogFrameChain(static_cast<uintptr_t>(exceptionPointers->ContextRecord->Rbp));
    }
#endif

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
        WriteCrashReport(exceptionPointers);
    }
    return CallPreviousFilter(exceptionPointers);
}

} // namespace

void Install(HMODULE selfModule) {
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) {
        return;
    }

    g_selfModule = selfModule;
    InitializePaths(g_selfModule);
    g_previousFilter = SetUnhandledExceptionFilter(&UnhandledCrashFilter);
}

} // namespace CrashHandler