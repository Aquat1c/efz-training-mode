#include "../include/game/macro_controller.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/game/game_state.h"
#include "../include/gui/overlay.h"
#include "../include/input/immediate_input.h"
#include "../include/input/input_core.h"      // GetPlayerPointer
#include "../include/input/input_buffer.h"    // INPUT_BUFFER_* constants
#include "../include/input/injection_control.h" // g_forceBypass
#include "../include/input/input_motion.h"      // g_manualInputOverride/g_manualInputMask
#include "../include/game/auto_action.h" // Enable/Restore P2 control helpers
#include "../include/utils/switch_players.h"
#include "../include/utils/pause_integration.h"
#include "../include/game/practice_offsets.h"
#include "../include/utils/utilities.h"   // GetEFZBase, IsEFZWindowActive, etc.
#include "../include/game/frame_monitor.h" // AreCharactersInitialized()
#include <vector>
#include <atomic>
#include <sstream>
#include <iomanip>

// Forward decls in case headers aren't visible due to include order in some TU configs
extern uintptr_t GetEFZBase();
bool AreCharactersInitialized();

namespace {
    using Mask = uint8_t;
    struct RLESpan { Mask mask; Mask buf; int ticks; int8_t facing; };
    struct Slot {
        std::vector<RLESpan> spans; // RLE of immediate+buf at 64 Hz
        // EfzRevival-style macro buffer: one byte per 64 Hz logic frame
        // This is the authoritative stream used for playback (StepReplay: Read→inject immediate-only)
        std::vector<uint8_t> macroStream;
        std::vector<uint8_t> bufStream; // full circular buffer stream captured during recording
        // Number of buffer entries observed per 64 Hz recorder tick (parallel to spans progression timing, not one-to-one)
        // This preserves how many raw buffer writes the engine produced between each recorder tick.
        std::vector<uint16_t> bufCountsPerTick;
        // Snapshot of the engine's buffer index each 64 Hz recorder tick
        std::vector<uint16_t> bufIndexPerTick;
        uint16_t bufStartIdx = 0; // buffer index at recording start
        uint16_t bufEndIdx = 0;   // buffer index at recording end
        bool hasData = false;
    };

    constexpr int kMaxSlots = 8; // simple ring of slots
    // Overlay placement for macro state banners (avoid overlapping status overlays)
    constexpr int kBannerX = 20;
    constexpr int kBannerY = 160;
    std::atomic<MacroController::State> s_state{ MacroController::State::Idle };
    Slot s_slots[kMaxSlots];
    std::atomic<int> s_curSlot{1}; // 1-based
    // Remember local side to restore after recording
    std::atomic<int> s_prevLocalSide{-1}; // -1 means unknown
    // Overlay banner ID (permanent message)
    int s_macroBannerId = -1;

    // Record/replay cursors
    int s_recLastMask = 0;
    int s_recSpanTicks = 0;    // ticks at 64 Hz-equivalent logical rate
    int s_recLastBuf = 0;      // last buffer mask captured
    int s_recPrevBufIdx = -1;  // previous observed circular buffer index
    int s_recLastFacing = 0;   // -1 left, +1 right, 0 unknown
    size_t s_playIndex = 0;    // index into spans
    int s_playSpanRemaining = 0;
    // Stream playback cursor (EfzRevival-style byte-per-frame macro buffer)
    size_t s_playStreamIndex = 0;
    // Cursor into recorded raw buffer stream for playback (writes per tick using counts-per-tick)
    size_t s_playBufStreamIndex = 0;
    // For stream playback, we derive per-tick recorded facing from the RLE spans
    std::vector<int8_t> s_streamFacingPerTick;
    // Queue of buffer bytes to write at most one per internal frame (192 Hz)
    // Deprecated: replaced by per-tick queue to keep writes within the same 64 Hz tick
    std::vector<uint8_t> s_bufWriteQueue; // legacy, unused after change (kept to preserve state during transitions)
    size_t s_bufQueueHead = 0;
    // New: per-tick buffer write queue so we can evenly distribute N writes across 3 subframes
    std::vector<uint8_t> s_tickBufQueue;
    size_t s_tickBufHead = 0;
    uint16_t s_writesLeftThisTick = 0; // how many buffer writes remain to issue in the current 64 Hz tick
    // Baseline immediate mask for the current 64 Hz tick
    uint8_t s_baselineMask = 0;

    // Progress pacing: we step at the 64 Hz ImmediateInput cadence by counting internal frames (192 Hz)
    int s_frameDiv = 0; // 0..2 cycles; advance when hits 0

    // Gamespeed probe (0 = frozen). Uses PauseIntegration's gamespeed reader.
    bool ReadGamespeedFrozen() {
        // Outside of a live match, treat as frozen to avoid progressing.
        if (GetCurrentGamePhase() != GamePhase::Match) return true;
        return PauseIntegration::IsGameSpeedFrozen();
    }

    inline int ClampSlot(int s){ if (s < 1) return 1; if (s > kMaxSlots) return kMaxSlots; return s; }

    static std::string ToHexString(int value, int width = 2) {
        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
        return oss.str();
    }

    static std::string MaskToButtons(Mask m) {
        // Use unified GAME_INPUT_* flags (input_core.h)
        std::string out;
        auto add = [&](const char* t){ if (!out.empty()) out += ' '; out += t; };
        if (m & GAME_INPUT_UP) add("U");
        if (m & GAME_INPUT_DOWN) add("D");
        if (m & GAME_INPUT_LEFT) add("L");
        if (m & GAME_INPUT_RIGHT) add("R");
        if (m & GAME_INPUT_A) add("A");
        if (m & GAME_INPUT_B) add("B");
        if (m & GAME_INPUT_C) add("C");
        if (m & GAME_INPUT_D) add("D");
        if (out.empty()) out = "<neutral>";
        return out;
    }

    // --- Diagnostic helpers: stream dumps for analysis ---
    static void LogVectorHex(const char* label, int slot, const char* phase, const std::vector<uint8_t>& v, size_t perLine = 32) {
        std::ostringstream line;
        line << "[MACRO][DUMP] slot=" << slot << " " << phase << " " << label << " (" << v.size() << "):";
        LogOut(line.str(), true);
        line.str(""); line.clear();
        size_t count = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            if (count == 0) {
                line << "[MACRO][DUMP]   ";
            }
            line << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)v[i];
            if (i + 1 < v.size()) line << ' ';
            if (++count >= perLine) {
                LogOut(line.str(), true);
                line.str(""); line.clear();
                count = 0;
            }
        }
        if (count > 0) {
            LogOut(line.str(), true);
        }
    }

    static void LogVectorU16(const char* label, int slot, const char* phase, const std::vector<uint16_t>& v, size_t perLine = 32) {
        std::ostringstream line;
        line << "[MACRO][DUMP] slot=" << slot << " " << phase << " " << label << " (" << v.size() << "):";
        LogOut(line.str(), true);
        line.str(""); line.clear();
        size_t count = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            if (count == 0) {
                line << "[MACRO][DUMP]   ";
            }
            line << v[i];
            if (i + 1 < v.size()) line << ' ';
            if (++count >= perLine) {
                LogOut(line.str(), true);
                line.str(""); line.clear();
                count = 0;
            }
        }
        if (count > 0) {
            LogOut(line.str(), true);
        }
    }

    static void LogPerTickOverview(int slot, const char* phase, const std::vector<uint8_t>& macroStream,
                                   const std::vector<uint16_t>& bufCountsPerTick, const std::vector<uint16_t>& bufIndexPerTick) {
        const size_t ticks = macroStream.size();
        std::ostringstream hdr;
        hdr << "[MACRO][DUMP] slot=" << slot << " " << phase << " per-tick overview (" << ticks << "):";
        LogOut(hdr.str(), true);
        // To avoid excessively huge logs, cap detailed per-tick lines to 512 ticks.
        constexpr size_t kDetailCap = 512;
        size_t lim = ticks < kDetailCap ? ticks : kDetailCap;
        for (size_t i = 0; i < lim; ++i) {
            uint8_t m = macroStream[i];
            uint16_t c = (i < bufCountsPerTick.size()) ? bufCountsPerTick[i] : 0;
            uint16_t idx = (i < bufIndexPerTick.size()) ? bufIndexPerTick[i] : 0xFFFF;
            std::ostringstream ln;
            ln << "[MACRO][DUMP]   t=" << i
               << " macro=0x" << ToHexString((int)m, 2)
               << " (" << MaskToButtons(m) << ")"
               << " bufCount=" << c
               << " bufIdx=" << idx;
            LogOut(ln.str(), true);
        }
        if (ticks > kDetailCap) {
            std::ostringstream note;
            note << "[MACRO][DUMP]   (" << (ticks - kDetailCap) << " more ticks omitted from per-tick lines; see full streams above)";
            LogOut(note.str(), true);
        }
    }

    static int ReadFacingSign(int playerNum) {
        uintptr_t pPtr = GetPlayerPointer(playerNum);
        if (!pPtr) return 0;
        uint8_t raw = 0;
        if (!SafeReadMemory(pPtr + FACING_DIRECTION_OFFSET, &raw, sizeof(raw))) return 0;
        if (raw == 1) return +1; // facing right
        if (raw == 255) return -1; // facing left
        return 0;
    }

    static Mask FlipMaskHoriz(Mask m) {
        // Swap left/right, preserve up/down and buttons
        bool left  = (m & GAME_INPUT_LEFT)  != 0;
        bool right = (m & GAME_INPUT_RIGHT) != 0;
        Mask out = m;
        out &= ~(GAME_INPUT_LEFT | GAME_INPUT_RIGHT);
        if (left)  out |= GAME_INPUT_RIGHT;
        if (right) out |= GAME_INPUT_LEFT;
        return out;
    }

    // Diagnostic: dump a tail of P2's input buffer and current index
    void LogP2BufferSnapshot(const char* label, int tail = 16) {
        uintptr_t p2Ptr = GetPlayerPointer(2);
        if (!p2Ptr) { LogOut(std::string("[MACRO][BUF] ") + label + ": P2 ptr null", true); return; }
        uint16_t idx = 0;
        if (!SafeReadMemory(p2Ptr + INPUT_BUFFER_INDEX_OFFSET, &idx, sizeof(idx))) {
            LogOut(std::string("[MACRO][BUF] ") + label + ": index read failed", true);
            return;
        }
        if (tail < 1) tail = 1; if (tail > (int)INPUT_BUFFER_SIZE) tail = (int)INPUT_BUFFER_SIZE;
        std::vector<uint8_t> vals(tail, 0);
        for (int i = 0; i < tail; ++i) {
            int w = ((int)idx - (tail - 1 - i));
            w %= (int)INPUT_BUFFER_SIZE; if (w < 0) w += (int)INPUT_BUFFER_SIZE;
            uint8_t v = 0; SafeReadMemory(p2Ptr + INPUT_BUFFER_OFFSET + (uintptr_t)w, &v, sizeof(v));
            vals[i] = v;
        }
        std::ostringstream ss; ss << "[MACRO][BUF] " << label << ": P2 idx=" << idx << " tail=" << tail << " vals=";
        for (int i = 0; i < tail; ++i) {
            ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)vals[i];
            if (i + 1 < tail) ss << ' ';
        }
        LogOut(ss.str(), true);
    }

    // Diagnostic: read and log P2 immediate registers (H/V and A-D)
    void LogP2ImmediateSnapshot(const char* label) {
        uintptr_t p2Ptr = GetPlayerPointer(2);
        if (!p2Ptr) { LogOut(std::string("[MACRO][IMM] ") + label + ": P2 ptr null", true); return; }
        uint8_t h=0, v=0, a=0, b=0, c=0, d=0;
        SafeReadMemory(p2Ptr + INPUT_HORIZONTAL_OFFSET, &h, sizeof(h));
        SafeReadMemory(p2Ptr + INPUT_VERTICAL_OFFSET, &v, sizeof(v));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_A_OFFSET, &a, sizeof(a));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_B_OFFSET, &b, sizeof(b));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_C_OFFSET, &c, sizeof(c));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_D_OFFSET, &d, sizeof(d));
        std::ostringstream ss;
        ss << "[MACRO][IMM] " << label << ": H=" << (int)h << " V=" << (int)v
           << " A=" << (int)a << " B=" << (int)b << " C=" << (int)c << " D=" << (int)d;
        LogOut(ss.str(), true);
    }

    void ResetPlayback() {
        s_playIndex = 0; s_playSpanRemaining = 0; s_playStreamIndex = 0; s_playBufStreamIndex = 0; s_frameDiv = 0;
        s_streamFacingPerTick.clear();
        s_bufWriteQueue.clear(); s_bufQueueHead = 0; s_baselineMask = 0;
        s_tickBufQueue.clear(); s_tickBufHead = 0; s_writesLeftThisTick = 0;
        ImmediateInput::Clear(2);
        // Default hook behavior
        g_forceBypass[1].store(false);
        g_forceBypass[2].store(false);
        g_injectImmediateOnly[1].store(false);
        g_injectImmediateOnly[2].store(false);
        g_manualInputOverride[1].store(false);
        g_manualInputOverride[2].store(false);
        // Ensure poll override is fully cleared for both players
        g_pollOverrideActive[1].store(false);
        g_pollOverrideActive[2].store(false);
        g_pollOverrideMask[1].store(0);
        g_pollOverrideMask[2].store(0);
    }

    void FinishRecording() {
        MacroController::State st = s_state.load();
        if (st != MacroController::State::Recording) return;
        // Flush any pending span
        if (s_recSpanTicks > 0) {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            s_slots[slotIdx].spans.push_back({ static_cast<Mask>(s_recLastMask & 0xFF), static_cast<Mask>(s_recLastBuf & 0xFF), s_recSpanTicks, (int8_t)s_recLastFacing });
            s_slots[slotIdx].hasData = !s_slots[slotIdx].spans.empty();
            // Log final span
            LogOut(std::string("[MACRO][REC] span imm=0x") + ToHexString((int)(s_recLastMask & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastMask & 0xFF)) + ") buf=0x" + ToHexString((int)(s_recLastBuf & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastBuf & 0xFF)) + ") ticks=" + std::to_string(s_recSpanTicks) +
                   " facing=" + std::to_string(s_recLastFacing), true);
        }
        // Capture any remaining buffer entries up to current index at finish (diagnostic stream for engine buffer)
        uintptr_t p2Ptr = GetPlayerPointer(2);
        uint16_t endIdx = 0;
        if (p2Ptr && SafeReadMemory(p2Ptr + INPUT_BUFFER_INDEX_OFFSET, &endIdx, sizeof(endIdx))) {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            // Append entries from s_recPrevBufIdx -> endIdx (exclusive of endIdx, inclusive start)
            if (s_recPrevBufIdx >= 0) {
                size_t beforeSize = s_slots[slotIdx].bufStream.size();
                int cur = s_recPrevBufIdx;
                while (cur != endIdx) {
                    uint8_t v = 0;
                    SafeReadMemory(p2Ptr + INPUT_BUFFER_OFFSET + (uintptr_t)cur, &v, sizeof(v));
                    s_slots[slotIdx].bufStream.push_back(v);
                    cur = (cur + 1) % (int)INPUT_BUFFER_SIZE;
                }
                // Attribute any tail entries to the last recorder tick for completeness
                size_t added = s_slots[slotIdx].bufStream.size() - beforeSize;
                if (added > 0) {
                    if (!s_slots[slotIdx].bufCountsPerTick.empty()) {
                        s_slots[slotIdx].bufCountsPerTick.back() = static_cast<uint16_t>(
                            (uint32_t)s_slots[slotIdx].bufCountsPerTick.back() + (uint32_t)added);
                    } else {
                        // If there were no ticks recorded (edge case), start with the added amount
                        s_slots[slotIdx].bufCountsPerTick.push_back(static_cast<uint16_t>(added));
                    }
                }
            }
            s_slots[slotIdx].bufEndIdx = endIdx;
        }
    s_recSpanTicks = 0; s_recLastMask = 0; s_recLastBuf = 0; s_recPrevBufIdx = -1; s_recLastFacing = 0;
        // Snapshot P2 buffer and immediate regs at end
        LogP2BufferSnapshot("end");
        LogP2ImmediateSnapshot("end");
      // Summary
        int slotIdx = ClampSlot(s_curSlot.load()) - 1;
        int totalTicks = 0; for (auto &sp : s_slots[slotIdx].spans) totalTicks += sp.ticks;
     LogOut("[MACRO][REC] finished slot=" + std::to_string(s_curSlot.load()) +
               " spans=" + std::to_string((int)s_slots[slotIdx].spans.size()) +
         " ticks=" + std::to_string(totalTicks) +
         " streamBytes=" + std::to_string((int)s_slots[slotIdx].macroStream.size()) +
               " bufEntries=" + std::to_string((int)s_slots[slotIdx].bufStream.size()) +
         " bufTicks=" + std::to_string((int)s_slots[slotIdx].bufCountsPerTick.size()) +
               " bufIdxStart=" + std::to_string((int)s_slots[slotIdx].bufStartIdx) +
               " bufIdxEnd=" + std::to_string((int)s_slots[slotIdx].bufEndIdx), true);
      // Full stream dumps for analysis
      {
        int slotNum = s_curSlot.load();
        LogVectorHex("macroStream", slotNum, "rec-finish", s_slots[slotIdx].macroStream);
        LogVectorHex("bufStream", slotNum, "rec-finish", s_slots[slotIdx].bufStream);
        LogVectorU16("bufCountsPerTick", slotNum, "rec-finish", s_slots[slotIdx].bufCountsPerTick);
        LogVectorU16("bufIndexPerTick", slotNum, "rec-finish", s_slots[slotIdx].bufIndexPerTick);
        LogPerTickOverview(slotNum, "rec-finish", s_slots[slotIdx].macroStream, s_slots[slotIdx].bufCountsPerTick, s_slots[slotIdx].bufIndexPerTick);
      }
        s_state.store(MacroController::State::Idle);
    // Restore P2 control if we overrode it during pre-record
    if (g_p2ControlOverridden) RestoreP2ControlState();
    // Swap back to P1 local side after recording for convenience
    SwitchPlayers::SetLocalSide(0);
    s_prevLocalSide.store(-1);
    LogOut("[MACRO][REC] post-finish: local side set to P1", true);
        // Remove persistent banner
        if (s_macroBannerId != -1) { DirectDrawHook::RemovePermanentMessage(s_macroBannerId); s_macroBannerId = -1; }
        DirectDrawHook::AddMessage("Macro: Recording stopped", "MACRO", RGB(255,220,120), 1200, 0, 120);
    }

    std::string StateName(MacroController::State st) {
        switch (st) {
            case MacroController::State::Idle: return "Idle";
            case MacroController::State::PreRecord: return "PreRecord";
            case MacroController::State::Recording: return "Recording";
            case MacroController::State::Replaying: return "Replaying";
        }
        return "?";
    }

    uint8_t ReadRecordSourceMask() {
        // Build unified mask from P2 immediate registers (independent of local side)
        uintptr_t p2Ptr = GetPlayerPointer(2);
        if (!p2Ptr) return 0;
        uint8_t h=0, v=0, a=0, b=0, c=0, d=0;
        SafeReadMemory(p2Ptr + INPUT_HORIZONTAL_OFFSET, &h, sizeof(h));
        SafeReadMemory(p2Ptr + INPUT_VERTICAL_OFFSET, &v, sizeof(v));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_A_OFFSET, &a, sizeof(a));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_B_OFFSET, &b, sizeof(b));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_C_OFFSET, &c, sizeof(c));
        SafeReadMemory(p2Ptr + INPUT_BUTTON_D_OFFSET, &d, sizeof(d));
        uint8_t mask = 0;
        if (h == 1) mask |= GAME_INPUT_RIGHT; else if (h == 255) mask |= GAME_INPUT_LEFT;
        if (v == 1) mask |= GAME_INPUT_DOWN;  else if (v == 255) mask |= GAME_INPUT_UP;
        if (a) mask |= GAME_INPUT_A;
        if (b) mask |= GAME_INPUT_B;
        if (c) mask |= GAME_INPUT_C;
        if (d) mask |= GAME_INPUT_D;
        return mask;
    }

    // Read the most recently written P2 input buffer value (unified mask)
    uint8_t ReadP2BufferLatestMask() {
        uintptr_t p2Ptr = GetPlayerPointer(2);
        if (!p2Ptr) return 0;
        uint16_t idx = 0;
        if (!SafeReadMemory(p2Ptr + INPUT_BUFFER_INDEX_OFFSET, &idx, sizeof(idx))) return 0;
        int last = ((int)idx - 1);
        last %= (int)INPUT_BUFFER_SIZE; if (last < 0) last += (int)INPUT_BUFFER_SIZE;
        uint8_t val = 0;
        SafeReadMemory(p2Ptr + INPUT_BUFFER_OFFSET + (uintptr_t)last, &val, sizeof(val));
        return val;
    }
}

namespace MacroController {

void Tick() {
    // Only operate during a valid match with characters initialized
    if (GetCurrentGamePhase() != GamePhase::Match || !AreCharactersInitialized()) return;

    // Pace counter for 64 Hz logical ticks using 192 Hz internal frames
    if (++s_frameDiv >= 3) s_frameDiv = 0;

    State st = s_state.load();
    if (st == State::Recording) {
        // Frame-step aware progression:
        // - When not frozen: advance every 3rd internal frame (approx 64 Hz).
        // - When frozen (paused): only advance when P2's input buffer index has advanced (indicates a stepped frame).
        const bool frozen = ReadGamespeedFrozen();
        // Probe current buffer index once up front to decide whether to progress while frozen
        uint16_t idxProbe = 0; bool haveIdx = false; bool bufAdvanced = false;
        {
            uintptr_t p2PtrProbe = GetPlayerPointer(2);
            if (p2PtrProbe && SafeReadMemory(p2PtrProbe + INPUT_BUFFER_INDEX_OFFSET, &idxProbe, sizeof(idxProbe))) {
                haveIdx = true;
                if (s_recPrevBufIdx >= 0 && (uint16_t)s_recPrevBufIdx != idxProbe) bufAdvanced = true;
            }
        }
        bool shouldAdvance = false;
        if (!frozen) {
            shouldAdvance = (s_frameDiv == 0);
        } else {
            // When frozen, only advance on observed buffer movement (frame-step)
            shouldAdvance = bufAdvanced;
        }
        if (!shouldAdvance) return;
        // If buffer-freeze is active for P2, avoid progressing to keep streams aligned
        if (g_bufferFreezingActive.load() && (g_activeFreezePlayer.load() == 2 || g_activeFreezePlayer.load() == 0)) return;
        uint8_t immMask = ReadRecordSourceMask();
        int facing = ReadFacingSign(2);
        uint8_t buf  = 0;
        // Capture all new buffer entries since last tick
        {
            uintptr_t p2Ptr = GetPlayerPointer(2);
            uint16_t idx = 0;
            if (p2Ptr && SafeReadMemory(p2Ptr + INPUT_BUFFER_INDEX_OFFSET, &idx, sizeof(idx)) && s_recPrevBufIdx >= 0) {
                int slotIdx = ClampSlot(s_curSlot.load()) - 1;
                size_t beforeSize = s_slots[slotIdx].bufStream.size();
                uint8_t addedBtnUnion = 0; // union of A-D seen in new entries this tick
                int cur = s_recPrevBufIdx;
                while (cur != idx) {
                    uint8_t v = 0;
                    SafeReadMemory(p2Ptr + INPUT_BUFFER_OFFSET + (uintptr_t)cur, &v, sizeof(v));
                    s_slots[slotIdx].bufStream.push_back(v);
                    addedBtnUnion |= (v & (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D));
                    cur = (cur + 1) % (int)INPUT_BUFFER_SIZE;
                }
                s_recPrevBufIdx = idx;
                // Record how many entries the engine produced this recorder tick
                size_t added = s_slots[slotIdx].bufStream.size() - beforeSize;
                s_slots[slotIdx].bufCountsPerTick.push_back(static_cast<uint16_t>(added));
                s_slots[slotIdx].bufIndexPerTick.push_back(idx);
                if (added > 0) {
                    buf = s_slots[slotIdx].bufStream.back();
                    // Merge buttons seen anywhere in this tick's new entries into our immediate view
                    if (addedBtnUnion) {
                        uint8_t immButtons = (uint8_t)(immMask & (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D));
                        immMask = (uint8_t)((immMask & (GAME_INPUT_UP | GAME_INPUT_DOWN | GAME_INPUT_LEFT | GAME_INPUT_RIGHT))
                                  | (immButtons | addedBtnUnion));
                    }
                }
            }
        }
        if (buf == 0) buf = ReadP2BufferLatestMask();
        // Combine: use buffer's directional bits (authoritative) + union of buffer/immediate button bits (A-D)
        const uint8_t DIR_MASK = (GAME_INPUT_UP | GAME_INPUT_DOWN | GAME_INPUT_LEFT | GAME_INPUT_RIGHT); // 0x0F
        const uint8_t BTN_MASK = (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D);            // 0xF0
        uint8_t dirBits = buf & DIR_MASK;
        uint8_t btnBits = (uint8_t)((immMask | buf) & BTN_MASK);
        uint8_t mask = (dirBits | btnBits);
        // EfzRevival-style: Write one byte per logic tick into per-slot macro stream
        {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            s_slots[slotIdx].macroStream.push_back(mask);
        }
        if (s_recSpanTicks == 0) {
            s_recLastMask = mask; s_recLastBuf = mask; s_recLastFacing = facing; s_recSpanTicks = 1;
        } else if (mask == s_recLastMask && buf == s_recLastBuf && facing == s_recLastFacing) {
            s_recSpanTicks++;
        } else {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            s_slots[slotIdx].spans.push_back({ static_cast<Mask>(s_recLastMask & 0xFF), static_cast<Mask>(s_recLastBuf & 0xFF), s_recSpanTicks, (int8_t)s_recLastFacing });
            s_slots[slotIdx].hasData = true;
            // Log completed span (immediate vs buffer)
            LogOut(std::string("[MACRO][REC] span imm=0x") + ToHexString((int)(s_recLastMask & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastMask & 0xFF)) + ") buf=0x" + ToHexString((int)(s_recLastBuf & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastBuf & 0xFF)) + ") ticks=" + std::to_string(s_recSpanTicks) +
                   " facing=" + std::to_string(s_recLastFacing), true);
            s_recLastMask = mask; s_recLastBuf = mask; s_recLastFacing = facing; s_recSpanTicks = 1;
        }
    } else if (st == State::Replaying) {
        // Replay runs every internal frame to better match engine read cadence
        if (ReadGamespeedFrozen()) return; // pause-safe
        // Pause while buffer-freeze is active for P2 to avoid fighting the engine
        if (g_bufferFreezingActive.load() && (g_activeFreezePlayer.load() == 2 || g_activeFreezePlayer.load() == 0)) return;
        int slotIdx = ClampSlot(s_curSlot.load()) - 1;
        // Prefer stream playback if present (EfzRevival-style). Fallback to spans if no stream captured.
        bool useStream = !s_slots[slotIdx].macroStream.empty();
        if (!s_slots[slotIdx].hasData || (s_slots[slotIdx].spans.empty() && !useStream)) {
            // Nothing to play
            s_state.store(State::Idle);
            ImmediateInput::Clear(2);
            g_manualInputOverride[2].store(false);
            g_forceBypass[2].store(false);
            g_injectImmediateOnly[2].store(false);
            DirectDrawHook::AddMessage("Macro: Replay empty", "MACRO", RGB(255,120,120), 1000, 0, 120);
            return;
        }
        if (useStream) {
            // On 64 Hz tick boundary: update baseline mask and enqueue this tick's buffer bytes
            if (s_frameDiv == 0) {
                if (s_playStreamIndex < s_slots[slotIdx].macroStream.size()) {
                    uint8_t mask = s_slots[slotIdx].macroStream[s_playStreamIndex];
                    if (!s_streamFacingPerTick.empty() && s_playStreamIndex < s_streamFacingPerTick.size()) {
                        int8_t recFacing = s_streamFacingPerTick[s_playStreamIndex];
                        int curFacing = ReadFacingSign(2);
                        if (recFacing != 0 && curFacing != 0 && recFacing != curFacing) {
                            mask = FlipMaskHoriz(mask);
                        }
                    }
                    s_baselineMask = mask;
                    // Prepare per-tick queue of recorded raw buffer bytes
                    s_tickBufQueue.clear();
                    s_tickBufHead = 0;
                    s_writesLeftThisTick = 0;
                    if (s_playStreamIndex < s_slots[slotIdx].bufCountsPerTick.size()) {
                        uint16_t writesThisTick = s_slots[slotIdx].bufCountsPerTick[s_playStreamIndex];
                        s_writesLeftThisTick = writesThisTick;
                        for (uint16_t i = 0; i < writesThisTick && s_playBufStreamIndex < s_slots[slotIdx].bufStream.size(); ++i) {
                            uint8_t raw = s_slots[slotIdx].bufStream[s_playBufStreamIndex++];
                            if (!s_streamFacingPerTick.empty() && s_playStreamIndex < s_streamFacingPerTick.size()) {
                                int8_t recFacing = s_streamFacingPerTick[s_playStreamIndex];
                                int curFacing = ReadFacingSign(2);
                                if (recFacing != 0 && curFacing != 0 && recFacing != curFacing) {
                                    raw = FlipMaskHoriz(raw);
                                }
                            }
                            s_tickBufQueue.push_back(raw);
                        }
                    }
                    ++s_playStreamIndex;
                }
            }
            // Every frame: write some of this tick's buffer bytes via engine by overriding the poll,
            // and set per-frame poll override to the intended immediate mask for exact engine cadence.
            const uint8_t DIR_MASK = (GAME_INPUT_UP | GAME_INPUT_DOWN | GAME_INPUT_LEFT | GAME_INPUT_RIGHT);
            const uint8_t BTN_MASK = (GAME_INPUT_A | GAME_INPUT_B | GAME_INPUT_C | GAME_INPUT_D);
            // Baseline applies fully from the first subframe; buffer button bits for this subframe override as they appear
            uint8_t frameMask = s_baselineMask;
            // Determine frames remaining in this tick (including this frame): 3 at s_frameDiv==0, 2 at 1, 1 at 2
            int framesLeft = 3 - s_frameDiv;
            // Compute how many writes to issue this subframe to finish by end of tick (ceil division)
            int writesToDo = 0;
            if (s_writesLeftThisTick > 0 && framesLeft > 0) {
                writesToDo = (s_writesLeftThisTick + framesLeft - 1) / framesLeft;
            }
            // Issue writesToDo from this tick's queue (clamped)
            for (int w = 0; w < writesToDo && s_tickBufHead < s_tickBufQueue.size(); ++w) {
                uint8_t raw = s_tickBufQueue[s_tickBufHead++];
                // Blend buttons and rely on engine history write from our poll override
                s_writesLeftThisTick = (s_writesLeftThisTick > 0) ? (uint16_t)(s_writesLeftThisTick - 1) : 0;
                // Blend button bits if present in any of today writes; later writes can overwrite earlier ones' btns
                uint8_t btnBits = (raw & BTN_MASK);
                if (btnBits) frameMask = (uint8_t)((frameMask & DIR_MASK) | btnBits);
            }
            // Drive the engine's poll directly this frame (P2 = index 2). This ensures
            // poll → immediate registers → history ring all reflect our desired state.
            g_pollOverrideMask[2].store(frameMask, std::memory_order_relaxed);
            g_pollOverrideActive[2].store(true, std::memory_order_relaxed);
            // Ensure we are not bypassing processCharacterInput; let the engine handle writes.
            g_forceBypass[2].store(false);
            g_injectImmediateOnly[2].store(false);
            // End condition: after last tick and queue drained
            if (s_playStreamIndex >= s_slots[slotIdx].macroStream.size() && s_tickBufHead >= s_tickBufQueue.size() && s_writesLeftThisTick == 0) {
                s_state.store(State::Idle);
          ImmediateInput::Clear(2);
          g_pollOverrideActive[2].store(false, std::memory_order_relaxed);
          g_forceBypass[2].store(false);
          g_injectImmediateOnly[2].store(false);
                if (s_macroBannerId != -1) { DirectDrawHook::RemovePermanentMessage(s_macroBannerId); s_macroBannerId = -1; }
                if (g_p2ControlOverridden) RestoreP2ControlState();
                LogOut("[MACRO][PLAY] finished slot=" + std::to_string(s_curSlot.load()) +
                       " streamBytes=" + std::to_string((int)s_slots[slotIdx].macroStream.size()) +
                       " bufWrites=" + std::to_string((int)s_slots[slotIdx].bufStream.size()), true);
                DirectDrawHook::AddMessage("Macro: Replay finished", "MACRO", RGB(180,255,180), 1200, 0, 120);
                return;
            }
        } else {
            // Fallback to existing RLE span playback (legacy path)
            if (s_frameDiv == 0 && s_playSpanRemaining <= 0) {
                if (s_playIndex >= s_slots[slotIdx].spans.size()) {
                    // End
                    s_state.store(State::Idle);
                    ImmediateInput::Clear(2);
                    g_manualInputOverride[2].store(false);
                    g_forceBypass[2].store(false);
                    g_injectImmediateOnly[2].store(false);
                    // Remove banner on finish
                    if (s_macroBannerId != -1) { DirectDrawHook::RemovePermanentMessage(s_macroBannerId); s_macroBannerId = -1; }
                    // Restore P2 control if we overrode it for playback
                    if (g_p2ControlOverridden) RestoreP2ControlState();
                    // Summary
                    int totalTicks = 0; for (auto &sp : s_slots[slotIdx].spans) totalTicks += sp.ticks;
                    LogOut("[MACRO][PLAY] finished slot=" + std::to_string(s_curSlot.load()) +
                           " spans=" + std::to_string((int)s_slots[slotIdx].spans.size()) +
                           " ticks=" + std::to_string(totalTicks), true);
                    DirectDrawHook::AddMessage("Macro: Replay finished", "MACRO", RGB(180,255,180), 1200, 0, 120);
                    return;
                }
                const RLESpan &sp = s_slots[slotIdx].spans[s_playIndex++];
                s_playSpanRemaining = sp.ticks;
                // Determine current facing and flip if needed
                int curFacing = ReadFacingSign(2);
                uint8_t maskToApply = sp.mask;
                if (sp.facing != 0 && curFacing != 0 && sp.facing != curFacing) {
                    maskToApply = FlipMaskHoriz(maskToApply);
                }
                // For RLE fallback, also use poll override so engine cadence is preserved
                g_pollOverrideMask[2].store(maskToApply, std::memory_order_relaxed);
                g_pollOverrideActive[2].store(true, std::memory_order_relaxed);
                g_forceBypass[2].store(false);
                g_injectImmediateOnly[2].store(false);
                LogOut(std::string("[MACRO][PLAY] span imm=0x") + ToHexString((int)sp.mask, 2) +
                       " (" + MaskToButtons(sp.mask) + ") buf=0x" + ToHexString((int)sp.buf, 2) +
                       " (" + MaskToButtons(sp.buf) + ") ticks=" + std::to_string(sp.ticks) +
                       " recFacing=" + std::to_string((int)sp.facing) + " curFacing=" + std::to_string(curFacing) +
                       " -> applied=0x" + ToHexString((int)maskToApply, 2) + " [poll-override]", true);
            } else if (s_playSpanRemaining > 0) {
                s_playSpanRemaining--;
                if (s_playSpanRemaining <= 0) {
                    // Force a neutral edge between spans to ensure clean transitions via poll
                    ImmediateInput::Clear(2);
                    g_pollOverrideMask[2].store(0, std::memory_order_relaxed);
                }
            }
        }
    }
}

void ToggleRecord() {
    // Guard: only during live match
    if (GetCurrentGamePhase() != GamePhase::Match || !AreCharactersInitialized()) {
        DirectDrawHook::AddMessage("Macro controls available only during Match", "MACRO", RGB(255, 180, 120), 900, 0, 120);
        return;
    }
    State st = s_state.load();
    if (st == State::Idle) {
        // Enter PreRecord: swap controls to make P2 local and remember current side
        if (GetCurrentGameMode() == GameMode::Practice) {
            PauseIntegration::EnsurePracticePointerCapture();
            void* p = PauseIntegration::GetPracticeControllerPtr();
            int curLocal = 0;
            if (p && SafeReadMemory((uintptr_t)p + PRACTICE_OFF_LOCAL_SIDE_IDX, &curLocal, sizeof(curLocal))) {
                s_prevLocalSide.store(curLocal);
                if (curLocal != 1) {
                    SwitchPlayers::SetLocalSide(1); // make P2 local for easier recording
                }
            }
            EnableP2ControlForAutoAction(); // ensure P2 is human-controlled
        }
        s_state.store(State::PreRecord);
        // Persistent yellow banner (lower left, non-overlapping with status)
        if (s_macroBannerId == -1) {
            s_macroBannerId = DirectDrawHook::AddPermanentMessage("Macro: PreRecord", RGB(255,255,0), kBannerX, kBannerY);
        } else {
            DirectDrawHook::UpdatePermanentMessage(s_macroBannerId, "Macro: PreRecord", RGB(255,255,0));
        }
        LogOut("[MACRO] PreRecord: prevLocal=" + std::to_string(s_prevLocalSide.load()) +
               " -> nowLocal=P2  slot=" + std::to_string(s_curSlot.load()), true);
    } else if (st == State::PreRecord) {
        // Start recording into current slot
        int slotIdx = ClampSlot(s_curSlot.load()) - 1;
        s_slots[slotIdx].spans.clear();
        s_slots[slotIdx].hasData = false;
            s_slots[slotIdx].macroStream.clear();
            s_slots[slotIdx].bufStream.clear();
            s_slots[slotIdx].bufCountsPerTick.clear();
            s_slots[slotIdx].bufIndexPerTick.clear();
            s_slots[slotIdx].bufStartIdx = 0;
            s_slots[slotIdx].bufEndIdx = 0;
            s_recLastMask = 0; s_recSpanTicks = 0; s_frameDiv = 0;
            // Record starting buffer index
            {
                uintptr_t p2Ptr = GetPlayerPointer(2);
                uint16_t startIdx = 0;
                if (p2Ptr && SafeReadMemory(p2Ptr + INPUT_BUFFER_INDEX_OFFSET, &startIdx, sizeof(startIdx))) {
                    s_slots[slotIdx].bufStartIdx = startIdx;
                    s_recPrevBufIdx = startIdx;
                } else {
                    s_recPrevBufIdx = -1;
                }
            }
        s_state.store(State::Recording);
        // Update persistent banner to red
        if (s_macroBannerId == -1) {
            s_macroBannerId = DirectDrawHook::AddPermanentMessage("Macro: Recording", RGB(255,80,80), kBannerX, kBannerY);
        } else {
            DirectDrawHook::UpdatePermanentMessage(s_macroBannerId, "Macro: Recording", RGB(255,80,80));
        }
     LogOut("[MACRO][REC] started slot=" + std::to_string(s_curSlot.load()) +
         " source=P2 -> sink=P2", true);
    // Snapshot P2 buffer and immediate regs at start
    LogP2BufferSnapshot("start");
    LogP2ImmediateSnapshot("start");
    } else if (st == State::Recording) {
        FinishRecording();
    } else if (st == State::Replaying) {
        // Stop replay
        s_state.store(State::Idle);
        ResetPlayback();
        // Remove banner when stopping
        if (s_macroBannerId != -1) { DirectDrawHook::RemovePermanentMessage(s_macroBannerId); s_macroBannerId = -1; }
        DirectDrawHook::AddMessage("Macro: Replay stopped", "MACRO", RGB(255,200,180), 1000, 0, 120);
    }
}

void Play() {
    // Guard: only during live match
    if (GetCurrentGamePhase() != GamePhase::Match || !AreCharactersInitialized()) {
        DirectDrawHook::AddMessage("Macro controls available only during Match", "MACRO", RGB(255, 180, 120), 900, 0, 120);
        return;
    }
    // If currently recording, finish first
    if (s_state.load() == State::Recording) FinishRecording();
    // Prepare playback
    int slotIdx = ClampSlot(s_curSlot.load()) - 1;
    if ((!s_slots[slotIdx].hasData || s_slots[slotIdx].spans.empty()) && s_slots[slotIdx].macroStream.empty()) {
        DirectDrawHook::AddMessage("Macro: Slot empty", "MACRO", RGB(255,120,120), 1000, 0, 120);
        return;
    }
    ResetPlayback();
    // Ensure player control: we control P1 locally while P2 is macro-driven.
    if (GetCurrentGameMode() == GameMode::Practice) {
        SwitchPlayers::SetLocalSide(0); // P1 is local side
    }
    // Precompute per-tick recorded facing for stream playback from RLE spans
    if (!s_slots[slotIdx].macroStream.empty() && !s_slots[slotIdx].spans.empty()) {
        size_t total = s_slots[slotIdx].macroStream.size();
        s_streamFacingPerTick.clear();
        s_streamFacingPerTick.reserve(total);
        for (const auto &sp : s_slots[slotIdx].spans) {
            for (int t = 0; t < sp.ticks && s_streamFacingPerTick.size() < total; ++t) {
                s_streamFacingPerTick.push_back(sp.facing);
            }
            if (s_streamFacingPerTick.size() >= total) break;
        }
        // If spans underflowed due to mismatch, pad remaining with 0 (unknown)
        while (s_streamFacingPerTick.size() < total) s_streamFacingPerTick.push_back(0);
    }
    // Ensure P2 is human-controlled during playback
    EnableP2ControlForAutoAction();
    // Drive inputs via poll override while playing for exact engine cadence
    g_pollOverrideActive[2].store(true, std::memory_order_relaxed);
    g_forceBypass[2].store(false);
    g_injectImmediateOnly[2].store(false);
    s_state.store(State::Replaying);
    // Persistent green banner
    if (s_macroBannerId == -1) {
        s_macroBannerId = DirectDrawHook::AddPermanentMessage("Macro: Replaying", RGB(120,255,120), kBannerX, kBannerY);
    } else {
        DirectDrawHook::UpdatePermanentMessage(s_macroBannerId, "Macro: Replaying", RGB(120,255,120));
    }
    int totalTicks = 0; for (auto &sp : s_slots[slotIdx].spans) totalTicks += sp.ticks;
    LogOut("[MACRO][PLAY] started slot=" + std::to_string(s_curSlot.load()) +
           " spans=" + std::to_string((int)s_slots[slotIdx].spans.size()) +
           " ticks=" + std::to_string(totalTicks) +
           " streamBytes=" + std::to_string((int)s_slots[slotIdx].macroStream.size()) +
           " sink=P2 (poll-override)", true);
    // Dump streams at playback start for analysis
    {
        int slotNum = s_curSlot.load();
        LogVectorHex("macroStream", slotNum, "play-start", s_slots[slotIdx].macroStream);
        LogVectorHex("bufStream", slotNum, "play-start", s_slots[slotIdx].bufStream);
        LogVectorU16("bufCountsPerTick", slotNum, "play-start", s_slots[slotIdx].bufCountsPerTick);
        LogVectorU16("bufIndexPerTick", slotNum, "play-start", s_slots[slotIdx].bufIndexPerTick);
        if (!s_streamFacingPerTick.empty()) {
            std::vector<uint8_t> face;
            face.reserve(s_streamFacingPerTick.size());
            for (auto f : s_streamFacingPerTick) face.push_back((uint8_t)f);
            LogVectorHex("streamFacingPerTick(+1/-1/0)", slotNum, "play-start", face);
        }
        LogPerTickOverview(slotNum, "play-start", s_slots[slotIdx].macroStream, s_slots[slotIdx].bufCountsPerTick, s_slots[slotIdx].bufIndexPerTick);
    }
}

void Stop() {
    // Stop can be called anytime; no gating so it can clean up if needed
    State st = s_state.load();
    if (st == State::Recording) FinishRecording();
    s_state.store(State::Idle);
    ResetPlayback();
    g_manualInputOverride[2].store(false);
    g_forceBypass[2].store(false);
    g_pollOverrideActive[2].store(false);
    g_pollOverrideMask[2].store(0);
    // Remove banner and restore side
    if (s_macroBannerId != -1) { DirectDrawHook::RemovePermanentMessage(s_macroBannerId); s_macroBannerId = -1; }
    if (g_p2ControlOverridden) RestoreP2ControlState();
    int prev = s_prevLocalSide.load();
    if (prev == 0 || prev == 1) {
        SwitchPlayers::SetLocalSide(prev);
        s_prevLocalSide.store(-1);
    }
}

int GetCurrentSlot() { return s_curSlot.load(); }
void SetCurrentSlot(int slot) { s_curSlot.store(ClampSlot(slot)); }
int GetSlotCount() { return kMaxSlots; }
bool IsSlotEmpty(int slot) { slot = ClampSlot(slot) - 1; return !s_slots[slot].hasData || s_slots[slot].spans.empty(); }
MacroController::State GetState() { return s_state.load(); }

std::string GetStatusLine() {
    std::ostringstream os;
    os << "Macro " << StateName(s_state.load()) << " | Slot " << s_curSlot.load();
    return os.str();
}

SlotStats GetSlotStats(int slot) {
    SlotStats stats{};
    slot = ClampSlot(slot) - 1;
    if (slot < 0) slot = 0; if (slot >= kMaxSlots) slot = kMaxSlots - 1;
    const Slot& s = s_slots[slot];
    stats.spanCount = static_cast<int>(s.spans.size());
    stats.totalTicks = 0; for (const auto& sp : s.spans) stats.totalTicks += sp.ticks;
    stats.bufEntries = static_cast<int>(s.bufStream.size());
    stats.bufTicks = static_cast<int>(s.bufCountsPerTick.size());
    stats.bufIndexTicks = static_cast<int>(s.bufIndexPerTick.size());
    stats.bufStartIdx = s.bufStartIdx;
    stats.bufEndIdx = s.bufEndIdx;
    stats.hasData = s.hasData && !s.spans.empty();
    return stats;
}

void NextSlot() {
    if (GetCurrentGamePhase() != GamePhase::Match || !AreCharactersInitialized()) return;
    int cur = s_curSlot.load();
    cur++; if (cur > kMaxSlots) cur = 1; s_curSlot.store(cur);
}
void PrevSlot() {
    if (GetCurrentGamePhase() != GamePhase::Match || !AreCharactersInitialized()) return;
    int cur = s_curSlot.load();
    cur--; if (cur < 1) cur = kMaxSlots; s_curSlot.store(cur);
}

} // namespace MacroController
