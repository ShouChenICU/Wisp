#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "wisp/base/result.hpp"

namespace wisp::os {

struct IsrQueueRecord {
    uint32_t source = 0;
    uint32_t value = 0;
    uint64_t timestamp_us = 0;
};

// A fixed-capacity record queue with a bounded, non-blocking producer path
// suitable for interrupt context. Exactly one consumer may call pop().
class IsrQueue {
public:
    static Result<std::unique_ptr<IsrQueue>> create(size_t capacity);
    ~IsrQueue();

    IsrQueue(const IsrQueue&) = delete;
    IsrQueue& operator=(const IsrQueue&) = delete;

    // A platform-owned ISR callback and internal-RAM context. No portable
    // service object or container is dereferenced on the interrupt path.
    // Detach all producers quiescently before destroying a binding/queue.
    struct EdgeBinding {
        static bool push_edge(void* context, bool pressed, uint64_t timestamp_us) noexcept;
        static void operator delete(void* ptr) noexcept;
        IsrQueue* queue;
        uint32_t source;
    };
    std::unique_ptr<EdgeBinding> bind_edge(uint32_t source);

    bool push_from_isr(const IsrQueueRecord& item) noexcept;
    bool push(const IsrQueueRecord& item) noexcept;

    // Returns true when a record was removed. Returns false on timeout or
    // when wake() interrupts the wait without adding a record. Finite waits
    // are rounded up to the port's scheduling tick (ESP: at most one extra
    // tick before ordinary scheduler latency); timestamps are not rounded.
    bool pop(IsrQueueRecord& item, uint32_t timeout_ms);
    void wake();
    uint32_t dropped() const;

    // Single-consumer resynchronization: atomically discard all queued records
    // and reset a sticky producer-attempt flag; return the drop count at that
    // boundary. Sample external levels, then validate with resync_unchanged().
    // Every push attempt (including overflow) invalidates that sample window.
    // Do not pop or begin another resync between the boundary and validation.
    // On false, discard and resample on a later turn; never replay old records.
    // A push after successful validation is part of the new input history.
    uint32_t begin_resync();
    bool resync_unchanged() const;

    static void operator delete(void* ptr) noexcept;

private:
    struct Impl;
    explicit IsrQueue(Impl* impl) : impl_(impl) {}

    Impl* impl_;
};

}  // namespace wisp::os
