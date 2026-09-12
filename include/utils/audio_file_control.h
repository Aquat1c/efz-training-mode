#pragma once
#include <windows.h>
#include <cstdio>

// Control plane only. Both DLLs name the same process-scoped Win32 mutex, so
// their independent C++ locks cannot admit overlapping shared-file operations.
// Order: module control mutex -> this mutex -> audio callback boundary. Never
// acquire a file transaction from a native callback. Recursive acquisition by
// the same control thread permits schema/import helpers to compose safely.
class EfzAudioFileTransaction {
public:
    EfzAudioFileTransaction() noexcept {
        char name[80]{};
        _snprintf_s(name, sizeof(name), _TRUNCATE, "Local\\EfzAudioConfigV1-%lu", GetCurrentProcessId());
        mutex_ = CreateMutexA(nullptr, FALSE, name);
        if (mutex_) {
            const DWORD result = WaitForSingleObject(mutex_, INFINITE);
            acquired_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            valid_ = result == WAIT_OBJECT_0;
        }
    }
    ~EfzAudioFileTransaction() {
        if (acquired_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    explicit operator bool() const noexcept { return valid_; }
    EfzAudioFileTransaction(const EfzAudioFileTransaction&) = delete;
    EfzAudioFileTransaction& operator=(const EfzAudioFileTransaction&) = delete;
private:
    HANDLE mutex_ = nullptr;
    bool acquired_ = false, valid_ = false;
};
