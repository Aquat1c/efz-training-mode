#include <fstream>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include "../include/gui/overlay.h"
#include "../include/gui/overlay_api.h"
#include "../include/gui/framebar.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"

#include "../include/core/memory.h"   
#include "../include/core/constants.h" 
#include "../include/game/efzrevival_addrs.h"
#include "../include/game/collision_display.h"
#include "../3rdparty/detours/include/detours.h"
#include <algorithm>
#include "../include/gui/imgui_impl.h"
#include <d3d9.h>
#include <mutex>
#include <deque>
#include <unordered_set>
#include "../include/gui/imgui_gui.h"
#include "../include/gui/custom_menu/renderer.h"
#include "../3rdparty/minhook/include/MinHook.h"
// ADD these includes for the new rendering loop
#include "../include/gui/imgui_impl.h"
#include "../include/utils/config.h"
#include <Xinput.h>
// XInput loaded dynamically via XInputShim
#include "../include/utils/xinput_shim.h"
#include <cmath>
#include "../../include/gui/gif_player.h"

// Avoid Windows min/max macro conflicts
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Global status message IDs
int g_AirtechStatusId = -1;
int g_JumpStatusId = -1;
int g_FrameAdvantageId = -1;
int g_FrameAdvantage2Id = -1;
int g_FrameGapId = -1;
// NEW: Individual trigger message IDs
int g_TriggerAfterBlockId = -1;
int g_TriggerOnWakeupId = -1;
int g_TriggerAfterHitstunId = -1;
int g_TriggerAfterAirtechId = -1;
int g_TriggerOnRGId = -1;
int g_FramestepStatusId = -1;
// Debug borders toggle default off
std::atomic<bool> g_ShowOverlayDebugBorders{false};

// Track RT size changes globally (shared across all EndScene calls)
namespace {
    std::mutex g_rtSizeMutex;
    UINT g_prevRtW = 0;
    UINT g_prevRtH = 0;
    std::atomic<bool> g_rtSizeLogged{false};  // Use atomic for thread-safe first-log detection

    ImU32 ImColorFromArgb(uint32_t argb) {
        return IM_COL32(
            static_cast<int>((argb >> 16) & 0xFFu),
            static_cast<int>((argb >> 8) & 0xFFu),
            static_cast<int>(argb & 0xFFu),
            static_cast<int>((argb >> 24) & 0xFFu));
    }

    bool QueryRenderTargetSize(LPDIRECT3DDEVICE9 pDevice, UINT& outWidth, UINT& outHeight) {
        outWidth = 0;
        outHeight = 0;
        if (!pDevice) {
            return false;
        }

        IDirect3DSurface9* rt = nullptr;
        if (FAILED(pDevice->GetRenderTarget(0, &rt)) || !rt) {
            return false;
        }

        D3DSURFACE_DESC desc{};
        const bool ok = SUCCEEDED(rt->GetDesc(&desc));
        rt->Release();
        if (!ok) {
            return false;
        }

        outWidth = desc.Width;
        outHeight = desc.Height;
        return true;
    }

    bool IsEfzFullscreenCached() {
        static HWND s_cachedHwnd = nullptr;
        static DWORD s_lastRefreshTick = 0;
        static bool s_cachedFullscreen = false;

        HWND hwnd = FindEFZWindow();
        if (!hwnd) {
            s_cachedHwnd = nullptr;
            s_cachedFullscreen = false;
            s_lastRefreshTick = 0;
            return false;
        }

        const DWORD now = GetTickCount();
        if (hwnd == s_cachedHwnd && s_lastRefreshTick != 0 && (now - s_lastRefreshTick) < 125) {
            return s_cachedFullscreen;
        }

        WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
        RECT wndRect{};
        HMONITOR mon = nullptr;
        MONITORINFO mi{ sizeof(MONITORINFO) };
        bool fullscreen = false;
        if (GetWindowPlacement(hwnd, &wp)
            && GetWindowRect(hwnd, &wndRect)
            && (mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)) != nullptr
            && GetMonitorInfo(mon, &mi)) {
            fullscreen = EqualRect(&wndRect, &mi.rcMonitor) || EqualRect(&wndRect, &mi.rcWork);
        }

        s_cachedHwnd = hwnd;
        s_cachedFullscreen = fullscreen;
        s_lastRefreshTick = now;
        return fullscreen;
    }

    std::atomic<bool> g_endSceneWatchdogStarted{false};
    std::atomic<bool> g_endSceneInProgress{false};
    std::atomic<DWORD> g_endSceneThreadId{0};
    std::atomic<DWORD> g_endSceneStartTick{0};
    std::atomic<unsigned long> g_endSceneSequence{0};
    std::atomic<const char*> g_endScenePhase{"idle"};
    std::atomic<bool> g_activeD3D9DeviceLogged{false};

    bool TryReadPointer(uintptr_t address, uintptr_t* outValue) {
        if (!address || !outValue) return false;
        __try {
            *outValue = *reinterpret_cast<const uintptr_t*>(address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string DescribeAddress(uintptr_t address) {
        char result[256] = {};
        MEMORY_BASIC_INFORMATION mbi{};
        if (address && VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) && mbi.AllocationBase) {
            char modulePath[MAX_PATH] = {};
            if (GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modulePath, MAX_PATH)) {
                _snprintf_s(result, sizeof(result), _TRUNCATE,
                            "%p %s+0x%lX",
                            reinterpret_cast<void*>(address),
                            modulePath,
                            static_cast<unsigned long>(address - reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
                return result;
            }
            _snprintf_s(result, sizeof(result), _TRUNCATE,
                        "%p allocationBase=%p",
                        reinterpret_cast<void*>(address),
                        mbi.AllocationBase);
            return result;
        }

        _snprintf_s(result, sizeof(result), _TRUNCATE, "%p <unmapped>", reinterpret_cast<void*>(address));
        return result;
    }

    void LogEndSceneHangSnapshot(DWORD tid, const char* phase, DWORD elapsedMs, unsigned long sequence) {
        if (!tid || tid == GetCurrentThreadId()) {
            return;
        }

        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE,
                                   tid);
        if (!thread) {
            LogOut("[OVERLAY][D3D9][HANG] Could not open render thread for snapshot; tid=" + std::to_string(tid), true);
            return;
        }

        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        uintptr_t ip = 0;
        uintptr_t sp = 0;
        uintptr_t bp = 0;
        uintptr_t stack[6] = {};
        bool stackOk[6] = {};

        const DWORD suspendResult = SuspendThread(thread);
        if (suspendResult == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            LogOut("[OVERLAY][D3D9][HANG] SuspendThread failed for render thread; tid=" + std::to_string(tid), true);
            return;
        }

        const BOOL gotContext = GetThreadContext(thread, &ctx);
#if defined(_M_IX86)
        if (gotContext) {
            ip = static_cast<uintptr_t>(ctx.Eip);
            sp = static_cast<uintptr_t>(ctx.Esp);
            bp = static_cast<uintptr_t>(ctx.Ebp);
            for (int i = 0; i < 6; ++i) {
                stackOk[i] = TryReadPointer(sp + static_cast<uintptr_t>(i) * sizeof(uintptr_t), &stack[i]);
            }
        }
#endif
        ResumeThread(thread);
        CloseHandle(thread);

        char header[320] = {};
        _snprintf_s(header, sizeof(header), _TRUNCATE,
                    "[OVERLAY][D3D9][HANG] EndScene active for %lums seq=%lu tid=%lu phase=%s ctx=%s ip=%s sp=%p bp=%p",
                    static_cast<unsigned long>(elapsedMs),
                    sequence,
                    static_cast<unsigned long>(tid),
                    phase ? phase : "unknown",
                    gotContext ? "ok" : "failed",
                    ip ? DescribeAddress(ip).c_str() : "<unavailable>",
                    reinterpret_cast<void*>(sp),
                    reinterpret_cast<void*>(bp));
        LogOut(header, true);

        if (gotContext && sp) {
            for (int i = 0; i < 6; ++i) {
                if (!stackOk[i]) continue;
                char line[320] = {};
                const std::string desc = DescribeAddress(stack[i]);
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "[OVERLAY][D3D9][HANG] stack[%d]=%s",
                            i,
                            desc.c_str());
                LogOut(line, true);
            }
        }
    }

    void StartEndSceneWatchdog() {
        bool expected = false;
        if (!g_endSceneWatchdogStarted.compare_exchange_strong(expected, true)) {
            return;
        }

        std::thread([] {
            unsigned long lastLoggedSequence = 0;
            DWORD lastLogTick = 0;
            for (;;) {
                Sleep(250);
                if (!g_endSceneInProgress.load(std::memory_order_acquire)) {
                    continue;
                }

                const DWORD now = GetTickCount();
                const DWORD start = g_endSceneStartTick.load(std::memory_order_acquire);
                const DWORD elapsed = now - start;
                if (elapsed < 2500) {
                    continue;
                }

                const unsigned long sequence = g_endSceneSequence.load(std::memory_order_acquire);
                if (sequence == lastLoggedSequence && (now - lastLogTick) < 5000) {
                    continue;
                }

                lastLoggedSequence = sequence;
                lastLogTick = now;
                LogEndSceneHangSnapshot(g_endSceneThreadId.load(std::memory_order_acquire),
                                         g_endScenePhase.load(std::memory_order_acquire),
                                         elapsed,
                                         sequence);
            }
        }).detach();
    }

    void SetEndScenePhase(const char* phase) {
        g_endScenePhase.store(phase ? phase : "unknown", std::memory_order_release);
    }

    struct EndSceneHangScope {
        EndSceneHangScope() {
            StartEndSceneWatchdog();
            g_endSceneThreadId.store(GetCurrentThreadId(), std::memory_order_release);
            g_endSceneStartTick.store(GetTickCount(), std::memory_order_release);
            g_endSceneSequence.fetch_add(1, std::memory_order_acq_rel);
            SetEndScenePhase("enter");
            g_endSceneInProgress.store(true, std::memory_order_release);
        }

        ~EndSceneHangScope() {
            SetEndScenePhase("idle");
            g_endSceneInProgress.store(false, std::memory_order_release);
        }
    };

    void LogActiveD3D9DeviceOnce(LPDIRECT3DDEVICE9 pDevice) {
        if (!pDevice) return;
        bool expected = false;
        if (!g_activeD3D9DeviceLogged.compare_exchange_strong(expected, true)) {
            return;
        }

        D3DDEVICE_CREATION_PARAMETERS cp{};
        const HRESULT cpHr = pDevice->GetCreationParameters(&cp);
        IDirect3D9* d3d = nullptr;
        const HRESULT d3dHr = pDevice->GetDirect3D(&d3d);
        if (SUCCEEDED(cpHr) && SUCCEEDED(d3dHr) && d3d) {
            D3DADAPTER_IDENTIFIER9 ident{};
            if (SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &ident))) {
                char idbuf[256] = {};
                _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE,
                            "[OVERLAY][D3D9] Active device adapter=%u vendor=0x%04X device=0x%04X desc=%s",
                            static_cast<unsigned>(cp.AdapterOrdinal),
                            ident.VendorId,
                            ident.DeviceId,
                            ident.Description);
                LogOut(idbuf, true);
                if (ident.VendorId == 0x1002) {
                    LogOut("[OVERLAY][D3D9] AMD adapter detected; EndScene watchdog and cooperative-level guards are active", true);
                }
            }
        } else {
            char buf[160] = {};
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[OVERLAY][D3D9] Active device adapter query failed cp=0x%08lX d3d=0x%08lX",
                        static_cast<unsigned long>(cpHr),
                        static_cast<unsigned long>(d3dHr));
            LogOut(buf, true);
        }

        if (d3d) {
            d3d->Release();
        }
    }

    bool SkipOverlayForLostDevice(LPDIRECT3DDEVICE9 pDevice) {
        if (!pDevice) return true;
        const HRESULT hr = pDevice->TestCooperativeLevel();
        if (hr == D3D_OK) {
            return false;
        }

        static HRESULT s_lastLoggedHr = D3D_OK;
        static DWORD s_lastLogTick = 0;
        const DWORD now = GetTickCount();
        if (hr != s_lastLoggedHr || (now - s_lastLogTick) >= 1000) {
            s_lastLoggedHr = hr;
            s_lastLogTick = now;
            char buf[160] = {};
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[OVERLAY][D3D9] TestCooperativeLevel=0x%08lX; %s",
                        static_cast<unsigned long>(hr),
                        (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET)
                            ? "skipping overlay render this frame"
                            : "continuing overlay render");
            LogOut(buf, true);
        }

        return hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET;
    }
}
std::atomic<bool> g_ShowRGDebugToasts{false};

// --- Define static members of DirectDrawHook ---
DirectDrawCreateFunc DirectDrawHook::originalDirectDrawCreate = nullptr;
DirectDrawEnumerateFunc DirectDrawHook::originalDirectDrawEnumerate = nullptr;
BlitFunc DirectDrawHook::originalBlit = nullptr;
FlipFunc DirectDrawHook::originalFlip = nullptr;
IDirectDrawSurface7* DirectDrawHook::primarySurface = nullptr;
HWND DirectDrawHook::gameWindow = nullptr;
std::mutex DirectDrawHook::messagesMutex;
std::deque<OverlayMessage> DirectDrawHook::messages;
std::vector<OverlayMessage> DirectDrawHook::permanentMessages;
int DirectDrawHook::nextMessageId = 0;
bool DirectDrawHook::isHooked = false;
// --- End static member definitions ---

// --- DPI Awareness Helper ---
static float GetDpiScale() {
    HWND hwnd = FindEFZWindow();  // Use safe multi-instance window finder
    if (!hwnd) return 1.0f;
    
    // Try Windows 10+ API first
    typedef UINT(WINAPI* GetDpiForWindowFunc)(HWND);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        GetDpiForWindowFunc pGetDpiForWindow = (GetDpiForWindowFunc)GetProcAddress(user32, "GetDpiForWindow");
        if (pGetDpiForWindow) {
            UINT dpi = pGetDpiForWindow(hwnd);
            return (float)dpi / 96.0f; // 96 DPI = 100% scaling
        }
    }
    
    // Fallback to DC method
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
        return (float)dpiX / 96.0f;
    }
    
    return 1.0f;
}
// --- End DPI Helper ---

// --- D3D9 Hooking Globals ---
typedef HRESULT(WINAPI* EndScene_t)(LPDIRECT3DDEVICE9);
HRESULT WINAPI HookedEndScene(LPDIRECT3DDEVICE9 pDevice);
static EndScene_t oEndScene = nullptr;
static void* g_EndSceneTarget = nullptr; // store target vtable entry for cleanup
static const char* g_EndSceneTargetSource = "none";
static std::atomic<bool> g_EndSceneHookEnabled{ false };
// Track if our EndScene hook has ever been called (for diagnostics)
static std::atomic<bool> g_EndSceneObserved{ false };
// Prefer the standalone ImGui host when the in-game render path is known to be unavailable.
static std::atomic<bool> g_ExternalMenuFallbackNeeded{ false };
// Track ImGui init state within EndScene with atomic for thread safety
static std::atomic<bool> g_endSceneImguiInit{ false };
static std::atomic<bool> g_endSceneRelatchInProgress{ false };
// --- End D3D9 Globals ---

namespace {

constexpr size_t kEndSceneVtableIndex = 42;
// DDRAW decomp shows EfzRender::initTextRender/resetDev using ((DWORD*)this + 17)
// as the active IDirect3DDevice9*.
constexpr uintptr_t kEfzRenderDeviceOffset = 17u * sizeof(uintptr_t);

struct EndSceneHookCandidate {
    void* target = nullptr;
    const char* source = nullptr;
    uintptr_t renderObject = 0;
    uintptr_t device = 0;
    uintptr_t vtable = 0;
    std::string modulePath;
};

std::string FormatPointerValue(uintptr_t value) {
    char buffer[32] = {};
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "0x%08IX", value);
    return std::string(buffer);
}

std::string DescribeModuleForAddress(void* address) {
    if (!address) {
        return "<null>";
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || !mbi.AllocationBase) {
        return "<unknown>";
    }

    char pathBuf[MAX_PATH] = {};
    const DWORD count = GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), pathBuf, MAX_PATH);
    if (count == 0) {
        return "<unknown>";
    }
    return std::string(pathBuf, count);
}

bool ReadPointerValue(uintptr_t address, uintptr_t& outValue) {
    outValue = 0;
    return address != 0 && SafeReadMemory(address, &outValue, sizeof(outValue)) && outValue != 0;
}

void LogEndSceneCandidate(const EndSceneHookCandidate& candidate, const char* prefix) {
    if (!candidate.target) {
        return;
    }

    std::ostringstream oss;
    oss << (prefix ? prefix : "[OVERLAY][D3D9]")
        << " source=" << (candidate.source ? candidate.source : "unknown")
        << " target=" << FormatPointerValue(reinterpret_cast<uintptr_t>(candidate.target))
        << " module=" << candidate.modulePath;
    if (candidate.renderObject) {
        oss << " render=" << FormatPointerValue(candidate.renderObject);
    }
    if (candidate.device) {
        oss << " device=" << FormatPointerValue(candidate.device);
    }
    if (candidate.vtable) {
        oss << " vtable=" << FormatPointerValue(candidate.vtable);
    }
    LogOut(oss.str(), true);
}

bool BuildEndSceneCandidateFromRenderObject(uintptr_t renderObject, const char* source, EndSceneHookCandidate& outCandidate) {
    uintptr_t device = 0;
    if (!ReadPointerValue(renderObject + kEfzRenderDeviceOffset, device)) {
        return false;
    }

    uintptr_t vtable = 0;
    if (!ReadPointerValue(device, vtable)) {
        return false;
    }

    uintptr_t endSceneTarget = 0;
    if (!ReadPointerValue(vtable + (kEndSceneVtableIndex * sizeof(uintptr_t)), endSceneTarget)) {
        return false;
    }

    outCandidate.target = reinterpret_cast<void*>(endSceneTarget);
    outCandidate.source = source;
    outCandidate.renderObject = renderObject;
    outCandidate.device = device;
    outCandidate.vtable = vtable;
    outCandidate.modulePath = DescribeModuleForAddress(reinterpret_cast<void*>(endSceneTarget));
    return true;
}

bool TryResolveLiveEndSceneFromDdrawExport(EndSceneHookCandidate& outCandidate) {
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        return false;
    }

    using GetEfzRenderFn = void* (__cdecl*)();
    auto getEfzRender = reinterpret_cast<GetEfzRenderFn>(GetProcAddress(ddrawModule, "getEfzRender"));
    if (!getEfzRender) {
        return false;
    }

    uintptr_t renderObject = reinterpret_cast<uintptr_t>(getEfzRender());
    if (!renderObject) {
        LogOut("[OVERLAY][D3D9] getEfzRender export returned null", true);
        return false;
    }

    if (!BuildEndSceneCandidateFromRenderObject(renderObject, "live-ddraw-export", outCandidate)) {
        LogOut("[OVERLAY][D3D9] Failed to resolve EndScene from DDRAW getEfzRender() object", true);
        return false;
    }
    return true;
}

bool TryResolveLiveEndSceneFromRevival(EndSceneHookCandidate& outCandidate) {
    HMODULE revivalModule = GetModuleHandleA("EfzRevival.dll");
    if (!revivalModule) {
        return false;
    }

    const uintptr_t renderCtxRva = EFZ_RVA_RenderContextGlobal();
    if (!renderCtxRva) {
        return false;
    }

    uintptr_t renderObject = 0;
    const uintptr_t renderObjectAddr = reinterpret_cast<uintptr_t>(revivalModule) + renderCtxRva;
    if (!ReadPointerValue(renderObjectAddr, renderObject)) {
        LogOut("[OVERLAY][D3D9] Revival render-context pointer is not ready yet at " + FormatPointerValue(renderObjectAddr), detailedLogging.load());
        return false;
    }

    if (!BuildEndSceneCandidateFromRenderObject(renderObject, "live-revival-renderctx", outCandidate)) {
        LogOut("[OVERLAY][D3D9] Failed to resolve EndScene from Revival render context", true);
        return false;
    }
    return true;
}

bool TryResolveLiveEndSceneCandidate(EndSceneHookCandidate& outCandidate) {
    if (TryResolveLiveEndSceneFromDdrawExport(outCandidate)) {
        return true;
    }
    return TryResolveLiveEndSceneFromRevival(outCandidate);
}

bool AttachEndSceneHookForCandidate(const EndSceneHookCandidate& candidate, const char* trigger) {
    if (!candidate.target) {
        return false;
    }

    std::ostringstream oss;
    oss << "[OVERLAY][D3D9] Attaching EndScene hook"
        << " trigger=" << (trigger ? trigger : "unknown")
        << " source=" << (candidate.source ? candidate.source : "unknown")
        << " target=" << FormatPointerValue(reinterpret_cast<uintptr_t>(candidate.target))
        << " module=" << candidate.modulePath;
    LogOut(oss.str(), true);

    MH_STATUS cr = MH_CreateHook(candidate.target, HookedEndScene, reinterpret_cast<void**>(&oEndScene));
    if (cr != MH_OK && cr != MH_ERROR_ALREADY_CREATED) {
        const char* status = MH_StatusToString(cr);
        LogOut(std::string("[OVERLAY][D3D9] Failed to create EndScene hook source=")
               + (candidate.source ? candidate.source : "unknown")
               + " status=" + (status ? status : "<unknown>"),
               true);
        return false;
    }

    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        LogOut("[OVERLAY][D3D9] Aborting EndScene enable because netplay suspend became active during attach", true);
        if (cr == MH_OK) {
            MH_RemoveHook(candidate.target);
        }
        return false;
    }

    MH_STATUS er = MH_EnableHook(candidate.target);
    if (er != MH_OK && er != MH_ERROR_ENABLED) {
        const char* status = MH_StatusToString(er);
        LogOut(std::string("[OVERLAY][D3D9] Failed to enable EndScene hook source=")
               + (candidate.source ? candidate.source : "unknown")
               + " status=" + (status ? status : "<unknown>"),
               true);
        if (cr == MH_OK) {
            MH_RemoveHook(candidate.target);
        }
        return false;
    }

    g_EndSceneTarget = candidate.target;
    g_EndSceneTargetSource = candidate.source ? candidate.source : "unknown";
    DirectDrawHook::isHooked = true;
    g_EndSceneObserved.store(false, std::memory_order_release);
    g_EndSceneHookEnabled.store(true, std::memory_order_release);
    g_ExternalMenuFallbackNeeded.store(false, std::memory_order_release);
    LogEndSceneCandidate(candidate, "[OVERLAY][D3D9] EndScene hook attached");
    return true;
}

void RemoveEndSceneHookTarget(void* target) {
    if (!target) {
        return;
    }
    MH_DisableHook(target);
    MH_RemoveHook(target);
}

bool TryRelatchEndSceneToLiveTarget(const char* trigger) {
    bool expected = false;
    if (!g_endSceneRelatchInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    EndSceneHookCandidate liveCandidate{};
    const bool haveLiveCandidate = TryResolveLiveEndSceneCandidate(liveCandidate);
    if (haveLiveCandidate) {
        LogEndSceneCandidate(liveCandidate, "[OVERLAY][D3D9] Resolved live EndScene candidate");
    }

    if (!haveLiveCandidate || !liveCandidate.target) {
        LogOut(std::string("[OVERLAY][D3D9] Live EndScene relatch skipped trigger=")
               + (trigger ? trigger : "unknown")
               + " reason=no-live-candidate",
               true);
        g_endSceneRelatchInProgress.store(false, std::memory_order_release);
        return false;
    }

    if (liveCandidate.target == g_EndSceneTarget) {
        LogOut(std::string("[OVERLAY][D3D9] Live EndScene candidate already matches current hook target source=")
               + g_EndSceneTargetSource,
               true);
        g_endSceneRelatchInProgress.store(false, std::memory_order_release);
        return false;
    }

    void* previousTarget = g_EndSceneTarget;
    const char* previousSource = g_EndSceneTargetSource;
    RemoveEndSceneHookTarget(previousTarget);
    g_EndSceneTarget = nullptr;
    DirectDrawHook::isHooked = false;
    g_EndSceneHookEnabled.store(false, std::memory_order_release);

    if (AttachEndSceneHookForCandidate(liveCandidate, trigger)) {
        LogOut(std::string("[OVERLAY][D3D9] Live EndScene relatch succeeded previousSource=")
               + (previousSource ? previousSource : "unknown")
               + " newSource=" + (liveCandidate.source ? liveCandidate.source : "unknown"),
               true);
        g_endSceneRelatchInProgress.store(false, std::memory_order_release);
        return true;
    }

    if (previousTarget && previousTarget != liveCandidate.target) {
        EndSceneHookCandidate previousCandidate{};
        previousCandidate.target = previousTarget;
        previousCandidate.source = previousSource ? previousSource : "previous";
        previousCandidate.modulePath = DescribeModuleForAddress(previousTarget);
        if (AttachEndSceneHookForCandidate(previousCandidate, "restore-after-relatch-failure")) {
            LogOut("[OVERLAY][D3D9] Restored previous EndScene hook after live relatch failure", true);
        } else {
            LogOut("[OVERLAY][D3D9] Failed to restore previous EndScene hook after live relatch failure", true);
        }
    }

    g_endSceneRelatchInProgress.store(false, std::memory_order_release);
    return false;
}

} // namespace

// --- FIX: Add missing implementations for obsolete DirectDraw hooks ---
HRESULT WINAPI DirectDrawHook::HookedDirectDrawCreate(GUID* lpGUID, LPVOID* lplpDD, IUnknown* pUnkOuter) {
    // This function is obsolete. It is stubbed to resolve linker errors.
    // The new overlay method uses D3D9 hooking.
    if (originalDirectDrawCreate) {
        return originalDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
    }
    return E_FAIL;
}

HRESULT WINAPI DirectDrawHook::HookedBlit(IDirectDrawSurface7* This, LPRECT lpDestRect, IDirectDrawSurface7* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx) {
    // This function is obsolete. It is stubbed to resolve linker errors.
    if (originalBlit) {
        return originalBlit(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
    }
    return E_FAIL;
}

HRESULT WINAPI DirectDrawHook::HookedFlip(IDirectDrawSurface7* This, IDirectDrawSurface7* lpDDSurfaceTargetOverride, DWORD dwFlags) {
    // This function is obsolete. It is stubbed to resolve linker errors.
    if (originalFlip) {
        return originalFlip(This, lpDDSurfaceTargetOverride, dwFlags);
    }
    return E_FAIL;
}
// --- End of fix ---

// --- REVISED AND CORRECTED D3D9 EndScene Hook ---
HRESULT WINAPI HookedEndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (!g_EndSceneObserved.load()) g_EndSceneObserved.store(true);

    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        return oEndScene(pDevice);
    }

    if (!g_EndSceneHookEnabled.load(std::memory_order_acquire)) {
        return oEndScene(pDevice);
    }

    EndSceneHangScope _hangScope;
    SetEndScenePhase("XInput snapshot");

    // Refresh XInput snapshot once per frame at the start of EndScene; other systems read cached state
    XInputShim::RefreshSnapshotOncePerFrame();
    // Minimal per-frame timing (RAII) to detect stalls without per-frame logs
    struct EndSceneFrameTimer {
        std::chrono::steady_clock::time_point t0;
        EndSceneFrameTimer() : t0(std::chrono::steady_clock::now()) {}
        ~EndSceneFrameTimer() {
            using namespace std::chrono;
            static uint64_t frames = 0;
            static double   sumMs = 0.0;
            static double   maxMs = 0.0;
            static uint32_t gt33 = 0, gt100 = 0, gt250 = 0, gt500 = 0;
            static steady_clock::time_point lastReport{};
            static steady_clock::time_point lastMenuSlowReport{};
            auto t1 = steady_clock::now();
            double ms = duration<double, std::milli>(t1 - t0).count();
            frames++; sumMs += ms; if (ms > maxMs) maxMs = ms;
            if (ms > 33.0)  gt33++;
            if (ms > 100.0) gt100++;
            if (ms > 250.0) gt250++;
            if (ms > 500.0) gt500++;
            if (ImGuiImpl::IsVisible() && ms >= 100.0
                && (lastMenuSlowReport.time_since_epoch().count() == 0
                    || t1 - lastMenuSlowReport >= std::chrono::seconds(1))) {
                char slowBuf[192];
                _snprintf_s(slowBuf, sizeof(slowBuf), _TRUNCATE,
                    "[OVERLAY][D3D9][SLOW] Menu EndScene frame took %.1fms custom=%d",
                    ms, Config::GetSettings().useCustomMenu ? 1 : 0);
                LogOut(slowBuf, true);
                lastMenuSlowReport = t1;
            }
            if (lastReport.time_since_epoch().count() == 0) lastReport = t1;
            if (t1 - lastReport >= std::chrono::seconds(5)) {
                // Only report if diagnostics are enabled in config
                if (Config::GetSettings().enableFpsDiagnostics) {
                    double avgMs = (frames > 0) ? (sumMs / (double)frames) : 0.0;
                    double fps = (avgMs > 0.0) ? (1000.0 / avgMs) : 0.0;
                    char buf[256];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[FPS] 5s window: frames=%llu ~fps=%.1f avg=%.2fms max=%.1fms >33ms=%u >100ms=%u >250ms=%u >500ms=%u",
                        (unsigned long long)frames, fps, avgMs, maxMs, gt33, gt100, gt250, gt500);
                    LogOut(buf, true);
                }
                frames = 0; sumMs = 0.0; maxMs = 0.0; gt33 = gt100 = gt250 = gt500 = 0; lastReport = t1;
            }
        }
    } _frameTimerScope;
    if (!pDevice) {
        SetEndScenePhase("original EndScene: null device");
        return oEndScene(pDevice);
    }

    SetEndScenePhase("TestCooperativeLevel");
    if (SkipOverlayForLostDevice(pDevice)) {
        SetEndScenePhase("original EndScene: lost device");
        return oEndScene(pDevice);
    }

    LogActiveD3D9DeviceOnce(pDevice);

    // Determine current render target size
    SetEndScenePhase("QueryRenderTargetSize");
    UINT rtW = 0, rtH = 0;
    QueryRenderTargetSize(pDevice, rtW, rtH);
    // Only render on the actual 640x480 game surface
    if (!(rtW == 640 && rtH == 480)) {
        SetEndScenePhase("original EndScene: non-640 render target");
        return oEndScene(pDevice);
    }

    // Run external overlay renderers (e.g. ImprovedReplayMenu) as early as possible -
    // BEFORE ImGui initialisation - so they appear on the very first 640x480 frame
    // instead of waiting ~1s for ImGui to finish initialising. Drawn beneath the
    // ImGui layer; SEH-guarded per renderer inside DispatchRenderers.
    SetEndScenePhase("external overlay renderers");
    OverlayApi::DispatchRenderers(pDevice, rtW, rtH);

    // Thread-safe ImGui initialization (only once)
    SetEndScenePhase("ImGui initialization");
    if (!g_endSceneImguiInit.load(std::memory_order_acquire)) {
        // Try to claim the init slot
        bool expected = false;
        if (g_endSceneImguiInit.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // We won - initialize ImGui
            if (ImGuiImpl::Initialize(pDevice)) {
                g_ExternalMenuFallbackNeeded.store(false, std::memory_order_release);
                LogOut("[OVERLAY] ImGui initialized from EndScene hook.", true);
            } else {
                g_ExternalMenuFallbackNeeded.store(true, std::memory_order_release);
                LogOut("[OVERLAY] ImGui failed to initialize from EndScene hook.", true);
                // Keep g_endSceneImguiInit true to prevent retry spam
            }
        }
        // If we lost the race or init failed, just continue to render pass
        if (!ImGuiImpl::IsInitialized()) {
            SetEndScenePhase("original EndScene: ImGui unavailable");
            return oEndScene(pDevice);
        }
    }

    if (ImGuiImpl::IsVisible() && Config::GetSettings().useCustomMenu) {
        SetEndScenePhase("CustomMenu::PrepareFrame");
        CustomMenu::PrepareFrame();
    }

    // Start a new ImGui frame and feed inputs before NewFrame
    SetEndScenePhase("ImGui DX9 NewFrame");
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Feed controller/virtual-cursor inputs before NewFrame so they apply this frame
    SetEndScenePhase("ImGui input feed");
    ImGuiImpl::PreNewFrameInputs();
    SetEndScenePhase("ImGui::NewFrame");
    ImGui::NewFrame();
    // Post-NewFrame snapshot for diagnostics (throttled)
    ImGuiImpl::PostNewFrameDiagnostics();

    // PreNewFrameInputs already set DisplaySize to 640x480
    ImGuiIO& io = ImGui::GetIO();

    // If the game window is minimized, skip rendering to avoid ImGui asserting on zero-size display
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        ImGui::EndFrame();
        SetEndScenePhase("original EndScene: minimized");
        return oEndScene(pDevice);
    }

    // Helper: fullscreen check
    const bool fullscreenNow = IsEfzFullscreenCached();

    // Ensure OS cursor is hidden only when the menu is visible and fullscreen; otherwise show OS cursor
    if (ImGuiImpl::IsVisible() && fullscreenNow) {
        io.MouseDrawCursor = true;                // backend hides OS cursor
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    } else {
        io.MouseDrawCursor = false;               // show OS cursor
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }

    // Initialize the GIF only after a screen actually asks for it. Decoding and
    // creating D3D textures here on first menu-open is a visible hitch on some drivers.
    if (ImGuiImpl::IsVisible() && GifPlayer::ShouldAttemptLoad()) {
        SetEndScenePhase("GifPlayer::Initialize");
        GifPlayer::Initialize(pDevice);
    }

    // Render our custom text overlays using the background draw list
    SetEndScenePhase("RenderD3D9Overlays");
    DirectDrawHook::RenderD3D9Overlays(pDevice, rtW, rtH);

    // Render the main ImGui configuration window if it's visible
    if (ImGuiImpl::IsVisible()) {
        // Temporarily reduce style complexity and tessellation to lower DX9 draw cost
        ImGuiStyle& style = ImGui::GetStyle();
        const bool oldAAFill = style.AntiAliasedFill;
        const bool oldAALines = style.AntiAliasedLines;
        const float oldCurveTol = style.CurveTessellationTol;
        const float oldWindowRounding = style.WindowRounding;
        const float oldFrameRounding = style.FrameRounding;
        const float oldPopupRounding = style.PopupRounding;
        const float oldScrollbarRounding = style.ScrollbarRounding;
        const float oldGrabRounding = style.GrabRounding;
        const float oldWindowBorder = style.WindowBorderSize;
        const float oldFrameBorder = style.FrameBorderSize;

        style.AntiAliasedFill = false;
        style.AntiAliasedLines = false;
        style.CurveTessellationTol = 1.5f;      // fewer verts on curves
        style.WindowRounding = 0.0f;
        style.FrameRounding = 0.0f;
        style.PopupRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding = 0.0f;
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;

        if (Config::GetSettings().useCustomMenu) {
            SetEndScenePhase("CustomMenu::Render");
            CustomMenu::Render();
        } else {
            SetEndScenePhase("ImGuiGui::RenderGui");
            ImGuiGui::RenderGui();
        }

        // Restore style
        style.AntiAliasedFill = oldAAFill;
        style.AntiAliasedLines = oldAALines;
        style.CurveTessellationTol = oldCurveTol;
        style.WindowRounding = oldWindowRounding;
        style.FrameRounding = oldFrameRounding;
        style.PopupRounding = oldPopupRounding;
        style.ScrollbarRounding = oldScrollbarRounding;
        style.GrabRounding = oldGrabRounding;
        style.WindowBorderSize = oldWindowBorder;
        style.FrameBorderSize = oldFrameBorder;
    }

    // Advance GIF animation timing at ~24 FPS only while a GIF row is visible.
    if (ImGuiImpl::IsVisible() && GifPlayer::WasRequestedThisFrame()) {
        static double gifAccum = 0.0;
        ImGuiIO& io = ImGui::GetIO();
        double dt = io.DeltaTime > 0.f ? (double)io.DeltaTime : (1.0/60.0);
        gifAccum += dt;
        const double interval = 1.0 / 24.0;
        if (gifAccum >= interval) {
            SetEndScenePhase("GifPlayer::Update");
            GifPlayer::Update(gifAccum);
            gifAccum = 0.0;
        }
    }
    GifPlayer::EndFrame();

    // End the frame and render all accumulated draw data
    SetEndScenePhase("ImGui::Render");
    ImGui::EndFrame();
    ImGui::Render();

    // Guard again in case size changed mid-frame
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        SetEndScenePhase("ImGui_ImplDX9_RenderDrawData");
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }

    SetEndScenePhase("original EndScene");
    return oEndScene(pDevice);
}


// Render text on the given surface
void DirectDrawHook::RenderText(HDC hdc, const std::string& text, int x, int y, COLORREF color) {
    if (!hdc || text.empty()) return;
    
    // Set up the text properties
    SetBkMode(hdc, TRANSPARENT);
    
    // Create a better font for our overlay - DPI-aware scaling
    float dpiScale = GetDpiScale();
    int fontSize = (int)(20.0f * dpiScale); // Scale font size with DPI
    
    HFONT font = CreateFont(
        fontSize,                  // Height (DPI-scaled)
        0,                         // Width
        0,                         // Escapement
        0,                         // Orientation
        FW_BOLD,                   // Weight
        FALSE,                     // Italic
        FALSE,                     // Underline
        FALSE,                     // StrikeOut
        DEFAULT_CHARSET,           // CharSet
        OUT_TT_PRECIS,             // OutPrecision (use TrueType for better quality)
        CLIP_DEFAULT_PRECIS,       // ClipPrecision
        CLEARTYPE_QUALITY,         // Quality (ClearType for smoother text)
        DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily
        "Arial"                    // FaceName
    );
    
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    
    // Get screen width to avoid going off-screen
    // Using 640 as default game window width, and allowing 20px margin
    const int screenWidth = 640;
    const int MAX_TEXT_WIDTH = screenWidth - x - 20;
    
    // Check if this is a trigger overlay by position (right-aligned text)
    bool isTriggerOverlay = (x >= 510 && y >= 100 && y <= 200);
    
    // For trigger overlay, adjust X position instead of truncating
    int adjustedX = x;
    std::string displayText = text;
    
    // Measure text size
    SIZE textSize;
    GetTextExtentPoint32A(hdc, text.c_str(), text.length(), &textSize);
    
    if (isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
        // Calculate how far left we need to move the text
        // to ensure the last character is at the right edge
        adjustedX = screenWidth - 20 - textSize.cx;
    } else if (!isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
        // For regular overlay, truncate text from the right
        displayText = FitTextToWidth(text, MAX_TEXT_WIDTH, hdc);
    }
    
    // Draw drop shadow first (for better visibility)
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, adjustedX + 2, y + 2, displayText.c_str(), displayText.length());
    
    // Draw the main text
    SetTextColor(hdc, color);
    TextOutA(hdc, adjustedX, y, displayText.c_str(), displayText.length());
    
    // Clean up
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// Render simple text with a black outline for visibility
void DirectDrawHook::RenderSimpleText(IDirectDrawSurface7* surface, const std::string& text, int x, int y, COLORREF color) {
    // This function is for the old DirectDraw hook and is no longer the primary rendering path.
    // Get device context for drawing
    HDC hdc;
    HRESULT hr = surface->GetDC(&hdc);
    if (FAILED(hr)) {
        LogOut("[OVERLAY] Failed to get DC: " + std::to_string(hr), true);
        return;
    }
    
    try {
        // Set up text properties similar to EFZ's style
        SetBkMode(hdc, TRANSPARENT);
        
        // Create a font that should be compatible with EFZ - DPI-aware scaling
        float dpiScale = GetDpiScale();
        int fontSize = (int)(14.0f * dpiScale); // Scale font size with DPI
        
        HFONT font = CreateFont(
            fontSize,                  // Height - DPI-scaled
            0,                         // Width (auto)
            0,                         // Escapement
            0,                         // Orientation
            FW_NORMAL,                 // Weight - normal instead of bold
            FALSE,                     // Italic
            FALSE,                     // Underline
            FALSE,                     // StrikeOut
            DEFAULT_CHARSET,           // CharSet
            OUT_TT_PRECIS,             // OutputPrecision (TrueType for better quality)
            CLIP_DEFAULT_PRECIS,       // ClipPrecision
            CLEARTYPE_QUALITY,         // Quality (ClearType for smoother text)
            DEFAULT_PITCH | FF_DONTCARE, // PitchAndFamily
            "MS Sans Serif"            // Face name
        );
        
        HFONT oldFont = (HFONT)SelectObject(hdc, font);
        
        // Get screen width to avoid going off-screen (640 is standard game width)
        const int screenWidth = 640;
        const int MAX_TEXT_WIDTH = screenWidth - x - 20;
        
        // Check if this is a trigger overlay by position (right-aligned text)
        bool isTriggerOverlay = (x >= 510 && y >= 100 && y <= 200);
        
        // For trigger overlay, adjust X position instead of truncating
        int adjustedX = x;
        std::string displayText = text;
        
        // Measure text size
        SIZE textSize;
        GetTextExtentPoint32A(hdc, text.c_str(), text.length(), &textSize);
        
        if (isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
            // Calculate how far left we need to move the text
            // to ensure the last character is at the right edge
            adjustedX = screenWidth - 20 - textSize.cx;
        } else if (!isTriggerOverlay && textSize.cx > MAX_TEXT_WIDTH) {
            // For regular overlay, truncate text from the right
            displayText = FitTextToWidth(text, MAX_TEXT_WIDTH, hdc);
        }
        
        // Draw black outline for better visibility (thinner outline)
        SetTextColor(hdc, RGB(0, 0, 0));
        TextOutA(hdc, adjustedX + 1, y, displayText.c_str(), displayText.length());
        TextOutA(hdc, adjustedX - 1, y, displayText.c_str(), displayText.length());
        TextOutA(hdc, adjustedX, y + 1, displayText.c_str(), displayText.length());
        TextOutA(hdc, adjustedX, y - 1, displayText.c_str(), displayText.length());
        
        // Draw main text
        SetTextColor(hdc, color);
        TextOutA(hdc, adjustedX, y, displayText.c_str(), displayText.length());
        
        // Clean up
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        
    } catch (...) {
        LogOut("[OVERLAY] Exception in RenderSimpleText", true);
    }
    
    surface->ReleaseDC(hdc);
}

// NEW: Implement the D3D9 overlay renderer
void DirectDrawHook::RenderD3D9Overlays(LPDIRECT3DDEVICE9 pDevice, UINT rtW, UINT rtH) {
    // Use background list for borders/messages and foreground for the cursor so it draws above windows
    auto bgList = ImGui::GetBackgroundDrawList();
    if (!bgList)
        return;

    std::vector<OverlayMessage> permanentSnapshot;
    std::vector<OverlayMessage> temporarySnapshot;
    {
        std::lock_guard<std::mutex> lock(messagesMutex);
        const auto now = std::chrono::steady_clock::now();
        messages.erase(std::remove_if(messages.begin(), messages.end(),
            [&](const OverlayMessage& msg) {
                return !msg.isPermanent && msg.expireTime <= now;
            }), messages.end());
        permanentSnapshot.assign(permanentMessages.begin(), permanentMessages.end());
        temporarySnapshot.assign(messages.begin(), messages.end());
    }

    // If the ImGui menu is visible and there are no messages, skip message rendering only
    // (but still allow the cursor to render on top of the UI)
    const bool menuVisibleNow = ImGuiImpl::IsVisible();
    const bool haveMessages = !permanentSnapshot.empty() || !temporarySnapshot.empty();
    const bool haveCollisionOverlay = CollisionDisplay::IsAnyLayerEnabled();
    if (!menuVisibleNow && !g_ShowOverlayDebugBorders.load() && !haveMessages && !haveCollisionOverlay) {
        return;
    }

    bool skipMessageRendering = menuVisibleNow && !g_ShowOverlayDebugBorders.load() && !haveMessages;

    // --- Identify current D3D9 render target (needed for mapping to inner 4:3 area) ---
    if ((rtW == 0 || rtH == 0) && !QueryRenderTargetSize(pDevice, rtW, rtH)) {
        return;
    }

    // Compute inner 4:3 game area within current RT (letterbox/pillarbox safe)
    const float baseW = 640.0f;
    const float baseH = 480.0f;
    float ox = 0.0f, oy = 0.0f, gw = (float)rtW, gh = (float)rtH, scale = 1.0f;
    if (rtW > 0 && rtH > 0) {
        const float sx = (float)rtW / baseW;
        const float sy = (float)rtH / baseH;
        scale = (sx < sy) ? sx : sy;
        gw = baseW * scale;
        gh = baseH * scale;
        ox = ((float)rtW - gw) * 0.5f;
        oy = ((float)rtH - gh) * 0.5f;
    }
    
    // Track render target size changes and log when it changes (mutex-protected to prevent duplicate logs)
    {
        std::lock_guard<std::mutex> lock(g_rtSizeMutex);
        
        // Use atomic exchange to ensure only ONE thread logs initially
        bool expected = false;
        if (g_rtSizeLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            // We won the race - this is the ONLY initial log
            g_prevRtW = rtW;
            g_prevRtH = rtH;
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            LogOut("[OVERLAY][D3D9] RT size=" + std::to_string(rtW) + "x" + std::to_string(rtH) +
                   " ImGui.DisplaySize=" + std::to_string((int)ds.x) + "x" + std::to_string((int)ds.y), true);
        } else if (rtW != g_prevRtW || rtH != g_prevRtH) {
            // Already logged before AND size changed - log the change
            g_prevRtW = rtW;
            g_prevRtH = rtH;
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            LogOut("[OVERLAY][D3D9] RT size CHANGED: " + std::to_string(rtW) + "x" + std::to_string(rtH) +
                   " ImGui.DisplaySize=" + std::to_string((int)ds.x) + "x" + std::to_string((int)ds.y), true);
        }
        // If exchange failed and size unchanged - do nothing
    }
    
    // If ImGui menu is visible, skip heavy debug borders to reduce draw load
    if (g_ShowOverlayDebugBorders.load() && !menuVisibleNow && rtW > 0 && rtH > 0) {
        // Full render-target border (red)
        bgList->AddRect(ImVec2(1.5f, 1.5f), ImVec2((float)rtW - 1.5f, (float)rtH - 1.5f), IM_COL32(255, 0, 0, 200), 0.0f, 0, 3.0f);
        // Label
        char lbl[64];
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "D3D9 RT %ux%u", rtW, rtH);
        bgList->AddText(ImVec2(8.0f, 8.0f), IM_COL32(255, 0, 0, 220), lbl);

        // Inner game-area border assuming 640x480 letterbox (green)
        bgList->AddRect(ImVec2(ox + 1.0f, oy + 1.0f), ImVec2(ox + gw - 1.0f, oy + gh - 1.0f), IM_COL32(0, 255, 0, 200), 0.0f, 0, 2.0f);
    }

    // Hitbox / hurtbox / collision display. The collector emits 640x480
    // virtual framebuffer coordinates, so use the same letterbox-safe mapping
    // as the rest of the custom overlay.
    if (haveCollisionOverlay) {
        CollisionDisplay::RebuildFrame();
        const std::size_t boxCount = CollisionDisplay::GetOverlayBoxCount();
        for (std::size_t i = 0; i < boxCount; ++i) {
            CollisionDisplay::OverlayBox box{};
            if (!CollisionDisplay::GetOverlayBox(i, &box)) {
                continue;
            }

            if (box.shape == CollisionDisplay::ShapeDot) {
                const ImVec2 center(ox + box.x * scale, oy + box.y * scale);
                const float radius = (box.w > 0.0f ? box.w : 2.5f) * scale;
                bgList->AddCircleFilled(center, radius, ImColorFromArgb(box.fillArgb), 16);
                bgList->AddCircle(center, radius + 1.0f, ImColorFromArgb(box.outlineArgb), 16, 1.0f);
            } else {
                const ImVec2 p0(ox + box.x * scale, oy + box.y * scale);
                const ImVec2 p1(ox + (box.x + box.w) * scale, oy + (box.y + box.h) * scale);
                bgList->AddRectFilled(p0, p1, ImColorFromArgb(box.fillArgb));
                bgList->AddRect(p0, p1, ImColorFromArgb(box.outlineArgb), 0.0f, 0, 1.0f);
            }
        }
    }

    // FrameBar overlay (toggle-gated). Drawn before other messages so message
    // text floats on top of the bar if they happen to overlap.
    if (FrameBar::g_enabled.load() && !menuVisibleNow) {
        FrameBar::DrawCtx fbCtx;
        fbCtx.ox = ox;
        fbCtx.oy = oy;
        fbCtx.scale = scale;
        FrameBar::Render(fbCtx);
    }

    // Optional: draw a single combined background for split frame-advantage messages
    bool faCombinedBgDrawn = false;
    if (!ImGuiImpl::IsVisible()) {
        if (g_FrameAdvantageId != -1 && g_FrameAdvantage2Id != -1) {
            const OverlayMessage* faLeft = nullptr;
            const OverlayMessage* faRight = nullptr;
            for (const auto& pm : permanentSnapshot) {
                if (pm.id == g_FrameAdvantageId) faLeft = &pm;
                else if (pm.id == g_FrameAdvantage2Id) faRight = &pm;
            }
            if (faLeft && faRight) {
                // Map positions
                ImVec2 leftPos(ox + faLeft->xPos * scale, oy + faLeft->yPos * scale);
                ImVec2 rightPos(ox + faRight->xPos * scale, oy + faRight->yPos * scale);
                // Measure text sizes (no additional scaling, consistent with existing renderer)
                ImVec2 leftSize = ImGui::CalcTextSize(faLeft->text.c_str());
                ImVec2 rightSize = ImGui::CalcTextSize(faRight->text.c_str());
                // Build union background rect with same padding as individual messages
                ImVec2 ul(
                    (leftPos.x < rightPos.x ? leftPos.x : rightPos.x) - 4.0f,
                    (leftPos.y < rightPos.y ? leftPos.y : rightPos.y) - 2.0f
                );
                ImVec2 lr(
                    (leftPos.x + leftSize.x > rightPos.x + rightSize.x ? leftPos.x + leftSize.x : rightPos.x + rightSize.x) + 4.0f,
                    (leftPos.y + leftSize.y > rightPos.y + rightSize.y ? leftPos.y + leftSize.y : rightPos.y + rightSize.y) + 2.0f
                );
                bgList->AddRectFilled(ul, lr, IM_COL32(0, 0, 0, 180));
                faCombinedBgDrawn = true;
            }
        }
    }

    // Helper lambda to render a message with a background
    auto renderMessage = [&](const OverlayMessage& msg) {
        // Check if this is a trigger overlay by position
        bool isTriggerOverlay = (msg.xPos >= 510 && msg.yPos >= 100 && msg.yPos <= 200);
        
    // Map starting position from 640x480 virtual space to inner game area within current RT
    ImVec2 textPos(ox + msg.xPos * scale, oy + msg.yPos * scale);
        // Avoid CalcTextSize if we won't draw a background or adjust alignment
        ImVec2 textSize(0.f, 0.f);
    const bool needSize = (!ImGuiImpl::IsVisible()) || isTriggerOverlay;
        if (needSize) {
            textSize = ImGui::CalcTextSize(msg.text.c_str());
        }
        
        // Background quads are expensive in DX9; skip them when menu is up
    if (!ImGuiImpl::IsVisible()) {
            // If we already drew a combined FA background, skip individual bgs for those ids
            if (faCombinedBgDrawn && (msg.id == g_FrameAdvantageId || msg.id == g_FrameAdvantage2Id)) {
                // no per-message background
            } else {
            if (isTriggerOverlay) {
                // For trigger overlays, adjust X so text ends at right edge of inner game area
                const float margin = 20.0f;        // Margin from screen edge in virtual space
                const float targetX = ox + (baseW - margin) * scale;  // right edge inside inner area
                if (textPos.x + textSize.x > targetX) {
                    textPos.x = targetX - textSize.x;
                }
            }
            // Draw background behind text only when menu is hidden
            if (textSize.x > 0.f && textSize.y > 0.f && msg.backgroundAlpha > 0) {
                bgList->AddRectFilled(
                    ImVec2(textPos.x - 4, textPos.y - 2),
                    ImVec2(textPos.x + textSize.x + 4, textPos.y + textSize.y + 2),
                    IM_COL32(0, 0, 0, msg.backgroundAlpha)
                );
            }
            }
        }
        
        // Extract color components
        int r = (msg.color & 0xFF);
        int g = ((msg.color >> 8) & 0xFF);
        int b = ((msg.color >> 16) & 0xFF);
        
        // Draw text
    bgList->AddText(ImVec2((float)textPos.x, (float)textPos.y), IM_COL32(r, g, b, 255), msg.text.c_str());
    };

    // Render messages with a soft cap when menu is open (unless skipped to reduce draw calls)
    if (!skipMessageRendering) {
        const bool limitMessages = ImGuiImpl::IsVisible();
        const int cap = limitMessages ? 24 : INT_MAX;
        int drawn = 0;
        // Permanent first
        for (const auto& msg : permanentSnapshot) {
            renderMessage(msg);
            if (++drawn >= cap) break;
        }
        // Then temporary until cap
        if (drawn < cap) {
            for (const auto& msg : temporarySnapshot) {
                renderMessage(msg);
                if (++drawn >= cap) break;
            }
        }
    }

    // --- Fullscreen cursor dot (mouse + gamepad) ---
    // Helper: fullscreen check
    // Draw the overlay cursor only when the ImGui menu is visible and fullscreen
    if (ImGuiImpl::IsVisible()) {
        const bool fullscreenNow_local = IsEfzFullscreenCached();
        if (!fullscreenNow_local) {
            // Do not draw dot in windowed mode
            // (menu still works; OS cursor is visible in windowed)
            // Continue without returning to allow any remaining overlay work after this block in future
        } else {
            ImGuiIO& io = ImGui::GetIO();
            // Initialize pad cursor to screen center
            static ImVec2 padPos = ImVec2(0.f, 0.f);
            if (padPos.x == 0.f && padPos.y == 0.f) {
                padPos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            }

            const bool customMenuActive = Config::GetSettings().useCustomMenu ||
                                          ImGuiImpl::IsExternalFallbackHost();

            // Dot visibility policy:
            //  - Only show after we've moved the physical mouse OR, for the legacy ImGui menu,
            //    the analog stick (not the dpad).
            //  - The custom menu owns controller navigation directly, so controller analog must
            //    not synthesize a fullscreen cursor dot there. Otherwise DirectInput fallback
            //    devices with bogus resting axes can look like stray mouse movement.
            //  - If the controller reports dpad and analog simultaneously (mixbox-style), suppress the dot briefly so only nav cursor moves.
            static bool  s_mouseDotActivated = false;  // latched after real physical mouse movement
            static bool  s_padDotActivated   = false;  // latched after legacy-menu analog movement
            static float s_dotSuppressSec = 0.0f;   // suppression cooldown while mixed input is detected
            static bool  s_prevMenuVis    = false;  // detect menu open edge to reset state
            // Declare physical mouse tracker near top so we can reset on menu open edge
            static ImVec2 s_lastPhysMouse(-1.f,-1.f);
            if (ImGuiImpl::IsVisible() && !s_prevMenuVis) {
                s_mouseDotActivated = false;
                s_padDotActivated = false;
                s_dotSuppressSec = 0.0f;
                // Reset physical mouse tracker so prior movement doesn't immediately activate the dot
                s_lastPhysMouse = ImVec2(-1.f, -1.f);
            }
            s_prevMenuVis = ImGuiImpl::IsVisible();

            // Poll gamepad
            bool padActive = false;
            if (!customMenuActive) {
                XINPUT_STATE state{};
                if (const XINPUT_STATE* s = XInputShim::GetCachedState(0)) { state = *s; {
                    auto applyDeadzone = [](SHORT v, SHORT dz) -> float {
                        int iv = (int)v;
                        if (iv > dz) iv -= dz; else if (iv < -dz) iv += dz; else iv = 0;
                        float n = (float)iv / (32767.0f - dz);
                        if (n > 1.f) n = 1.f; if (n < -1.f) n = -1.f;
                        return n;
                    };
                    float nx = applyDeadzone(state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                    float ny = applyDeadzone(state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                    float dpadX = 0.f, dpadY = 0.f;
                    if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) dpadX += 1.f;
                    if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  dpadX -= 1.f;
                    if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP)    dpadY -= 1.f;
                    if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  dpadY += 1.f;

                    const float dt = io.DeltaTime > 0.f ? io.DeltaTime : (1.f/60.f);
                    const bool fast      = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
                    const float baseSpeed = fast ? 1800.f : 900.f;

                    // Determine input sources
                    const bool analogMoved = (fabsf(nx) > 0.02f || fabsf(ny) > 0.02f);
                    const bool dpadMoved   = (dpadX != 0.f || dpadY != 0.f);

                    // Mixed input => suppress dot briefly so only navigational cursor responds
                    if (analogMoved && dpadMoved) {
                        s_dotSuppressSec = 0.25f; // ~250 ms
                    }

                    // Decrement suppression timer
                    if (s_dotSuppressSec > 0.f) {
                        s_dotSuppressSec -= dt;
                        if (s_dotSuppressSec < 0.f) s_dotSuppressSec = 0.f;
                    }

                    // Activate dot only on pure analog movement (no dpad)
                    if (analogMoved && !dpadMoved) {
                        s_padDotActivated = true;
                        padActive = true;
                        padPos.x += nx * baseSpeed * dt;
                        padPos.y += -ny * baseSpeed * dt;
                    }
                }}
            } else {
                s_dotSuppressSec = 0.0f;
            }

            // Physical mouse movement detection (ignore backend remap deltas):
            // We only activate from real OS cursor movement (>=2px in client space).
            bool physicalMouseMoved = false;
            HWND hwnd = FindEFZWindow();
            if (hwnd) {
                POINT pt; if (GetCursorPos(&pt) && ScreenToClient(hwnd,&pt)) {
                    ImVec2 cur((float)pt.x,(float)pt.y);
                    if (s_lastPhysMouse.x >= 0.f) {
                        float dx = cur.x - s_lastPhysMouse.x;
                        float dy = cur.y - s_lastPhysMouse.y;
                        if ((dx*dx + dy*dy) > 4.f) physicalMouseMoved = true; // >2px
                    }
                    s_lastPhysMouse = cur;
                }
            }
            if (physicalMouseMoved) {
                s_mouseDotActivated = true; // real mouse movement only
            }
            ImVec2 dot = physicalMouseMoved ? io.MousePos : (padActive ? padPos : io.MousePos);

            // Clamp to ImGui display size (we render on 640x480 RT only)
            float maxX = io.DisplaySize.x - 1.0f;
            float maxY = io.DisplaySize.y - 1.0f;
            if (maxX < 0.f) maxX = 0.f; if (maxY < 0.f) maxY = 0.f;
            dot.x = (dot.x < 0.0f ? 0.0f : (dot.x > maxX ? maxX : dot.x));
            dot.y = (dot.y < 0.0f ? 0.0f : (dot.y > maxY ? maxY : dot.y));

            // Draw dot only when activated (from physical mouse or pure analog) and not suppressed
            const bool dotActivated = s_mouseDotActivated ||
                                      (!customMenuActive && s_padDotActivated);
            if (dotActivated && s_dotSuppressSec <= 0.f) {
                auto cursorList = ImGui::GetForegroundDrawList();
                if (!cursorList) cursorList = bgList;
                const float r = 4.5f;
                cursorList->PushClipRectFullScreen();
                cursorList->AddCircleFilled(dot, r, IM_COL32(255, 255, 255, 230), 20);
                cursorList->AddCircle(dot, r + 1.2f, IM_COL32(0, 0, 0, 200), 24, 2.0f);
                cursorList->PopClipRect();
            }
        }
    }
}

// Render all current messages on the surface
void DirectDrawHook::RenderAllMessages(IDirectDrawSurface7* surface) {
    if (!surface) return;
    
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    // Get the current time
    auto now = std::chrono::steady_clock::now();
    
    // Check if character data is initialized
    uintptr_t base = GetEFZBase();
    bool showHelloWorld = false;
    
    if (base) {
        // Check if both players have valid HP values (indicating game is active)
        uintptr_t hpAddr1 = ResolvePointer(base, EFZ_BASE_OFFSET_P1, HP_OFFSET);
        uintptr_t hpAddr2 = ResolvePointer(base, EFZ_BASE_OFFSET_P2, HP_OFFSET);
        
        if (hpAddr1 && hpAddr2) {
            int hp1 = 0, hp2 = 0;
            if (SafeReadMemory(hpAddr1, &hp1, sizeof(int)) && 
                SafeReadMemory(hpAddr2, &hp2, sizeof(int))) {
                // Show "Hello, world" if both players have valid HP (indicating match is active)
                showHelloWorld = (hp1 > 0 && hp2 > 0 && hp1 <= MAX_HP && hp2 <= MAX_HP);
                
                if (showHelloWorld) {
                    LogOut("[OVERLAY] Showing Hello World - HP1: " + std::to_string(hp1) + ", HP2: " + std::to_string(hp2), detailedLogging.load());
                }
            }
        }
    }
    
    // --- DEBUG: Log unique surface and draw a border to visualize which surface we render to ---
    static std::unordered_set<IDirectDrawSurface7*> s_seen;
    if (s_seen.insert(surface).second) {
        char ptrbuf[32] = {};
        _snprintf_s(ptrbuf, sizeof(ptrbuf), _TRUNCATE, "%p", surface);
        DDSURFACEDESC2 desc{}; desc.dwSize = sizeof(desc);
        if (SUCCEEDED(surface->GetSurfaceDesc(&desc))) {
         LogOut(std::string("[OVERLAY][DDRAW] First seen surface=") + ptrbuf +
             " size=" + std::to_string((int)desc.dwWidth) + "x" + std::to_string((int)desc.dwHeight), detailedLogging.load());
        } else {
            LogOut(std::string("[OVERLAY][DDRAW] First seen surface=") + ptrbuf + " (GetSurfaceDesc failed)", detailedLogging.load());
        }
    }

    // CRITICAL FIX: Use a more aggressive rendering approach
    HDC hdc;
    if (FAILED(surface->GetDC(&hdc))) {
    LogOut("[OVERLAY] Failed to get surface DC", detailedLogging.load());
        return;
    }
    
    try {
        // Set up for high-visibility rendering
        SetBkMode(hdc, TRANSPARENT);

        // Draw a thick magenta border and label around the whole surface for on-screen identification
        if (g_ShowOverlayDebugBorders.load()) {
            DDSURFACEDESC2 desc{}; desc.dwSize = sizeof(desc);
            if (SUCCEEDED(surface->GetSurfaceDesc(&desc))) {
                HPEN pen = CreatePen(PS_SOLID, 4, RGB(255, 0, 255));
                HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
                HPEN oldPen = (HPEN)SelectObject(hdc, pen);
                int w = (int)desc.dwWidth, h = (int)desc.dwHeight;
                Rectangle(hdc, 1, 1, w - 1, h - 1);
                // Label
                SetTextColor(hdc, RGB(255, 0, 255));
                char ptrbuf[32] = {};
                _snprintf_s(ptrbuf, sizeof(ptrbuf), _TRUNCATE, "%p", surface);
                std::string label = "DDRAW " + std::to_string(w) + "x" + std::to_string(h) + " " + ptrbuf;
                TextOutA(hdc, 6, 6, label.c_str(), (int)label.size());
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBrush);
                DeleteObject(pen);
            }
        }
        
        // Render "Hello, world" text if characters are initialized
        if (showHelloWorld) {
            // Use multiple rendering techniques to ensure visibility
            
            // Method 1: Large, bold text with heavy outline
            HFONT bigFont = CreateFont(
                32,                        // Much larger height
                0,                         // Width (auto)
                0,                         // Escapement
                0,                         // Orientation
                FW_BOLD,                   // Bold weight
                FALSE,                     // Italic
                FALSE,                     // Underline
                FALSE,                     // StrikeOut
                DEFAULT_CHARSET,           // CharSet
                OUT_DEFAULT_PRECIS,        // OutputPrecision
                CLIP_DEFAULT_PRECIS,       // ClipPrecision
                ANTIALIASED_QUALITY,       // Quality
                DEFAULT_PITCH | FF_SWISS,  // PitchAndFamily
                "Arial"                    // Face name
            );
            
            HFONT oldFont = (HFONT)SelectObject(hdc, bigFont);
            
            // Draw heavy black outline (multiple passes)
            SetTextColor(hdc, RGB(0, 0, 0));
            for (int dx = -3; dx <= 3; dx++) {
                for (int dy = -3; dy <= 3; dy++) {
                    if (dx != 0 || dy != 0) {
                        TextOutA(hdc, 50 + dx, 50 + dy, "Hello, world!", 13);
                    }
                }
            }
            
            // Draw main text in bright yellow
            SetTextColor(hdc, RGB(255, 255, 0));
            TextOutA(hdc, 50, 50, "Hello, world!", 13);
            
            // Also draw at multiple positions to ensure visibility
            SetTextColor(hdc, RGB(0, 255, 0));
            TextOutA(hdc, 200, 100, "OVERLAY TEST", 12);
            
            SetTextColor(hdc, RGB(255, 0, 255));
            TextOutA(hdc, 400, 150, "EFZ TRAINING", 12);
            
            SelectObject(hdc, oldFont);
            DeleteObject(bigFont);
            
            LogOut("[OVERLAY] Rendered Hello World with enhanced visibility", detailedLogging.load());
        }
        
        // Render permanent messages
        for (const auto& msg : permanentMessages) {
            RenderText(hdc, msg.text, msg.xPos, msg.yPos, msg.color);
        }
        
        // Render temporary messages and remove expired ones
        auto it = messages.begin();
        while (it != messages.end()) {
            if (now >= it->expireTime) {
                it = messages.erase(it);
            } else {
                RenderText(hdc, it->text, it->xPos, it->yPos, it->color);
                ++it;
            }
        }
        
    } catch (...) {
    LogOut("[OVERLAY] Exception in RenderAllMessages", detailedLogging.load());
    }
    
    // Release the device context
    surface->ReleaseDC(hdc);
}

// Initialize the hook
bool DirectDrawHook::Initialize() {
    if (isHooked) return true;
    
    LogOut("[OVERLAY] Initializing DirectDraw hook", true);
    
    // Use the robust FindEFZWindow function (validates PID for multi-instance safety)
    gameWindow = FindEFZWindow();
    if (!gameWindow) {
        LogOut("[OVERLAY] Could not find EFZ window using FindEFZWindow()", true);
        
        // Fallback: try FindWindowA but validate PID
        DWORD ourPid = GetCurrentProcessId();
        auto tryWindow = [ourPid](const char* title) -> HWND {
            HWND hwnd = FindWindowA(NULL, title);
            if (hwnd) {
                DWORD windowPid = 0;
                GetWindowThreadProcessId(hwnd, &windowPid);
                if (windowPid == ourPid) return hwnd;
            }
            return NULL;
        };
        
        gameWindow = tryWindow("Eternal Fighter Zero");
        if (!gameWindow) {
            gameWindow = tryWindow("Eternal Fighter Zero -Revival-");
        }
        if (!gameWindow) {
            gameWindow = tryWindow("Eternal Fighter Zero -Revival- 1.02e");
        }
        
        if (!gameWindow) {
            LogOut("[OVERLAY] All window finding methods failed", true);
            return false;
        } else {
            LogOut("[OVERLAY] Found EFZ window using alternative method", true);
        }
    } else {
        LogOut("[OVERLAY] Found EFZ window using FindEFZWindow()", true);
    }
    
    // Get the module handle for ddraw.dll
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        LogOut("[OVERLAY] ddraw.dll not loaded", true);
        return false;
    }
    
    // Get the DirectDrawCreate function address
    originalDirectDrawCreate = reinterpret_cast<DirectDrawCreateFunc>(
        GetProcAddress(ddrawModule, "DirectDrawCreate"));
        
    if (!originalDirectDrawCreate) {
        LogOut("[OVERLAY] Could not find DirectDrawCreate function", true);
        return false;
    }
    
    // Hook DirectDrawCreate using Microsoft Detours
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    
    DetourAttach(&(PVOID&)originalDirectDrawCreate, HookedDirectDrawCreate);
    
    LONG result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        LogOut("[OVERLAY] Failed to hook DirectDrawCreate: " + std::to_string(result), true);
        return false;
    }
    
    isHooked = true;
    LogOut("[OVERLAY] DirectDraw hook initialized successfully", true);
    return true;
}

// Clean up the hook
void DirectDrawHook::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(messagesMutex);
        messages.clear();
        permanentMessages.clear();
    }
    ShutdownD3D9();
    // Detach DirectDrawCreate detour if installed
    if (originalDirectDrawCreate) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)originalDirectDrawCreate, HookedDirectDrawCreate);
        DetourTransactionCommit();
        isHooked = false;
        LogOut("[OVERLAY] DirectDrawCreate detour detached", true);
    }
}

// Add a temporary message
void DirectDrawHook::AddMessage(const std::string& text, const std::string& category, COLORREF color, int durationMs, int x, int y) {
    std::lock_guard<std::mutex> lock(messagesMutex);

    // Remove existing temporary messages of the same category
    if (!category.empty()) {
        messages.erase(std::remove_if(messages.begin(), messages.end(),
            [&](const OverlayMessage& msg) {
                return !msg.isPermanent && msg.category == category;
            }), messages.end());
    }

    auto expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    messages.push_back({text, color, expireTime, x, y, false, -1, category, 180});
}

// Add a permanent message
int DirectDrawHook::AddPermanentMessage(const std::string& text, COLORREF color, int x, int y, unsigned char backgroundAlpha) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    // FIX: Declare newId and increment the static counter
    int newId = nextMessageId++;
    
    // FIX: Add the missing 'category' member to the initializer list.
    // Permanent messages don't need a category, so we use an empty string.
    permanentMessages.push_back({text, color, {}, x, y, true, newId, "", backgroundAlpha});
    
    return newId;
}

// Update an existing permanent message
void DirectDrawHook::UpdatePermanentMessage(int id, const std::string& newText, COLORREF newColor) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    
    for (auto& msg : permanentMessages) {
        if (msg.id == id) {
            if (msg.text == newText && msg.color == newColor) {
                break;
            }
            msg.text = newText;
            msg.color = newColor;
            break;
        }
    }
}

// Remove a permanent message
void DirectDrawHook::RemovePermanentMessage(int id) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    permanentMessages.erase(std::remove_if(permanentMessages.begin(), permanentMessages.end(),
        [id](const OverlayMessage& msg) { return msg.id == id; }), permanentMessages.end());
}

// NEW: Remove messages by category
void DirectDrawHook::RemoveMessagesByCategory(const std::string& category) {
    std::lock_guard<std::mutex> lock(messagesMutex);
    if (category.empty()) return;

    // Remove temporary messages of the specified category
    messages.erase(std::remove_if(messages.begin(), messages.end(),
        [&](const OverlayMessage& msg) {
            return !msg.isPermanent && msg.category == category;
        }), messages.end());
}

// Remove all messages
void DirectDrawHook::ClearAllMessages() {
    std::lock_guard<std::mutex> lock(messagesMutex);
    messages.clear();
    permanentMessages.clear();
}

// Thread-safe D3D9 init guard
static std::atomic<bool> s_d3d9InitInProgress{false};
static std::atomic<bool> s_lastD3D9InitDeferredForNetplay{false};

// --- NEW: D3D9 Hook Initialization and Shutdown ---
bool DirectDrawHook::InitializeD3D9() {
    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        s_lastD3D9InitDeferredForNetplay.store(true, std::memory_order_release);
        LogOut("[OVERLAY] Skipping D3D9 initialization while netplay suspend is active.", detailedLogging.load());
        return false;
    }

    s_lastD3D9InitDeferredForNetplay.store(false, std::memory_order_release);

    // Fast path: already hooked
    if (isHooked) {
        return SetD3D9Active(true);
    }

    // Prevent concurrent initialization attempts
    bool expected = false;
    if (!s_d3d9InitInProgress.compare_exchange_strong(expected, true)) {
        // Another thread is initializing - wait and check result
        LogOut("[OVERLAY] D3D9 init already in progress, waiting...", true);
        Sleep(100);
        return isHooked;
    }
    
    // Find the game window (best-effort; not strictly required for dummy device)
    HWND gameWindow = FindEFZWindow();
    if (gameWindow) {
        LogOut("[OVERLAY] Found game window: " + Logger::hwndToString(gameWindow), true);
    } else {
        LogOut("[OVERLAY] EFZ window not found yet; proceeding with dummy device.", true);
    }
    
    // Get the D3D9 module handle
    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    if (!d3d9Module) {
        // Try to load it if it's not already loaded
        d3d9Module = LoadLibraryA("d3d9.dll");
        if (!d3d9Module) {
            LogOut("[OVERLAY] Failed to get d3d9.dll module", true);
            g_ExternalMenuFallbackNeeded.store(true, std::memory_order_release);
            s_d3d9InitInProgress.store(false);
            return false;
        }
    }
    // Log the module path for d3d9.dll
    {
        char pathBuf[MAX_PATH] = {0};
        DWORD n = GetModuleFileNameA(d3d9Module, pathBuf, MAX_PATH);
        if (n > 0) {
            LogOut(std::string("[OVERLAY][D3D9] Using d3d9 module: ") + pathBuf, true);
        } else {
            LogOut("[OVERLAY][D3D9] GetModuleFileNameA(d3d9.dll) failed", detailedLogging.load());
        }
    }
    
    // Get the address of Direct3DCreate9
    auto Direct3DCreate9_fn = (LPDIRECT3D9(WINAPI*)(UINT))(GetProcAddress(d3d9Module, "Direct3DCreate9"));
    if (!Direct3DCreate9_fn) {
        LogOut("[OVERLAY] Failed to get Direct3DCreate9 address", true);
        g_ExternalMenuFallbackNeeded.store(true, std::memory_order_release);
        s_d3d9InitInProgress.store(false);
        return false;
    }
    else {
        char ptrbuf[32] = {};
        _snprintf_s(ptrbuf, sizeof(ptrbuf), _TRUNCATE, "%p", (void*)Direct3DCreate9_fn);
        LogOut(std::string("[OVERLAY][D3D9] Direct3DCreate9 at ") + ptrbuf, detailedLogging.load());
    }
    
    // Create a D3D9 object
    LPDIRECT3D9 d3d9 = Direct3DCreate9_fn(D3D_SDK_VERSION);
    if (!d3d9) {
        LogOut("[OVERLAY] Failed to create D3D9 object", true);
        g_ExternalMenuFallbackNeeded.store(true, std::memory_order_release);
        s_d3d9InitInProgress.store(false);
        return false;
    }

    // Log adapter information for diagnostics
    UINT adapterCount = d3d9->GetAdapterCount();
    LogOut("[OVERLAY][D3D9] Adapter count: " + std::to_string((unsigned)adapterCount), detailedLogging.load());
    D3DADAPTER_IDENTIFIER9 ident{};
    if (SUCCEEDED(d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident))) {
        LogOut(std::string("[OVERLAY][D3D9] Default adapter: ") + ident.Description, detailedLogging.load());
        char idbuf[128] = {};
        _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "VendorID=0x%04X DeviceID=0x%04X SubSysID=0x%08X Revision=%u",
            ident.VendorId, ident.DeviceId, ident.SubSysId, ident.Revision);
        LogOut(std::string("[OVERLAY][D3D9] ") + idbuf, detailedLogging.load());
    } else {
        LogOut("[OVERLAY][D3D9] GetAdapterIdentifier failed", detailedLogging.load());
    }
    
    // Create a hidden dummy window to safely create a temporary device
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "EFZ_TM_D3D9_DUMMY";
    if (!RegisterClassA(&wc)) {
        DWORD le = GetLastError();
        LogOut("[OVERLAY][D3D9] RegisterClassA failed: " + std::to_string((unsigned)le), detailedLogging.load());
    }
    HWND dummyWnd = CreateWindowExA(0, wc.lpszClassName, "efz_tm_dummy", WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummyWnd) {
        DWORD le = GetLastError();
        LogOut("[OVERLAY][D3D9] CreateWindowExA(dummy) failed: " + std::to_string((unsigned)le), true);
    }

    // Set up present parameters
    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp.BackBufferWidth = 2;
    d3dpp.BackBufferHeight = 2;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    d3dpp.hDeviceWindow = dummyWnd;

    // Create a temporary device to get the VTable
    LPDIRECT3DDEVICE9 tempDevice;
    HRESULT hr = d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d3dpp.hDeviceWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &tempDevice);
        
    if (FAILED(hr)) {
        // Log HRESULT in hex and decimal for easier lookup
        char hrbuf[64] = {};
        _snprintf_s(hrbuf, sizeof(hrbuf), _TRUNCATE, "0x%08lX (%ld)", (unsigned long)hr, (long)hr);
        LogOut(std::string("[OVERLAY] Failed to create temp D3D9 device: ") + hrbuf, true);
        g_ExternalMenuFallbackNeeded.store(true, std::memory_order_release);
        d3d9->Release();
        s_d3d9InitInProgress.store(false);
        return false;
    }
    
    // Get the baseline function pointer for EndScene from a stock dummy device.
    void** vTable = *reinterpret_cast<void***>(tempDevice);
    void* endSceneAddr = vTable[42]; // EndScene is at index 42
    {
        char ptrbuf1[32] = {}, ptrbuf2[32] = {};
        _snprintf_s(ptrbuf1, sizeof(ptrbuf1), _TRUNCATE, "%p", (void*)vTable);
        _snprintf_s(ptrbuf2, sizeof(ptrbuf2), _TRUNCATE, "%p", endSceneAddr);
        LogOut(std::string("[OVERLAY][D3D9] Device VTable=") + ptrbuf1 + " EndScene@42=" + ptrbuf2, true);
    }

    EndSceneHookCandidate stockCandidate{};
    stockCandidate.target = endSceneAddr;
    stockCandidate.source = "stock-dummy-device";
    stockCandidate.modulePath = DescribeModuleForAddress(endSceneAddr);
    LogEndSceneCandidate(stockCandidate, "[OVERLAY][D3D9] Baseline EndScene candidate");

    EndSceneHookCandidate liveCandidate{};
    const bool haveLiveCandidate = TryResolveLiveEndSceneCandidate(liveCandidate);
    if (haveLiveCandidate) {
        LogEndSceneCandidate(liveCandidate, "[OVERLAY][D3D9] Live EndScene candidate");
        if (liveCandidate.target != stockCandidate.target) {
            LogOut("[OVERLAY][D3D9] Live EndScene target differs from stock entry; preferring live candidate to chain behind vtable owners", true);
        } else {
            LogOut("[OVERLAY][D3D9] Live EndScene target matches stock entry", detailedLogging.load());
        }
    } else {
        LogOut("[OVERLAY][D3D9] Live EndScene candidate unavailable; using stock dummy-device target", true);
    }

    const EndSceneHookCandidate* primaryCandidate = (haveLiveCandidate && liveCandidate.target) ? &liveCandidate : &stockCandidate;
    const EndSceneHookCandidate* fallbackCandidate = (primaryCandidate == &liveCandidate && liveCandidate.target != stockCandidate.target)
        ? &stockCandidate
        : nullptr;

    if (g_onlineModeActive.load(std::memory_order_relaxed)) {
        s_lastD3D9InitDeferredForNetplay.store(true, std::memory_order_release);
        LogOut("[OVERLAY] Aborting EndScene enable because netplay suspend became active during initialization.", true);
        tempDevice->Release();
        d3d9->Release();
        if (dummyWnd) {
            DestroyWindow(dummyWnd);
            UnregisterClassA("EFZ_TM_D3D9_DUMMY", GetModuleHandleA(nullptr));
        }
        s_d3d9InitInProgress.store(false);
        return false;
    }

    bool attached = AttachEndSceneHookForCandidate(*primaryCandidate, "startup");
    if (!attached && fallbackCandidate) {
        LogOut("[OVERLAY][D3D9] Primary live EndScene attach failed; retrying with stock target", true);
        attached = AttachEndSceneHookForCandidate(*fallbackCandidate, "startup-fallback");
    }
    if (!attached) {
        g_ExternalMenuFallbackNeeded.store(true, std::memory_order_release);
        tempDevice->Release();
        d3d9->Release();
        if (dummyWnd) {
            DestroyWindow(dummyWnd);
            UnregisterClassA("EFZ_TM_D3D9_DUMMY", GetModuleHandleA(nullptr));
        }
        s_d3d9InitInProgress.store(false);
        return false;
    }
    
    // Clean up temporary objects
    tempDevice->Release();
    d3d9->Release();
    if (dummyWnd) {
        DestroyWindow(dummyWnd);
        UnregisterClassA("EFZ_TM_D3D9_DUMMY", GetModuleHandleA(nullptr));
    }
    
    s_d3d9InitInProgress.store(false);  // Release init lock
    LogOut("[OVERLAY] D3D9 EndScene hook installed successfully.", true);

    // Diagnostic: Verify EndScene is observed soon; if not, try to relatch to the live target once.
    std::thread([]{
        Sleep(6000);
        if (!g_EndSceneObserved.load()) {
            if (TryRelatchEndSceneToLiveTarget("watchdog")) {
                Sleep(4000);
            }
        }
        if (!g_EndSceneObserved.load()) {
            g_ExternalMenuFallbackNeeded.store(true, std::memory_order_release);
            LogOut("[OVERLAY][D3D9] EndScene not observed within 6s after hook enable.", true);
            LogOut(std::string("[OVERLAY][D3D9] Current hook source=") + g_EndSceneTargetSource
                   + " target=" + FormatPointerValue(reinterpret_cast<uintptr_t>(g_EndSceneTarget)),
                   true);
            LogOut("[OVERLAY][D3D9] Possible causes: EFZ is not rendering via D3D9 yet (no wrapper), game not yet in a render loop, or another overlay modified the live vtable after our attach.", true);
        }
    }).detach();
    return true;
}

bool DirectDrawHook::WasLastD3D9InitDeferredForNetplay() {
    return s_lastD3D9InitDeferredForNetplay.load(std::memory_order_acquire);
}

bool DirectDrawHook::ShouldUseExternalMenuFallback() {
    return g_ExternalMenuFallbackNeeded.load(std::memory_order_acquire);
}

bool DirectDrawHook::SetD3D9Active(bool active) {
    if (active && g_onlineModeActive.load(std::memory_order_relaxed)) {
        return false;
    }

    if (!g_EndSceneTarget) {
        if (active && !isHooked) {
            return InitializeD3D9();
        }
        return !active;
    }

    const bool currentlyEnabled = g_EndSceneHookEnabled.load(std::memory_order_acquire);
    if (currentlyEnabled == active) {
        if (active
            && !g_EndSceneObserved.load(std::memory_order_acquire)
            && g_ExternalMenuFallbackNeeded.load(std::memory_order_acquire)) {
            (void)TryRelatchEndSceneToLiveTarget("reactivate");
        }
        return true;
    }

    g_EndSceneHookEnabled.store(active, std::memory_order_release);
    if (!active) {
        g_ExternalMenuFallbackNeeded.store(false, std::memory_order_release);
    } else if (!g_EndSceneObserved.load(std::memory_order_acquire)
               && g_ExternalMenuFallbackNeeded.load(std::memory_order_acquire)) {
        (void)TryRelatchEndSceneToLiveTarget("enable");
    }
    LogOut(std::string("[OVERLAY] D3D9 EndScene hook ") + (active ? "enabled" : "disabled")
           + " source=" + g_EndSceneTargetSource,
           true);
    return true;
}

void DirectDrawHook::ShutdownD3D9() {
    LogOut("[OVERLAY] Shutting down D3D9 hooks.", true);
    GifPlayer::Shutdown();
    if (g_EndSceneTarget) {
        MH_DisableHook(g_EndSceneTarget);
        MH_RemoveHook(g_EndSceneTarget);
        g_EndSceneTarget = nullptr;
    }
    g_EndSceneTargetSource = "none";
    g_EndSceneHookEnabled.store(false, std::memory_order_release);
    isHooked = false;
    
    // REMOVED: MH_Uninitialize() is now called globally in dllmain.cpp
}

// NEW: Text fitting utility to prevent text from going off-screen
std::string DirectDrawHook::FitTextToWidth(const std::string& text, int maxWidth, HDC hdc) {
    if (text.empty() || maxWidth <= 0) return text;
    
    // Create a temporary DC if none provided
    HDC tempDC = hdc;
    bool needToReleaseDC = false;
    
    if (!tempDC) {
        tempDC = CreateCompatibleDC(NULL);
        needToReleaseDC = true;
    }
    
    // Create and select font to measure text accurately - DPI-aware
    float dpiScale = GetDpiScale();
    int fontSize = (int)(20.0f * dpiScale);
    
    HFONT font = CreateFont(
        fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial"
    );
    HFONT oldFont = (HFONT)SelectObject(tempDC, font);
    
    // Measure text
    SIZE textSize;
    GetTextExtentPoint32A(tempDC, text.c_str(), text.length(), &textSize);
    
    std::string result = text;
    
    // If text is too long, truncate and add ellipsis
    if (textSize.cx > maxWidth) {
        // Start with whole string and binary search to find fitting length
        int left = 0;
        int right = text.length();
        std::string ellipsis = "...";
        SIZE ellipsisSize;
        GetTextExtentPoint32A(tempDC, ellipsis.c_str(), ellipsis.length(), &ellipsisSize);
        
        while (left < right) {
            int mid = (left + right + 1) / 2;
            std::string testStr = text.substr(0, mid);
            SIZE testSize;
            GetTextExtentPoint32A(tempDC, testStr.c_str(), testStr.length(), &testSize);
            
            if (testSize.cx + ellipsisSize.cx <= maxWidth) {
                left = mid;
            } else {
                right = mid - 1;
            }
        }
        
        // Ensure we don't cut in the middle of a word if possible
        int cutPoint = left;
        if (cutPoint > 10) {  // Only if we have enough text to work with
            while (cutPoint > 0 && text[cutPoint] != ' ' && text[cutPoint] != ',') {
                cutPoint--;
            }
            if (text[cutPoint] == ' ' || text[cutPoint] == ',') {
                cutPoint++; // Move past the space/comma
            } else {
                cutPoint = left; // No good word break found, revert to original
            }
        }
        
        result = text.substr(0, cutPoint) + ellipsis;
    }
    
    // Clean up
    SelectObject(tempDC, oldFont);
    DeleteObject(font);
    
    if (needToReleaseDC) {
        DeleteDC(tempDC);
    }
    
    return result;
}

// NEW: Text fitting utility to fit text within a width by truncating from the left side
std::string DirectDrawHook::FitTextToWidthFromLeft(const std::string& text, int maxWidth, HDC hdc) {
    if (text.empty() || maxWidth <= 0) return text;
    
    // Create a temporary DC if none provided
    HDC tempDC = hdc;
    bool needToReleaseDC = false;
    
    if (!tempDC) {
        tempDC = CreateCompatibleDC(NULL);
        needToReleaseDC = true;
    }
    
    // Create and select font to measure text accurately - DPI-aware
    float dpiScale = GetDpiScale();
    int fontSize = (int)(20.0f * dpiScale);
    
    HFONT font = CreateFont(
        fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial"
    );
    HFONT oldFont = (HFONT)SelectObject(tempDC, font);
    
    // Measure text
    SIZE textSize;
    GetTextExtentPoint32A(tempDC, text.c_str(), text.length(), &textSize);
    
    std::string result = text;
    
    // For right-aligned text, we don't truncate but calculate the adjusted X position
    // so that the last character is always at the right edge (maxWidth)
    if (textSize.cx > maxWidth) {
        // This function doesn't actually modify the text, it just returns the original
        // The caller will need to adjust the X position when drawing
        
        // We return the original text, as we'll adjust position instead
        result = text;
    }
    
    // Clean up
    SelectObject(tempDC, oldFont);
    DeleteObject(font);
    
    if (needToReleaseDC) {
        DeleteDC(tempDC);
    }
    
    return result;
}
