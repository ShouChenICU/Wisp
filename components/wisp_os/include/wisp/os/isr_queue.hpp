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

    bool push_from_isr(const IsrQueueRecord& item) noexcept;
    bool push(const IsrQueueRecord& item) noexcept;

    // Returns true when a record was removed. Returns false on timeout or
    // when wake() interrupts the wait without adding a record.
    bool pop(IsrQueueRecord& item, uint32_t timeout_ms);
    void wake();
    uint32_t dropped() const;

    static void operator delete(void* ptr) noexcept;

private:
    struct Impl;
    explicit IsrQueue(Impl* impl) : impl_(impl) {}

    Impl* impl_;
};

}  // namespace wisp::os
