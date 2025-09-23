#include "../include/game/macro_controller.h"
#include "../include/core/logger.h"
#include "../include/core/memory.h"
#include "../include/core/constants.h"
#include "../include/game/game_state.h"
#include "../include/gui/overlay.h"
#include "../include/input/immediate_input.h"
#include "../include/input/input_core.h"      // GetPlayerPointer
#include "../include/input/input_buffer.h"    // INPUT_BUFFER_* constants
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
    struct RLESpan { Mask mask; Mask buf; int ticks; };
    struct Slot {
        std::vector<RLESpan> spans; // RLE of immediate+buf at 64 Hz
        std::vector<uint8_t> bufStream; // full circular buffer stream captured during recording
        uint16_t bufStartIdx = 0; // buffer index at recording start
        uint16_t bufEndIdx = 0;   // buffer index at recording end
        bool hasData = false;
    };

    constexpr int kMaxSlots = 8; // simple ring of slots
    // Overlay placement for macro state banners (avoid overlapping status overlays)
    constexpr int kBannerX = 20;
    constexpr int kBannerY = 260;
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
    size_t s_playIndex = 0;    // index into spans
    int s_playSpanRemaining = 0;

    // Progress pacing: we step at the 64 Hz ImmediateInput cadence by counting internal frames (192 Hz)
    int s_frameDiv = 0; // 0..2 cycles; advance when hits 0

    // Gamespeed probe (0 = frozen)
    bool ReadGamespeedFrozen() {
        // Reuse the helper from pause_integration indirectly via memory: read via known chain is not exposed here.
        // Fallback: when not in Match or characters uninitialized, treat as frozen for macro progression.
        if (GetCurrentGamePhase() != GamePhase::Match) return true;
        uintptr_t base = GetEFZBase();
        if (!base) return true;
        // If base exists, assume not frozen; our FrameMonitor pacing continues anyway.
        // We rely on frameDiv pacing to approximate 64Hz and pause behavior handled by Match gating above.
        return false;
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
        s_playIndex = 0; s_playSpanRemaining = 0; s_frameDiv = 0;
        ImmediateInput::Clear(2);
    }

    void FinishRecording() {
        MacroController::State st = s_state.load();
        if (st != MacroController::State::Recording) return;
        // Flush any pending span
        if (s_recSpanTicks > 0) {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            s_slots[slotIdx].spans.push_back({ static_cast<Mask>(s_recLastMask & 0xFF), static_cast<Mask>(s_recLastBuf & 0xFF), s_recSpanTicks });
            s_slots[slotIdx].hasData = !s_slots[slotIdx].spans.empty();
            // Log final span
            LogOut(std::string("[MACRO][REC] span imm=0x") + ToHexString((int)(s_recLastMask & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastMask & 0xFF)) + ") buf=0x" + ToHexString((int)(s_recLastBuf & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastBuf & 0xFF)) + ") ticks=" + std::to_string(s_recSpanTicks), true);
        }
        // Capture any remaining buffer entries up to current index at finish
        uintptr_t p2Ptr = GetPlayerPointer(2);
        uint16_t endIdx = 0;
        if (p2Ptr && SafeReadMemory(p2Ptr + INPUT_BUFFER_INDEX_OFFSET, &endIdx, sizeof(endIdx))) {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            // Append entries from s_recPrevBufIdx -> endIdx (exclusive of endIdx, inclusive start)
            if (s_recPrevBufIdx >= 0) {
                int cur = s_recPrevBufIdx;
                while (cur != endIdx) {
                    uint8_t v = 0;
                    SafeReadMemory(p2Ptr + INPUT_BUFFER_OFFSET + (uintptr_t)cur, &v, sizeof(v));
                    s_slots[slotIdx].bufStream.push_back(v);
                    cur = (cur + 1) % (int)INPUT_BUFFER_SIZE;
                }
            }
            s_slots[slotIdx].bufEndIdx = endIdx;
        }
        s_recSpanTicks = 0; s_recLastMask = 0; s_recLastBuf = 0; s_recPrevBufIdx = -1;
        // Snapshot P2 buffer and immediate regs at end
        LogP2BufferSnapshot("end");
        LogP2ImmediateSnapshot("end");
        // Summary
        int slotIdx = ClampSlot(s_curSlot.load()) - 1;
        int totalTicks = 0; for (auto &sp : s_slots[slotIdx].spans) totalTicks += sp.ticks;
        LogOut("[MACRO][REC] finished slot=" + std::to_string(s_curSlot.load()) +
               " spans=" + std::to_string((int)s_slots[slotIdx].spans.size()) +
               " ticks=" + std::to_string(totalTicks) +
               " bufEntries=" + std::to_string((int)s_slots[slotIdx].bufStream.size()) +
               " bufIdxStart=" + std::to_string((int)s_slots[slotIdx].bufStartIdx) +
               " bufIdxEnd=" + std::to_string((int)s_slots[slotIdx].bufEndIdx), true);
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

    // Pace to 64 Hz logical ticks using 192 Hz internal frames
    if (++s_frameDiv >= 3) s_frameDiv = 0;
    if (s_frameDiv != 0) return; // only advance every 3rd internal frame

    State st = s_state.load();
    if (st == State::Recording) {
        if (ReadGamespeedFrozen()) return; // pause-safe
        uint8_t mask = ReadRecordSourceMask();
        uint8_t buf  = ReadP2BufferLatestMask();
        // Capture all new buffer entries since last tick
        {
            uintptr_t p2Ptr = GetPlayerPointer(2);
            uint16_t idx = 0;
            if (p2Ptr && SafeReadMemory(p2Ptr + INPUT_BUFFER_INDEX_OFFSET, &idx, sizeof(idx)) && s_recPrevBufIdx >= 0) {
                int slotIdx = ClampSlot(s_curSlot.load()) - 1;
                int cur = s_recPrevBufIdx;
                while (cur != idx) {
                    uint8_t v = 0;
                    SafeReadMemory(p2Ptr + INPUT_BUFFER_OFFSET + (uintptr_t)cur, &v, sizeof(v));
                    s_slots[slotIdx].bufStream.push_back(v);
                    cur = (cur + 1) % (int)INPUT_BUFFER_SIZE;
                }
                s_recPrevBufIdx = idx;
            }
        }
        if (s_recSpanTicks == 0) {
            s_recLastMask = mask; s_recLastBuf = buf; s_recSpanTicks = 1;
        } else if (mask == s_recLastMask && buf == s_recLastBuf) {
            s_recSpanTicks++;
        } else {
            int slotIdx = ClampSlot(s_curSlot.load()) - 1;
            s_slots[slotIdx].spans.push_back({ static_cast<Mask>(s_recLastMask & 0xFF), static_cast<Mask>(s_recLastBuf & 0xFF), s_recSpanTicks });
            s_slots[slotIdx].hasData = true;
            // Log completed span (immediate vs buffer)
            LogOut(std::string("[MACRO][REC] span imm=0x") + ToHexString((int)(s_recLastMask & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastMask & 0xFF)) + ") buf=0x" + ToHexString((int)(s_recLastBuf & 0xFF), 2) +
                   " (" + MaskToButtons((Mask)(s_recLastBuf & 0xFF)) + ") ticks=" + std::to_string(s_recSpanTicks), true);
            s_recLastMask = mask; s_recLastBuf = buf; s_recSpanTicks = 1;
        }
    } else if (st == State::Replaying) {
        if (ReadGamespeedFrozen()) return; // pause-safe
        int slotIdx = ClampSlot(s_curSlot.load()) - 1;
        if (!s_slots[slotIdx].hasData || s_slots[slotIdx].spans.empty()) {
            // Nothing to play
            s_state.store(State::Idle);
            ImmediateInput::Clear(2);
            DirectDrawHook::AddMessage("Macro: Replay empty", "MACRO", RGB(255,120,120), 1000, 0, 120);
            return;
        }

        if (s_playSpanRemaining <= 0) {
            if (s_playIndex >= s_slots[slotIdx].spans.size()) {
                // End
                s_state.store(State::Idle);
                ImmediateInput::Clear(2);
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
            // Apply desired mask via ImmediateInput
            if (sp.mask == 0) {
                ImmediateInput::Clear(2);
            } else {
                ImmediateInput::Set(2, sp.mask);
            }
            LogOut(std::string("[MACRO][PLAY] span imm=0x") + ToHexString((int)sp.mask, 2) +
                   " (" + MaskToButtons(sp.mask) + ") buf=0x" + ToHexString((int)sp.buf, 2) +
                   " (" + MaskToButtons(sp.buf) + ") ticks=" + std::to_string(sp.ticks), true);
        } else {
            s_playSpanRemaining--;
            if (s_playSpanRemaining <= 0) {
                // Force a neutral edge between spans to ensure clean transitions
                ImmediateInput::Clear(2);
            }
        }
    }
}

void ToggleRecord() {
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
            s_slots[slotIdx].bufStream.clear();
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
    // If currently recording, finish first
    if (s_state.load() == State::Recording) FinishRecording();
    // Prepare playback
    int slotIdx = ClampSlot(s_curSlot.load()) - 1;
    if (!s_slots[slotIdx].hasData || s_slots[slotIdx].spans.empty()) {
        DirectDrawHook::AddMessage("Macro: Slot empty", "MACRO", RGB(255,120,120), 1000, 0, 120);
        return;
    }
    ResetPlayback();
    // Ensure P2 is human-controlled during playback
    EnableP2ControlForAutoAction();
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
           " sink=P2", true);
}

void Stop() {
    State st = s_state.load();
    if (st == State::Recording) FinishRecording();
    s_state.store(State::Idle);
    ResetPlayback();
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
    stats.bufStartIdx = s.bufStartIdx;
    stats.bufEndIdx = s.bufEndIdx;
    stats.hasData = s.hasData && !s.spans.empty();
    return stats;
}

void NextSlot() {
    int cur = s_curSlot.load();
    cur++; if (cur > kMaxSlots) cur = 1; s_curSlot.store(cur);
}
void PrevSlot() {
    int cur = s_curSlot.load();
    cur--; if (cur < 1) cur = kMaxSlots; s_curSlot.store(cur);
}

} // namespace MacroController
