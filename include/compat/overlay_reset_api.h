#pragma once

#include <stdint.h>
#include <windows.h>

// Shared DisplayManager/overlay C ABI. This table is 16 bytes on Win32.
// Keep the provider module loaded from query through the last notification.
// Calls are synchronous on the observed game render thread, outside a frame.
typedef struct EfzOverlayResetApiV1 {
    uint32_t size;
    uint32_t version;
    HRESULT (__cdecl *beforeReset)(void* device, uint32_t epoch);
    HRESULT (__cdecl *afterReset)(void* device, uint32_t epoch, uint32_t ready);
} EfzOverlayResetApiV1;

// beforeReset: S_OK acknowledges only the provider's own backend resources.
// A nonzero epoch also gates lazy initialization for the target device.
// afterReset ready=0 leaves the target gated. ready=1 recreates the matching
// owner's device objects and opens admission only after successful recreation;
// call only after successful Reset AND required renderer recovery, or when
// cancelling a transition before any real Reset. It does not initialize UI.
// A failed ready=1 remains gated and may be retried with the same epoch.
// Wrong device/epoch/thread and concurrent/reentrant work fail without waiting.
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) const EfzOverlayResetApiV1* __cdecl EFZ_QueryOverlayResetApiV1(void);
#ifdef __cplusplus
}
#if defined(_WIN32) && !defined(_WIN64)
static_assert(sizeof(EfzOverlayResetApiV1) == 16, "overlay reset ABI must be 16 bytes on x86");
#endif
#endif

// MSVC x86 otherwise exports the C symbol with its leading underscore.
#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:EFZ_QueryOverlayResetApiV1=_EFZ_QueryOverlayResetApiV1")
#endif
