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

// Ring depths. The region ring retains enough history for a forensic trigger
// plus post-trigger frames without stopping the recorder.
constexpr size_t kRegionRingDepth = 128;
constexpr size_t kSampleRingDepth = 512;
constexpr size_t kCompareLogDepth = 64;

// This threshold schedules a forensic dump; it is NOT a gameplay disconnect
// policy. Native RNG-only inequalities may be short, may reconverge, or may
// persist while the other printed Sync fields still agree.
constexpr int kGameplayForensicMismatchFrames = 4;
constexpr int kForensicPostTriggerFrames = 16;

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
constexpr uintptr_t kEffectRecordBase = 10760;        // + 112*i per-slot record
constexpr uintptr_t kEffectRecordStride = 112;
// Record-relative offsets of the deterministic per-particle fields.
constexpr uintptr_t kEffectFieldBehaviorId = 0;   // WORD
constexpr uintptr_t kEffectFieldAnimFrame = 2;    // WORD
constexpr uintptr_t kEffectFieldAnimTick = 4;     // WORD
constexpr uintptr_t kEffectFieldPosX = 24;        // double
constexpr uintptr_t kEffectFieldPosY = 32;        // double
constexpr uintptr_t kEffectFieldVelX = 40;        // double
constexpr uintptr_t kEffectFieldVelY = 48;        // double
constexpr uint16_t kEffectBehaviorType47 = 47;
constexpr size_t kTrackedEffectTraceSlots = 32;

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
// v3 adds exact checksum-schema/mask identity, chronological packet sequence,
// explicit sample validity, and a persistent forensic-trigger announcement.
// Mixed monitor versions remain passive and never affect the match.
constexpr uint8_t kPacketVersion = 3;
// Bump whenever the recorded region order/offsets or committed-input hashing
// changes. The learned mask hash is checked independently.
constexpr uint32_t kChecksumSchemaId = 0x00030001u;
constexpr int kSamplesPerPacket = 4;
constexpr int kSendEverySamples = 1;

constexpr uint8_t kWireFlagMaskReady = 0x01;
constexpr uint8_t kWireFlagForensicTriggered = 0x02;

constexpr uint8_t kSampleValidGameplay = 0x01;
constexpr uint8_t kSampleValidEffect = 0x02;
constexpr uint8_t kSampleValidRng = 0x04;

#pragma pack(push, 1)
struct WirePacket
{
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint8_t role;
    uint8_t flags;
    uint32_t sequence;
    uint32_t checksumSchemaId;
    uint32_t maskHash;
    uint16_t maskByteCount;
    uint16_t recordRegionBytes;
    int32_t forensicFrame;
    struct
    {
        int32_t frame;
        uint32_t checksum;
        uint32_t effectHash;
        int32_t rngState;
        uint8_t validFlags;
        uint8_t reserved[3];
    } samples[kSamplesPerPacket];
};
#pragma pack(pop)

static_assert(sizeof(WirePacket) < 512, "desync side-channel packet must stay compact");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct EffectTraceEntry
{
    uint16_t slot = 0;
    uint16_t behaviorId = 0;
    uint16_t animFrame = 0;
    uint16_t animTick = 0;
    uint32_t generation = 0;
    int activationFrame = -1;
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
    uint8_t telemetryValid = 0;
    uint16_t effectAllocCursor = 0xFFFF;
    uint16_t effectProcCursor = 0xFFFF;
    uint16_t effectActiveCount = 0;
    uint8_t effectTraceCount = 0;
    uint8_t effectTraceTruncated = 0;
    EffectTraceEntry effectTrace[kTrackedEffectTraceSlots];
    uint8_t bytes[kRecordRegionBytes];
};

struct ChecksumSample
{
    int frame = -1;
    uint32_t checksum = 0;
    uint32_t effectHash = 0;
    int32_t rngState = -1;
    uint8_t validFlags = 0;
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
    uint8_t localValidFlags = 0;
    uint8_t remoteValidFlags = 0;
    bool gameplayComparable = false;
};

CRITICAL_SECTION g_lock;
bool g_lockInited = false;

bool g_sessionActive = false;
bool g_dumped = false;
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

int g_mismatchStreak = 0;
int g_firstMismatchFrame = -1;
int g_lastMismatchFrame = -1;
int g_lastComparedFrame = -1;
volatile LONG g_desyncConfirmed = 0;
int g_confirmedFrame = -1;
int g_dumpReadyFrame = -1;
char g_forensicReason[48] = {};
char g_forensicOrigin[16] = {};

// Layer-attribution runs (diagnostic only; confirmation is driven solely by
// the gameplay checksum).  "Begins at frame N" / "reconverged after M"
// pairs in the log localize which state layer diverged first.
int g_effectLayerRun = 0;
int g_effectLayerFirstFrame = -1;
int g_rngLayerRun = 0;
int g_rngLayerFirstFrame = -1;

unsigned g_sampleCount = 0;
bool g_peerSeen = false;
DWORD g_sessionStartTick = 0;
bool g_peerAbsenceLogged = false;

// Change-mask calibration state (single writer: the game thread).
uint8_t g_changeMask[kRecordRegionBytes];
uint8_t g_prevRegionBytes[kRecordRegionBytes];
bool g_prevRegionValid = false;
unsigned g_maskSamples = 0;
bool g_maskFrozen = false;
unsigned g_maskByteCount = 0;
uint32_t g_maskHash = 0;
bool g_peerSchemaSeen = false;
bool g_peerSchemaCompatible = false;
bool g_schemaMismatchLogged = false;
uint32_t g_peerSchemaId = 0;
uint32_t g_peerMaskHash = 0;
uint16_t g_peerMaskByteCount = 0;
bool g_leftBattleSinceLastSample = false;
int g_lastSampledFrame = -1;

uint32_t g_sendSequence = 0;
uint32_t g_lastPeerSequence = 0;
bool g_lastPeerSequenceValid = false;

// Memory validation is cached per stable battle object/profile. The hot path
// then performs direct fixed-range reads without VirtualQuery per effect slot.
uintptr_t g_cachedEffectGameSys = 0;
bool g_cachedEffectRangeValid = false;
uintptr_t g_cachedRngAddress = 0;
bool g_cachedRngAddressValid = false;
bool g_effectSlotWasActive[kEffectRingSlots] = {};
uint16_t g_effectSlotLastBehavior[kEffectRingSlots] = {};
uint32_t g_effectSlotGeneration[kEffectRingSlots] = {};
int g_effectSlotActivationFrame[kEffectRingSlots] = {};

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

// Hash the deterministic fields of every active effect slot plus the ring
// cursors.  Equal on healthy peers; the first cross-peer difference marks
// the effect-layer divergence that precedes an RNG-stream split.  Returns 0
// when the ring is unreadable so "no data" never fakes a mismatch.
uint32_t ComputeEffectRingHash(uintptr_t gameSys)
{
    if (gameSys == 0)
    {
        return 0;
    }
    static uint32_t activeFlags[kEffectRingSlots];
    if (!IsReadableRange(
            reinterpret_cast<const void*>(gameSys + kEffectActiveFlagBase),
            sizeof(activeFlags)))
    {
        return 0;
    }
    std::memcpy(
        activeFlags,
        reinterpret_cast<const void*>(gameSys + kEffectActiveFlagBase),
        sizeof(activeFlags));

    uint32_t hash = 2166136261u;
    uint16_t cursors[2] = {0, 0};
    if (IsReadableRange(
            reinterpret_cast<const void*>(gameSys + kEffectAllocCursorOffset), 4))
    {
        std::memcpy(
            cursors,
            reinterpret_cast<const void*>(gameSys + kEffectAllocCursorOffset),
            sizeof(cursors));
    }
    hash = Fnv1a(hash, cursors, sizeof(cursors));

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
        if (!IsReadableRange(
                reinterpret_cast<const void*>(record), kEffectRecordStride))
        {
            continue;
        }
        struct
        {
            uint32_t slot;
            uint16_t behaviorId;
            uint16_t animFrame;
            uint16_t animTick;
            uint16_t pad;
            double x;
            double y;
            double vx;
            double vy;
        } fields = {};
        fields.slot = i;
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
        hash = Fnv1a(hash, &fields, sizeof(fields));
    }
    hash = Fnv1a(hash, &activeCount, sizeof(activeCount));
    return hash;
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
    int state = -1;
    (void)SafeReadInt(
        reinterpret_cast<const void*>(
            reinterpret_cast<uintptr_t>(revival) + profile->rngEngineStateOffset),
        &state);
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

    mod::Log(
        "DESYNC_MONITOR: side channel open role=%d listenPort=%u peer='%s'",
        g_role,
        g_role == 0 ? static_cast<unsigned>(g_hostPort + kSideChannelPortOffset) : 0u,
        g_role == 0 ? "(waiting for joiner)" : g_peerAddress);
    return true;
}

// Send the most recent samples to the peer. Called from the game thread;
// a UDP sendto on a bound datagram socket is a few microseconds.
void SendRecentSamplesLocked()
{
    if (g_socket == INVALID_SOCKET
        || InterlockedCompareExchange(&g_peerEndpointValid, 0, 0) == 0)
    {
        return;
    }

    WirePacket packet = {};
    packet.magic = kPacketMagic;
    packet.version = kPacketVersion;
    packet.role = static_cast<uint8_t>(g_role);

    int count = 0;
    for (int i = 0; i < kSamplesPerPacket; ++i)
    {
        const size_t index =
            (g_localRingNext + kSampleRingDepth - 1 - static_cast<size_t>(i))
            % kSampleRingDepth;
        if (g_localRing[index].frame < 0)
        {
            break;
        }
        packet.samples[count].frame = g_localRing[index].frame;
        packet.samples[count].checksum = g_localRing[index].checksum;
        packet.samples[count].effectHash = g_localRing[index].effectHash;
        packet.samples[count].rngState = g_localRing[index].rngState;
        ++count;
    }
    if (count == 0)
    {
        return;
    }
    packet.count = static_cast<uint8_t>(count);

    (void)sendto(
        g_socket,
        reinterpret_cast<const char*>(&packet),
        static_cast<int>(sizeof(packet)),
        0,
        reinterpret_cast<const sockaddr*>(&g_peerEndpoint),
        sizeof(g_peerEndpoint));
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

// Compare a remote sample against the local ring. Caller holds the lock.
// Returns true when the desync just became confirmed.  Confirmation is
// driven ONLY by the gameplay checksum; the effect-ring hash and RNG engine
// state are compared for layer attribution ("which state diverged first")
// and never terminate anything.
bool CompareRemoteSampleLocked(
    int frame,
    uint32_t remoteSum,
    uint32_t remoteEffectHash,
    int32_t remoteRngState)
{
    const ChecksumSample* local = FindSample(g_localRing, frame);
    if (local == nullptr)
    {
        return false;
    }

    const bool match = (local->checksum == remoteSum);

    CompareEntry& entry = g_compareLog[g_compareLogNext];
    g_compareLogNext = (g_compareLogNext + 1) % kCompareLogDepth;
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
                "DESYNC_MONITOR_LAYER: EFFECT-state divergence begins at frame %d "
                "(gameplay=%s rng=%s) localEff=0x%08lX remoteEff=0x%08lX",
                frame,
                match ? "match" : "MISMATCH",
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
                "DESYNC_MONITOR_LAYER: effect-state reconverged at frame %d "
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
                "DESYNC_MONITOR_LAYER: RNG-state divergence begins at frame %d "
                "(gameplay=%s effects=%s) localRng=%ld remoteRng=%ld",
                frame,
                match ? "match" : "MISMATCH",
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
                "DESYNC_MONITOR_LAYER: RNG-state reconverged at frame %d "
                "(run of %d from frame %d)",
                frame,
                g_rngLayerRun,
                g_rngLayerFirstFrame);
            g_rngLayerRun = 0;
            g_rngLayerFirstFrame = -1;
        }
    }

    if (match)
    {
        if (g_mismatchStreak != 0 && frame >= g_firstMismatchFrame)
        {
            mod::Log(
                "DESYNC_MONITOR: mismatch streak reset by matching frame %d "
                "(streak was %d from frame %d - rollback transient)",
                frame,
                g_mismatchStreak,
                g_firstMismatchFrame);
            g_mismatchStreak = 0;
            g_firstMismatchFrame = -1;
            g_lastMismatchFrame = -1;
        }
        return false;
    }

    if (g_mismatchStreak == 0)
    {
        g_firstMismatchFrame = frame;
        g_lastMismatchFrame = frame;
        g_mismatchStreak = 1;
        mod::Log(
            "DESYNC_MONITOR: checksum mismatch at frame %d local=0x%08lX remote=0x%08lX",
            frame,
            static_cast<unsigned long>(local->checksum),
            static_cast<unsigned long>(remoteSum));
        return false;
    }

    if (frame != g_lastMismatchFrame)
    {
        g_lastMismatchFrame = frame;
        ++g_mismatchStreak;
    }

    if (g_mismatchStreak >= kConfirmMismatchFrames
        && InterlockedExchange(&g_desyncConfirmed, 1) == 0)
    {
        g_confirmedFrame = g_firstMismatchFrame;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

void AppendText(HANDLE file, const char* text)
{
    DWORD written = 0;
    (void)WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
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

// Runs on the worker thread. Snapshots the rings and writes the dump folder.
void WriteDesyncDump()
{
    // Snapshot state under the lock; file I/O happens after release.
    static FrameRecord regionCopy[kRegionRingDepth];
    static CompareEntry compareCopy[kCompareLogDepth];
    int confirmedFrame = -1;
    int firstMismatch = -1;
    unsigned sampleCount = 0;

    EnterCriticalSection(&g_lock);
    std::memcpy(regionCopy, g_regionRing, sizeof(g_regionRing));
    std::memcpy(compareCopy, g_compareLog, sizeof(g_compareLog));
    confirmedFrame = g_confirmedFrame;
    firstMismatch = g_firstMismatchFrame;
    sampleCount = g_sampleCount;
    const unsigned maskBytes = g_maskByteCount;
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
        "%04u%02u%02u_%02u%02u%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

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
        return;
    }

    mod::Log(
        "DESYNC_MONITOR: *** DESYNC CONFIRMED *** frame=%d dumping to '%s'",
        confirmedFrame,
        dumpDir.c_str());

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
    if (report != INVALID_HANDLE_VALUE)
    {
        AppendFormat(report, "EFZ InGameNetplay desync report %s\r\n", stamp);
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
        AppendFormat(report, "first mismatching frame: %d\r\n", firstMismatch);
        AppendFormat(report, "confirmed after %d consecutive mismatching frames\r\n",
            kConfirmMismatchFrames);
        AppendFormat(report, "frames sampled this session: %u\r\n", sampleCount);
        AppendFormat(report,
            "checksum change-mask: %u of %u recorded bytes (must match the peer's report;\r\n"
            "a differing count means calibration diverged - compare frames.txt anyway)\r\n\r\n",
            maskBytes,
            static_cast<unsigned>(kRecordRegionBytes));

        // ---- Bridge status + delay metrics --------------------------------
        {
            const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
            AppendFormat(report,
                "bridge: phase=%s role=%d roleFlag=%d ping=%d rollback=%d port=%u address='%s'\r\n",
                netplay::bridge::PhaseToString(
                    static_cast<netplay::bridge::NetbridgePhase>(status.phase)),
                status.role,
                status.roleFlag,
                status.pingMs,
                status.rollbackFrames,
                static_cast<unsigned>(status.port),
                status.address);
            AppendFormat(report,
                "bridge: p1='%s' p2='%s' helperPid=%lu syncGameMode=%d sessionByte=%d flags4964/65=%d/%d\r\n",
                status.p1Name,
                status.p2Name,
                static_cast<unsigned long>(status.processId),
                status.syncGameMode,
                status.syncSessionByte,
                status.syncGlobalFlag4964,
                status.syncGlobalFlag4965);
            const netplay::bridge::DelayPromptMetrics metrics =
                netplay::bridge::GetDelayPromptMetrics();
            AppendFormat(report,
                "delay prompt: avgPing=%d minPing=%d maxPing=%d recommended=%d range=%d..%d\r\n",
                metrics.averagePingMs,
                metrics.minPingMs,
                metrics.maxPingMs,
                metrics.recommendedDelay,
                metrics.minDelay,
                metrics.maxDelay);
        }

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
            "Recent checksum comparisons (oldest first; MISMATCH marks divergence):\r\n");
        for (size_t i = 0; i < kCompareLogDepth; ++i)
        {
            const CompareEntry& e = compareCopy[(g_compareLogNext + i) % kCompareLogDepth];
            if (e.frame < 0)
            {
                continue;
            }
            AppendFormat(
                report,
                "  frame=%d local=0x%08lX remote=0x%08lX eff=0x%08lX/0x%08lX%s "
                "rng=%ld/%ld%s %s\r\n",
                e.frame,
                static_cast<unsigned long>(e.localSum),
                static_cast<unsigned long>(e.remoteSum),
                static_cast<unsigned long>(e.localEffect),
                static_cast<unsigned long>(e.remoteEffect),
                (e.localEffect != 0 && e.remoteEffect != 0
                 && e.localEffect != e.remoteEffect)
                    ? "(DIFF)"
                    : "",
                static_cast<long>(e.localRng),
                static_cast<long>(e.remoteRng),
                (e.localRng != -1 && e.remoteRng != -1 && e.localRng != e.remoteRng)
                    ? "(DIFF)"
                    : "",
                e.match ? "match" : "MISMATCH");
        }
        AppendText(report,
            "\r\nSee frames.txt for memory dumps of the recorded frames around the divergence.\r\n"
            "Compare frames.txt from both sides to find the first differing bytes.\r\n");
        CloseHandle(report);
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
    if (frames != INVALID_HANDLE_VALUE)
    {
        // Oldest-to-newest pass over the ring; dump frames within
        // [confirmed-16, confirmed+16] so the 10 frames before the desync
        // and the desync frames themselves are all present.
        for (size_t i = 0; i < kRegionRingDepth; ++i)
        {
            const FrameRecord& rec = regionCopy[(g_regionRingNext + i) % kRegionRingDepth];
            if (rec.frame < 0
                || rec.frame < confirmedFrame - 16
                || rec.frame > confirmedFrame + 16)
            {
                continue;
            }
            char p1Text[64] = {};
            char p2Text[64] = {};
            FormatInputPair(p1Text, sizeof(p1Text), rec.p1Input);
            FormatInputPair(p2Text, sizeof(p2Text), rec.p2Input);
            AppendFormat(
                frames,
                "==== frame %d checksum=0x%08lX inputs p1=0x%04X (%s) p2=0x%04X (%s) %s\r\n",
                rec.frame,
                static_cast<unsigned long>(rec.checksum),
                static_cast<unsigned>(rec.p1Input),
                p1Text,
                static_cast<unsigned>(rec.p2Input),
                p2Text,
                rec.frame >= confirmedFrame ? "[DESYNCED]" : "");
            AppendFormat(
                frames,
                "     commit=%d syncFeed=%d localLen=%d remoteLen=%d ping=%d "
                "effectHash=0x%08lX rngState=%ld\r\n",
                rec.commitFrame,
                rec.syncFeed,
                rec.localLen,
                rec.remoteLen,
                rec.pingMs,
                static_cast<unsigned long>(rec.effectHash),
                static_cast<long>(rec.rngState));
            const uint8_t* cursor = rec.bytes;
            AppendHexRegion(frames, "gameSystem", rec.srcGameSys, cursor,
                kGameSysSliceSize, (rec.validMask & 1) != 0);
            cursor += kGameSysSliceSize;
            AppendHexRegion(frames, "battleScreen", rec.srcBattle, cursor,
                kBattleSliceSize, (rec.validMask & 2) != 0);
            cursor += kBattleSliceSize;
            AppendHexRegion(frames, "characterP1", rec.srcCharP1, cursor,
                kCharSliceSize, (rec.validMask & 4) != 0);
            cursor += kCharSliceSize;
            AppendHexRegion(frames, "characterP2", rec.srcCharP2, cursor,
                kCharSliceSize, (rec.validMask & 8) != 0);
        }
        CloseHandle(frames);
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
                int windowBegin = confirmedFrame - 192;
                int windowEnd = confirmedFrame + 64;
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
                        f == confirmedFrame ? "  <== first mismatching frame" : "");
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

    mod::Log("DESYNC_MONITOR: dump complete '%s'", dumpDir.c_str());
}

// ---------------------------------------------------------------------------
// Worker thread: receive peer checksums, compare, dump on confirmation.
// ---------------------------------------------------------------------------

DWORD WINAPI WorkerThreadProc(LPVOID)
{
    while (InterlockedCompareExchange(&g_workerStop, 0, 0) == 0)
    {
        const SOCKET sock = g_socket;
        if (sock == INVALID_SOCKET)
        {
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval timeout = {};
        timeout.tv_sec = 1;
        const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR)
        {
            break;
        }
        if (selected == 0)
        {
            // Periodic peer-absence note so vanilla peers are diagnosable.
            if (!g_peerSeen
                && !g_peerAbsenceLogged
                && g_sessionStartTick != 0
                && GetTickCount() - g_sessionStartTick > 15000)
            {
                g_peerAbsenceLogged = true;
                mod::Log(
                    "DESYNC_MONITOR: no side-channel packets from peer after 15s - "
                    "peer likely runs without the mod; detection passive, recorder active");
            }
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
        if (received < static_cast<int>(sizeof(uint32_t) + 4)
            || packet.magic != kPacketMagic
            || packet.version != kPacketVersion
            || packet.count == 0
            || packet.count > kSamplesPerPacket)
        {
            continue;
        }

        bool confirmedNow = false;
        EnterCriticalSection(&g_lock);
        if (g_sessionActive)
        {
            if (!g_peerSeen)
            {
                g_peerSeen = true;
                mod::Log(
                    "DESYNC_MONITOR: peer side-channel established (peer role=%u)",
                    static_cast<unsigned>(packet.role));
            }
            // Host learns/refreshes the joiner endpoint from its packets.
            if (g_role == 0)
            {
                g_peerEndpoint = from;
                InterlockedExchange(&g_peerEndpointValid, 1);
            }
            for (int i = 0; i < packet.count; ++i)
            {
                ChecksumSample& slot = g_remoteRing[g_remoteRingNext];
                g_remoteRingNext = (g_remoteRingNext + 1) % kSampleRingDepth;
                slot.frame = packet.samples[i].frame;
                slot.checksum = packet.samples[i].checksum;
                slot.effectHash = packet.samples[i].effectHash;
                slot.rngState = packet.samples[i].rngState;
                if (InterlockedCompareExchange(&g_desyncConfirmed, 0, 0) == 0)
                {
                    confirmedNow |= CompareRemoteSampleLocked(
                        packet.samples[i].frame,
                        packet.samples[i].checksum,
                        packet.samples[i].effectHash,
                        packet.samples[i].rngState);
                }
            }
        }
        LeaveCriticalSection(&g_lock);

        if (confirmedNow && !g_dumped)
        {
            g_dumped = true;
            WriteDesyncDump();
        }
    }
    return 0;
}

void StopWorkerAndChannel()
{
    InterlockedExchange(&g_workerStop, 1);
    CloseSideChannel();
    if (g_workerThread != nullptr)
    {
        (void)WaitForSingleObject(g_workerThread, 3000);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
}

void ResetMaskLocked(const char* reason)
{
    std::memset(g_changeMask, 0, sizeof(g_changeMask));
    g_prevRegionValid = false;
    g_maskSamples = 0;
    g_maskFrozen = false;
    g_maskByteCount = 0;
    if (reason != nullptr)
    {
        mod::Log("DESYNC_MONITOR: change-mask calibration restarted (%s)", reason);
    }
}

void ResetRingsLocked()
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
    g_regionRingNext = 0;
    g_localRingNext = 0;
    g_remoteRingNext = 0;
    g_compareLogNext = 0;
    g_mismatchStreak = 0;
    g_firstMismatchFrame = -1;
    g_lastMismatchFrame = -1;
    g_effectLayerRun = 0;
    g_effectLayerFirstFrame = -1;
    g_rngLayerRun = 0;
    g_rngLayerFirstFrame = -1;
    g_confirmedFrame = -1;
    g_sampleCount = 0;
    g_peerSeen = false;
    g_peerAbsenceLogged = false;
    g_leftBattleSinceLastSample = false;
    g_lastSampledFrame = -1;
    ResetMaskLocked(nullptr);
    InterlockedExchange(&g_desyncConfirmed, 0);
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
    if (!netplay::mod_settings::IsDesyncDetectionEnabled())
    {
        return;
    }
    // Host (0) and Join (1) only; spectators (2) and spectate-redirect joins
    // (3) receive the state feed with their own frame numbering and cannot
    // be compared against the players.
    if (netplayRole != 0 && netplayRole != 1)
    {
        return;
    }

    EnsureLock();
    StopWorkerAndChannel();

    EnterCriticalSection(&g_lock);
    ResetRingsLocked();
    g_sessionActive = true;
    g_dumped = false;
    g_role = (netplayRole == 0) ? 0 : 1;
    g_hostPort = port;
    g_sessionStartTick = GetTickCount();
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
        "DESYNC_MONITOR: session started role=%d port=%u peer='%s' recorder=on channel=%s",
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
    EnterCriticalSection(&g_lock);
    wasActive = g_sessionActive;
    g_sessionActive = false;
    LeaveCriticalSection(&g_lock);

    if (wasActive)
    {
        StopWorkerAndChannel();
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

void RecordFrameTick(uintptr_t sessionPtr, int frame, int commitFrame)
{
    if (!g_lockInited || !g_sessionActive || sessionPtr == 0 || frame < 0)
    {
        return;
    }
    if (InterlockedCompareExchange(&g_desyncConfirmed, 0, 0) != 0)
    {
        return;
    }
    // Only confirmed post-tick states are comparable across peers: skip
    // ticks that ended with unconfirmed predicted frames outstanding.
    if (commitFrame < 0 || frame - commitFrame > 1)
    {
        return;
    }
    // Battle screen only - menus don't carry sync-relevant state.  Leaving
    // the battle invalidates the change mask: the next match may lay out its
    // heap objects differently, so calibration restarts per battle.
    uint8_t screen = 0xFF;
    if (!netplay::bridge::takeover::SafeReadByte(
            reinterpret_cast<const void*>(kScreenIndexAddr), &screen)
        || screen != 3)
    {
        g_leftBattleSinceLastSample = true;
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

    staging.effectHash = ComputeEffectRingHash(gameSys);
    staging.rngState = ReadRevivalRngState();

    EnterCriticalSection(&g_lock);
    if (!g_sessionActive)
    {
        LeaveCriticalSection(&g_lock);
        return;
    }

    // Battle re-entry or frame regression restarts calibration.
    if (g_leftBattleSinceLastSample
        || (g_lastSampledFrame >= 0 && frame < g_lastSampledFrame))
    {
        g_leftBattleSinceLastSample = false;
        ResetMaskLocked("battle_reentry");
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
            mod::Log(
                "DESYNC_MONITOR: change mask frozen at frame %d "
                "(%u of %u bytes participate in checksums)",
                frame,
                g_maskByteCount,
                static_cast<unsigned>(kRecordRegionBytes));
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

    std::memcpy(&g_regionRing[g_regionRingNext], &staging, sizeof(staging));
    g_regionRingNext = (g_regionRingNext + 1) % kRegionRingDepth;

    if (g_maskFrozen)
    {
        ChecksumSample& sample = g_localRing[g_localRingNext];
        g_localRingNext = (g_localRingNext + 1) % kSampleRingDepth;
        sample.frame = frame;
        sample.checksum = checksum;
        sample.effectHash = staging.effectHash;
        sample.rngState = staging.rngState;

        ++g_sampleCount;
        if ((g_sampleCount % kSendEverySamples) == 0)
        {
            SendRecentSamplesLocked();
        }
    }
    LeaveCriticalSection(&g_lock);
}

} // namespace netplay::bridge::desync_monitor
