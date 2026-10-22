#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Fixed-capacity, single-producer/single-consumer storage for attack-button
// edges observed by EFZ's input poll. The input hook is the only producer and
// mission recording is the only consumer. Keeping this policy independent of
// the hook makes its ordering/overflow contract testable without the game.
namespace InputPollAttackEdgeJournalPolicy {

struct Event {
    uint32_t serial{0};
    uint8_t mask{0};
};

struct DrainResult {
    std::size_t count{0};
    uint32_t droppedEvents{0};
    bool overflowed{false};
};

template <std::size_t Capacity>
class Journal {
public:
    static_assert(Capacity > 0, "attack-edge journal capacity must be non-zero");

    // Producer only. A full journal keeps the already-published events in
    // order and drops the newest edge. The consumer receives an explicit
    // dropped-event count with its next drain.
    bool Push(const Event& event) noexcept {
        const uint32_t write = m_write.load(std::memory_order_relaxed);
        const uint32_t read = m_read.load(std::memory_order_acquire);
        if (static_cast<uint32_t>(write - read) >= Capacity) {
            m_dropped.fetch_add(1, std::memory_order_release);
            return false;
        }

        m_events[write % Capacity] = event;
        m_write.store(write + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. The acquired write cursor is a stable upper bound for
    // this call: events published concurrently remain queued for the next
    // drain. Supplying a smaller output span performs a partial FIFO drain.
    DrainResult Drain(Event* output, std::size_t outputCapacity) noexcept {
        DrainResult result{};
        if (output != nullptr && outputCapacity != 0) {
            const uint32_t read = m_read.load(std::memory_order_relaxed);
            const uint32_t write = m_write.load(std::memory_order_acquire);
            const uint32_t available = static_cast<uint32_t>(write - read);
            result.count = std::min<std::size_t>(available, outputCapacity);
            for (std::size_t index = 0; index < result.count; ++index) {
                output[index] = m_events[(read + static_cast<uint32_t>(index)) %
                                         Capacity];
            }
            m_read.store(read + static_cast<uint32_t>(result.count),
                         std::memory_order_release);
        }

        result.droppedEvents =
            m_dropped.exchange(0, std::memory_order_acq_rel);
        result.overflowed = result.droppedEvents != 0;
        return result;
    }

    // Consumer/boundary owner only. Discard everything published before the
    // acquired cursor. A producer edge racing after that cursor remains
    // visible, so a boundary cannot accidentally erase a newly published
    // event. Serial numbers remain owned by the input hook and stay monotonic.
    void Reset() noexcept {
        const uint32_t write = m_write.load(std::memory_order_acquire);
        m_read.store(write, std::memory_order_release);
        m_dropped.exchange(0, std::memory_order_acq_rel);
    }

private:
    std::array<Event, Capacity> m_events{};
    std::atomic<uint32_t> m_write{0};
    std::atomic<uint32_t> m_read{0};
    std::atomic<uint32_t> m_dropped{0};
};

} // namespace InputPollAttackEdgeJournalPolicy
