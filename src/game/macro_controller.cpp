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
#include <cctype>
#include <algorithm>

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
        // Optional diagnostics: reason code per tick (U=unfrozen frameDiv, B=frozen buf advance, S=frozen step, X=frozen both)
        std::vector<char> tickReason;
        // Raw immediate mask sampled at start of each recorder tick BEFORE any merging / union logic.
        std::vector<uint8_t> immPerTick;
        // Latest buffer entry value (ring[idx-1]) as observed this tick BEFORE any synthetic write insertion.
        std::vector<uint8_t> bufLatestPerTick;
        // Optional: full snapshot of the entire input buffer ring each tick (heavy, gated by constant below).
        std::vector<std::vector<uint8_t>> fullBufferSnapshots;
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
    char s_recLastReason = '?'; // reason code for current span
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
    // When finishing playback, we optionally inject one neutral frame (poll override = 0)
    // to guarantee the engine writes a neutral value into the input history immediately.
    bool s_finishing = false;
    int  s_finishNeutralFrames = 0; // number of neutral frames to inject for a full clear tick (3 subframes)
    bool s_finishPendingClearTick = false; // wait until next tick boundary (s_frameDiv==0) to start neutral clear tick
    // After the neutral clear tick, hold a short guard window to wait for the engine
    // to commit a detected command into a MoveID (special/super/dash) before we
    // restore control. This mitigates repeats caused by restoring too early.
    bool s_finishGuardActive = false;
    int  s_finishGuardFramesLeft = 0;  // internal frames (192 Hz)
    uint16_t s_finishGuardStartMoveId = 0;
    // Baseline immediate mask for the current 64 Hz tick
    uint8_t s_baselineMask = 0;

    // Progress pacing: we step at the 64 Hz ImmediateInput cadence by counting internal frames (192 Hz)
    int s_frameDiv = 0; // 0..2 cycles; advance when hits 0
    // Diagnostics: detect abnormal cycle lengths (should always be 3 calls between div0 events if Tick called once per internal frame)
    static int s_callsSinceDiv0 = 0;
    static bool s_firstDiv0Seen = false;

    // Gating constants for heavy diagnostics
    constexpr bool kEnableFullBufferSnapshots = true; // set false if memory/log size becomes an issue

    // Unified freeze detector for macro timing.
    // Treat as frozen when:
    //  * Not in Match phase (avoid progressing during intros / menus)
    //  * Practice pause flag is set
    //  * Gamespeed byte is 0 (engine globally frozen)
    // We intentionally do NOT require both pause flag and gamespeed=0 because
    // some pause paths (official toggle) may leave gamespeed non‑zero while a
    // patch-based or flag-only pause is active, and vice versa for emergency
    // gamespeed freezes without the flag. Macros should stall in all of those.
    bool ReadGamespeedFrozen() {
        const GamePhase phase = GetCurrentGamePhase();
        bool phaseFrozen = (phase != GamePhase::Match);
        bool pauseFlag  = PauseIntegration::IsPracticePaused();
        bool speedFrozen = PauseIntegration::IsGameSpeedFrozen();
        bool frozen = phaseFrozen || pauseFlag || speedFrozen;
        // One-shot transition log (helps verify correctness without log spam)
        static bool s_lastFrozen = frozen;
        if (frozen != s_lastFrozen) {
            std::ostringstream oss;
            oss << "[MACRO][FRZ] " << (frozen ? "ENTER" : "EXIT")
                << " freeze phase=" << (int)phase
                << " phaseFrozen=" << (phaseFrozen?1:0)
                << " practicePause=" << (pauseFlag?1:0)
                << " gamespeedFrozen=" << (speedFrozen?1:0);
            LogOut(oss.str(), true);
            s_lastFrozen = frozen;
        }
        return frozen;
    }

    inline int ClampSlot(int s){ if (s < 1) return 1; if (s > kMaxSlots) return kMaxSlots; return s; }

    static std::string ToHexString(int value, int width = 2) {
        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
        return oss.str();
    }

    // Helper: classify activation-worthy MoveIDs to gate control restore
    static bool IsActivationMove(uint16_t mv) {
        // Supers (>=300), specials (>=250), dash starts (explicit), Kaori dash special-case
        if (mv >= 300) return true;           // supers
        if (mv >= 250) return true;           // specials and Kaori forward dash start (250)
        if (mv == FORWARD_DASH_START_ID) return true;   // 163
        if (mv == BACKWARD_DASH_START_ID) return true;  // 165
        return false;
    }

    // Direction mask <-> numpad helpers
    static char DirMaskToNumpad(uint8_t m) {
        bool u = (m & GAME_INPUT_UP) != 0;
        bool d = (m & GAME_INPUT_DOWN) != 0;
        bool l = (m & GAME_INPUT_LEFT) != 0;
        bool r = (m & GAME_INPUT_RIGHT) != 0;
        // Resolve invalid combos by neutral (5)
        if ((u && d) || (l && r)) return '5';
        if (u && r) return '9';
        if (u && l) return '7';
        if (d && r) return '3';
        if (d && l) return '1';
        if (u) return '8';
        if (d) return '2';
        if (r) return '6';
        if (l) return '4';
        return '5';
    }

    static uint8_t NumpadCharToDirMask(char c) {
        switch (c) {
            case '1': return (GAME_INPUT_DOWN | GAME_INPUT_LEFT);
            case '2': return (GAME_INPUT_DOWN);
            case '3': return (GAME_INPUT_DOWN | GAME_INPUT_RIGHT);
            case '4': return (GAME_INPUT_LEFT);
            case '5': return 0;
            case '6': return (GAME_INPUT_RIGHT);
            case '7': return (GAME_INPUT_UP | GAME_INPUT_LEFT);
            case '8': return (GAME_INPUT_UP);
            case '9': return (GAME_INPUT_UP | GAME_INPUT_RIGHT);
            case 'N': return 0; // alias
            default:  return 0xFF; // invalid sentinel
        }
    }

    static std::string MaskToToken(uint8_t m) {
        uint8_t dir = m & (GAME_INPUT_UP | GAME_INPUT_DOWN | GAME_INPUT_LEFT | GAME_INPUT_RIGHT);
        std::string t;
        t.push_back(DirMaskToNumpad(dir));
        // Append buttons in A..D order
        if (m & GAME_INPUT_A) t.push_back('A');
        if (m & GAME_INPUT_B) t.push_back('B');
        if (m & GAME_INPUT_C) t.push_back('C');
        if (m & GAME_INPUT_D) t.push_back('D');
        return t;
    }

    static bool TryTokenToMask(const std::string& tok, uint8_t& outMask) {
        if (tok.empty()) return false;
        // Accept 'N' or 'n' as neutral
        if (tok.size() == 1 && (tok[0] == 'N' || tok[0] == 'n')) { outMask = 0; return true; }
        char d = tok[0];
        if (d >= 'a' && d <= 'z') d = (char)std::toupper((unsigned char)d);
        uint8_t dir = NumpadCharToDirMask(d);
        if (dir == 0xFF) return false;
        uint8_t btn = 0;
        for (size_t i = 1; i < tok.size(); ++i) {
            char c = tok[i];
            if (c >= 'a' && c <= 'z') c = (char)std::toupper((unsigned char)c);
            if (c == 'A') btn |= GAME_INPUT_A;
            else if (c == 'B') btn |= GAME_INPUT_B;
            else if (c == 'C') btn |= GAME_INPUT_C;
            else if (c == 'D') btn |= GAME_INPUT_D;
            else return false; // unexpected char
        }
        outMask = (dir | btn);
        return true;
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

    static void LogTickReasons(const char* phase, int slot, const std::vector<char>& reasons) {
        std::ostringstream line;
        line << "[MACRO][DUMP] slot=" << slot << " " << phase << " tickReason (" << reasons.size() << "):";
        LogOut(line.str(), true);
        line.str(""); line.clear();
        size_t perLine = 64; size_t count = 0;
        for (size_t i = 0; i < reasons.size(); ++i) {
            if (count == 0) line << "[MACRO][DUMP]   ";
            line << reasons[i];
            if (i + 1 < reasons.size()) line << ' ';
            if (++count >= perLine) {
                LogOut(line.str(), true);
                line.str(""); line.clear();
                count = 0;
            }
        }
        if (count > 0) LogOut(line.str(), true);
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
        s_finishing = false; s_finishNeutralFrames = 0; s_finishPendingClearTick = false;
        s_finishGuardActive = false; s_finishGuardFramesLeft = 0; s_finishGuardStartMoveId = 0;
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
                LogTickReasons("rec-finish", slotNum, s_slots[slotIdx].tickReason);
        LogPerTickOverview(slotNum, "rec-finish", s_slots[slotIdx].macroStream, s_slots[slotIdx].bufCountsPerTick, s_slots[slotIdx].bufIndexPerTick);
        LogVectorHex("immPerTick(raw)", slotNum, "rec-finish", s_slots[slotIdx].immPerTick);
        LogVectorHex("bufLatestPerTick", slotNum, "rec-finish", s_slots[slotIdx].bufLatestPerTick);
        if (kEnableFullBufferSnapshots) {
            std::ostringstream ss; ss << "[MACRO][DUMP] slot=" << slotNum << " rec-finish fullBufferSnapshots count=" << s_slots[slotIdx].fullBufferSnapshots.size() << " (each=" << (int)INPUT_BUFFER_SIZE << ")"; LogOut(ss.str(), true);
            // To avoid massive spam, dump only first and last snapshot (if distinct)
            if (!s_slots[slotIdx].fullBufferSnapshots.empty()) {
                auto dumpSnap = [&](size_t i, const char* tag){
                    const auto &snap = s_slots[slotIdx].fullBufferSnapshots[i];
                    std::ostringstream hdr; hdr << "[MACRO][DUMP]   snapshot[" << i << "](" << tag << "):"; LogOut(hdr.str(), true);
                    std::ostringstream line; size_t count=0; for (size_t b=0;b<snap.size();++b){ if(count==0){ line<<"[MACRO][DUMP]     "; }
                        line<< std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)snap[b]; if (b+1<snap.size()) line<<' ';
                        if(++count>=32){ LogOut(line.str(), true); line.str(""); line.clear(); count=0; }
                    }
                    if(count>0) LogOut(line.str(), true);
                };
                dumpSnap(0, "first");
                if (s_slots[slotIdx].fullBufferSnapshots.size() > 1) dumpSnap(s_slots[slotIdx].fullBufferSnapshots.size()-1, "last");
            }
        }
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
    if (++s_frameDiv >= 3) {
        s_frameDiv = 0;
        if (s_firstDiv0Seen) {
            if (s_callsSinceDiv0 != 3) {
                LogOut("[MACRO][DIAG] frameDiv cycleLen=" + std::to_string(s_callsSinceDiv0) + " (expected 3) -- possible double Tick invocation", true);
            }
        } else {
            s_firstDiv0Seen = true;
        }
        s_callsSinceDiv0 = 0;
    }
    ++s_callsSinceDiv0;

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
        // Frame-step counter (practice step advance) – captures a manual single-frame advance even if
        // the input buffer ring did not write a new entry (e.g. pure neutral frame).
        bool stepAdvanced = false;
        if (frozen) {
            stepAdvanced = PauseIntegration::ConsumeStepAdvance();
        }
        bool shouldAdvance = false;
        if (!frozen) {
            shouldAdvance = (s_frameDiv == 0);
        } else {
            // When frozen (paused), advance if either the buffer index moved OR a step counter increment occurred.
            // This preserves neutral delay frames during frame stepping that previously were dropped when the
            // engine skipped writing a redundant neutral buffer entry.
            shouldAdvance = (bufAdvanced || stepAdvanced);
            if (shouldAdvance && stepAdvanced && !bufAdvanced) {
                LogOut("[MACRO][REC] step-advance tick (no buffer write)", true);
            }
        }
        if (!shouldAdvance) return;
        char reasonCode='?';
        if (!frozen) reasonCode='U';
        else if (bufAdvanced && stepAdvanced) reasonCode='X';
        else if (bufAdvanced) reasonCode='B';
        else if (stepAdvanced) reasonCode='S';
        // If buffer-freeze is active for P2, avoid progressing to keep streams aligned
        if (g_bufferFreezingActive.load() && (g_activeFreezePlayer.load() == 2 || g_activeFreezePlayer.load() == 0)) return;
        uint8_t immMask = ReadRecordSourceMask();
        uint8_t immMaskRaw = immMask; // preserve original before any merging
        int facing = ReadFacingSign(2);
        uint8_t buf  = 0;
        bool noWritesThisTick = false; // track if engine produced zero buffer writes (pre-synthesis)
        uint8_t latestBufValPreSynth = ReadP2BufferLatestMask();
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
                } else {
                    noWritesThisTick = true;
                }
            }
        }
        if (buf == 0) buf = ReadP2BufferLatestMask();
        // If we advanced due to a frame-step (no buffer writes, likely neutral delay) and the immediate
        // differs from the last recorded mask, treat immediate as authoritative for this logical tick.
        // This prevents neutral delays being merged into the prior action span because the last buffer
        // entry still held previous button bits.
        if (noWritesThisTick) {
            // Compare against last recorded span mask (s_recLastMask) only if we already have progress.
            uint8_t lastMask = (s_recSpanTicks > 0) ? (uint8_t)(s_recLastMask & 0xFF) : 0xFF; // 0xFF sentinel so first span always sets
            if (s_recSpanTicks == 0 || immMask != lastMask) {
                buf = immMask; // override with immediate state to create/extend proper neutral span
            }
        }
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
            if (s_slots[slotIdx].tickReason.size() < s_slots[slotIdx].macroStream.size()) {
                s_slots[slotIdx].tickReason.push_back(reasonCode);
            }
            // Per-tick raw immediate & latest buffer (pre-synthetic) capture
            s_slots[slotIdx].immPerTick.push_back(immMaskRaw);
            s_slots[slotIdx].bufLatestPerTick.push_back(latestBufValPreSynth);
            if (kEnableFullBufferSnapshots) {
                uintptr_t p2PtrSnap = GetPlayerPointer(2);
                std::vector<uint8_t> snap;
                if (p2PtrSnap) {
                    snap.resize(INPUT_BUFFER_SIZE, 0);
                    for (size_t bi = 0; bi < snap.size(); ++bi) {
                        uint8_t v=0; SafeReadMemory(p2PtrSnap + INPUT_BUFFER_OFFSET + (uintptr_t)bi, &v, sizeof(v)); snap[bi]=v;
                    }
                }
                s_slots[slotIdx].fullBufferSnapshots.push_back(std::move(snap));
            }
        }
        // Ensure we "capture the buffer" every recorder tick: if the engine produced
        // zero new raw buffer writes this tick (common for neutral frame-steps), synthesize
        // one entry so playback can reproduce a per-tick buffer cadence and precise delays.
        {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            if (!s_slots[slotIdx].bufCountsPerTick.empty() && s_slots[slotIdx].bufCountsPerTick.back() == 0) {
                s_slots[slotIdx].bufStream.push_back(mask); // synthetic neutral/held state
                s_slots[slotIdx].bufCountsPerTick.back() = 1;
                buf = mask; // treat buffer value as this synthetic entry for span comparison logic
                LogOut(std::string("[MACRO][REC] synthetic-buf write (neutral tick) reason=") + reasonCode, true);
            }
        }
        if (s_recSpanTicks == 0) {
            s_recLastMask = mask; s_recLastBuf = mask; s_recLastFacing = facing; s_recSpanTicks = 1; s_recLastReason = reasonCode;
        } else if (mask == s_recLastMask && buf == s_recLastBuf && facing == s_recLastFacing) {
            // Merge regardless of reason; if provenance differs mark as Mixed 'M'. This allows
            // a contiguous neutral delay built from step (S) and unfrozen (U) ticks to compress into
            // a single span length, restoring the expected large delay counts (e.g. 24).
            if (s_recLastReason != reasonCode) s_recLastReason = 'M';
            s_recSpanTicks++;
        } else {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            s_slots[slotIdx].spans.push_back({ static_cast<Mask>(s_recLastMask & 0xFF), static_cast<Mask>(s_recLastBuf & 0xFF), s_recSpanTicks, (int8_t)s_recLastFacing });
            s_slots[slotIdx].hasData = true;
            // Log completed span (immediate vs buffer)
            LogOut(std::string("[MACRO][REC] span imm=0x") + ToHexString((int)(s_recLastMask & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastMask & 0xFF)) + ") buf=0x" + ToHexString((int)(s_recLastBuf & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastBuf & 0xFF)) + ") ticks=" + std::to_string(s_recSpanTicks) +
                   " facing=" + std::to_string(s_recLastFacing) + " reason=" + s_recLastReason, true);
            s_recLastMask = mask; s_recLastBuf = mask; s_recLastFacing = facing; s_recSpanTicks = 1; s_recLastReason = reasonCode;
        }
    } else if (st == State::Replaying) {
        // Finish guard: after neutral clear tick, hold neutral until moveID activation or timeout
        if (s_finishGuardActive) {
            // Keep neutral override while guarding
            g_pollOverrideMask[2].store(0, std::memory_order_relaxed);
            g_pollOverrideActive[2].store(true, std::memory_order_relaxed);
            // Probe current MoveID
            uint16_t mv = GetPlayerMoveID(2);
            bool activated = IsActivationMove(mv);
            if (activated || s_finishGuardFramesLeft <= 0) {
                // Finalize: restore control and cleanup
                s_finishGuardActive = false;
                s_state.store(State::Idle);
                ImmediateInput::Clear(2);
                (void)ClearPlayerCommandFlags(2);
                g_pollOverrideActive[2].store(false, std::memory_order_relaxed);
                g_forceBypass[2].store(false);
                g_injectImmediateOnly[2].store(false);
                if (s_macroBannerId != -1) { DirectDrawHook::RemovePermanentMessage(s_macroBannerId); s_macroBannerId = -1; }
                if (g_p2ControlOverridden) RestoreP2ControlState();
                int slotIdx = ClampSlot(s_curSlot.load()) - 1;
                LogOut(std::string("[MACRO][PLAY] finish-guard exit ") + (activated?"on-activation":"on-timeout") +
                       " slot=" + std::to_string(s_curSlot.load()) +
                       " lastMoveID=" + std::to_string((int)mv) +
                       " streamBytes=" + std::to_string((int)s_slots[slotIdx].macroStream.size()) +
                       " bufWrites=" + std::to_string((int)s_slots[slotIdx].bufStream.size()), true);
                DirectDrawHook::AddMessage("Macro: Replay finished", "MACRO", RGB(180,255,180), 1200, 0, 120);
                return;
            }
            // Count down guard frames
            if (s_finishGuardFramesLeft > 0) s_finishGuardFramesLeft--;
            return;
        }
        // If we are in or pending the finishing phase, handle the neutral clear tick.
        if (s_finishPendingClearTick && s_frameDiv == 0) {
            // Start a full neutral clear tick at the next 64 Hz boundary (3 subframes)
            s_finishPendingClearTick = false;
            s_finishing = true;
            s_finishNeutralFrames = 3; // full tick at 192 Hz pacing
            // Clear any residual queues/baseline
            s_tickBufQueue.clear();
            s_tickBufHead = 0;
            s_writesLeftThisTick = 0;
            s_baselineMask = 0;
        }
        // While waiting for the next boundary, maintain neutral override to avoid tail holds
        if (s_finishPendingClearTick && s_frameDiv != 0) {
            g_pollOverrideMask[2].store(0, std::memory_order_relaxed);
            g_pollOverrideActive[2].store(true, std::memory_order_relaxed);
            return;
        }
        if (s_finishing) {
            // Drive a neutral poll override for the duration of the clear tick
            g_pollOverrideMask[2].store(0, std::memory_order_relaxed);
            g_pollOverrideActive[2].store(true, std::memory_order_relaxed);
            if (s_finishNeutralFrames > 0) {
                s_finishNeutralFrames--;
                return; // keep neutral for remaining subframes
            }
            // After the neutral clear tick, enter a short guard window waiting for MoveID activation
            s_finishing = false;
            s_finishGuardActive = true;
            // Default guard window: 6 internal frames (~2 visual frames)
            s_finishGuardFramesLeft = 6;
            s_finishGuardStartMoveId = GetPlayerMoveID(2);
            // Keep neutral override active during guard and clear command flags once
            (void)ClearPlayerCommandFlags(2);
            return;
        }
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
            (void)ClearPlayerCommandFlags(2);
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
                    // Facing-aware: if recorded facing is unknown (0), assume P1-facing (+1)
                    int8_t recFacing = 0;
                    if (!s_streamFacingPerTick.empty() && s_playStreamIndex < s_streamFacingPerTick.size()) recFacing = s_streamFacingPerTick[s_playStreamIndex];
                    if (recFacing == 0) recFacing = +1;
                    int curFacing = ReadFacingSign(2);
                    if (curFacing != 0 && recFacing != curFacing) {
                        mask = FlipMaskHoriz(mask);
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
                            int8_t recFacingRaw = 0;
                            if (!s_streamFacingPerTick.empty() && s_playStreamIndex < s_streamFacingPerTick.size()) recFacingRaw = s_streamFacingPerTick[s_playStreamIndex];
                            if (recFacingRaw == 0) recFacingRaw = +1;
                            int curFacingRaw = ReadFacingSign(2);
                            if (curFacingRaw != 0 && recFacingRaw != curFacingRaw) {
                                raw = FlipMaskHoriz(raw);
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
                // Defer to the next tick boundary, then inject a full neutral clear tick.
                // IMPORTANT: Neutralize immediately so we don't keep holding the last mask for leftover subframes.
                s_finishPendingClearTick = true;
                s_baselineMask = 0;
                g_pollOverrideMask[2].store(0, std::memory_order_relaxed);
                g_pollOverrideActive[2].store(true, std::memory_order_relaxed);
                // Early clear of command flags as we enter finish sequence
                (void)ClearPlayerCommandFlags(2);
                return;
            }
        } else {
            // Fallback to existing RLE span playback (legacy path)
            if (s_frameDiv == 0 && s_playSpanRemaining <= 0) {
                if (s_playIndex >= s_slots[slotIdx].spans.size()) {
                    // End via spans: defer to next tick boundary then perform full neutral clear tick
                    s_finishPendingClearTick = true;
                    // Early clear of command flags as we enter finish sequence (spans path)
                    (void)ClearPlayerCommandFlags(2);
                    return;
                }
                const RLESpan &sp = s_slots[slotIdx].spans[s_playIndex++];
                s_playSpanRemaining = sp.ticks;
                // Determine current facing and flip if needed
                int curFacing = ReadFacingSign(2);
                uint8_t maskToApply = sp.mask;
                int recFacing = (sp.facing == 0) ? +1 : sp.facing;
                if (curFacing != 0 && recFacing != curFacing) {
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
            bool readOk = (p && SafeReadMemory((uintptr_t)p + PRACTICE_OFF_LOCAL_SIDE_IDX, &curLocal, sizeof(curLocal)));
            if (readOk) {
                s_prevLocalSide.store(curLocal);
                if (curLocal != 1) {
                    SwitchPlayers::SetLocalSide(1); // make P2 local for easier recording
                }
            } else {
                // Vanilla fallback: if we can't read the practice controller local side,
                // still force P2 local so the user controls P2 during macro recording.
                // This path does not touch Revival-specific state and uses our unified
                // side-switcher which already guards Revival vs vanilla under the hood.
                SwitchPlayers::SetLocalSide(1);
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
            s_slots[slotIdx].tickReason.clear();
            s_slots[slotIdx].immPerTick.clear();
            s_slots[slotIdx].bufLatestPerTick.clear();
            s_slots[slotIdx].fullBufferSnapshots.clear();
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
    // Ensure engine command flags are cleared on manual stop
    (void)ClearPlayerCommandFlags(2);
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
bool IsSlotEmpty(int slot) {
    slot = ClampSlot(slot) - 1;
    const Slot& s = s_slots[slot];
    // Consider either spans or stream as data; require hasData true
    bool hasAny = (!s.spans.empty() || !s.macroStream.empty());
    return !(s.hasData && hasAny);
}
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
    stats.hasData = s.hasData && (!s.spans.empty() || !s.macroStream.empty());
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

// ---- Serialization / Deserialization implementation ----
namespace MacroController {

std::string SerializeSlot(int slot, bool includeBuffers) {
    slot = ClampSlot(slot);
    const Slot& s = s_slots[slot - 1];
    // Build per-tick macro masks
    std::vector<uint8_t> ticks;
    std::vector<int8_t> faces; // recorded facing per tick (-1 left, +1 right, 0 unknown)
    if (!s.macroStream.empty()) {
        ticks = s.macroStream; // copy
        // Try to derive per-tick facing from spans if available
        if (!s.spans.empty()) {
            faces.reserve(ticks.size());
            for (const auto& sp : s.spans) {
                for (int t = 0; t < sp.ticks && faces.size() < ticks.size(); ++t) faces.push_back(sp.facing);
                if (faces.size() >= ticks.size()) break;
            }
            while (faces.size() < ticks.size()) faces.push_back(0);
        } else {
            faces.assign(ticks.size(), 0);
        }
    } else {
        // Expand spans
        for (const auto& sp : s.spans) {
            for (int t = 0; t < sp.ticks; ++t) {
                ticks.push_back(sp.mask);
                faces.push_back(sp.facing);
            }
        }
        if (faces.size() < ticks.size()) faces.resize(ticks.size(), 0);
    }
    std::ostringstream out;
    out << "EFZMACRO 1";
    if (ticks.empty()) return out.str();

    // Prepare per-tick buffer slices if requested
    size_t bufPos = 0;
    auto buildBufToken = [&](size_t tickIndex) {
        std::ostringstream bt;
        uint16_t k = 0;
        if (tickIndex < s.bufCountsPerTick.size()) k = s.bufCountsPerTick[tickIndex];
        bt << "{" << k << ":";
        if (k > 0) bt << ' ';
        for (uint16_t i = 0; i < k && bufPos < s.bufStream.size(); ++i) {
            uint8_t v = s.bufStream[bufPos++];
            // Normalize buffer value to P1-facing if we know recorded facing for this tick
            int8_t f = (tickIndex < faces.size()) ? faces[tickIndex] : 0;
            if (f == -1) v = FlipMaskHoriz(v);
            // Prefer token form when it matches our mapping; fallback to hex otherwise
            std::string valTok = MaskToToken(v);
            // No way to disambiguate invalid combos, but MaskToToken always yields something;
            // Emit hex when v has impossible combinations (both U&D or L&R)
            bool u = (v & GAME_INPUT_UP) != 0, d = (v & GAME_INPUT_DOWN) != 0, l = (v & GAME_INPUT_LEFT) != 0, r = (v & GAME_INPUT_RIGHT) != 0;
            if ((u && d) || (l && r)) {
                bt << "0x" << ToHexString((int)v, 2);
            } else {
                bt << valTok;
            }
            if (i + 1 < k) bt << ' ';
        }
        bt << "}";
        return bt.str();
    };

    // Build tokens with optional RLE compression
    std::vector<std::string> perTick;
    perTick.reserve(ticks.size());
    for (size_t i = 0; i < ticks.size(); ++i) {
        uint8_t m = ticks[i];
        // Normalize to P1-facing in text: if recorded facing was left, flip 4/6 (and diagonals)
        int8_t f = (i < faces.size()) ? faces[i] : 0;
        if (f == -1) m = FlipMaskHoriz(m);
        std::string tok = MaskToToken(m);
        if (includeBuffers) {
            tok += ' ';
            tok += buildBufToken(i);
        }
        perTick.push_back(std::move(tok));
    }
    // RLE compress only identical full tokens
    std::ostringstream seq;
    seq << ' ';
    size_t i = 0;
    while (i < perTick.size()) {
        size_t j = i + 1;
        while (j < perTick.size() && perTick[j] == perTick[i]) ++j;
        size_t run = j - i;
        seq << perTick[i];
        if (run > 1) seq << 'x' << run;
        if (j < perTick.size()) seq << ' ';
        i = j;
    }
    out << seq.str();
    return out.str();
}

static void ClearSlotForImport(Slot& s) {
    s.spans.clear();
    s.macroStream.clear();
    s.bufStream.clear();
    s.bufCountsPerTick.clear();
    s.bufIndexPerTick.clear();
    s.tickReason.clear();
    s.immPerTick.clear();
    s.bufLatestPerTick.clear();
    s.fullBufferSnapshots.clear();
    s.bufStartIdx = 0;
    s.bufEndIdx = 0;
    s.hasData = false;
}

static void BuildSpansFromStream(Slot& s) {
    s.spans.clear();
    if (s.macroStream.empty()) return;
    uint8_t last = s.macroStream[0];
    int count = 1;
    for (size_t i = 1; i < s.macroStream.size(); ++i) {
        if (s.macroStream[i] == last) {
            ++count;
        } else {
            s.spans.push_back({ last, last, count, 0 });
            last = s.macroStream[i];
            count = 1;
        }
    }
    s.spans.push_back({ last, last, count, 0 });
}

bool DeserializeSlot(int slot, const std::string& text, std::string& errorOut) {
    errorOut.clear();
    slot = ClampSlot(slot);
    Slot& dst = s_slots[slot - 1];

    // Tokenize with brace-aware scanning
    std::vector<std::string> tokens;
    tokens.reserve(256);
    size_t n = text.size();
    size_t p = 0;
    auto skipSpace = [&](size_t& i){ while (i < n && std::isspace((unsigned char)text[i])) ++i; };
    skipSpace(p);
    // Optional header "EFZMACRO 1"
    if (p < n) {
        size_t hdrEnd = p;
        // Read first two non-space tokens to check header
        std::string t1, t2;
        while (hdrEnd < n && !std::isspace((unsigned char)text[hdrEnd])) ++hdrEnd;
        t1 = text.substr(p, hdrEnd - p);
        p = hdrEnd; skipSpace(p);
        hdrEnd = p; while (hdrEnd < n && !std::isspace((unsigned char)text[hdrEnd])) ++hdrEnd;
        t2 = text.substr(p, hdrEnd - p);
        if (!t1.empty() && !t2.empty() && (_stricmp(t1.c_str(), "EFZMACRO") == 0)) {
            // Verify version
            if (t2 != "1") { errorOut = "Unsupported macro version: " + t2; return false; }
            p = hdrEnd; // move beyond version
        } else {
            // No header; reset to start to parse normally
            p = 0;
        }
    }
    skipSpace(p);
    while (p < n) {
        if (std::isspace((unsigned char)text[p])) { ++p; continue; }
        size_t start = p;
        if (text[p] == '{') {
            // Should not start with group; groups attach to preceding tick token
            errorOut = "Unexpected '{' without preceding tick token";
            return false;
        }
        // Read until whitespace OR brace start (we'll include following group as part of this token pack)
        while (p < n && !std::isspace((unsigned char)text[p])) {
            if (text[p] == '{') break;
            ++p;
        }
        size_t baseEnd = p;
        // Allow optional whitespace between base token and its attached brace group
        // Example: "5 {3: ...}" should be treated as a single pack just like "5{3: ...}"
        if (p < n && std::isspace((unsigned char)text[p])) {
            size_t q = p;
            // Peek past spaces to see if a brace group follows
            while (q < n && std::isspace((unsigned char)text[q])) ++q;
            if (q < n && text[q] == '{') {
                p = q; // Attach the upcoming brace group to this pack
            }
        }
        // Capture any attached brace group including spaces inside until matching '}'
        int brace = 0;
        if (p < n && text[p] == '{') {
            brace = 1; ++p;
            while (p < n && brace > 0) {
                if (text[p] == '{') ++brace;
                else if (text[p] == '}') --brace;
                ++p;
            }
            if (brace != 0) { errorOut = "Unterminated buffer group"; return false; }
        }
        // Attach an immediate or space-separated repeat suffix xN/XN to this pack
        // Example forms to accept: "5C}x12", "5C} x12", and even "5C x12" (no group)
        if (p < n) {
            size_t qx = p;
            // Skip any spaces between token and suffix
            while (qx < n && std::isspace((unsigned char)text[qx])) ++qx;
            if (qx < n && (text[qx] == 'x' || text[qx] == 'X')) {
                size_t r = qx + 1;
                size_t rStart = r;
                while (r < n && std::isdigit((unsigned char)text[r])) ++r;
                if (r > rStart) {
                    p = r; // consume suffix into this token pack
                }
            }
        }
        size_t end = p;
        tokens.push_back(text.substr(start, end - start));
        skipSpace(p);
    }

    if (tokens.empty()) {
        // Allow clearing slot
        ClearSlotForImport(dst);
        return true;
    }

    // Parse sequence
    std::vector<uint8_t> macro;
    std::vector<uint16_t> counts;
    std::vector<uint8_t> buf;

    auto parseUInt = [](const std::string& s, size_t i, uint32_t& out)->size_t{
        out = 0; size_t start = i; while (i < s.size() && std::isdigit((unsigned char)s[i])) { out = out*10 + (s[i]-'0'); ++i; }
        return (i > start) ? i : start;
    };

    for (size_t iTok = 0; iTok < tokens.size(); ++iTok) {
        const std::string& packFull = tokens[iTok];
        // Handle trailing repeat suffix xN or XN at end of pack (applies to both base-only and base+group forms)
        uint32_t repeat = 1;
        std::string pack = packFull;
        if (!pack.empty()) {
            size_t end = pack.size();
            size_t j = end;
            // Move j back over trailing digits
            while (j > 0 && std::isdigit((unsigned char)pack[j - 1])) --j;
            if (j > 0 && j < end && (pack[j - 1] == 'x' || pack[j - 1] == 'X')) {
                // Parse repeat
                uint32_t val = 0; size_t k = j; k = parseUInt(pack, k, val);
                if (k == j || val == 0) { errorOut = "Invalid repeat suffix in '" + pack + "'"; return false; }
                repeat = val;
                // Remove the suffix from the working pack
                pack = pack.substr(0, j - 1);
                // Trim trailing spaces
                while (!pack.empty() && std::isspace((unsigned char)pack.back())) pack.pop_back();
            }
        }
        // Split into base and optional group on the adjusted pack (without trailing xN)
        size_t bracePos = pack.find('{');
        std::string base = (bracePos == std::string::npos) ? pack : pack.substr(0, bracePos);
        std::string group = (bracePos == std::string::npos) ? std::string() : pack.substr(bracePos);
        // Trim trailing spaces from base
        while (!base.empty() && std::isspace((unsigned char)base.back())) base.pop_back();
        std::string baseTok = base;
    // Parse base token mask
        uint8_t baseMask = 0;
        if (!TryTokenToMask(baseTok, baseMask)) { errorOut = "Bad tick token: '" + baseTok + "'"; return false; }

        // Optional group parsing {k: v1 v2 ...}
        std::vector<uint8_t> thisTickBuf;
        uint16_t thisTickK = 0;
        if (!group.empty()) {
            // Strip braces
            if (group.front() != '{') { errorOut = "Malformed buffer group in '" + packFull + "'"; return false; }
            if (group.back() != '}') { errorOut = "Malformed buffer group in '" + packFull + "'"; return false; }
            std::string inner = group.substr(1, group.size()-2);
            // Parse k:
            size_t q = 0; while (q < inner.size() && std::isspace((unsigned char)inner[q])) ++q;
            uint32_t kVal = 0; size_t q2 = parseUInt(inner, q, kVal);
            if (q2 == q) { errorOut = "Buffer group missing count in '" + pack + "'"; return false; }
            while (q2 < inner.size() && std::isspace((unsigned char)inner[q2])) ++q2;
            if (q2 >= inner.size() || inner[q2] != ':') { errorOut = "Buffer group missing ':' in '" + pack + "'"; return false; }
            q = q2 + 1;
            // Parse values
            while (q < inner.size()) {
                while (q < inner.size() && std::isspace((unsigned char)inner[q])) ++q;
                if (q >= inner.size()) break;
                // Read next token until space
                size_t start = q; while (q < inner.size() && !std::isspace((unsigned char)inner[q])) ++q;
                std::string vtok = inner.substr(start, q - start);
                if (vtok.empty()) break;
                uint8_t vmask = 0;
                if (vtok.size() >= 3 && (vtok[0] == '0') && (vtok[1] == 'x' || vtok[1] == 'X')) {
                    // Hex
                    uint32_t vv = 0;
                    std::stringstream ss; ss << std::hex << vtok; ss >> vv;
                    vmask = (uint8_t)(vv & 0xFF);
                } else {
                    if (!TryTokenToMask(vtok, vmask)) { errorOut = "Bad buffer value token: '" + vtok + "'"; return false; }
                }
                thisTickBuf.push_back(vmask);
            }
            thisTickK = (uint16_t)kVal;
            if (thisTickK != thisTickBuf.size()) {
                errorOut = "Buffer group count mismatch (k!=values) in '" + pack + "'";
                return false;
            }
        } else {
            // Default: one write equal to tick mask
            thisTickK = 1; thisTickBuf.push_back(baseMask);
        }

        // Emit repeat
        for (uint32_t r = 0; r < repeat; ++r) {
            macro.push_back(baseMask);
            counts.push_back(thisTickK);
            buf.insert(buf.end(), thisTickBuf.begin(), thisTickBuf.end());
        }
    }

    // Commit to slot
    ClearSlotForImport(dst);
    dst.macroStream = std::move(macro);
    dst.bufCountsPerTick = std::move(counts);
    dst.bufStream = std::move(buf);
    dst.hasData = !dst.macroStream.empty();
    BuildSpansFromStream(dst);
    return true;
}

} // namespace MacroController