#include "../include/utils/update_check.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <string>
#include <type_traits>

#include "../include/core/globals.h"   // g_isShuttingDown
#include "../include/core/logger.h"
#include "../include/core/version.h"
#include "../include/utils/config.h"
#include "../include/utils/network.h"
#include "../include/utils/utilities.h"
#include "../include/utils/xp_compat.h"
#include "../include/utils/update_check_policy.h"


namespace UpdateCheck {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/Aquat1c/efz-training-mode/releases/latest";
constexpr char    kReleasesPage[] = "https://github.com/Aquat1c/efz-training-mode/releases";

// Acknowledgement lives in its own sidecar, NOT efz_training_config.ini:
// SaveSettings() rewrites that file with std::ios::trunc and re-emits only the
// keys it knows, so a key written underneath it would be silently dropped on
// the user's next save. WritePrivateProfileStringA also flushes immediately.
constexpr char kStateFileName[] = "efz_training_update_check.ini";
constexpr char kStateSection[]  = "UpdateCheck";
constexpr char kStateKey[]      = "AcknowledgedVersion";
// Test hook. When this key is present in the sidecar ini the worker skips the
// network entirely and behaves as though GitHub returned that tag, so the whole
// badge / acknowledge / About-text flow can be exercised offline and
// deterministically. Absent (the shipped state) it costs one ini read on the
// single Start() call and nothing else. It is deliberately not exposed in the
// menu: creating the key is an explicit act.
//   [UpdateCheck]
//   SimulateLatestVersion=9.9.9
constexpr char kStateKeySimulate[] = "SimulateLatestVersion";

constexpr DWORD kResolveTimeoutMs = 5000;
constexpr DWORD kConnectTimeoutMs = 5000;
constexpr DWORD kSendTimeoutMs    = 5000;
constexpr DWORD kReceiveTimeoutMs = 8000;

// ONE successful check per training session - that is the whole cadence. A new
// release raises the badge on the next launch, because acknowledgement is stored
// per-tag and only a tag newer than the acknowledged one counts.
//
// The single exception is a FAILED attempt. Latching on failure would mean a
// user who happened to open the menu before their network came up gets nothing
// for the rest of the session; retrying on every menu open would hammer GitHub
// while offline. So a failure is retryable, but not more than once per window.
constexpr unsigned long long kRetryAfterFailureMs = 5ull * 60ull * 1000ull;   // 5m

constexpr size_t kMaxResponseBytes = 256u * 1024u;   // GitHub embeds the changelog

// NOTE: every one of these is at NAMESPACE scope on purpose. The XP build adds
// /Zc:threadSafeInit- (CMakeLists.txt:60), which removes the compiler's guard
// on function-local statics - so the usual "static X& Get() { static X x; }"
// singleton is a real data race in the shipping configuration.
CRITICAL_SECTION g_lock;
volatile LONG    g_lockReady = 0;
volatile LONG    g_fetchInFlight = 0;    // exactly one worker at a time
volatile LONG    g_completed = 0;        // a check SUCCEEDED this session
unsigned long long g_retryNotBeforeTick = 0;  // guarded by g_lock; failure cooldown
volatile LONG    g_status = static_cast<LONG>(Status::Idle);
volatile LONG    g_newerRelease = 0;
volatile LONG    g_updateAvailable = 0;
std::string      g_latest;         // guarded by g_lock
std::string      g_acknowledged;   // guarded by g_lock

void EnsureLock() {
    if (InterlockedCompareExchange(&g_lockReady, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lockReady, 2);
        return;
    }
    while (InterlockedCompareExchange(&g_lockReady, 2, 2) != 2) {
        Sleep(0);
    }
}

struct ScopedLock {
    ScopedLock()  { EnsureLock(); EnterCriticalSection(&g_lock); }
    ~ScopedLock() { LeaveCriticalSection(&g_lock); }
};

void SetStatus(Status s) {
    InterlockedExchange(&g_status, static_cast<LONG>(s));
}

// Called once per worker exit, whatever the outcome. Success closes the session
// out entirely; failure only opens a cooldown.
void FinishAttempt(bool succeeded) {
    if (succeeded) {
        InterlockedExchange(&g_completed, 1);
    } else {
        ScopedLock guard;
        g_retryNotBeforeTick = XPCompat::GetTickCount64Compat() + kRetryAfterFailureMs;
    }
    InterlockedExchange(&g_fetchInFlight, 0);
}

// Resolve our own directory from a function address rather than by module name:
// the DLL can be renamed, and a name lookup would then silently write the
// sidecar into the game root.
std::string ModuleDirectory() {
    char modulePath[MAX_PATH] = {};
    HMODULE selfModule = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ModuleDirectory), &selfModule)
        || selfModule == nullptr
        || GetModuleFileNameA(selfModule, modulePath, MAX_PATH) == 0) {
        return {};
    }
    std::string dir(modulePath);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return {};
    dir.resize(slash);
    return dir;
}

std::string StateFilePath() {
    const std::string dir = ModuleDirectory();
    if (dir.empty()) return {};
    return dir + "\\" + kStateFileName;
}

std::string ReadStateValue(const char* key) {
    const std::string path = StateFilePath();
    if (path.empty()) return {};
    char buffer[64] = {};
    GetPrivateProfileStringA(kStateSection, key, "", buffer,
                             static_cast<DWORD>(sizeof(buffer)), path.c_str());
    return buffer;
}

std::string ReadAcknowledgedFromDisk() { return ReadStateValue(kStateKey); }

bool WriteAcknowledgedToDisk(const std::string& version) {
    const std::string path = StateFilePath();
    if (path.empty()) return false;
    return WritePrivateProfileStringA(kStateSection, kStateKey,
                                      version.c_str(), path.c_str()) != FALSE;
}

void RecomputeAvailabilityLocked() {
    const Policy::Availability a =
        Policy::Decide(g_latest, EFZ_TRAINING_MODE_VERSION, g_acknowledged);
    InterlockedExchange(&g_newerRelease, a.hasNewerRelease ? 1 : 0);
    InterlockedExchange(&g_updateAvailable, a.updateAvailable ? 1 : 0);
}

// ---------------------------------------------------------------------------
// WinHTTP, resolved at runtime.
//
// Deliberately NOT a static import: a user with the check disabled should never
// pay a winhttp.dll load, and keeping it out of the import table means a broken
// or missing winhttp can never stop our DLL from loading at all.
// ---------------------------------------------------------------------------
struct WinHttpApi {
    HMODULE module = nullptr;
    decltype(&WinHttpOpen)             Open = nullptr;
    decltype(&WinHttpSetTimeouts)      SetTimeouts = nullptr;
    decltype(&WinHttpSetOption)        SetOption = nullptr;
    decltype(&WinHttpConnect)          Connect = nullptr;
    decltype(&WinHttpOpenRequest)      OpenRequest = nullptr;
    decltype(&WinHttpSendRequest)      SendRequest = nullptr;
    decltype(&WinHttpReceiveResponse)  ReceiveResponse = nullptr;
    decltype(&WinHttpQueryHeaders)     QueryHeaders = nullptr;
    decltype(&WinHttpQueryDataAvailable) QueryDataAvailable = nullptr;
    decltype(&WinHttpReadData)         ReadData = nullptr;
    decltype(&WinHttpCloseHandle)      CloseHandle = nullptr;
    bool ok = false;
};

bool LoadWinHttp(WinHttpApi& api) {
    api.module = LoadLibraryA("winhttp.dll");
    if (!api.module) return false;
    auto resolve = [&api](auto& fn, const char* name) -> bool {
        fn = reinterpret_cast<typename std::remove_reference<decltype(fn)>::type>(
            GetProcAddress(api.module, name));
        return fn != nullptr;
    };
    api.ok = resolve(api.Open, "WinHttpOpen")
        && resolve(api.SetTimeouts, "WinHttpSetTimeouts")
        && resolve(api.SetOption, "WinHttpSetOption")
        && resolve(api.Connect, "WinHttpConnect")
        && resolve(api.OpenRequest, "WinHttpOpenRequest")
        && resolve(api.SendRequest, "WinHttpSendRequest")
        && resolve(api.ReceiveResponse, "WinHttpReceiveResponse")
        && resolve(api.QueryHeaders, "WinHttpQueryHeaders")
        && resolve(api.QueryDataAvailable, "WinHttpQueryDataAvailable")
        && resolve(api.ReadData, "WinHttpReadData")
        && resolve(api.CloseHandle, "WinHttpCloseHandle");
    if (!api.ok) {
        FreeLibrary(api.module);
        api.module = nullptr;
    }
    return api.ok;
}

bool HttpsGet(std::string* outBody, std::string* outError) {
    WinHttpApi api;
    if (!LoadWinHttp(api)) {
        *outError = "winhttp.dll unavailable";
        return false;
    }
    bool success = false;
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    // The User-Agent is the ONLY thing this request discloses beyond the fact of
    // the connection. Keep it to the mod name and version - no machine name, no
    // install id, no query parameters. GitHub rejects UA-less requests with 403.
    std::wstring userAgent = L"efz-training-mode/";
    for (const char* p = EFZ_TRAINING_MODE_VERSION; *p != '\0'; ++p) {
        userAgent.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }

    session = api.Open(userAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *outError = "WinHttpOpen failed"; goto cleanup; }

    api.SetTimeouts(session, static_cast<int>(kResolveTimeoutMs),
                    static_cast<int>(kConnectTimeoutMs),
                    static_cast<int>(kSendTimeoutMs),
                    static_cast<int>(kReceiveTimeoutMs));

    // Windows 7 / 8.0 default WinHTTP to SSL3 + TLS1.0 only. Without this the
    // request fails there too, not just on XP. Best-effort: older WinHTTP
    // rejects the option and we simply proceed with its defaults.
    {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1
                        | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1
                        | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        api.SetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                      &protocols, sizeof(protocols));
    }

    connect = api.Connect(session, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { *outError = "WinHttpConnect failed"; goto cleanup; }

    request = api.OpenRequest(connect, L"GET", kApiPath, nullptr,
                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              WINHTTP_FLAG_SECURE);
    if (!request) { *outError = "WinHttpOpenRequest failed"; goto cleanup; }

    if (!api.SendRequest(request, L"Accept: application/vnd.github+json\r\n",
                         static_cast<DWORD>(-1L),
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !api.ReceiveResponse(request, nullptr)) {
        const DWORD err = GetLastError();
        *outError = (err == ERROR_WINHTTP_SECURE_FAILURE)
            ? "TLS handshake failed (expected on Windows XP)"
            : ("request failed, error " + std::to_string(err));
        goto cleanup;
    }

    {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (api.QueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                             &statusSize, WINHTTP_NO_HEADER_INDEX)
            && statusCode != 200) {
            // 403 = rate limited or UA rejected, 404 = no published releases.
            // Either way this is a permanent give-up, never a retry.
            *outError = "HTTP status " + std::to_string(statusCode);
            goto cleanup;
        }

        std::string body;
        for (;;) {
            DWORD available = 0;
            if (!api.QueryDataAvailable(request, &available) || available == 0) break;
            if (body.size() + available > kMaxResponseBytes) {
                *outError = "response exceeded size cap";
                goto cleanup;
            }
            const size_t offset = body.size();
            body.resize(offset + available);
            DWORD read = 0;
            if (!api.ReadData(request, &body[offset], available, &read)) {
                *outError = "WinHttpReadData failed";
                goto cleanup;
            }
            body.resize(offset + read);
            if (read == 0) break;
        }
        if (body.empty()) { *outError = "empty response"; goto cleanup; }
        *outBody = body;
        success = true;
    }

cleanup:
    if (request) api.CloseHandle(request);
    if (connect) api.CloseHandle(connect);
    if (session) api.CloseHandle(session);
    if (api.module) FreeLibrary(api.module);
    return success;
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    // MANDATORY. CrashHandler installs a std::set_terminate whose handler ends in
    // TerminateProcess (src/utils/crash_handler.cpp:1305), so any exception that
    // escapes this thread kills EFZ mid-match and writes a dump blaming this mod.
    try {
        // Offline test hook - see kStateKeySimulate. Exercises everything below
        // this line (decision, badge, acknowledgement, About text) with no
        // network and no dependency on what is actually published.
        const std::string simulated = Policy::NormalizeTag(ReadStateValue(kStateKeySimulate));
        if (!simulated.empty() && Policy::IsSafeTag(simulated)) {
            bool simNewer = false;
            {
                ScopedLock guard;
                g_latest = simulated;
                RecomputeAvailabilityLocked();
                simNewer = g_newerRelease != 0;
            }
            SetStatus(simNewer ? Status::UpdateAvailable : Status::UpToDate);
            LogOut("[UPDATE] SIMULATED latest release " + simulated +
                       " (SimulateLatestVersion set; no network request made)", true);
            FinishAttempt(true);
            return 0;
        }

        std::string body;
        std::string error;
        if (!HttpsGet(&body, &error)) {
            LogOut("[UPDATE] check failed: " + error, true);
            SetStatus(Status::Failed);
            FinishAttempt(false);
            return 0;
        }
        if (g_isShuttingDown.load()) { FinishAttempt(false); return 0; }

        const std::string tag = Policy::ExtractTagFromReleaseJson(body);
        if (tag.empty()) {
            LogOut("[UPDATE] no usable tag_name in response (" +
                       std::to_string(body.size()) + " bytes)", true);
            SetStatus(Status::Failed);
            FinishAttempt(false);
            return 0;
        }

        bool newer = false;
        bool available = false;
        {
            ScopedLock guard;
            g_latest = tag;
            RecomputeAvailabilityLocked();
            newer = g_newerRelease != 0;
            available = g_updateAvailable != 0;
        }
        SetStatus(newer ? Status::UpdateAvailable : Status::UpToDate);
        LogOut(std::string("[UPDATE] latest release ") + tag +
                   ", running " + EFZ_TRAINING_MODE_VERSION + " -> " +
                   (newer ? "update available" : "up to date") +
                   ((newer && !available) ? " (already acknowledged)" : ""),
               true);
        FinishAttempt(true);
    } catch (...) {
        SetStatus(Status::Failed);
        FinishAttempt(false);
    }
    return 0;
}

} // namespace

void Start() {
    // Called on every menu open. Gates first, so a call made while disabled or
    // netplay-gated cannot poison later attempts in this process.
    if (!Config::GetSettings().checkForUpdates) {
        SetStatus(Status::Disabled);
        return;
    }
    if (IsNetplaySuspendActive() || IsNetplaySessionActive()
        || IsNetplayFlowActive() || IsNetplayMenuActive()) {
        return;
    }
    if (g_isShuttingDown.load()) return;

    // One successful check per session.
    if (InterlockedCompareExchange(&g_completed, 0, 0) != 0) {
        return;
    }
    // A previous attempt failed; wait out its cooldown before trying again.
    {
        ScopedLock guard;
        if (g_retryNotBeforeTick != 0
            && XPCompat::GetTickCount64Compat() < g_retryNotBeforeTick) {
            return;
        }
    }
    // One worker at a time. A menu opened repeatedly while a fetch is in flight
    // must not stack threads on the same request.
    if (InterlockedCompareExchange(&g_fetchInFlight, 1, 0) != 0) {
        return;
    }
    {
        ScopedLock guard;
        // Re-read on every attempt: the sidecar is the source of truth and may
        // have been edited between checks.
        g_acknowledged = ReadAcknowledgedFromDisk();
    }
    SetStatus(Status::Checking);
    HANDLE thread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    if (!thread) {
        LogOut("[UPDATE] worker thread creation failed (" +
                   std::to_string(GetLastError()) + ")", true);
        SetStatus(Status::Failed);
        FinishAttempt(false);
        return;
    }
    // Detached, matching every other worker in this DLL. DLL_PROCESS_DETACH is
    // signal-only (src/dllmain.cpp:458-467) and nothing FreeLibrary's us, so
    // there is no unload-while-blocked hazard to join against.
    CloseHandle(thread);
}

std::string LatestVersion() {
    ScopedLock guard;
    return g_latest;
}

bool HasNewerRelease() {
    return InterlockedCompareExchange(&g_newerRelease, 0, 0) != 0;
}

bool IsUpdateAvailable() {
    return InterlockedCompareExchange(&g_updateAvailable, 0, 0) != 0;
}

const char* BadgeText() {
    // A literal, never a pointer into the lock-guarded string: the render thread
    // holds this across the frame and cannot reason about that lifetime.
    return IsUpdateAvailable() ? "[!]" : "";
}

void AcknowledgeLatest() {
    if (!IsUpdateAvailable()) {
        return;   // nothing newer, or this tag was already acknowledged
    }
    std::string latest;
    {
        ScopedLock guard;
        latest = g_latest;
        g_acknowledged = latest;
        RecomputeAvailabilityLocked();
    }
    if (!WriteAcknowledgedToDisk(latest)) {
        LogOut("[UPDATE] could not persist acknowledgement of " + latest, true);
    } else {
        LogOut("[UPDATE] acknowledged " + latest + " (badge hidden until a newer release)", true);
    }
}

Status GetStatus() {
    return static_cast<Status>(InterlockedCompareExchange(&g_status, 0, 0));
}

const char* ReleasesPageUrl() {
    return kReleasesPage;
}

} // namespace UpdateCheck
