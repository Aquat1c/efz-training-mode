// Desync detection and forensics. See include/netplay/bridge/desync_monitor.h.

// inet_addr is the XP-compatible parser (inet_pton needs Vista+).
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>

#include "netplay/bridge/desync_monitor.h"
#include "netplay/bridge/netplay_state_export.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/mod_settings.h"

#include "logger.h"
#include "mod_version.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

namespace netplay::bridge::desync_monitor
{
namespace
{
using netplay::bridge::takeover::SafeReadInt;
using netplay::bridge::takeover::SafeReadPtr;
using netplay::bridge::takeover::IsReadableRange;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// EFZ.exe fixed addresses (EFZ.exe never changes across Revival versions).
constexpr uintptr_t kScreenTableAddr = 0x00790110u;
constexpr uintptr_t kScreenIndexAddr = 0x00790148u;
constexpr uint32_t kOffsetGameSystem = 0x1C;
constexpr uint32_t kOffsetBattleP1Char = 12;
constexpr uint32_t kOffsetBattleP2Char = 16;

// Sync-relevant regions checksummed and recorded per frame.
constexpr uint32_t kGameSysSliceOffset = 4864;  // mode/winner/counter area
constexpr uint32_t kGameSysSliceSize = 160;
constexpr uint32_t kBattleSliceOffset = 0x430;  // battle screen state area
constexpr uint32_t kBattleSliceSize = 256;
constexpr uint32_t kCharSliceSize = 0x400;      // per-character state window

constexpr size_t kRecordRegionBytes =
    kGameSysSliceSize + kBattleSliceSize + 2u * kCharSliceSize;

// Ring depths. The causal window freezes at the first comparable selected-
// field/RNG difference. A separate fixed array below retains the complete
// selected-gameplay confirmation run even when that begins much later.
constexpr size_t kRegionRingDepth = 128;
constexpr size_t kSampleRingDepth = 512;
constexpr size_t kCompareLogDepth = 64;
constexpr size_t kPostEvidenceDepth = 64;
constexpr size_t kEffectTraceSlots = 64;

// A diagnostic report is armed after this many exact, contiguous gameplay-
// checksum mismatches.  This is not an RNG classifier: an RNG-only offset
// may reconverge or remain latent until later gameplay consumes randomness.
// The native warning remains non-fatal and the session is never modified.
// Must stay below kRegionRingDepth so the dump retains pre-divergence frames.
constexpr int kConfirmMismatchFrames = 96;
constexpr size_t kGameplayRunDepth = kConfirmMismatchFrames;
static_assert(
    kGameplayRunDepth >= static_cast<size_t>(kConfirmMismatchFrames),
    "gameplay confirmation evidence must retain the complete run");

// EFZ effect-ring layout inside gameSystem: the presentation-effect state
// that owns the RNG-consuming particles (type-47 bounce etc., see
// docs/NAYUKI_AWAKE_AIR_THROW_RNG_DESYNC.md).  A per-frame hash of the
// active slots is exchanged alongside the gameplay checksum so cross-peer
// comparison can attribute a divergence to the effect layer BEFORE it
// surfaces through the shared RNG stream.  Layout provenance: efz.c
// effect-ring allocation/processing (sub_4075B0 family).
constexpr uint32_t kEffectRingSlots = 640;
constexpr uintptr_t kEffectAllocCursorOffset = 4992;  // WORD next-alloc cursor
constexpr uintptr_t kEffectProcCursorOffset = 4994;   // WORD oldest/processing
constexpr uintptr_t kEffectActiveFlagBase = 4996;     // + 4*i DWORD per slot
constexpr uintptr_t kEffectStatusBase = 7556;         // + 4*i DWORD per slot
constexpr uintptr_t kEffectRecordBase = 10760;        // + 112*i per-slot record
constexpr uintptr_t kEffectRecordStride = 112;
constexpr size_t kEffectRecordArenaBytes =
    static_cast<size_t>(kEffectRingSlots) * kEffectRecordStride;
// Record-relative offsets of the deterministic per-particle fields.
constexpr uintptr_t kEffectFieldBehaviorId = 0;   // WORD
constexpr uintptr_t kEffectFieldAnimFrame = 2;    // WORD
constexpr uintptr_t kEffectFieldAnimTick = 4;     // WORD
constexpr uintptr_t kEffectFieldPosX = 24;        // double
constexpr uintptr_t kEffectFieldPosY = 32;        // double
constexpr uintptr_t kEffectFieldVelX = 40;        // double
constexpr uintptr_t kEffectFieldVelY = 48;        // double
constexpr uintptr_t kEffectFieldParameter = 76;   // DWORD, type-47 direction/branch data

// The recorded windows contain heap pointers (character struct members,
// sprite/object pointers) that legitimately differ between the two machines.
// Checksums therefore only cover bytes that CHANGED at least once during the
// first N sampled frames of each battle: pointers are constant for the whole
// match and drop out, while sync-relevant state (positions, HP, timers,
// meters) mutates constantly and stays in.  While the peers are in sync they
// observe identical byte changes, so both sides derive the same mask.
constexpr unsigned kMaskCalibrationSamples = 64;

// UDP side channel: host listens on gamePort + this offset.
constexpr uint16_t kSideChannelPortOffset = 2;
constexpr uint32_t kPacketMagic = 0x445A4645u; // 'EFZD' little-endian
// v3 adds a control handshake, a session nonce, an agreed future start
// frame, and exact change-mask identity. Mixed builds remain passive.
constexpr uint8_t kPacketVersion = 3;
constexpr uint32_t kSchemaLayoutId = 0x33474645u; // 'EFG3': reviewed v3 layout
constexpr int kSamplesPerPacket = 4;
constexpr int kHandshakeLeadFrames = 120;
constexpr int kHandshakeSafetyFrames = 30;
constexpr int kReorderWaitFrames = 8;

enum class PacketKind : uint8_t
{
    Hello = 1,
    HelloAck = 2,
    Ready = 3,
    Start = 4,
    StartAck = 5,
    Samples = 6,
    Abort = 7,
};

#pragma pack(push, 1)
struct WirePacket
{
    uint32_t magic;
    uint8_t version;
    uint8_t kind;
    uint8_t role;
    uint8_t count;
    uint32_t sessionNonce;
    uint32_t schemaId;
    int32_t startFrame;
    uint32_t maskHash;
    uint16_t maskByteCount;
    uint16_t battleEpoch;
    struct
    {
        int32_t frame;
        uint32_t checksum;
        uint32_t effectHash;
        int32_t rngState;
    } samples[kSamplesPerPacket];
};
#pragma pack(pop)
static_assert(sizeof(WirePacket) == 92, "wire ABI changed; bump protocol/schema");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct EffectProjectionRecord
{
    uint16_t slot = 0;
    uint16_t behaviorId = 0;
    uint16_t animFrame = 0;
    uint16_t animTick = 0;
    uint32_t activeFlag = 0;
    uint32_t status = 0;
    uint32_t parameter = 0;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
};

struct FrameRecord
{
    int frame = -1;
    uint32_t checksum = 0;
    uint16_t p1Input = 0;
    uint16_t p2Input = 0;
    uint8_t validMask = 0; // bit0 gameSys, bit1 battle, bit2 charP1, bit3 charP2
    uint32_t srcGameSys = 0;
    uint32_t srcBattle = 0;
    uint32_t srcCharP1 = 0;
    uint32_t srcCharP2 = 0;
    // Per-frame Revival session context for the dump.
    int commitFrame = -1;
    int syncFeed = -1;
    int localLen = -1;
    int remoteLen = -1;
    int pingMs = -1;
    uint32_t effectHash = 0;
    int32_t rngState = -1;
    uint16_t effectAllocCursor = 0;
    uint16_t effectProcCursor = 0;
    uint16_t effectTraceCount = 0;
    uint16_t effectTraceOverflow = 0;
    EffectProjectionRecord effectTrace[kEffectTraceSlots];
    uint8_t bytes[kRecordRegionBytes];
};

struct ChecksumSample
{
    int frame = -1;
    uint32_t checksum = 0;
    uint32_t effectHash = 0;
    int32_t rngState = -1;
    uint32_t maskHash = 0;
    uint16_t maskByteCount = 0;
};

struct CompareEntry
{
    int frame = -1;
    uint32_t localSum = 0;
    uint32_t remoteSum = 0;
    bool match = false;
    uint32_t localEffect = 0;
    uint32_t remoteEffect = 0;
    int32_t localRng = -1;
    int32_t remoteRng = -1;
};

CRITICAL_SECTION g_lock;
bool g_lockInited = false;

volatile LONG g_sessionActive = 0;
bool g_dumped = false;
bool g_dumpPending = false;
int g_role = -1;             // NetbridgeRole numeric (0 host, 1 join)
uint16_t g_hostPort = 0;
char g_peerAddress[64] = {};
char g_nickname[64] = {};

FrameRecord g_regionRing[kRegionRingDepth];
size_t g_regionRingNext = 0;

ChecksumSample g_localRing[kSampleRingDepth];
size_t g_localRingNext = 0;

ChecksumSample g_remoteRing[kSampleRingDepth];
size_t g_remoteRingNext = 0;

CompareEntry g_compareLog[kCompareLogDepth];
size_t g_compareLogNext = 0;

// The first comparable post-calibration selected-field/RNG difference freezes the rotating
// pre-window without copying it on the hot path. Subsequent records go into
// bounded append-only post arrays, so session-end disk I/O cannot lose the
// onset even when play continues for thousands of frames.
FrameRecord g_postEvidenceFrames[kPostEvidenceDepth];
size_t g_postEvidenceFrameCount = 0;
CompareEntry g_postEvidenceCompare[kPostEvidenceDepth];
size_t g_postEvidenceCompareCount = 0;
CompareEntry g_gameplayMismatchRun[kGameplayRunDepth];
size_t g_gameplayMismatchRunCount = 0;
bool g_gameplayMismatchRunFrozen = false;
bool g_evidenceTriggered = false;
int g_evidenceTriggerFrame = -1;
LONG g_evidenceBattleEpoch = 0;
char g_evidenceTriggerLayer[24] = {};
size_t g_evidenceRegionNext = 0;
size_t g_evidenceCompareNext = 0;
uint8_t g_evidenceMask[kRecordRegionBytes] = {};
unsigned g_evidenceMaskByteCount = 0;
uint32_t g_evidenceMaskHash = 0;

int g_mismatchStreak = 0;
int g_firstMismatchFrame = -1;
int g_lastMismatchFrame = -1;
int g_lastComparedFrame = -1;
int g_nextCompareFrame = -1;
int g_highestLocalFrame = -1;
int g_highestRemoteFrame = -1;
volatile LONG g_desyncConfirmed = 0;
int g_confirmedFrame = -1;

// Layer-attribution runs (diagnostic only; window capture is driven solely
// by the selected gameplay checksum).  Begin/match-again pairs localize the
// first observed difference without claiming complete state convergence.
int g_effectLayerRun = 0;
int g_effectLayerFirstFrame = -1;
int g_rngLayerRun = 0;
int g_rngLayerFirstFrame = -1;

unsigned g_sampleCount = 0;
unsigned g_sentSampleCount = 0;
bool g_peerSeen = false;
DWORD g_sessionStartTick = 0;
bool g_peerAbsenceLogged = false;
uint32_t g_sessionNonce = 0;
int g_handshakePhase = 0;
DWORD g_lastControlSendTick = 0;
volatile LONG g_latestFrame = -1;
volatile LONG g_captureStartFrame = -1;
volatile LONG g_captureArmed = 0;
volatile LONG g_battleEpoch = 0;
bool g_inBattle = false;
bool g_maskMismatchLogged = false;
bool g_epochMismatchLogged = false;

// Change-mask calibration state (single writer: the game thread).
uint8_t g_changeMask[kRecordRegionBytes];
uint8_t g_prevRegionBytes[kRecordRegionBytes];
bool g_prevRegionValid = false;
unsigned g_maskSamples = 0;
bool g_maskFrozen = false;
unsigned g_maskByteCount = 0;
uint32_t g_maskHash = 0;
bool g_leftBattleSinceLastSample = false;
int g_lastSampledFrame = -1;

// Validated once per stable battle object. The game thread is the sole
// writer/reader, so no synchronization is needed for these caches.
uintptr_t g_validatedEffectGameSys = 0;
bool g_effectArenaReadable = false;
uintptr_t g_validatedRngAddress = 0;
bool g_rngAddressReadable = false;

SOCKET g_socket = INVALID_SOCKET;
bool g_wsaStarted = false;
sockaddr_in g_peerEndpoint = {};
volatile LONG g_peerEndpointValid = 0;
HANDLE g_workerThread = nullptr;
volatile LONG g_workerStop = 0;

void EnsureLock()
{
    if (!g_lockInited)
    {
        InitializeCriticalSection(&g_lock);
        g_lockInited = true;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint32_t Fnv1a(uint32_t hash, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

uint32_t WireSchemaIdentity()
{
    // Require the same layout and exact diagnostic build. The value is a
    // capability identity, not game state.
    static uint32_t identity = 0;
    if (identity == 0)
    {
        uint32_t hash = Fnv1a(
            2166136261u, &kSchemaLayoutId, sizeof(kSchemaLayoutId));
        const uint32_t packetSize = static_cast<uint32_t>(sizeof(WirePacket));
        hash = Fnv1a(hash, &packetSize, sizeof(packetSize));
        hash = Fnv1a(
            hash,
            netplay::build_info::kVersion,
            std::strlen(netplay::build_info::kVersion));
        hash = Fnv1a(
            hash,
            netplay::build_info::kBuildTimestamp,
            std::strlen(netplay::build_info::kBuildTimestamp));
        identity = hash != 0 ? hash : 1;
    }
    return identity;
}

// Hash a reviewed projection of every active effect slot plus the ring
// cursors. The first cross-peer difference can identify an effect-layer lead,
// but equality does not prove the unselected bytes are equal. Returns 0 when
// the ring is unreadable so "no data" never fakes a mismatch.
uint32_t ComputeEffectRingHash(uintptr_t gameSys, FrameRecord* frameRecord)
{
    if (frameRecord != nullptr)
    {
        frameRecord->effectAllocCursor = 0;
        frameRecord->effectProcCursor = 0;
        frameRecord->effectTraceCount = 0;
        frameRecord->effectTraceOverflow = 0;
    }
    if (gameSys == 0)
    {
        return 0;
    }
    static uint32_t activeFlags[kEffectRingSlots];
    static uint32_t statusFlags[kEffectRingSlots];
    if (g_validatedEffectGameSys != gameSys)
    {
        g_validatedEffectGameSys = gameSys;
        g_effectArenaReadable =
            IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectAllocCursorOffset),
                sizeof(uint16_t) * 2u)
            && IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectActiveFlagBase),
                sizeof(activeFlags))
            && IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectStatusBase),
                sizeof(statusFlags))
            && IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectRecordBase),
                kEffectRecordArenaBytes);
    }
    if (!g_effectArenaReadable)
    {
        return 0;
    }

    uint32_t hash = 2166136261u;
    uint16_t cursors[2] = {0, 0};
    __try
    {
        std::memcpy(
            activeFlags,
            reinterpret_cast<const void*>(gameSys + kEffectActiveFlagBase),
            sizeof(activeFlags));
        std::memcpy(
            statusFlags,
            reinterpret_cast<const void*>(gameSys + kEffectStatusBase),
            sizeof(statusFlags));
        std::memcpy(
            cursors,
            reinterpret_cast<const void*>(gameSys + kEffectAllocCursorOffset),
            sizeof(cursors));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_effectArenaReadable = false;
        return 0;
    }
    hash = Fnv1a(hash, cursors, sizeof(cursors));
    if (frameRecord != nullptr)
    {
        frameRecord->effectAllocCursor = cursors[0];
        frameRecord->effectProcCursor = cursors[1];
    }

    uint32_t activeCount = 0;
    for (uint32_t i = 0; i < kEffectRingSlots; ++i)
    {
        if (activeFlags[i] == 0)
        {
            continue;
        }
        ++activeCount;
        const uintptr_t record =
            gameSys + kEffectRecordBase + kEffectRecordStride * i;
        EffectProjectionRecord fields = {};
        fields.slot = static_cast<uint16_t>(i);
        fields.activeFlag = activeFlags[i];
        fields.status = statusFlags[i];
        __try
        {
            std::memcpy(&fields.behaviorId,
                        reinterpret_cast<const void*>(record + kEffectFieldBehaviorId), 2);
            std::memcpy(&fields.animFrame,
                        reinterpret_cast<const void*>(record + kEffectFieldAnimFrame), 2);
            std::memcpy(&fields.animTick,
                        reinterpret_cast<const void*>(record + kEffectFieldAnimTick), 2);
            std::memcpy(&fields.x,
                        reinterpret_cast<const void*>(record + kEffectFieldPosX), 8);
            std::memcpy(&fields.y,
                        reinterpret_cast<const void*>(record + kEffectFieldPosY), 8);
            std::memcpy(&fields.vx,
                        reinterpret_cast<const void*>(record + kEffectFieldVelX), 8);
            std::memcpy(&fields.vy,
                        reinterpret_cast<const void*>(record + kEffectFieldVelY), 8);
            std::memcpy(&fields.parameter,
                        reinterpret_cast<const void*>(record + kEffectFieldParameter), 4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_effectArenaReadable = false;
            return 0;
        }
        hash = Fnv1a(hash, &fields.slot, sizeof(fields.slot));
        hash = Fnv1a(hash, &fields.activeFlag, sizeof(fields.activeFlag));
        hash = Fnv1a(hash, &fields.status, sizeof(fields.status));
        hash = Fnv1a(hash, &fields.behaviorId, sizeof(fields.behaviorId));
        hash = Fnv1a(hash, &fields.animFrame, sizeof(fields.animFrame));
        hash = Fnv1a(hash, &fields.animTick, sizeof(fields.animTick));
        hash = Fnv1a(hash, &fields.parameter, sizeof(fields.parameter));
        hash = Fnv1a(hash, &fields.x, sizeof(fields.x));
        hash = Fnv1a(hash, &fields.y, sizeof(fields.y));
        hash = Fnv1a(hash, &fields.vx, sizeof(fields.vx));
        hash = Fnv1a(hash, &fields.vy, sizeof(fields.vy));
        if (frameRecord != nullptr)
        {
            if (frameRecord->effectTraceCount < kEffectTraceSlots)
            {
                frameRecord->effectTrace[frameRecord->effectTraceCount++] = fields;
            }
            else
            {
                ++frameRecord->effectTraceOverflow;
            }
        }
    }
    hash = Fnv1a(hash, &activeCount, sizeof(activeCount));
    return hash != 0 ? hash : 1;
}

// Read the live Revival minstd_rand engine state (the value Revival logs as
// "Rng" and stores in Sync records at +56).  -1 when unavailable.
int32_t ReadRevivalRngState()
{
    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    if (profile == nullptr || profile->rngEngineStateOffset == 0)
    {
        return -1;
    }
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return -1;
    }
    const uintptr_t address =
        reinterpret_cast<uintptr_t>(revival) + profile->rngEngineStateOffset;
    if (g_validatedRngAddress != address)
    {
        g_validatedRngAddress = address;
        g_rngAddressReadable =
            IsReadableRange(reinterpret_cast<const void*>(address), sizeof(int32_t));
    }
    if (!g_rngAddressReadable)
    {
        return -1;
    }
    int state = -1;
    __try
    {
        std::memcpy(&state, reinterpret_cast<const void*>(address), sizeof(state));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_rngAddressReadable = false;
        return -1;
    }
    return state;
}

bool CopyRegion(uintptr_t src, void* dst, size_t size)
{
    if (src == 0 || !IsReadableRange(reinterpret_cast<const void*>(src), size))
    {
        std::memset(dst, 0, size);
        return false;
    }
    std::memcpy(dst, reinterpret_cast<const void*>(src), size);
    return true;
}

bool ReadHistoryInput(
    uintptr_t sessionPtr,
    uintptr_t vecOffset,
    int frame,
    uint16_t* outInput)
{
    *outInput = 0;
    uintptr_t begin = 0;
    uintptr_t end = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + vecOffset), &begin)
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + vecOffset + 4), &end)
        || begin == 0
        || end <= begin)
    {
        return false;
    }
    const uintptr_t elem = begin + 2u * static_cast<uintptr_t>(frame);
    if (frame < 0 || elem + 2 > end
        || !IsReadableRange(reinterpret_cast<const void*>(elem), 2))
    {
        return false;
    }
    *outInput = *reinterpret_cast<const uint16_t*>(elem);
    return true;
}

std::string ModuleDirectory()
{
    std::string path = netplay::bridge::takeover::ModulePath(
        netplay::bridge::takeover::SelfModule());
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
    {
        path.resize(slash);
    }
    return path;
}

// Length (in 2-byte entries) of a Revival input-history vector.
int ReadHistoryLength(uintptr_t sessionPtr, uintptr_t vecOffset)
{
    uintptr_t begin = 0;
    uintptr_t end = 0;
    if (vecOffset == 0
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + vecOffset), &begin)
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + vecOffset + 4), &end)
        || begin == 0
        || end < begin)
    {
        return -1;
    }
    return static_cast<int>((end - begin) >> 1);
}

// Head/tail of a Revival named shared-memory wire (same 8-byte header the
// SYNC_DIAG ring probe reads).
bool ProbeWireHeadTail(const char* legacyName, DWORD* outHead, DWORD* outTail)
{
    *outHead = 0;
    *outTail = 0;
    const char* wireName = netplay::bridge::takeover::RevivalWireName(legacyName);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, wireName);
    if (mapping == nullptr)
    {
        return false;
    }
    bool ok = false;
    const volatile DWORD* view = static_cast<const volatile DWORD*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 8));
    if (view != nullptr)
    {
        *outHead = view[0];
        *outTail = view[1];
        ok = true;
        UnmapViewOfFile(const_cast<DWORD*>(view));
    }
    CloseHandle(mapping);
    return ok;
}

// Decode the synced meta byte (high byte of the input pair): bit 0x20 is
// ESC, bits 0x01..0x10 are F4..F8.
void FormatInputPair(char* out, size_t outSize, uint16_t pair)
{
    const uint8_t pad = static_cast<uint8_t>(pair & 0xFF);
    const uint8_t meta = static_cast<uint8_t>(pair >> 8);
    char metaText[32] = {};
    int pos = 0;
    if ((meta & 0x20) != 0) pos += std::snprintf(metaText + pos, sizeof(metaText) - pos, "ESC ");
    for (int bit = 0; bit < 5; ++bit)
    {
        if ((meta & (1u << bit)) != 0)
        {
            pos += std::snprintf(metaText + pos, sizeof(metaText) - pos, "F%d ", 4 + bit);
        }
    }
    if (pos > 0)
    {
        metaText[pos - 1] = '\0';
    }
    std::snprintf(
        out,
        outSize,
        "pad=%02X meta=%02X%s%s%s",
        pad,
        meta,
        metaText[0] != '\0' ? " [" : "",
        metaText,
        metaText[0] != '\0' ? "]" : "");
}

// Session-object wchar_t[64] name field (raw array inside the config
// snapshot; may sit at an unaligned offset) converted to UTF-8.
void ReadSessionName(uintptr_t sessionPtr, uintptr_t nameOffset, char* out, size_t outSize)
{
    out[0] = '\0';
    if (sessionPtr == 0 || nameOffset == 0)
    {
        return;
    }
    wchar_t wide[64] = {};
    if (!IsReadableRange(
            reinterpret_cast<const void*>(sessionPtr + nameOffset), sizeof(wide)))
    {
        return;
    }
    std::memcpy(wide, reinterpret_cast<const void*>(sessionPtr + nameOffset), sizeof(wide));
    wide[63] = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(outSize), nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Side channel
// ---------------------------------------------------------------------------

bool ParseIpv4(const char* text, uint32_t* outAddr)
{
    // Accept "a.b.c.d" and "a.b.c.d:port"; IPv6 is not supported by the
    // side channel (detection stays passive).
    char host[64] = {};
    size_t n = 0;
    for (; text[n] != '\0' && text[n] != ':' && n < sizeof(host) - 1; ++n)
    {
        host[n] = text[n];
    }
    host[n] = '\0';
    const unsigned long parsed = inet_addr(host);
    if (parsed == INADDR_NONE || parsed == 0)
    {
        return false;
    }
    *outAddr = static_cast<uint32_t>(parsed);
    return true;
}

void CloseSideChannel()
{
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
}

bool OpenSideChannel()
{
    if (!g_wsaStarted)
    {
        WSADATA wsaData = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            return false;
        }
        g_wsaStarted = true;
    }

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;

    InterlockedExchange(&g_peerEndpointValid, 0);
    std::memset(&g_peerEndpoint, 0, sizeof(g_peerEndpoint));

    if (g_role == 0)
    {
        // Host: fixed listen port; the joiner initiates.
        local.sin_port = htons(static_cast<uint16_t>(g_hostPort + kSideChannelPortOffset));
    }
    else
    {
        // Joiner: ephemeral local port; the host replies to our source addr,
        // which also opens the NAT path (same shape as the game traffic).
        uint32_t hostAddr = 0;
        if (!ParseIpv4(g_peerAddress, &hostAddr))
        {
            mod::Log(
                "DESYNC_MONITOR: side channel disabled (address '%s' is not IPv4)",
                g_peerAddress);
            CloseSideChannel();
            return false;
        }
        g_peerEndpoint.sin_family = AF_INET;
        g_peerEndpoint.sin_addr.s_addr = hostAddr;
        g_peerEndpoint.sin_port =
            htons(static_cast<uint16_t>(g_hostPort + kSideChannelPortOffset));
        InterlockedExchange(&g_peerEndpointValid, 1);
        local.sin_port = 0;
    }

    if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
    {
        mod::Log(
            "DESYNC_MONITOR: side channel bind failed role=%d port=%u err=%d",
            g_role,
            static_cast<unsigned>(g_hostPort + kSideChannelPortOffset),
            WSAGetLastError());
        CloseSideChannel();
        return false;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(g_socket, FIONBIO, &nonBlocking) != 0)
    {
        mod::Log(
            "DESYNC_MONITOR: side channel nonblocking setup failed err=%d",
            WSAGetLastError());
        CloseSideChannel();
        return false;
    }

    mod::Log(
        "DESYNC_MONITOR: side channel open role=%d listenPort=%u peer='%s'",
        g_role,
        g_role == 0 ? static_cast<unsigned>(g_hostPort + kSideChannelPortOffset) : 0u,
        g_role == 0 ? "(waiting for joiner)" : g_peerAddress);
    return true;
}

bool SendPacket(const WirePacket& packet)
{
    if (g_socket == INVALID_SOCKET
        || InterlockedCompareExchange(&g_peerEndpointValid, 0, 0) == 0)
    {
        return false;
    }

    return sendto(
        g_socket,
        reinterpret_cast<const char*>(&packet),
        static_cast<int>(sizeof(packet)),
        0,
        reinterpret_cast<const sockaddr*>(&g_peerEndpoint),
        sizeof(g_peerEndpoint)) == static_cast<int>(sizeof(packet));
}

void InitializeWireHeader(WirePacket* packet, PacketKind kind)
{
    std::memset(packet, 0, sizeof(*packet));
    packet->magic = kPacketMagic;
    packet->version = kPacketVersion;
    packet->kind = static_cast<uint8_t>(kind);
    packet->role = static_cast<uint8_t>(g_role);
    packet->sessionNonce = g_sessionNonce;
    packet->schemaId = WireSchemaIdentity();
    packet->battleEpoch = static_cast<uint16_t>(
        InterlockedCompareExchange(&g_battleEpoch, 0, 0));
    packet->startFrame = static_cast<int32_t>(
        InterlockedCompareExchange(&g_captureStartFrame, -1, -1));
}

void SendControlPacket(PacketKind kind, int startFrame)
{
    WirePacket packet = {};
    InitializeWireHeader(&packet, kind);
    packet.startFrame = startFrame;
    (void)SendPacket(packet);
    // Throttle retries even if the nonblocking socket is temporarily full.
    g_lastControlSendTick = GetTickCount();
}

// Build one ascending packet from the fixed local ring. The worker calls this
// while the game thread only appends samples; no socket operation occurs on
// the game thread or while g_lock is held.
bool BuildNextSamplePacket(
    WirePacket* packet,
    unsigned* outFirstSequence,
    LONG* outEpoch)
{
    if (packet == nullptr || outFirstSequence == nullptr || outEpoch == nullptr)
    {
        return false;
    }

    EnterCriticalSection(&g_lock);
    if (InterlockedCompareExchange(&g_sessionActive, 0, 0) == 0
        || InterlockedCompareExchange(&g_captureArmed, 0, 0) == 0
        || !g_maskFrozen)
    {
        LeaveCriticalSection(&g_lock);
        return false;
    }

    const unsigned total = g_sampleCount;
    const unsigned oldest =
        total > static_cast<unsigned>(kSampleRingDepth)
            ? total - static_cast<unsigned>(kSampleRingDepth)
            : 0;
    if (g_sentSampleCount < oldest)
    {
        mod::Log(
            "DESYNC_MONITOR: outgoing sample overflow dropped=%u epoch=%ld",
            oldest - g_sentSampleCount,
            static_cast<long>(InterlockedCompareExchange(&g_battleEpoch, 0, 0)));
        g_sentSampleCount = oldest;
    }
    if (g_sentSampleCount >= total)
    {
        LeaveCriticalSection(&g_lock);
        return false;
    }

    InitializeWireHeader(packet, PacketKind::Samples);
    packet->maskHash = g_maskHash;
    packet->maskByteCount = static_cast<uint16_t>(g_maskByteCount);
    const unsigned pending = total - g_sentSampleCount;
    const int count = pending < static_cast<unsigned>(kSamplesPerPacket)
        ? static_cast<int>(pending)
        : kSamplesPerPacket;
    const unsigned firstSequence = g_sentSampleCount;
    for (int i = 0; i < count; ++i)
    {
        const unsigned sequence = firstSequence + static_cast<unsigned>(i);
        const size_t distanceFromNext =
            static_cast<size_t>(total - sequence);
        const size_t index =
            (g_localRingNext + kSampleRingDepth - distanceFromNext)
            % kSampleRingDepth;
        if (g_localRing[index].frame < 0)
        {
            LeaveCriticalSection(&g_lock);
            return false;
        }
        packet->samples[i].frame = g_localRing[index].frame;
        packet->samples[i].checksum = g_localRing[index].checksum;
        packet->samples[i].effectHash = g_localRing[index].effectHash;
        packet->samples[i].rngState = g_localRing[index].rngState;
    }
    packet->count = static_cast<uint8_t>(count);
    *outFirstSequence = firstSequence;
    *outEpoch = InterlockedCompareExchange(&g_battleEpoch, 0, 0);
    LeaveCriticalSection(&g_lock);
    return true;
}

void PumpOutgoingSamples()
{
    for (int packetBudget = 0; packetBudget < 16; ++packetBudget)
    {
        WirePacket packet = {};
        unsigned firstSequence = 0;
        LONG epoch = 0;
        if (!BuildNextSamplePacket(&packet, &firstSequence, &epoch))
        {
            return;
        }
        if (!SendPacket(packet))
        {
            return;
        }
        EnterCriticalSection(&g_lock);
        if (epoch == InterlockedCompareExchange(&g_battleEpoch, 0, 0)
            && g_sentSampleCount == firstSequence)
        {
            g_sentSampleCount += packet.count;
        }
        LeaveCriticalSection(&g_lock);
    }
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

const ChecksumSample* FindSample(
    const ChecksumSample* ring,
    int frame)
{
    for (size_t i = 0; i < kSampleRingDepth; ++i)
    {
        if (ring[i].frame == frame)
        {
            return &ring[i];
        }
    }
    return nullptr;
}

void TriggerEvidenceLocked(int frame, const char* layer)
{
    if (g_evidenceTriggered)
    {
        return;
    }
    g_evidenceTriggered = true;
    g_evidenceTriggerFrame = frame;
    g_evidenceBattleEpoch =
        InterlockedCompareExchange(&g_battleEpoch, 0, 0);
    g_evidenceRegionNext = g_regionRingNext;
    g_evidenceCompareNext = g_compareLogNext;
    g_evidenceMaskByteCount = g_maskByteCount;
    g_evidenceMaskHash = g_maskHash;
    std::memcpy(g_evidenceMask, g_changeMask, sizeof(g_evidenceMask));
    std::snprintf(
        g_evidenceTriggerLayer,
        sizeof(g_evidenceTriggerLayer),
        "%s",
        layer != nullptr ? layer : "selected-state");
    mod::Log(
        "DESYNC_MONITOR: forensic onset frozen frame=%d layer=%s "
        "(disk I/O deferred; post-window capture continues)",
        frame,
        g_evidenceTriggerLayer);
}

// Compare a remote sample against the local ring. Caller holds the lock.
// Returns true when a contiguous checksum-difference window is first
// captured. The selected gameplay checksum drives that window; the effect
// projection and RNG scalar are only attribution evidence and never mutate
// or terminate the session.
bool CompareRemoteSampleLocked(
    int frame,
    uint32_t remoteSum,
    uint32_t remoteEffectHash,
    int32_t remoteRngState,
    bool gameplayComparable)
{
    const ChecksumSample* local = FindSample(g_localRing, frame);
    if (local == nullptr)
    {
        return false;
    }

    // UDP packets can be duplicated, reordered, or skipped. Only a strictly
    // ascending exact-frame sequence can extend a diagnostic run.
    if (frame <= g_lastComparedFrame)
    {
        return false;
    }
    if (g_lastComparedFrame >= 0 && frame != g_lastComparedFrame + 1)
    {
        if (g_mismatchStreak != 0 || g_effectLayerRun != 0 || g_rngLayerRun != 0)
        {
            mod::Log(
                "DESYNC_MONITOR: comparison gap %d -> %d; all contiguous runs reset",
                g_lastComparedFrame,
                frame);
        }
        g_mismatchStreak = 0;
        g_firstMismatchFrame = -1;
        g_lastMismatchFrame = -1;
        if (!g_gameplayMismatchRunFrozen)
        {
            g_gameplayMismatchRunCount = 0;
        }
        g_effectLayerRun = 0;
        g_effectLayerFirstFrame = -1;
        g_rngLayerRun = 0;
        g_rngLayerFirstFrame = -1;
    }
    g_lastComparedFrame = frame;

    const bool match =
        !gameplayComparable || (local->checksum == remoteSum);

    CompareEntry scratch = {};
    CompareEntry* entryPtr = &scratch;
    if (!g_evidenceTriggered)
    {
        entryPtr = &g_compareLog[g_compareLogNext];
        g_compareLogNext = (g_compareLogNext + 1) % kCompareLogDepth;
    }
    else if (g_postEvidenceCompareCount < kPostEvidenceDepth)
    {
        entryPtr = &g_postEvidenceCompare[g_postEvidenceCompareCount++];
    }
    CompareEntry& entry = *entryPtr;
    entry.frame = frame;
    entry.localSum = local->checksum;
    entry.remoteSum = remoteSum;
    entry.match = match;
    entry.localEffect = local->effectHash;
    entry.remoteEffect = remoteEffectHash;
    entry.localRng = local->rngState;
    entry.remoteRng = remoteRngState;

    // Layer attribution.  Only compare when both sides produced data (a 0
    // effect hash or -1 RNG state means "unavailable", not "different").
    const bool effectComparable =
        local->effectHash != 0 && remoteEffectHash != 0;
    const bool effectMatch =
        !effectComparable || local->effectHash == remoteEffectHash;
    const bool rngComparable =
        local->rngState != -1 && remoteRngState != -1;
    const bool rngMatch =
        !rngComparable || local->rngState == remoteRngState;

    if (effectComparable)
    {
        if (!effectMatch && g_effectLayerRun == 0)
        {
            g_effectLayerFirstFrame = frame;
            mod::Log(
                "DESYNC_MONITOR_LAYER: effect projection differs at frame %d "
                "(gameplay=%s rng=%s) localEff=0x%08lX remoteEff=0x%08lX",
                frame,
                gameplayComparable ? (match ? "match" : "MISMATCH") : "not-comparable",
                rngMatch ? "match" : "MISMATCH",
                static_cast<unsigned long>(local->effectHash),
                static_cast<unsigned long>(remoteEffectHash));
        }
        if (!effectMatch)
        {
            ++g_effectLayerRun;
        }
        else if (g_effectLayerRun != 0)
        {
            mod::Log(
                "DESYNC_MONITOR_LAYER: effect projection matches again at frame %d "
                "(run of %d from frame %d)",
                frame,
                g_effectLayerRun,
                g_effectLayerFirstFrame);
            g_effectLayerRun = 0;
            g_effectLayerFirstFrame = -1;
        }
    }
    if (rngComparable)
    {
        if (!rngMatch && g_rngLayerRun == 0)
        {
            g_rngLayerFirstFrame = frame;
            mod::Log(
                "DESYNC_MONITOR_LAYER: RNG scalar differs at frame %d "
                "(gameplay=%s effects=%s) localRng=%ld remoteRng=%ld",
                frame,
                gameplayComparable ? (match ? "match" : "MISMATCH") : "not-comparable",
                effectMatch ? "match" : "MISMATCH",
                static_cast<long>(local->rngState),
                static_cast<long>(remoteRngState));
        }
        if (!rngMatch)
        {
            ++g_rngLayerRun;
        }
        else if (g_rngLayerRun != 0)
        {
            mod::Log(
                "DESYNC_MONITOR_LAYER: RNG scalar matches again at frame %d "
                "(run of %d from frame %d)",
                frame,
                g_rngLayerRun,
                g_rngLayerFirstFrame);
            g_rngLayerRun = 0;
            g_rngLayerFirstFrame = -1;
        }
    }


    const bool effectDiff = effectComparable && !effectMatch;
    const bool rngDiff = rngComparable && !rngMatch;
    const bool gameplayDiff = gameplayComparable && !match;
    if (effectDiff || rngDiff || gameplayDiff)
    {
        const char* layer =
            (effectDiff && rngDiff) ? "effect+rng"
            : effectDiff ? "effect-projection"
            : rngDiff ? "rng-scalar"
            : "selected-gameplay";
        TriggerEvidenceLocked(frame, layer);
    }

    if (!gameplayComparable)
    {
        return false;
    }

    if (match)
    {
        if (g_mismatchStreak != 0 && frame >= g_firstMismatchFrame)
        {
            mod::Log(
                "DESYNC_MONITOR: checksum-difference run ended at matching frame %d "
                "(run was %d from frame %d; hidden state may still differ)",
                frame,
                g_mismatchStreak,
                g_firstMismatchFrame);
            g_mismatchStreak = 0;
            g_firstMismatchFrame = -1;
            g_lastMismatchFrame = -1;
            if (!g_gameplayMismatchRunFrozen)
            {
                g_gameplayMismatchRunCount = 0;
            }
        }
        return false;
    }

    if (g_mismatchStreak == 0)
    {
        g_firstMismatchFrame = frame;
        g_lastMismatchFrame = frame;
        g_mismatchStreak = 1;
        if (!g_gameplayMismatchRunFrozen)
        {
            g_gameplayMismatchRunCount = 0;
            g_gameplayMismatchRun[g_gameplayMismatchRunCount++] = entry;
        }
        mod::Log(
            "DESYNC_MONITOR: checksum mismatch at frame %d local=0x%08lX remote=0x%08lX",
            frame,
            static_cast<unsigned long>(local->checksum),
            static_cast<unsigned long>(remoteSum));
        return false;
    }

    g_lastMismatchFrame = frame;
    ++g_mismatchStreak;
    if (!g_gameplayMismatchRunFrozen
        && g_gameplayMismatchRunCount < kGameplayRunDepth)
    {
        g_gameplayMismatchRun[g_gameplayMismatchRunCount++] = entry;
    }

    if (g_mismatchStreak >= kConfirmMismatchFrames
        && InterlockedExchange(&g_desyncConfirmed, 1) == 0)
    {
        g_confirmedFrame = g_gameplayMismatchRunCount > 0
            ? g_gameplayMismatchRun[0].frame
            : g_firstMismatchFrame;
        g_gameplayMismatchRunFrozen = true;
        return true;
    }
    return false;
}

int FindLowestCommonFrameAfterLocked(int frameExclusive)
{
    int lowest = -1;
    for (size_t i = 0; i < kSampleRingDepth; ++i)
    {
        const int frame = g_localRing[i].frame;
        if (frame <= frameExclusive || (lowest >= 0 && frame >= lowest))
        {
            continue;
        }
        if (FindSample(g_remoteRing, frame) != nullptr)
        {
            lowest = frame;
        }
    }
    return lowest;
}

bool DrainComparableSamplesLocked()
{
    bool captured = false;
    const int frontier =
        g_highestLocalFrame < g_highestRemoteFrame
            ? g_highestLocalFrame
            : g_highestRemoteFrame;
    if (frontier < 0)
    {
        return false;
    }

    if (g_nextCompareFrame < 0)
    {
        const int first = FindLowestCommonFrameAfterLocked(-1);
        if (first < 0 || frontier - first < kReorderWaitFrames)
        {
            return false;
        }
        g_nextCompareFrame = first;
    }

    for (int budget = 0; budget < 64; ++budget)
    {
        const ChecksumSample* local = FindSample(g_localRing, g_nextCompareFrame);
        const ChecksumSample* remote = FindSample(g_remoteRing, g_nextCompareFrame);
        if (local == nullptr || remote == nullptr)
        {
            if (frontier - g_nextCompareFrame < kReorderWaitFrames)
            {
                break;
            }
            const int nextAvailable =
                FindLowestCommonFrameAfterLocked(g_nextCompareFrame);
            if (nextAvailable < 0 || nextAvailable > frontier)
            {
                break;
            }
            g_nextCompareFrame = nextAvailable;
            local = FindSample(g_localRing, g_nextCompareFrame);
            remote = FindSample(g_remoteRing, g_nextCompareFrame);
            if (local == nullptr || remote == nullptr)
            {
                break;
            }
        }

        const bool gameplayComparable =
            g_maskFrozen
            && remote->maskHash != 0
            && remote->maskHash == g_maskHash
            && remote->maskByteCount == g_maskByteCount;
        if (!gameplayComparable && g_maskFrozen && !g_maskMismatchLogged)
        {
            g_maskMismatchLogged = true;
            mod::Log(
                "DESYNC_MONITOR: selected-gameplay masks differ; gameplay-window "
                "trigger disabled localMask=0x%08lX/%u remoteMask=0x%08lX/%u "
                "(effect/RNG attribution continues)",
                static_cast<unsigned long>(g_maskHash),
                g_maskByteCount,
                static_cast<unsigned long>(remote->maskHash),
                static_cast<unsigned>(remote->maskByteCount));
        }
        captured |= CompareRemoteSampleLocked(
            g_nextCompareFrame,
            remote->checksum,
            remote->effectHash,
            remote->rngState,
            gameplayComparable);
        ++g_nextCompareFrame;
    }
    return captured;
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

bool g_dumpWriteOk = true;

void AppendText(HANDLE file, const char* text)
{
    DWORD written = 0;
    const DWORD expected = static_cast<DWORD>(std::strlen(text));
    if (!WriteFile(file, text, expected, &written, nullptr)
        || written != expected)
    {
        g_dumpWriteOk = false;
    }
}

void AppendFormat(HANDLE file, const char* format, ...)
{
    char buffer[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    AppendText(file, buffer);
}

void AppendHexRegion(
    HANDLE file,
    const char* label,
    uint32_t srcAddr,
    const uint8_t* bytes,
    size_t size,
    bool valid)
{
    AppendFormat(
        file,
        "  %s src=0x%08lX size=%u valid=%d\r\n",
        label,
        static_cast<unsigned long>(srcAddr),
        static_cast<unsigned>(size),
        valid ? 1 : 0);
    if (!valid)
    {
        return;
    }
    char line[128];
    for (size_t offset = 0; offset < size; offset += 16)
    {
        int pos = std::snprintf(line, sizeof(line), "    +%04X ", static_cast<unsigned>(offset));
        for (size_t i = 0; i < 16 && offset + i < size; ++i)
        {
            pos += std::snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[offset + i]);
        }
        pos += std::snprintf(line + pos, sizeof(line) - pos, "\r\n");
        AppendText(file, line);
    }
}

void CopyLogInto(const std::string& dumpDir, const std::string& sourcePath, const char* destName)
{
    const std::string dest = dumpDir + "\\" + destName;
    const BOOL ok = CopyFileA(sourcePath.c_str(), dest.c_str(), FALSE);
    mod::Log(
        "DESYNC_MONITOR: copy '%s' -> '%s' result=%d err=%lu",
        sourcePath.c_str(),
        destName,
        ok ? 1 : 0,
        ok ? 0ul : static_cast<unsigned long>(GetLastError()));
}

void AppendCompareEntry(HANDLE report, const CompareEntry& entry)
{
    if (entry.frame < 0)
    {
        return;
    }
    AppendFormat(
        report,
        "  frame=%d local=0x%08lX remote=0x%08lX eff=0x%08lX/0x%08lX%s "
        "rng=%ld/%ld%s %s\r\n",
        entry.frame,
        static_cast<unsigned long>(entry.localSum),
        static_cast<unsigned long>(entry.remoteSum),
        static_cast<unsigned long>(entry.localEffect),
        static_cast<unsigned long>(entry.remoteEffect),
        (entry.localEffect != 0 && entry.remoteEffect != 0
         && entry.localEffect != entry.remoteEffect) ? "(DIFF)" : "",
        static_cast<long>(entry.localRng),
        static_cast<long>(entry.remoteRng),
        (entry.localRng != -1 && entry.remoteRng != -1
         && entry.localRng != entry.remoteRng) ? "(DIFF)" : "",
        entry.match ? "selected-match" : "selected-DIFF");
}

void AppendFrameRecord(HANDLE frames, const FrameRecord& record, int triggerFrame)
{
    char p1Text[64] = {};
    char p2Text[64] = {};
    FormatInputPair(p1Text, sizeof(p1Text), record.p1Input);
    FormatInputPair(p2Text, sizeof(p2Text), record.p2Input);
    AppendFormat(
        frames,
        "==== frame %d checksum=0x%08lX inputs p1=0x%04X (%s) "
        "p2=0x%04X (%s) %s\r\n",
        record.frame,
        static_cast<unsigned long>(record.checksum),
        static_cast<unsigned>(record.p1Input),
        p1Text,
        static_cast<unsigned>(record.p2Input),
        p2Text,
        record.frame == triggerFrame ? "[FORENSIC-TRIGGER]" : "");
    AppendFormat(
        frames,
        "     commit=%d syncFeed=%d localLen=%d remoteLen=%d ping=%d "
        "effectHash=0x%08lX rngState=%ld effectCursors=%u/%u "
        "effectRecords=%u overflow=%u\r\n",
        record.commitFrame,
        record.syncFeed,
        record.localLen,
        record.remoteLen,
        record.pingMs,
        static_cast<unsigned long>(record.effectHash),
        static_cast<long>(record.rngState),
        static_cast<unsigned>(record.effectAllocCursor),
        static_cast<unsigned>(record.effectProcCursor),
        static_cast<unsigned>(record.effectTraceCount),
        static_cast<unsigned>(record.effectTraceOverflow));
    for (uint16_t i = 0; i < record.effectTraceCount; ++i)
    {
        const EffectProjectionRecord& effect = record.effectTrace[i];
        uint64_t xBits = 0;
        uint64_t yBits = 0;
        uint64_t vxBits = 0;
        uint64_t vyBits = 0;
        std::memcpy(&xBits, &effect.x, sizeof(xBits));
        std::memcpy(&yBits, &effect.y, sizeof(yBits));
        std::memcpy(&vxBits, &effect.vx, sizeof(vxBits));
        std::memcpy(&vyBits, &effect.vy, sizeof(vyBits));
        AppendFormat(
            frames,
            "     effect slot=%u active=0x%08lX status=0x%08lX behavior=%u "
            "anim=%u/%u parameter=0x%08lX pos=%.9g,%.9g vel=%.9g,%.9g "
            "bits=x:%08lX%08lX y:%08lX%08lX vx:%08lX%08lX vy:%08lX%08lX\r\n",
            static_cast<unsigned>(effect.slot),
            static_cast<unsigned long>(effect.activeFlag),
            static_cast<unsigned long>(effect.status),
            static_cast<unsigned>(effect.behaviorId),
            static_cast<unsigned>(effect.animFrame),
            static_cast<unsigned>(effect.animTick),
            static_cast<unsigned long>(effect.parameter),
            effect.x,
            effect.y,
            effect.vx,
            effect.vy,
            static_cast<unsigned long>(xBits >> 32),
            static_cast<unsigned long>(xBits & 0xFFFFFFFFu),
            static_cast<unsigned long>(yBits >> 32),
            static_cast<unsigned long>(yBits & 0xFFFFFFFFu),
            static_cast<unsigned long>(vxBits >> 32),
            static_cast<unsigned long>(vxBits & 0xFFFFFFFFu),
            static_cast<unsigned long>(vyBits >> 32),
            static_cast<unsigned long>(vyBits & 0xFFFFFFFFu));
    }
    const uint8_t* cursor = record.bytes;
    AppendHexRegion(frames, "gameSystem", record.srcGameSys, cursor,
        kGameSysSliceSize, (record.validMask & 1) != 0);
    cursor += kGameSysSliceSize;
    AppendHexRegion(frames, "battleScreen", record.srcBattle, cursor,
        kBattleSliceSize, (record.validMask & 2) != 0);
    cursor += kBattleSliceSize;
    AppendHexRegion(frames, "characterP1", record.srcCharP1, cursor,
        kCharSliceSize, (record.validMask & 4) != 0);
    cursor += kCharSliceSize;
    AppendHexRegion(frames, "characterP2", record.srcCharP2, cursor,
        kCharSliceSize, (record.validMask & 8) != 0);
}

// Called only after the worker has stopped at session teardown. Snapshots the
// rings and writes the dump folder without perturbing an active rollback loop.
bool WriteDesyncDump()
{
    // Snapshot state under the lock; file I/O happens after release.
    static FrameRecord regionCopy[kRegionRingDepth];
    static FrameRecord postRegionCopy[kPostEvidenceDepth];
    static CompareEntry compareCopy[kCompareLogDepth];
    static CompareEntry postCompareCopy[kPostEvidenceDepth];
    static CompareEntry gameplayRunCopy[kGameplayRunDepth];
    int confirmedFrame = -1;
    int triggerFrame = -1;
    LONG triggerEpoch = 0;
    char triggerLayer[24] = {};
    unsigned sampleCount = 0;

    EnterCriticalSection(&g_lock);
    std::memcpy(regionCopy, g_regionRing, sizeof(g_regionRing));
    std::memcpy(postRegionCopy, g_postEvidenceFrames, sizeof(g_postEvidenceFrames));
    std::memcpy(compareCopy, g_compareLog, sizeof(g_compareLog));
    std::memcpy(postCompareCopy, g_postEvidenceCompare, sizeof(g_postEvidenceCompare));
    std::memcpy(
        gameplayRunCopy,
        g_gameplayMismatchRun,
        sizeof(g_gameplayMismatchRun));
    confirmedFrame = g_confirmedFrame;
    triggerFrame = g_evidenceTriggerFrame;
    triggerEpoch = g_evidenceBattleEpoch;
    std::memcpy(triggerLayer, g_evidenceTriggerLayer, sizeof(triggerLayer));
    const size_t regionNext = g_evidenceRegionNext;
    const size_t compareNext = g_evidenceCompareNext;
    const size_t postRegionCount = g_postEvidenceFrameCount;
    const size_t postCompareCount = g_postEvidenceCompareCount;
    const size_t gameplayRunCount = g_gameplayMismatchRunCount;
    sampleCount = g_sampleCount;
    const unsigned maskBytes = g_evidenceMaskByteCount;
    const uint32_t maskHash = g_evidenceMaskHash;
    uint8_t maskCopy[kRecordRegionBytes] = {};
    std::memcpy(maskCopy, g_evidenceMask, sizeof(maskCopy));
    const uint32_t sessionNonce = g_sessionNonce;
    const int captureStartFrame = static_cast<int>(
        InterlockedCompareExchange(&g_captureStartFrame, 0, 0));
    const int role = g_role;
    char peer[64];
    char nick[64];
    std::memcpy(peer, g_peerAddress, sizeof(peer));
    std::memcpy(nick, g_nickname, sizeof(nick));
    const uint16_t hostPort = g_hostPort;
    LeaveCriticalSection(&g_lock);

    const std::string dllDir = ModuleDirectory();
    const std::string gameDir = netplay::bridge::takeover::GameDirectory();

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char stamp[64] = {};
    std::snprintf(
        stamp,
        sizeof(stamp),
        "%04u%02u%02u_%02u%02u%02u_%03u_p%lu_r%d_n%08lX",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds,
        static_cast<unsigned long>(GetCurrentProcessId()),
        role,
        static_cast<unsigned long>(sessionNonce));

    const std::string logsDir = dllDir + "\\logs";
    (void)CreateDirectoryA(logsDir.c_str(), nullptr);
    const std::string dumpDir = logsDir + "\\desync_" + stamp;
    if (!CreateDirectoryA(dumpDir.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        mod::Log(
            "DESYNC_MONITOR: dump directory creation failed '%s' err=%lu",
            dumpDir.c_str(),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    mod::Log(
        "DESYNC_MONITOR: *** CHECKSUM WINDOW CAPTURED *** frame=%d dumping to '%s'",
        triggerFrame,
        dumpDir.c_str());
    bool requiredArtifactsOk = true;

    // ---- report.txt ------------------------------------------------------
    const std::string reportPath = dumpDir + "\\report.txt";
    HANDLE report = CreateFileA(
        reportPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    g_dumpWriteOk = true;
    if (report != INVALID_HANDLE_VALUE)
    {
        AppendFormat(report, "EFZ InGameNetplay checksum-divergence report %s\r\n", stamp);
        AppendFormat(report, "mod=%s build=%s\r\n",
            netplay::build_info::kVersion,
            netplay::build_info::kBuildTimestamp);
        AppendFormat(report, "revival=%s wine=%d\r\n",
            (netplay::bridge::takeover::g_activeRevival != nullptr
             && netplay::bridge::takeover::g_activeRevival->versionTag != nullptr)
                ? netplay::bridge::takeover::g_activeRevival->versionTag
                : "unknown",
            netplay::bridge::IsRunningUnderWine() ? 1 : 0);
        AppendFormat(report, "role=%d (0=host 1=join) nickname='%s' peer='%s' hostPort=%u\r\n",
            role, nick, peer, static_cast<unsigned>(hostPort));
        AppendFormat(report,
            "tracer: wire=%u schema=0x%08lX nonce=0x%08lX negotiatedStart=%d\r\n",
            static_cast<unsigned>(kPacketVersion),
            static_cast<unsigned long>(WireSchemaIdentity()),
            static_cast<unsigned long>(sessionNonce),
            captureStartFrame);
        AppendFormat(report,
            "forensic trigger: frame=%d layer=%s battleEpoch=%ld\r\n",
            triggerFrame,
            triggerLayer,
            static_cast<long>(triggerEpoch));
        AppendFormat(report,
            "selected-gameplay 96-frame window start: %d (-1 means not reached)\r\n",
            confirmedFrame);
        AppendFormat(report, "frames sampled this session: %u\r\n", sampleCount);
        AppendFormat(report,
            "checksum change-mask: identity=0x%08lX bytes=%u/%u\r\n"
            "Only identical hash+count peers are gameplay-checksum comparable. This is a\r\n"
            "selected-field projection; scalar equality does not prove full-state equality.\r\n\r\n",
            static_cast<unsigned long>(maskHash),
            maskBytes,
            static_cast<unsigned>(kRecordRegionBytes));

        // Do not call bridge/takeover status getters here: session teardown
        // owns those mutexes on normal cancellation paths. Per-frame context
        // already captured in FrameRecord remains lock-independent.

        // ---- Exported game state (chars / stage / round / wins) -----------
        {
            const EFZNetplayState* state =
                netplay::bridge::state_export::GetExportedState();
            if (state != nullptr)
            {
                AppendFormat(report,
                    "game: activity=%u p1Char=%u p2Char=%u stage=%u round=%u "
                    "wins=%d-%d matchCounter=%d local='%s' p1='%s' p2='%s'\r\n",
                    static_cast<unsigned>(state->activityPhase),
                    static_cast<unsigned>(state->p1CharId),
                    static_cast<unsigned>(state->p2CharId),
                    static_cast<unsigned>(state->stageId),
                    static_cast<unsigned>(state->roundIndex),
                    state->p1Wins,
                    state->p2Wins,
                    state->matchCounter,
                    state->localNickname,
                    state->p1Name,
                    state->p2Name);
            }
        }

        // ---- Revival session object snapshot -------------------------------
        {
            const uintptr_t session = netplay::bridge::takeover::g_lastValidatedSessionPtr;
            const netplay::bridge::takeover::RevivalAddressProfile* profile =
                netplay::bridge::takeover::g_activeRevival;
            if (session != 0 && profile != nullptr)
            {
                int curFrame = -1;
                int commit = -1;
                int syncFeed = -1;
                int matchId = -1;
                int prevMode = -1;
                int curMode = -1;
                int matchStart = -1;
                int advCtr = -1;
                int inputDelay = -1;
                int pingMs = -1;
                int activePlayer = -1;
                int queuePlayer = -1;
                int p1Wins = -1;
                int p2Wins = -1;
                const uintptr_t gmBase = session + profile->sessionOffsetGameModeSnapshot;
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetCurrentFrame), &curFrame);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 16), &commit);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), &syncFeed);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetMatchId), &matchId);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase), &prevMode);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 4), &curMode);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 8), &matchStart);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 12), &advCtr);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetInputDelay), &inputDelay);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetPingMs), &pingMs);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetActivePlayer), &activePlayer);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetQueuePlayer), &queuePlayer);
                if (profile->sessionOffsetP1Wins != 0)
                {
                    (void)SafeReadInt(reinterpret_cast<const void*>(
                        session + profile->sessionOffsetP1Wins), &p1Wins);
                    (void)SafeReadInt(reinterpret_cast<const void*>(
                        session + profile->sessionOffsetP2Wins), &p2Wins);
                }
                char p1SessionName[128] = {};
                char p2SessionName[128] = {};
                ReadSessionName(session, profile->sessionOffsetP1Name,
                    p1SessionName, sizeof(p1SessionName));
                ReadSessionName(session, profile->sessionOffsetP2Name,
                    p2SessionName, sizeof(p2SessionName));
                AppendFormat(report,
                    "session: ptr=0x%08lX matchId=%d frame=%d commit=%d syncFeed=%d "
                    "mode=%d->%d matchStart=%d advCtr=%d\r\n",
                    static_cast<unsigned long>(session),
                    matchId, curFrame, commit, syncFeed,
                    prevMode, curMode, matchStart, advCtr);
                AppendFormat(report,
                    "session: delay=%d ping=%d active=%d queue=%d wins=%d-%d "
                    "localLen=%d remoteLen=%d p1='%s' p2='%s'\r\n",
                    inputDelay, pingMs, activePlayer, queuePlayer, p1Wins, p2Wins,
                    ReadHistoryLength(session, profile->sessionOffsetHistoryPrimaryVec),
                    ReadHistoryLength(session, profile->sessionOffsetHistorySecondaryVec),
                    p1SessionName,
                    p2SessionName);
            }
        }

        // ---- Wire rings -----------------------------------------------------
        {
            static const char* const kWires[] = {
                "InputP1", "InputP2", "Sync", "Net", "Quit", "LoadMatch", "Init"};
            AppendText(report, "wires:");
            for (size_t i = 0; i < sizeof(kWires) / sizeof(kWires[0]); ++i)
            {
                DWORD head = 0;
                DWORD tail = 0;
                const bool ok = ProbeWireHeadTail(kWires[i], &head, &tail);
                AppendFormat(report, " %s(%s h=%lu t=%lu)",
                    kWires[i],
                    ok ? "ok" : "NO",
                    static_cast<unsigned long>(head),
                    static_cast<unsigned long>(tail));
            }
            AppendText(report, "\r\n");
        }

        // ---- EfzRevival.ini highlights (config mismatches cause desyncs) ---
        {
            const std::string iniPath = gameDir + "\\EfzRevival.ini";
            AppendFormat(report,
                "ini: MaxRollback=%u Port=%u Debug=%u (full copy: EfzRevival.ini)\r\n\r\n",
                GetPrivateProfileIntA("Network", "MaxRollback", 0, iniPath.c_str()),
                GetPrivateProfileIntA("Network", "Port", 0, iniPath.c_str()),
                GetPrivateProfileIntA("Global", "Debug", 0, iniPath.c_str()));
        }
        AppendText(report,
            "Checksum comparisons around the first forensic trigger "
            "(oldest first):\r\n");
        for (size_t i = 0; i < kCompareLogDepth; ++i)
        {
            AppendCompareEntry(
                report,
                compareCopy[(compareNext + i) % kCompareLogDepth]);
        }
        for (size_t i = 0; i < postCompareCount; ++i)
        {
            AppendCompareEntry(report, postCompareCopy[i]);
        }
        AppendText(report,
            "\r\nSelected-gameplay contiguous confirmation run "
            "(separate from causal onset window):\r\n");
        if (gameplayRunCount < static_cast<size_t>(kConfirmMismatchFrames))
        {
            AppendFormat(report,
                "not confirmed; retained %u/%d contiguous mismatches\r\n",
                static_cast<unsigned>(gameplayRunCount),
                kConfirmMismatchFrames);
        }
        for (size_t i = 0; i < gameplayRunCount; ++i)
        {
            AppendCompareEntry(report, gameplayRunCopy[i]);
        }
        AppendText(report,
            "\r\nSee frames.txt for memory dumps of the recorded frames around the divergence.\r\n"
            "Compare frames.txt from both sides to find the first differing bytes.\r\n");
        CloseHandle(report);
        requiredArtifactsOk = requiredArtifactsOk && g_dumpWriteOk;
    }
    else
    {
        requiredArtifactsOk = false;
        mod::Log(
            "DESYNC_MONITOR: required report creation failed '%s' err=%lu",
            reportPath.c_str(),
            static_cast<unsigned long>(GetLastError()));
    }

    // Exact learned-mask identity. Equal byte counts alone are insufficient
    // to establish that two peers hashed the same selected-field function.
    const std::string maskPath = dumpDir + "\\change_mask.bin";
    HANDLE maskFile = CreateFileA(
        maskPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (maskFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        const BOOL wrote = WriteFile(
            maskFile,
            maskCopy,
            static_cast<DWORD>(sizeof(maskCopy)),
            &written,
            nullptr);
        CloseHandle(maskFile);
        if (!wrote || written != static_cast<DWORD>(sizeof(maskCopy)))
        {
            requiredArtifactsOk = false;
        }
    }
    else
    {
        requiredArtifactsOk = false;
    }

    // ---- frames.txt ------------------------------------------------------
    const std::string framesPath = dumpDir + "\\frames.txt";
    HANDLE frames = CreateFileA(
        framesPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    g_dumpWriteOk = true;
    if (frames != INVALID_HANDLE_VALUE)
    {
        // Oldest-to-newest frozen pre-window plus the bounded append-only
        // post-window. Continuing play cannot overwrite either onset side.
        for (size_t i = 0; i < kRegionRingDepth; ++i)
        {
            const FrameRecord& rec = regionCopy[(regionNext + i) % kRegionRingDepth];
            if (rec.frame < 0
                || rec.frame < triggerFrame - 32
                || rec.frame > triggerFrame + 32)
            {
                continue;
            }
            AppendFrameRecord(frames, rec, triggerFrame);
        }
        for (size_t i = 0; i < postRegionCount; ++i)
        {
            const FrameRecord& rec = postRegionCopy[i];
            if (rec.frame >= triggerFrame && rec.frame <= triggerFrame + 32)
            {
                AppendFrameRecord(frames, rec, triggerFrame);
            }
        }
        CloseHandle(frames);
        requiredArtifactsOk = requiredArtifactsOk && g_dumpWriteOk;
    }
    else
    {
        requiredArtifactsOk = false;
    }

    // ---- inputs.txt ------------------------------------------------------
    // Wide window of the committed input pairs straight from the session's
    // history vectors: diffing both sides' inputs.txt separates "inputs
    // diverged" (transport bug) from "same inputs, states diverged" (logic
    // desync).  The vectors persist for the whole match, so this can reach
    // much further back than the region ring.
    {
        const uintptr_t session = netplay::bridge::takeover::g_lastValidatedSessionPtr;
        const netplay::bridge::takeover::RevivalAddressProfile* profile =
            netplay::bridge::takeover::g_activeRevival;
        if (session != 0 && profile != nullptr)
        {
            const std::string inputsPath = dumpDir + "\\inputs.txt";
            HANDLE inputs = CreateFileA(
                inputsPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (inputs != INVALID_HANDLE_VALUE)
            {
                const int localLen =
                    ReadHistoryLength(session, profile->sessionOffsetHistoryPrimaryVec);
                const int remoteLen =
                    ReadHistoryLength(session, profile->sessionOffsetHistorySecondaryVec);
                const int maxLen = (localLen < remoteLen) ? localLen : remoteLen;
                int windowBegin = triggerFrame - 192;
                int windowEnd = triggerFrame + 64;
                if (windowBegin < 0) windowBegin = 0;
                if (windowEnd > maxLen) windowEnd = maxLen;
                AppendFormat(inputs,
                    "committed input pairs, frames %d..%d (localLen=%d remoteLen=%d)\r\n",
                    windowBegin, windowEnd - 1, localLen, remoteLen);
                for (int f = windowBegin; f < windowEnd; ++f)
                {
                    uint16_t p1 = 0;
                    uint16_t p2 = 0;
                    const bool ok1 = ReadHistoryInput(
                        session, profile->sessionOffsetHistoryPrimaryVec, f, &p1);
                    const bool ok2 = ReadHistoryInput(
                        session, profile->sessionOffsetHistorySecondaryVec, f, &p2);
                    AppendFormat(inputs,
                        "%6d p1=%04X p2=%04X%s%s\r\n",
                        f,
                        static_cast<unsigned>(p1),
                        static_cast<unsigned>(p2),
                        (!ok1 || !ok2) ? " [read-failed]" : "",
                        f == triggerFrame ? "  <== forensic trigger frame" : "");
                }
                CloseHandle(inputs);
            }
        }
    }

    // ---- log copies ------------------------------------------------------
    CopyLogInto(dumpDir, gameDir + "\\EfzRevival.ini", "EfzRevival.ini");
    CopyLogInto(dumpDir, gameDir + "\\logEfz.txt", "logEfz.txt");
    CopyLogInto(dumpDir, gameDir + "\\logNet.txt", "logNet.txt");
    CopyLogInto(dumpDir, gameDir + "\\logDdraw.txt", "logDdraw.txt");
    CopyLogInto(dumpDir, dllDir + "\\logs\\native_host\\logEfz.txt", "native_host_logEfz.txt");
    CopyLogInto(dumpDir, dllDir + "\\logs\\native_host\\logNet.txt", "native_host_logNet.txt");
    CopyLogInto(dumpDir, dllDir + "\\logs\\efz_netplay_mod.log", "efz_netplay_mod.log");

    if (!requiredArtifactsOk)
    {
        mod::Log(
            "DESYNC_MONITOR: dump incomplete '%s'; required artifact write "
            "failed",
            dumpDir.c_str());
        return false;
    }
    mod::Log("DESYNC_MONITOR: dump complete '%s'", dumpDir.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Worker thread: negotiate a symmetric future start, receive peer checksums,
// and compare them. Disk output is deferred until session teardown.
// ---------------------------------------------------------------------------

DWORD WINAPI WorkerThreadProc(LPVOID)
{
    while (InterlockedCompareExchange(&g_workerStop, 0, 0) == 0)
    {
        PumpOutgoingSamples();
        bool pendingWindowCaptured = false;
        EnterCriticalSection(&g_lock);
        if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
        {
            pendingWindowCaptured = DrainComparableSamplesLocked();
        }
        LeaveCriticalSection(&g_lock);
        if (pendingWindowCaptured)
        {
            mod::Log(
                "DESYNC_MONITOR: contiguous selected-gameplay window captured; "
                "disk dump deferred until session end");
        }
        const SOCKET sock = g_socket;
        if (sock == INVALID_SOCKET)
        {
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval timeout = {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR)
        {
            break;
        }
        if (selected == 0)
        {
            const DWORD now = GetTickCount();
            EnterCriticalSection(&g_lock);
            if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0
                && now - g_lastControlSendTick >= 500u)
            {
                const int startFrame = static_cast<int>(
                    InterlockedCompareExchange(&g_captureStartFrame, -1, -1));
                if (g_role == 1)
                {
                    if (g_handshakePhase == 0)
                        SendControlPacket(PacketKind::Hello, -1);
                    else if (g_handshakePhase == 1)
                        SendControlPacket(PacketKind::Ready, startFrame);
                    else if (g_handshakePhase == 2)
                        SendControlPacket(PacketKind::StartAck, startFrame);
                    else if (g_handshakePhase == 4)
                        SendControlPacket(PacketKind::Abort, startFrame);
                }
                else
                {
                    if (g_handshakePhase == 1)
                        SendControlPacket(PacketKind::HelloAck, -1);
                    else if (g_handshakePhase == 2)
                        SendControlPacket(PacketKind::Start, startFrame);
                    else if (g_handshakePhase == 4)
                        SendControlPacket(PacketKind::Abort, startFrame);
                }
            }
            if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0
                && !g_peerSeen
                && !g_peerAbsenceLogged
                && g_sessionStartTick != 0
                && now - g_sessionStartTick > 15000u)
            {
                g_peerAbsenceLogged = true;
                mod::Log(
                    "DESYNC_MONITOR: no compatible peer handshake after 15s; "
                    "recorder remained idle (normal gameplay path unchanged)");
            }
            LeaveCriticalSection(&g_lock);
            continue;
        }

        WirePacket packet = {};
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        const int received = recvfrom(
            sock,
            reinterpret_cast<char*>(&packet),
            static_cast<int>(sizeof(packet)),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen);
        if (received == SOCKET_ERROR)
        {
            if (InterlockedCompareExchange(&g_workerStop, 0, 0) != 0)
            {
                break;
            }
            continue;
        }
        if (received != static_cast<int>(sizeof(packet))
            || packet.magic != kPacketMagic
            || packet.version != kPacketVersion
            || packet.schemaId != WireSchemaIdentity()
            || packet.role > 1
            || packet.role == static_cast<uint8_t>(g_role))
        {
            continue;
        }

        bool confirmedNow = false;
        EnterCriticalSection(&g_lock);
        if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
        {
            const PacketKind kind = static_cast<PacketKind>(packet.kind);

            // The first joiner HELLO creates the nonce/endpoint. Duplicate
            // control packets are response-only: no accepted transition is
            // ever rewound and an unrelated nonce cannot replace the peer.
            if (kind == PacketKind::Hello
                && g_role == 0
                && packet.role == 1
                && packet.sessionNonce != 0)
            {
                if (g_sessionNonce == 0 && g_handshakePhase == 0)
                {
                    g_sessionNonce = packet.sessionNonce;
                    g_peerEndpoint = from;
                    InterlockedExchange(&g_peerEndpointValid, 1);
                    g_peerSeen = true;
                    g_handshakePhase = 1;
                    mod::Log(
                        "DESYNC_MONITOR: compatible peer HELLO nonce=0x%08lX",
                        static_cast<unsigned long>(g_sessionNonce));
                }
                if (packet.sessionNonce == g_sessionNonce)
                {
                    const int chosenStart = static_cast<int>(
                        InterlockedCompareExchange(&g_captureStartFrame, 0, 0));
                    if (g_handshakePhase == 1)
                        SendControlPacket(PacketKind::HelloAck, -1);
                    else if (g_handshakePhase == 2 || g_handshakePhase == 3)
                        SendControlPacket(PacketKind::Start, chosenStart);
                    else if (g_handshakePhase == 4)
                        SendControlPacket(PacketKind::Abort, chosenStart);
                }
            }
            else if (packet.sessionNonce == g_sessionNonce
                && g_sessionNonce != 0)
            {
                if (!g_peerSeen)
                {
                    g_peerSeen = true;
                    mod::Log(
                        "DESYNC_MONITOR: compatible peer side-channel established role=%u",
                        static_cast<unsigned>(packet.role));
                }

                if (kind == PacketKind::Abort)
                {
                    if (g_handshakePhase != 4)
                    {
                        mod::Log(
                            "DESYNC_MONITOR: peer aborted tracer handshake; "
                            "gameplay continues without capture");
                    }
                    g_handshakePhase = 4;
                    InterlockedExchange(&g_captureArmed, 0);
                }
                else if (kind == PacketKind::HelloAck && g_role == 1
                    && g_handshakePhase == 0)
                {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int startFrame =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeLeadFrames;
                    InterlockedExchange(&g_captureStartFrame, startFrame);
                    g_handshakePhase = 1;
                    SendControlPacket(PacketKind::Ready, startFrame);
                }
                else if (kind == PacketKind::HelloAck && g_role == 1
                    && g_handshakePhase == 1)
                {
                    SendControlPacket(
                        PacketKind::Ready,
                        static_cast<int>(InterlockedCompareExchange(
                            &g_captureStartFrame, 0, 0)));
                }
                else if (kind == PacketKind::Ready && g_role == 0
                    && g_handshakePhase == 1)
                {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int minimumStart =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeLeadFrames;
                    const int startFrame =
                        packet.startFrame > minimumStart
                            ? packet.startFrame
                            : minimumStart;
                    InterlockedExchange(&g_captureStartFrame, startFrame);
                    InterlockedExchange(&g_captureArmed, 0);
                    g_handshakePhase = 2;
                    SendControlPacket(PacketKind::Start, startFrame);
                    mod::Log(
                        "DESYNC_MONITOR: symmetric capture scheduled at frame %d nonce=0x%08lX",
                        startFrame,
                        static_cast<unsigned long>(g_sessionNonce));
                }
                else if (kind == PacketKind::Ready && g_role == 0
                    && g_handshakePhase == 2)
                {
                    SendControlPacket(
                        PacketKind::Start,
                        static_cast<int>(InterlockedCompareExchange(
                            &g_captureStartFrame, 0, 0)));
                }
                else if (kind == PacketKind::Start && g_role == 1
                    && (g_handshakePhase == 1 || g_handshakePhase == 2)
                    && packet.startFrame >= 0)
                {
                    const int chosenStart = static_cast<int>(
                        InterlockedCompareExchange(&g_captureStartFrame, 0, 0));
                    if (g_handshakePhase == 2 && packet.startFrame == chosenStart)
                    {
                        // Idempotent retry after an ACK was lost. Do not apply
                        // the future-frame test again as the agreed frame
                        // naturally approaches while capture is armed.
                        SendControlPacket(PacketKind::StartAck, packet.startFrame);
                    }
                    else if (g_handshakePhase == 2)
                    {
                        g_handshakePhase = 4;
                        InterlockedExchange(&g_captureArmed, 0);
                        SendControlPacket(PacketKind::Abort, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: tracer handshake aborted; peer changed "
                            "accepted start frame %d -> %d",
                            chosenStart,
                            packet.startFrame);
                    }
                    else
                    {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int minimumSafe =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeSafetyFrames;
                    if (packet.startFrame < minimumSafe)
                    {
                        g_handshakePhase = 4;
                        InterlockedExchange(&g_captureArmed, 0);
                        SendControlPacket(PacketKind::Abort, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: tracer handshake aborted; start frame %d "
                            "arrived too late (minimum safe %d)",
                            packet.startFrame,
                            minimumSafe);
                    }
                    else
                    {
                        InterlockedExchange(&g_captureStartFrame, packet.startFrame);
                        InterlockedExchange(&g_captureArmed, 1);
                        g_handshakePhase = 2;
                        SendControlPacket(PacketKind::StartAck, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: symmetric capture accepted at frame %d nonce=0x%08lX",
                            packet.startFrame,
                            static_cast<unsigned long>(g_sessionNonce));
                    }
                    }
                }
                else if (kind == PacketKind::StartAck && g_role == 0
                    && g_handshakePhase == 2
                    && packet.startFrame ==
                        InterlockedCompareExchange(&g_captureStartFrame, 0, 0))
                {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int minimumSafe =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeSafetyFrames;
                    if (packet.startFrame < minimumSafe)
                    {
                        g_handshakePhase = 4;
                        InterlockedExchange(&g_captureArmed, 0);
                        SendControlPacket(PacketKind::Abort, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: tracer handshake aborted; ACK for frame %d "
                            "arrived too late (minimum safe %d)",
                            packet.startFrame,
                            minimumSafe);
                    }
                    else
                    {
                        InterlockedExchange(&g_captureArmed, 1);
                        g_handshakePhase = 3;
                        mod::Log("DESYNC_MONITOR: capture start acknowledged by peer");
                    }
                }
                else if (kind == PacketKind::Samples
                    && InterlockedCompareExchange(&g_captureArmed, 0, 0) != 0
                    && packet.count > 0
                    && packet.count <= kSamplesPerPacket)
                {
                    const uint16_t localEpoch = static_cast<uint16_t>(
                        InterlockedCompareExchange(&g_battleEpoch, 0, 0));
                    if (packet.battleEpoch != localEpoch)
                    {
                        const int16_t epochDelta = static_cast<int16_t>(
                            static_cast<uint16_t>(packet.battleEpoch - localEpoch));
                        if (!g_epochMismatchLogged)
                        {
                            g_epochMismatchLogged = true;
                            mod::Log(
                                "DESYNC_MONITOR: capture-epoch mismatch local=%u "
                                "remote=%u relation=%s; no cross-battle samples "
                                "compared",
                                static_cast<unsigned>(localEpoch),
                                static_cast<unsigned>(packet.battleEpoch),
                                epochDelta < 0 ? "stale" : "future");
                        }
                        if (epochDelta > 0)
                        {
                            g_handshakePhase = 4;
                            InterlockedExchange(&g_captureArmed, 0);
                            // This is already the worker thread and the socket
                            // is nonblocking, so notify the peer immediately;
                            // continuous incoming samples cannot starve Abort.
                            SendControlPacket(
                                PacketKind::Abort,
                                static_cast<int>(InterlockedCompareExchange(
                                    &g_captureStartFrame, -1, -1)));
                        }
                    }
                    else
                    {
                        for (int i = 0; i < packet.count; ++i)
                        {
                            ChecksumSample& slot = g_remoteRing[g_remoteRingNext];
                            g_remoteRingNext = (g_remoteRingNext + 1) % kSampleRingDepth;
                            slot.frame = packet.samples[i].frame;
                            slot.checksum = packet.samples[i].checksum;
                            slot.effectHash = packet.samples[i].effectHash;
                            slot.rngState = packet.samples[i].rngState;
                            slot.maskHash = packet.maskHash;
                            slot.maskByteCount = packet.maskByteCount;
                            if (slot.frame > g_highestRemoteFrame)
                            {
                                g_highestRemoteFrame = slot.frame;
                            }
                        }
                        confirmedNow |= DrainComparableSamplesLocked();
                    }
                }

                // A game-thread capture fault can move the tracer to Abort
                // while peer Samples keep the socket continuously readable.
                // Retry control here as well as in the select-timeout path so
                // incoming traffic cannot starve the peer notification.
                const DWORD controlNow = GetTickCount();
                if (g_handshakePhase == 4
                    && controlNow - g_lastControlSendTick >= 500u)
                {
                    SendControlPacket(
                        PacketKind::Abort,
                        static_cast<int>(InterlockedCompareExchange(
                            &g_captureStartFrame, -1, -1)));
                }
            }
        }
        LeaveCriticalSection(&g_lock);

        if (confirmedNow)
        {
            mod::Log(
                "DESYNC_MONITOR: contiguous gameplay mismatch window captured; "
                "disk dump deferred until session end");
        }
    }
    return 0;
}

bool StopWorkerAndChannel()
{
    InterlockedExchange(&g_workerStop, 1);
    CloseSideChannel();
    if (g_workerThread != nullptr)
    {
        const DWORD waitResult = WaitForSingleObject(g_workerThread, 3000);
        if (waitResult != WAIT_OBJECT_0)
        {
            mod::Log(
                "DESYNC_MONITOR: worker did not stop safely result=%lu; "
                "handle retained and restart refused",
                static_cast<unsigned long>(waitResult));
            return false;
        }
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    return true;
}

void ResetMaskLocked(const char* reason)
{
    std::memset(g_changeMask, 0, sizeof(g_changeMask));
    g_prevRegionValid = false;
    g_maskSamples = 0;
    g_maskFrozen = false;
    g_maskByteCount = 0;
    g_maskHash = 0;
    g_maskMismatchLogged = false;
    if (reason != nullptr)
    {
        mod::Log("DESYNC_MONITOR: change-mask calibration restarted (%s)", reason);
    }
}

void ResetBattleCaptureLocked(const char* reason, bool incrementEpoch)
{
    for (size_t i = 0; i < kRegionRingDepth; ++i)
    {
        g_regionRing[i].frame = -1;
    }
    for (size_t i = 0; i < kSampleRingDepth; ++i)
    {
        g_localRing[i].frame = -1;
        g_remoteRing[i].frame = -1;
    }
    for (size_t i = 0; i < kCompareLogDepth; ++i)
    {
        g_compareLog[i].frame = -1;
    }
    for (size_t i = 0; i < kPostEvidenceDepth; ++i)
    {
        g_postEvidenceFrames[i].frame = -1;
        g_postEvidenceCompare[i].frame = -1;
    }
    for (size_t i = 0; i < kGameplayRunDepth; ++i)
    {
        g_gameplayMismatchRun[i].frame = -1;
    }
    g_regionRingNext = 0;
    g_localRingNext = 0;
    g_remoteRingNext = 0;
    g_compareLogNext = 0;
    g_postEvidenceFrameCount = 0;
    g_postEvidenceCompareCount = 0;
    g_gameplayMismatchRunCount = 0;
    g_gameplayMismatchRunFrozen = false;
    g_evidenceTriggered = false;
    g_evidenceTriggerFrame = -1;
    g_evidenceBattleEpoch = 0;
    g_evidenceTriggerLayer[0] = '\0';
    g_evidenceRegionNext = 0;
    g_evidenceCompareNext = 0;
    g_evidenceMaskByteCount = 0;
    g_evidenceMaskHash = 0;
    std::memset(g_evidenceMask, 0, sizeof(g_evidenceMask));
    g_mismatchStreak = 0;
    g_firstMismatchFrame = -1;
    g_lastMismatchFrame = -1;
    g_lastComparedFrame = -1;
    g_nextCompareFrame = -1;
    g_highestLocalFrame = -1;
    g_highestRemoteFrame = -1;
    g_epochMismatchLogged = false;
    g_effectLayerRun = 0;
    g_effectLayerFirstFrame = -1;
    g_rngLayerRun = 0;
    g_rngLayerFirstFrame = -1;
    g_confirmedFrame = -1;
    g_sampleCount = 0;
    g_sentSampleCount = 0;
    g_leftBattleSinceLastSample = false;
    g_lastSampledFrame = -1;
    g_validatedEffectGameSys = 0;
    g_effectArenaReadable = false;
    g_validatedRngAddress = 0;
    g_rngAddressReadable = false;
    ResetMaskLocked(nullptr);
    InterlockedExchange(&g_desyncConfirmed, 0);
    if (incrementEpoch)
    {
        const LONG epoch = InterlockedIncrement(&g_battleEpoch);
        mod::Log(
            "DESYNC_MONITOR: battle capture epoch=%ld reset reason=%s",
            static_cast<long>(epoch),
            reason != nullptr ? reason : "battle_boundary");
    }
}

void ResetRingsLocked()
{
    InterlockedExchange(&g_battleEpoch, 0);
    ResetBattleCaptureLocked(nullptr, false);
    g_peerSeen = false;
    g_peerAbsenceLogged = false;
    g_handshakePhase = 0;
    g_lastControlSendTick = 0;
    g_inBattle = false;
    InterlockedExchange(&g_latestFrame, -1);
    InterlockedExchange(&g_captureStartFrame, -1);
    InterlockedExchange(&g_captureArmed, 0);
}

// Freeze the worker before touching evidence. A timed-out stop keeps the
// handle, rings, and pending-dump latch intact so Shutdown or the next session
// start can retry instead of silently erasing the capture.
bool TryFinalizeEndedCapture()
{
    if (!StopWorkerAndChannel())
    {
        return false;
    }

    bool shouldDump = false;
    EnterCriticalSection(&g_lock);
    shouldDump = g_dumpPending && g_evidenceTriggered && !g_dumped;
    LeaveCriticalSection(&g_lock);

    if (shouldDump)
    {
        if (!WriteDesyncDump())
        {
            mod::Log(
                "DESYNC_MONITOR: forensic dump failed; evidence remains "
                "latched for a later retry");
            return false;
        }
        EnterCriticalSection(&g_lock);
        g_dumped = true;
        g_dumpPending = false;
        LeaveCriticalSection(&g_lock);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void NotifySessionStarted(
    int netplayRole,
    const char* address,
    uint16_t port,
    const char* nickname)
{
    const bool tracingEnabled =
        netplay::mod_settings::IsDesyncDetectionEnabled();
    const bool comparableRole = netplayRole == 0 || netplayRole == 1;
    if ((!tracingEnabled || !comparableRole)
        && g_lockInited
        && InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
    {
        NotifySessionEnded("replaced_by_untraced_session");
    }
    if (!tracingEnabled)
    {
        return;
    }
    // Host (0) and Join (1) only; spectators (2) and spectate-redirect joins
    // (3) receive the state feed with their own frame numbering and cannot
    // be compared against the players.
    if (!comparableRole)
    {
        return;
    }

    EnsureLock();
    EnterCriticalSection(&g_lock);
    const bool replacedActiveSession =
        InterlockedExchange(&g_sessionActive, 0) != 0;
    if (replacedActiveSession)
    {
        InterlockedExchange(&g_captureArmed, 0);
        if (g_evidenceTriggered && !g_dumped)
        {
            g_dumpPending = true;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (replacedActiveSession)
    {
        mod::Log(
            "DESYNC_MONITOR: session start replaced an active capture; "
            "old evidence will be finalized before reset");
    }
    if (!TryFinalizeEndedCapture())
    {
        mod::Log(
            "DESYNC_MONITOR: session start skipped because the previous "
            "worker/evidence capture is not finalized");
        return;
    }

    EnterCriticalSection(&g_lock);
    ResetRingsLocked();
    InterlockedExchange(&g_sessionActive, 1);
    g_dumped = false;
    g_dumpPending = false;
    g_role = (netplayRole == 0) ? 0 : 1;
    g_hostPort = port;
    g_sessionStartTick = GetTickCount();
    if (g_role == 1)
    {
        g_sessionNonce =
            (static_cast<uint32_t>(GetCurrentProcessId()) * 2654435761u)
            ^ g_sessionStartTick
            ^ (static_cast<uint32_t>(port) << 16)
            ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_sessionNonce));
        if (g_sessionNonce == 0)
            g_sessionNonce = 1;
    }
    else
    {
        g_sessionNonce = 0;
    }
    std::snprintf(g_peerAddress, sizeof(g_peerAddress), "%s",
        address != nullptr ? address : "");
    std::snprintf(g_nickname, sizeof(g_nickname), "%s",
        nickname != nullptr ? nickname : "");
    LeaveCriticalSection(&g_lock);

    InterlockedExchange(&g_workerStop, 0);
    if (OpenSideChannel())
    {
        g_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
        if (g_workerThread == nullptr)
        {
            mod::Log("DESYNC_MONITOR: worker thread creation failed");
            CloseSideChannel();
        }
    }
    mod::Log(
        "DESYNC_MONITOR: session started role=%d port=%u peer='%s' "
        "recorder=waiting_for_peer channel=%s",
        g_role,
        static_cast<unsigned>(g_hostPort),
        g_peerAddress,
        g_workerThread != nullptr ? "on" : "off");
}

void NotifySessionEnded(const char* reason)
{
    if (!g_lockInited)
    {
        return;
    }
    bool wasActive = false;
    bool mustFinalize = false;
    EnterCriticalSection(&g_lock);
    wasActive = InterlockedExchange(&g_sessionActive, 0) != 0;
    InterlockedExchange(&g_captureArmed, 0);
    if (g_evidenceTriggered && !g_dumped)
    {
        g_dumpPending = true;
    }
    mustFinalize = wasActive || g_dumpPending || g_workerThread != nullptr;
    LeaveCriticalSection(&g_lock);

    if (mustFinalize)
    {
        if (!TryFinalizeEndedCapture() && g_dumpPending)
        {
            mod::Log(
                "DESYNC_MONITOR: forensic dump pending; retry will occur at "
                "shutdown or before the next traced session");
        }
    }
    if (wasActive)
    {
        mod::Log(
            "DESYNC_MONITOR: session ended reason='%s' samples=%u peerSeen=%d",
            reason != nullptr ? reason : "",
            g_sampleCount,
            g_peerSeen ? 1 : 0);
    }
}

void Shutdown()
{
    NotifySessionEnded("shutdown");
}

bool IsSessionTracing()
{
    return g_lockInited
        && InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0;
}

bool IsCaptureArmed()
{
    return IsSessionTracing()
        && InterlockedCompareExchange(&g_captureArmed, 0, 0) != 0;
}

void ObserveFrame(int frame)
{
    if (IsSessionTracing() && frame >= 0)
    {
        InterlockedExchange(&g_latestFrame, frame);
    }
}

void RecordFrameTick(uintptr_t sessionPtr, int frame, int commitFrame)
{
    if (!IsCaptureArmed() || sessionPtr == 0 || frame < 0)
    {
        return;
    }
    InterlockedExchange(&g_latestFrame, frame);
    // Battle screen only - menus don't carry sync-relevant state. This
    // experimental capture is intentionally single-battle: re-entering the
    // battle screen requires a fresh peer handshake rather than letting two
    // locally observed transitions invent different wire identities.
    uint8_t screen = 0xFF;
    if (!netplay::bridge::takeover::SafeReadByte(
            reinterpret_cast<const void*>(kScreenIndexAddr), &screen)
        || screen != 3)
    {
        if (g_inBattle)
        {
            EnterCriticalSection(&g_lock);
            g_leftBattleSinceLastSample = true;
            InterlockedExchange(&g_captureArmed, 0);
            g_handshakePhase = 4;
            if (g_evidenceTriggered)
            {
                mod::Log(
                    "DESYNC_MONITOR: battle ended after forensic trigger; "
                    "capture sealed until session teardown");
            }
            else
            {
                mod::Log(
                    "DESYNC_MONITOR: battle ended before a comparable "
                    "difference; tracer closed for this session");
            }
            LeaveCriticalSection(&g_lock);
        }
        g_inBattle = false;
        return;
    }

    // Boundary handling above must run even if a transition reset the frame
    // or commit cursor. Only actual battle samples are gated by the negotiated
    // start and the bounded post-tick cursor relationship.
    const LONG captureStart =
        InterlockedCompareExchange(&g_captureStartFrame, -1, -1);
    if (InterlockedCompareExchange(&g_captureArmed, 0, 0) == 0
        || captureStart < 0
        || frame < captureStart)
    {
        return;
    }
    // gmBase+16 is a commit cursor, not uniformly an inclusive committed-
    // frame number: observed Revival builds normally report cursor==frame+1,
    // while transition/rollback phases may expose frame or frame-1. These
    // samples are attribution evidence, not proof of complete synchronized
    // state at the native snapshot boundary.
    const int commitCursorDelta = commitFrame - frame;
    if (commitFrame < 0 || commitCursorDelta < -1 || commitCursorDelta > 1)
    {
        return;
    }

    // One logical frame is sampled once even if the outer hook is called
    // repeatedly during a zero-simulation batch. A same-screen regression is
    // a rollback/resimulation boundary, not a peer-symmetric battle identity:
    // abort the optional tracer instead of advancing one peer's wire epoch.
    const bool newBattle = !g_inBattle;
    const bool frameRegression =
        g_inBattle && g_lastSampledFrame >= 0 && frame < g_lastSampledFrame;
    if (!newBattle && !frameRegression && frame == g_lastSampledFrame)
    {
        return;
    }
    g_inBattle = true;
    if (frameRegression)
    {
        EnterCriticalSection(&g_lock);
        if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
        {
            InterlockedExchange(&g_captureArmed, 0);
            if (g_evidenceTriggered)
            {
                g_handshakePhase = 4;
                mod::Log(
                    "DESYNC_MONITOR: capture sealed at same-screen frame "
                    "regression %d -> %d to preserve forensic onset "
                    "epoch=%ld frame=%d",
                    g_lastSampledFrame,
                    frame,
                    static_cast<long>(g_evidenceBattleEpoch),
                    g_evidenceTriggerFrame);
            }
            else
            {
                g_handshakePhase = 4;
                mod::Log(
                    "DESYNC_MONITOR: tracer aborted at same-screen frame "
                    "regression %d -> %d; capture epoch and rings preserved",
                    g_lastSampledFrame,
                    frame);
            }
        }
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (newBattle)
    {
        bool abortCapture = false;
        EnterCriticalSection(&g_lock);
        if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
        {
            if (g_leftBattleSinceLastSample)
            {
                abortCapture = true;
                InterlockedExchange(&g_captureArmed, 0);
                if (g_evidenceTriggered)
                {
                    g_handshakePhase = 4;
                    mod::Log(
                        "DESYNC_MONITOR: capture sealed before battle re-entry "
                        "to preserve forensic onset epoch=%ld frame=%d",
                        static_cast<long>(g_evidenceBattleEpoch),
                        g_evidenceTriggerFrame);
                }
                else
                {
                    g_handshakePhase = 4;
                    mod::Log(
                        "DESYNC_MONITOR: tracer aborted on battle re-entry; "
                        "a fresh session handshake is required");
                }
            }
            else
            {
                ResetBattleCaptureLocked("battle_entry", true);
            }
        }
        LeaveCriticalSection(&g_lock);
        if (abortCapture)
            return;
    }

    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    if (profile == nullptr)
    {
        return;
    }

    // Resolve region sources.
    uintptr_t battleScreen = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(kScreenTableAddr + 4u * 3u), &battleScreen);
    uintptr_t gameSys = 0;
    uintptr_t charP1 = 0;
    uintptr_t charP2 = 0;
    if (battleScreen != 0)
    {
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(battleScreen + kOffsetGameSystem), &gameSys);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(battleScreen + kOffsetBattleP1Char), &charP1);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(battleScreen + kOffsetBattleP2Char), &charP2);
    }

    // Copy regions into a stack-independent staging record (static: single
    // writer, the game thread).
    static FrameRecord staging;
    staging.frame = frame;
    staging.validMask = 0;
    staging.srcGameSys = static_cast<uint32_t>(gameSys != 0 ? gameSys + kGameSysSliceOffset : 0);
    staging.srcBattle = static_cast<uint32_t>(battleScreen != 0 ? battleScreen + kBattleSliceOffset : 0);
    staging.srcCharP1 = static_cast<uint32_t>(charP1);
    staging.srcCharP2 = static_cast<uint32_t>(charP2);

    uint8_t* cursor = staging.bytes;
    if (CopyRegion(staging.srcGameSys, cursor, kGameSysSliceSize))
    {
        staging.validMask |= 1;
    }
    cursor += kGameSysSliceSize;
    if (CopyRegion(staging.srcBattle, cursor, kBattleSliceSize))
    {
        staging.validMask |= 2;
    }
    cursor += kBattleSliceSize;
    if (CopyRegion(staging.srcCharP1, cursor, kCharSliceSize))
    {
        staging.validMask |= 4;
    }
    cursor += kCharSliceSize;
    if (CopyRegion(staging.srcCharP2, cursor, kCharSliceSize))
    {
        staging.validMask |= 8;
    }

    // The character objects are the sync-critical payload; without them the
    // checksum would compare mostly-static data and miss real divergence.
    if ((staging.validMask & 12) != 12)
    {
        return;
    }

    (void)ReadHistoryInput(
        sessionPtr, profile->sessionOffsetHistoryPrimaryVec, frame, &staging.p1Input);
    (void)ReadHistoryInput(
        sessionPtr, profile->sessionOffsetHistorySecondaryVec, frame, &staging.p2Input);

    staging.commitFrame = commitFrame;
    staging.syncFeed = -1;
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            sessionPtr + profile->sessionOffsetGameModeSnapshot + 20),
        &staging.syncFeed);
    staging.localLen =
        ReadHistoryLength(sessionPtr, profile->sessionOffsetHistoryPrimaryVec);
    staging.remoteLen =
        ReadHistoryLength(sessionPtr, profile->sessionOffsetHistorySecondaryVec);
    staging.pingMs = -1;
    if (profile->sessionOffsetPingMs != 0)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + profile->sessionOffsetPingMs),
            &staging.pingMs);
    }

    staging.effectHash = ComputeEffectRingHash(gameSys, &staging);
    staging.rngState = ReadRevivalRngState();

    EnterCriticalSection(&g_lock);
    if (InterlockedCompareExchange(&g_sessionActive, 0, 0) == 0)
    {
        LeaveCriticalSection(&g_lock);
        return;
    }

    g_lastSampledFrame = frame;

    if (!g_maskFrozen)
    {
        // Calibration: accumulate the set of bytes that mutate frame to
        // frame.  No checksums are recorded or exchanged until the mask is
        // frozen, but the region ring still records for the dumps.
        if (g_prevRegionValid)
        {
            for (size_t i = 0; i < kRecordRegionBytes; ++i)
            {
                if (staging.bytes[i] != g_prevRegionBytes[i])
                {
                    g_changeMask[i] = 1;
                }
            }
            ++g_maskSamples;
        }
        std::memcpy(g_prevRegionBytes, staging.bytes, kRecordRegionBytes);
        g_prevRegionValid = true;

        if (g_maskSamples >= kMaskCalibrationSamples)
        {
            g_maskFrozen = true;
            g_maskByteCount = 0;
            for (size_t i = 0; i < kRecordRegionBytes; ++i)
            {
                g_maskByteCount += g_changeMask[i];
            }
            g_maskHash = Fnv1a(
                2166136261u, g_changeMask, sizeof(g_changeMask));
            if (g_maskHash == 0)
                g_maskHash = 1;
            mod::Log(
                "DESYNC_MONITOR: change mask frozen at frame %d "
                "(%u of %u bytes participate, identity=0x%08lX)",
                frame,
                g_maskByteCount,
                static_cast<unsigned>(kRecordRegionBytes),
                static_cast<unsigned long>(g_maskHash));
        }
    }

    uint32_t checksum = 2166136261u;
    if (g_maskFrozen)
    {
        for (size_t i = 0; i < kRecordRegionBytes; ++i)
        {
            if (g_changeMask[i] != 0)
            {
                checksum ^= staging.bytes[i];
                checksum *= 16777619u;
            }
        }
        checksum = Fnv1a(checksum, &staging.p1Input, sizeof(staging.p1Input));
        checksum = Fnv1a(checksum, &staging.p2Input, sizeof(staging.p2Input));
    }
    staging.checksum = checksum;

    if (!g_evidenceTriggered)
    {
        std::memcpy(&g_regionRing[g_regionRingNext], &staging, sizeof(staging));
        g_regionRingNext = (g_regionRingNext + 1) % kRegionRingDepth;
    }
    else if (g_postEvidenceFrameCount < kPostEvidenceDepth)
    {
        std::memcpy(
            &g_postEvidenceFrames[g_postEvidenceFrameCount++],
            &staging,
            sizeof(staging));
    }

    if (g_maskFrozen)
    {
        ChecksumSample& sample = g_localRing[g_localRingNext];
        g_localRingNext = (g_localRingNext + 1) % kSampleRingDepth;
        sample.frame = frame;
        sample.checksum = checksum;
        sample.effectHash = staging.effectHash;
        sample.rngState = staging.rngState;
        sample.maskHash = g_maskHash;
        sample.maskByteCount = static_cast<uint16_t>(g_maskByteCount);
        if (frame > g_highestLocalFrame)
        {
            g_highestLocalFrame = frame;
        }

        ++g_sampleCount;
    }
    LeaveCriticalSection(&g_lock);
}

} // namespace netplay::bridge::desync_monitor
