#define WIN32_LEAN_AND_MEAN
#include <fstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <vector>
#include <mutex>
#include "../include/utils/network.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"
#include "../include/core/memory.h"
#include "../include/game/game_state.h"
#include "../include/game/practice_patch.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

std::atomic<bool> isOnlineMatch(false);

namespace {

using NetplayGetStateFn = const EFZNetplayState* (__cdecl*)(void);

constexpr size_t kRequiredExportSize =
    offsetof(EFZNetplayState, stateSeq) + sizeof(uint32_t);

std::atomic<int> s_cachedRevivalVer{0};
std::atomic<int> s_cachedRevivalFlavor{0};

std::mutex s_reasonMutex;
std::string s_lastOnlineReason;

std::mutex s_runtimeMutex;
NetplayRuntimeState s_runtimeState{};
std::atomic<bool> s_suspendTraining{false};
std::atomic<bool> s_sessionActive{false};
std::atomic<bool> s_exportAvailable{false};
std::atomic<bool> s_inNetplayMenu{false};
std::atomic<bool> s_inNetplayFlow{false};
std::atomic<int> s_source{static_cast<int>(NetplayStateSource::None)};

HANDLE s_sharedStateHandle = nullptr;
const EFZNetplayState* s_sharedStateView = nullptr;
HMODULE s_exportModule = nullptr;
NetplayGetStateFn s_exportFn = nullptr;

void SetOnlineReason(const std::string& reason) {
    std::lock_guard<std::mutex> lock(s_reasonMutex);
    s_lastOnlineReason = reason;
}

std::string CopyOnlineReason() {
    std::lock_guard<std::mutex> lock(s_reasonMutex);
    return s_lastOnlineReason;
}

std::string DescribeRuntimeState(const NetplayRuntimeState& state) {
    std::ostringstream oss;
    oss << "source=" << NetplayStateSourceName(state.source)
        << " suspend=" << (state.suspendTraining ? "1" : "0")
        << " session=" << (state.sessionActive ? "1" : "0")
        << " menu=" << (state.inNetplayMenu ? "1" : "0")
        << " flow=" << (state.inNetplayFlow ? "1" : "0");

    if (state.exportAvailable) {
        oss << " export[mode=" << state.exportState.sessionMode
            << " phase=" << state.exportState.sessionPhase
            << " activity=" << static_cast<int>(state.exportState.activityPhase)
            << " charsel=" << static_cast<int>(state.exportState.inNetplayCharacterSelect)
            << " match=" << static_cast<int>(state.exportState.inNetplayMatch)
            << " end=" << static_cast<int>(state.exportState.endReason)
            << "]";
    } else {
        oss << " legacy=" << OnlineStateName(state.legacyOnlineState);
    }

    return oss.str();
}

bool HasMeaningfulRuntimeChange(const NetplayRuntimeState& previousState, const NetplayRuntimeState& nextState) {
    if (previousState.source != nextState.source
        || previousState.exportAvailable != nextState.exportAvailable
        || previousState.sessionActive != nextState.sessionActive
        || previousState.suspendTraining != nextState.suspendTraining
        || previousState.inNetplayMenu != nextState.inNetplayMenu
        || previousState.inNetplayFlow != nextState.inNetplayFlow
        || previousState.inNetplayCharacterSelect != nextState.inNetplayCharacterSelect
        || previousState.inNetplayMatch != nextState.inNetplayMatch
        || previousState.legacyOnlineState != nextState.legacyOnlineState) {
        return true;
    }

    if (!previousState.exportAvailable || !nextState.exportAvailable) {
        return false;
    }

    return previousState.exportState.sessionMode != nextState.exportState.sessionMode
        || previousState.exportState.sessionPhase != nextState.exportState.sessionPhase
        || previousState.exportState.activityPhase != nextState.exportState.activityPhase
        || previousState.exportState.inNetplayMenu != nextState.exportState.inNetplayMenu
        || previousState.exportState.inNetplayCharacterSelect != nextState.exportState.inNetplayCharacterSelect
        || previousState.exportState.inNetplayMatch != nextState.exportState.inNetplayMatch
        || previousState.exportState.endReason != nextState.exportState.endReason
        || previousState.exportState.sessionId != nextState.exportState.sessionId
        || previousState.exportState.setId != nextState.exportState.setId;
}

std::string GetEFZWindowTitleA() {
    HWND hwnd = FindEFZWindow();
    if (!hwnd) return std::string();
    char titleA[256] = {0};
    if (GetWindowTextA(hwnd, titleA, static_cast<int>(sizeof(titleA) - 1)) > 0) {
        return std::string(titleA);
    }
    return std::string();
}

bool RegionContainsU32(uintptr_t start, size_t size, uint32_t value) {
    if (!start || size < sizeof(value)) return false;

    BYTE bytes[0x220] = {};
    if (size > sizeof(bytes)) {
        size = sizeof(bytes);
    }
    if (!SafeReadMemory(start, bytes, size)) {
        return false;
    }

    for (size_t i = 0; i + sizeof(value) <= size; ++i) {
        uint32_t found = 0;
        std::memcpy(&found, bytes + i, sizeof(found));
        if (found == value) {
            return true;
        }
    }
    return false;
}

bool LooksLikeRevivalPatchToggler(HMODULE module, uintptr_t rva) {
    if (!module || !rva) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(module) + rva;

    uint8_t first = 0;
    if (!SafeReadMemory(start, &first, sizeof(first)) || first != 0x55) {
        return false;
    }

    static const uint32_t kPatchedCalls[] = {
        0x0076425F, 0x0075E183, 0x0075DFB5, 0x0075DFD0, 0x0075E055,
        0x0075E0DA, 0x0076420A, 0x00764AEB, 0x00765E59, 0x00765E7A
    };

    int hits = 0;
    for (uint32_t addr : kPatchedCalls) {
        if (RegionContainsU32(start, 0x220, addr)) {
            ++hits;
        }
    }
    return hits >= 8;
}

bool ProcessHasActiveUdpConnection() {
    DWORD currentPid = GetCurrentProcessId();

    ULONG tableSize = 0;
    DWORD result = GetExtendedUdpTable(nullptr, &tableSize, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }

    std::vector<BYTE> buffer(tableSize);
    auto* udpTable = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(buffer.data());
    result = GetExtendedUdpTable(udpTable, &tableSize, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (result != NO_ERROR) {
        return false;
    }

    for (DWORD i = 0; i < udpTable->dwNumEntries; ++i) {
        if (udpTable->table[i].dwOwningPid == currentPid) {
            return true;
        }
    }
    return false;
}

bool ValidateExportHeader(const EFZNetplayState& state) {
    return state.magic == EFZ_NETPLAY_STATE_MAGIC
        && state.version <= EFZ_NETPLAY_STATE_VERSION
        && state.structSize >= kRequiredExportSize
        && state.structSize <= sizeof(EFZNetplayState);
}

bool CopyStableExportSnapshot(const EFZNetplayState* raw, EFZNetplayState& outState) {
    if (!raw) return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        EFZNetplayState header = {};
        std::memcpy(&header, raw, sizeof(header.magic) + sizeof(header.version) + sizeof(header.structSize));
        if (!ValidateExportHeader(header)) {
            return false;
        }

        uint32_t seqBefore = raw->stateSeq;
        EFZNetplayState local = {};
        std::memcpy(&local, raw, header.structSize);
        uint32_t seqAfter = raw->stateSeq;

        if (seqBefore != seqAfter) {
            continue;
        }
        if (!ValidateExportHeader(local)) {
            return false;
        }
        outState = local;
        return true;
    }

    return false;
}

bool IsNetplayModInProcess() {
    return GetModuleHandleA("efz_netplay_mod.dll") != nullptr
        || GetModuleHandleA("efz_netplay_mod") != nullptr;
}

void ReleaseSharedMemoryView() {
    if (s_sharedStateView != nullptr) {
        UnmapViewOfFile(s_sharedStateView);
        s_sharedStateView = nullptr;
    }
    if (s_sharedStateHandle != nullptr) {
        CloseHandle(s_sharedStateHandle);
        s_sharedStateHandle = nullptr;
    }
}

bool EnsureSharedMemoryView() {
    // Only use shared memory if efz_netplay_mod is loaded in THIS process.
    // The shared memory name is global; without this guard, a second EFZ
    // instance running netplay would cause our practice copy to read the
    // other process's state and incorrectly suspend.
    if (!IsNetplayModInProcess()) {
        if (s_sharedStateView != nullptr) {
            ReleaseSharedMemoryView();
        }
        return false;
    }

    if (s_sharedStateView != nullptr) {
        return true;
    }

    if (s_sharedStateHandle == nullptr) {
        s_sharedStateHandle = OpenFileMappingA(FILE_MAP_READ, FALSE, EFZ_NETPLAY_STATE_SHM_NAME);
        if (s_sharedStateHandle == nullptr) {
            return false;
        }
    }

    s_sharedStateView = static_cast<const EFZNetplayState*>(
        MapViewOfFile(s_sharedStateHandle, FILE_MAP_READ, 0, 0, sizeof(EFZNetplayState)));
    if (s_sharedStateView == nullptr) {
        CloseHandle(s_sharedStateHandle);
        s_sharedStateHandle = nullptr;
        return false;
    }

    return true;
}

NetplayGetStateFn ResolveExportFunction() {
    HMODULE module = GetModuleHandleA("efz_netplay_mod.dll");
    if (module == nullptr) {
        module = GetModuleHandleA("efz_netplay_mod");
    }

    if (module == nullptr) {
        s_exportModule = nullptr;
        s_exportFn = nullptr;
        return nullptr;
    }

    if (module != s_exportModule || s_exportFn == nullptr) {
        s_exportModule = module;
        s_exportFn = reinterpret_cast<NetplayGetStateFn>(
            GetProcAddress(module, "EFZNetplay_GetState"));
    }

    return s_exportFn;
}

bool TryReadExportSnapshot(EFZNetplayState& outState, NetplayStateSource& outSource) {
    // Always prefer the DLL export: it calls into OUR process's copy of
    // efz_netplay_mod and returns a pointer to its in-process state.
    // Shared memory is a global named object and is unsafe when multiple
    // EFZ instances run on the same machine (both write to it, causing
    // the practice copy to read the hosting copy's "netplay active" state).
    if (auto fn = ResolveExportFunction()) {
        const EFZNetplayState* raw = fn();
        if (CopyStableExportSnapshot(raw, outState)) {
            outSource = NetplayStateSource::ExportDll;
            // Release any stale shared memory view since we don't need it
            if (s_sharedStateView != nullptr) {
                ReleaseSharedMemoryView();
            }
            return true;
        }
    }

    // Shared memory fallback: only safe when the netplay mod is NOT loaded
    // in our process (external monitoring scenario). If it IS loaded but
    // the export failed above, the shared memory could contain state from
    // another EFZ instance, so skip it.
    if (!IsNetplayModInProcess()) {
        if (EnsureSharedMemoryView() && CopyStableExportSnapshot(s_sharedStateView, outState)) {
            outSource = NetplayStateSource::ExportSharedMemory;
            return true;
        }
    }

    outSource = NetplayStateSource::None;
    return false;
}

bool DetectLegacyUnsupportedRevivalOnline(uintptr_t base, OnlineState& outState, std::string& outReason) {
    const uintptr_t candidateRVAs[] = {
        0xA15FC,
        0xA05F0,
        0xA05D0
    };

    for (uintptr_t rva : candidateRVAs) {
        int state = -1;
        if (!SafeReadMemory(base + rva, &state, sizeof(state))) {
            continue;
        }
        if (state < 0 || state > 3) {
            continue;
        }

        if (state == 0 || state == 1 || state == 3) {
            int verify1 = 0, verify2 = 0;
            bool mem1 = SafeReadMemory(base + rva - 4, &verify1, sizeof(verify1));
            bool mem2 = SafeReadMemory(base + rva + 4, &verify2, sizeof(verify2));
            if (!mem1 || !mem2 || (verify1 == 0 && verify2 == 0 && state == 0)) {
                continue;
            }
        }

        switch (state) {
        case 0:
            outState = OnlineState::Netplay;
            outReason = std::string("Legacy RVA 0x") + FormatHexAddress(rva) + " => Netplay";
            return true;
        case 1:
            outState = OnlineState::Spectating;
            outReason = std::string("Legacy RVA 0x") + FormatHexAddress(rva) + " => Spectating";
            return true;
        case 2:
            outState = OnlineState::Offline;
            outReason = std::string("Legacy RVA 0x") + FormatHexAddress(rva) + " => Offline";
            return true;
        case 3:
            outState = OnlineState::Tournament;
            outReason = std::string("Legacy RVA 0x") + FormatHexAddress(rva) + " => Tournament";
            return true;
        default:
            break;
        }
    }

    outState = OnlineState::Offline;
    outReason = "Legacy unsupported Revival probe => Offline";
    return false;
}

bool DetectLegacyOnlineState(OnlineState& outState, std::string& outReason) {
    EfzRevivalVersion version = GetEfzRevivalVersion();
    outState = OnlineState::Offline;
    outReason = "Legacy fallback => Offline";

    if (version == EfzRevivalVersion::Vanilla) {
        return false;
    }

    HMODULE revivalModule = GetModuleHandleA("EfzRevival.dll");
    if (revivalModule == nullptr) {
        return false;
    }

    if (version == EfzRevivalVersion::Other || version == EfzRevivalVersion::Unknown) {
        return DetectLegacyUnsupportedRevivalOnline(
            reinterpret_cast<uintptr_t>(revivalModule), outState, outReason);
    }

    OnlineState state = ReadEfzRevivalOnlineState();
    if (state == OnlineState::Unknown) {
        outState = OnlineState::Offline;
        outReason = "Legacy supported Revival read => Unknown";
        return false;
    }

    outState = state;
    outReason = std::string("Legacy supported Revival => ") + OnlineStateName(state);
    return state == OnlineState::Netplay
        || state == OnlineState::Spectating
        || state == OnlineState::Tournament;
}

void CommitRuntimeState(const NetplayRuntimeState& nextState) {
    NetplayRuntimeState previousState = {};
    {
        std::lock_guard<std::mutex> lock(s_runtimeMutex);
        previousState = s_runtimeState;
        s_runtimeState = nextState;
    }

    s_suspendTraining.store(nextState.suspendTraining, std::memory_order_release);
    s_sessionActive.store(nextState.sessionActive, std::memory_order_release);
    s_exportAvailable.store(nextState.exportAvailable, std::memory_order_release);
    s_inNetplayMenu.store(nextState.inNetplayMenu, std::memory_order_release);
    s_inNetplayFlow.store(nextState.inNetplayFlow, std::memory_order_release);
    s_source.store(static_cast<int>(nextState.source), std::memory_order_release);
    isOnlineMatch.store(nextState.suspendTraining, std::memory_order_release);

    if (previousState.source != nextState.source) {
        LogOut(
            std::string("[NETPLAY] State source: ")
            + NetplayStateSourceName(previousState.source)
            + " -> "
            + NetplayStateSourceName(nextState.source),
            true);
    }
    if (previousState.exportAvailable != nextState.exportAvailable) {
        LogOut(
            std::string("[NETPLAY] Export ")
            + (nextState.exportAvailable ? "available" : "unavailable"),
            true);
    }
    if (HasMeaningfulRuntimeChange(previousState, nextState)) {
        LogOut(
            std::string("[NETPLAY] Snapshot: ")
            + DescribeRuntimeState(nextState)
            + " reason=" + CopyOnlineReason(),
            true);
    }
}

} // namespace

EfzRevivalVersion GetEfzRevivalVersion() {
    int cached = s_cachedRevivalVer.load(std::memory_order_acquire);
    if (cached != 0) {
        return static_cast<EfzRevivalVersion>(cached);
    }

    std::string title = GetEFZWindowTitleA();
    if (title.empty()) {
        return EfzRevivalVersion::Unknown;
    }

    std::transform(title.begin(), title.end(), title.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });

    EfzRevivalVersion version = EfzRevivalVersion::Vanilla;
    if (title.find("-revival-") != std::string::npos) {
        if (title.find("1.02f") != std::string::npos) version = EfzRevivalVersion::Revival102f;
        else if (title.find("1.02e") != std::string::npos) version = EfzRevivalVersion::Revival102e;
        else if (title.find("1.02g") != std::string::npos) version = EfzRevivalVersion::Revival102g;
        else if (title.find("1.02h") != std::string::npos) version = EfzRevivalVersion::Revival102h;
        else if (title.find("1.02i") != std::string::npos) version = EfzRevivalVersion::Revival102i;
        else version = EfzRevivalVersion::Other;
    }

    s_cachedRevivalVer.store(static_cast<int>(version), std::memory_order_release);
    return version;
}

EfzRevivalDllFlavor GetEfzRevivalDllFlavor() {
    int cached = s_cachedRevivalFlavor.load(std::memory_order_acquire);
    if (cached != 0) {
        return static_cast<EfzRevivalDllFlavor>(cached);
    }

    EfzRevivalVersion version = GetEfzRevivalVersion();
    if (version == EfzRevivalVersion::Vanilla) {
        s_cachedRevivalFlavor.store(static_cast<int>(EfzRevivalDllFlavor::Standard), std::memory_order_release);
        return EfzRevivalDllFlavor::Standard;
    }
    if (version != EfzRevivalVersion::Revival102f) {
        EfzRevivalDllFlavor flavor = (version == EfzRevivalVersion::Unknown || version == EfzRevivalVersion::Other)
            ? EfzRevivalDllFlavor::Unknown
            : EfzRevivalDllFlavor::Standard;
        if (flavor != EfzRevivalDllFlavor::Unknown) {
            s_cachedRevivalFlavor.store(static_cast<int>(flavor), std::memory_order_release);
        }
        return flavor;
    }

    HMODULE module = GetModuleHandleA("EfzRevival.dll");
    if (!module) {
        return EfzRevivalDllFlavor::Unknown;
    }

    const bool classic = LooksLikeRevivalPatchToggler(module, 0x006B2A0);
    const bool subframe = LooksLikeRevivalPatchToggler(module, 0x006B4C0);

    EfzRevivalDllFlavor flavor = EfzRevivalDllFlavor::Unknown;
    if (subframe && !classic) {
        flavor = EfzRevivalDllFlavor::Revival102fSubframe;
    } else if (classic) {
        flavor = EfzRevivalDllFlavor::Revival102fClassic;
    }

    if (flavor != EfzRevivalDllFlavor::Unknown) {
        s_cachedRevivalFlavor.store(static_cast<int>(flavor), std::memory_order_release);
        std::ostringstream oss;
        oss << "[REVIVAL] Detected DLL flavor: " << EfzRevivalDllFlavorName(flavor);
        LogOut(oss.str(), true);
    }
    return flavor;
}

bool IsEfzRevival102fSubframeBuild() {
    return GetEfzRevivalDllFlavor() == EfzRevivalDllFlavor::Revival102fSubframe;
}

bool IsEfzRevival102fClassicBuild() {
    return GetEfzRevivalDllFlavor() == EfzRevivalDllFlavor::Revival102fClassic;
}

const char* EfzRevivalVersionName(EfzRevivalVersion v) {
    switch (v) {
    case EfzRevivalVersion::Unknown: return "Unknown";
    case EfzRevivalVersion::Vanilla: return "Vanilla";
    case EfzRevivalVersion::Revival102f:
        switch (GetEfzRevivalDllFlavor()) {
        case EfzRevivalDllFlavor::Revival102fClassic: return "Revival 1.02f (classic)";
        case EfzRevivalDllFlavor::Revival102fSubframe: return "Revival 1.02f (subframe)";
        default: return "Revival 1.02f";
        }
    case EfzRevivalVersion::Revival102e: return "Revival 1.02e";
    case EfzRevivalVersion::Revival102g: return "Revival 1.02g";
    case EfzRevivalVersion::Revival102h: return "Revival 1.02h";
    case EfzRevivalVersion::Revival102i: return "Revival 1.02i";
    case EfzRevivalVersion::Other: return "Revival (Other)";
    default: return "(invalid)";
    }
}

const char* EfzRevivalDllFlavorName(EfzRevivalDllFlavor flavor) {
    switch (flavor) {
    case EfzRevivalDllFlavor::Unknown: return "Unknown";
    case EfzRevivalDllFlavor::Standard: return "Standard";
    case EfzRevivalDllFlavor::Revival102fClassic: return "Revival 1.02f classic";
    case EfzRevivalDllFlavor::Revival102fSubframe: return "Revival 1.02f subframe";
    default: return "(invalid)";
    }
}

const char* NetplayStateSourceName(NetplayStateSource source) {
    switch (source) {
    case NetplayStateSource::None: return "None";
    case NetplayStateSource::ExportSharedMemory: return "ExportSharedMemory";
    case NetplayStateSource::ExportDll: return "ExportDll";
    case NetplayStateSource::LegacyRevival: return "LegacyRevival";
    default: return "(invalid)";
    }
}

bool IsEfzRevivalVersionSupported(EfzRevivalVersion v) {
    EfzRevivalVersion version = (v == static_cast<EfzRevivalVersion>(0)) ? GetEfzRevivalVersion() : v;
    switch (version) {
    case EfzRevivalVersion::Vanilla:
    case EfzRevivalVersion::Revival102e:
    case EfzRevivalVersion::Revival102g:
    case EfzRevivalVersion::Revival102h:
    case EfzRevivalVersion::Revival102i:
        return true;
    case EfzRevivalVersion::Revival102f: {
        EfzRevivalDllFlavor flavor = GetEfzRevivalDllFlavor();
        return flavor == EfzRevivalDllFlavor::Revival102fClassic
            || flavor == EfzRevivalDllFlavor::Revival102fSubframe;
    }
    default:
        return false;
    }
}

OnlineState ReadEfzRevivalOnlineState() {
    static std::atomic<bool> s_loggedOnce{false};
    bool shouldLog = !s_loggedOnce.exchange(true, std::memory_order_relaxed);

    auto mapState = [](int value) -> OnlineState {
        switch (value) {
        case 0: return OnlineState::Netplay;
        case 1: return OnlineState::Spectating;
        case 2: return OnlineState::Offline;
        case 3: return OnlineState::Tournament;
        default: return OnlineState::Unknown;
        }
    };

    EfzRevivalVersion version = GetEfzRevivalVersion();
    if (version == EfzRevivalVersion::Unknown
        || version == EfzRevivalVersion::Vanilla
        || version == EfzRevivalVersion::Other) {
        return OnlineState::Unknown;
    }

    HMODULE revivalModule = GetModuleHandleA("EfzRevival.dll");
    if (!revivalModule) {
        return OnlineState::Unknown;
    }

    uintptr_t base = reinterpret_cast<uintptr_t>(revivalModule);
    uintptr_t ctx = 0;
    uintptr_t ctxPtrAddr = base + 0x26A4;
    if (shouldLog) {
        LogOut("[ONLINE_STATE] Checking pointer-based path at EfzRevival.dll+0x26A4 (VA=0x"
            + FormatHexAddress(ctxPtrAddr) + ")", true);
    }

    if (SafeReadMemory(ctxPtrAddr, &ctx, sizeof(ctx)) && ctx != 0) {
        size_t offset = (version == EfzRevivalVersion::Revival102i) ? 0x37C : 0x370;
        int raw = 0;
        uintptr_t stateAddr = ctx + offset;
        if (SafeReadMemory(stateAddr, &raw, sizeof(raw))) {
            OnlineState state = mapState(raw);
            if (state == OnlineState::Unknown) state = mapState(raw & 0xFF);
            if (state == OnlineState::Unknown) state = mapState(raw & 0x03);
            if (state != OnlineState::Unknown) {
                SetOnlineReason(std::string("Pointer-based ctx+0x")
                                + FormatHexAddress(offset)
                                + " => "
                                + OnlineStateName(state));
                return state;
            }
        } else if (shouldLog) {
            LogOut("[ONLINE_STATE] Failed to read pointer-based state", true);
        }
    }

    uintptr_t rva = 0;
    switch (version) {
    case EfzRevivalVersion::Revival102f:
    case EfzRevivalVersion::Revival102e:
    case EfzRevivalVersion::Revival102g:
        rva = 0x00A05D0;
        break;
    case EfzRevivalVersion::Revival102h:
        rva = 0x00A05F0;
        break;
    case EfzRevivalVersion::Revival102i:
        rva = 0x00A15FC;
        break;
    default:
        return OnlineState::Unknown;
    }

    int raw = 0;
    if (SafeReadMemory(base + rva, &raw, sizeof(raw))) {
        OnlineState state = mapState(raw);
        if (state == OnlineState::Unknown) state = mapState(raw & 0xFF);
        if (state == OnlineState::Unknown) state = mapState(raw & 0x03);
        if (state != OnlineState::Unknown) {
            SetOnlineReason(std::string("Legacy RVA 0x")
                            + FormatHexAddress(rva)
                            + " => "
                            + OnlineStateName(state));
            return state;
        }
    }

    if (ProcessHasActiveUdpConnection()) {
        SetOnlineReason("Active UDP socket(s) detected => Netplay");
        return OnlineState::Netplay;
    }

    SetOnlineReason("Revival reads unavailable => Offline");
    return OnlineState::Offline;
}

const char* OnlineStateName(OnlineState state) {
    switch (state) {
    case OnlineState::Netplay: return "Netplay";
    case OnlineState::Spectating: return "Spectating";
    case OnlineState::Offline: return "Offline";
    case OnlineState::Tournament: return "Tournament";
    default: return "Unknown";
    }
}

void RefreshNetplayRuntimeState() {
    NetplayRuntimeState nextState = {};
    nextState.refreshTick = GetTickCount();
    nextState.source = NetplayStateSource::None;
    nextState.legacyOnlineState = OnlineState::Offline;
    nextState.exportState.magic = 0;
    nextState.exportState.version = EFZ_NETPLAY_STATE_VERSION;
    nextState.exportState.structSize = sizeof(EFZNetplayState);

    EFZNetplayState exportState = {};
    NetplayStateSource exportSource = NetplayStateSource::None;
    if (TryReadExportSnapshot(exportState, exportSource)) {
        nextState.exportAvailable = true;
        nextState.source = exportSource;
        nextState.exportState = exportState;
        const uint32_t caps = exportState.capabilityFlags;
        const bool hasSession = (caps & EFZ_CAP_SESSION) != 0;
        const bool hasMenu = (caps & EFZ_CAP_MENU) != 0;
        const bool hasGameFlow = (caps & EFZ_CAP_GAME_FLOW) != 0;
        const bool hasActivity = (caps & EFZ_CAP_ACTIVITY) != 0;
        const bool hasRelevantCaps = hasSession || hasMenu || hasGameFlow || hasActivity;

        const bool inMenu = hasMenu && exportState.inNetplayMenu != 0;
        const bool inCharSelect = hasGameFlow && exportState.inNetplayCharacterSelect != 0;
        const bool inMatch = hasGameFlow && exportState.inNetplayMatch != 0;
        const bool inFlow = inCharSelect || inMatch;
        const bool activityActive = hasActivity
            && exportState.activityPhase != EFZ_ACTIVITY_IDLE
            && exportState.activityPhase != EFZ_ACTIVITY_HOST_IDLE;
        const bool phaseTerminal =
            hasSession
            && (exportState.sessionPhase == EFZ_PHASE_FAILED
            || exportState.sessionPhase == EFZ_PHASE_SESSION_ENDED);
        const bool phaseActive =
            hasSession
            && exportState.sessionPhase != EFZ_PHASE_IDLE
            && !phaseTerminal;
        bool legacyOnline = false;
        OnlineState legacyState = OnlineState::Offline;
        std::string legacyReason;
        if (!hasRelevantCaps) {
            legacyOnline = DetectLegacyOnlineState(legacyState, legacyReason);
            nextState.legacyOnlineState = legacyState;
        }
        const bool exportOwnsRuntime =
            inMenu || inFlow || activityActive || phaseActive || legacyOnline;

        nextState.inNetplayMenu = inMenu;
        nextState.inNetplayCharacterSelect = inCharSelect;
        nextState.inNetplayMatch = inMatch;
        nextState.inNetplayFlow = inFlow;
        nextState.sessionActive = exportOwnsRuntime;
        nextState.suspendTraining = exportOwnsRuntime;

        std::ostringstream reason;
        reason << "Export " << NetplayStateSourceName(exportSource)
               << " caps=0x" << std::hex << std::uppercase << caps << std::dec
               << " mode=" << exportState.sessionMode
               << " phase=" << (hasSession ? std::to_string(exportState.sessionPhase) : std::string("n/a"))
               << " activity=" << (hasActivity ? std::to_string(exportState.activityPhase) : std::string("n/a"))
               << " menu=" << (inMenu ? "1" : "0")
               << " flow=" << (inFlow ? "1" : "0");
        if (!hasRelevantCaps) {
            reason << " legacyFallback=" << OnlineStateName(legacyState)
                   << " legacyReason=" << legacyReason;
        }
        SetOnlineReason(reason.str());
    } else {
        OnlineState legacyState = OnlineState::Offline;
        std::string reason;
        bool legacyOnline = DetectLegacyOnlineState(legacyState, reason);
        nextState.source = (legacyOnline || legacyState != OnlineState::Offline)
            ? NetplayStateSource::LegacyRevival
            : NetplayStateSource::None;
        nextState.legacyOnlineState = legacyState;
        nextState.sessionActive = legacyOnline;
        nextState.suspendTraining = legacyOnline;
        SetOnlineReason(reason);
    }

    CommitRuntimeState(nextState);
}

NetplayRuntimeState GetNetplayRuntimeState() {
    std::lock_guard<std::mutex> lock(s_runtimeMutex);
    return s_runtimeState;
}

bool IsNetplaySuspendActive() {
    return s_suspendTraining.load(std::memory_order_acquire);
}

bool IsNetplaySessionActive() {
    return s_sessionActive.load(std::memory_order_acquire);
}

bool IsNetplayMenuActive() {
    return s_inNetplayMenu.load(std::memory_order_acquire);
}

bool IsNetplayFlowActive() {
    return s_inNetplayFlow.load(std::memory_order_acquire);
}

bool IsNetplayExportAvailable() {
    return s_exportAvailable.load(std::memory_order_acquire);
}

bool DetectOnlineMatch() {
    NetplayRuntimeState state = GetNetplayRuntimeState();
    if (state.source != NetplayStateSource::None) {
        return state.suspendTraining;
    }

    OnlineState legacyState = OnlineState::Offline;
    std::string reason;
    bool online = DetectLegacyOnlineState(legacyState, reason);
    SetOnlineReason(reason);
    return online;
}

std::string GetLastOnlineDetectionReason() {
    std::lock_guard<std::mutex> lock(s_reasonMutex);
    return s_lastOnlineReason;
}
