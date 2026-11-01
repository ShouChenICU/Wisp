#pragma once

// Portable, header-only bounded queue built only from wisp::os::Mutex and
// wisp::os::Semaphore, so any port that implements os.hpp gets Queue for
// free without writing its own ring buffer.

#include <cassert>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "wisp/os/os.hpp"

namespace wisp::os {

// A fixed-capacity FIFO ring buffer, safe for any number of concurrent
// producers and consumers calling push()/pop(). push_drop_oldest() is
// additionally safe against any number of concurrent pop() callers (it
// retries if it loses a race for the last filled/free slot to a concurrent
// pop()); it is intended for a single-producer overflow policy (e.g. an
// event queue that drops its oldest entry rather than blocking), so
// concurrent callers of push_drop_oldest() itself (or mixed with push())
// on the same queue instance are not supported.
//
// T must be default-constructible (the backing storage is pre-sized) and
// movable.
template <class T>
class Queue {
public:
    explicit Queue(size_t capacity)
        : capacity_(capacity),
          buffer_(capacity),
          free_slots_(static_cast<uint32_t>(capacity), static_cast<uint32_t>(capacity)),
          filled_slots_(static_cast<uint32_t>(capacity), 0) {
        assert(capacity_ > 0);
    }

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    // Waits up to `timeout_ms` for a free slot, then enqueues `item`.
    // Returns false if the queue was still full when the timeout elapsed.
    bool push(T item, uint32_t timeout_ms) {
        if (!free_slots_.take(timeout_ms)) {
            return false;
        }
        {
            std::lock_guard<Mutex> lock(mutex_);
            buffer_[head_] = std::move(item);
            head_ = (head_ + 1) % capacity_;
            ++size_;
        }
        filled_slots_.give();
        return true;
    }

    // Enqueues `item`, evicting the oldest element first if the queue is
    // full. Returns true if an element was dropped to make room.
    bool push_drop_oldest(T item) {
        while (true) {
            if (free_slots_.take(0)) {
                // A slot was genuinely free (either the queue wasn't full,
                // or a concurrent pop() just finished and gave one back):
                // a plain insert, not a drop.
                {
                    std::lock_guard<Mutex> lock(mutex_);
                    buffer_[head_] = std::move(item);
                    head_ = (head_ + 1) % capacity_;
                    ++size_;
                }
                filled_slots_.give();
                return false;
            }

            // No free slot right now. Try to claim removal rights to the
            // oldest entry so it can be evicted. This can race with a
            // concurrent pop(), which also tries to claim that same right
            // via filled_slots_.take(): if we lose that race,
            // filled_slots_.take(0) below fails (the queue is neither free
            // nor "ours to evict from" at this instant) and we must not
            // touch tail_/size_ — loop and retry instead. Eviction and
            // insertion happen under a single locked section so no other
            // push()/pop() can observe (or race on) the half-evicted
            // state in between.
            if (filled_slots_.take(0)) {
                {
                    std::lock_guard<Mutex> lock(mutex_);
                    tail_ = (tail_ + 1) % capacity_;
                    --size_;
                    buffer_[head_] = std::move(item);
                    head_ = (head_ + 1) % capacity_;
                    ++size_;
                }
                filled_slots_.give();
                return true;
            }

            // Neither semaphore could be claimed: a concurrent pop() has
            // the in-flight token for the only available slot right now.
            // Back off briefly and retry; it will finish shortly and give
            // back either a free slot (via free_slots_) — in which case
            // we won't even need to drop — or let us claim the filled
            // slot on a later iteration. Use sleep_ms(1), not
            // sleep_ms(0): on FreeRTOS, vTaskDelay(0) does not yield to
            // lower-priority tasks, so a higher-priority caller stuck in
            // this loop could spin forever waiting on a lower-priority
            // pop() that never gets scheduled (livelock). sleep_ms(1)
            // actually yields the CPU for at least one tick.
            sleep_ms(1);
        }
    }

    // Waits up to `timeout_ms` for a filled slot, then dequeues into `out`.
    // Returns false if the queue was still empty when the timeout elapsed.
    bool pop(T& out, uint32_t timeout_ms) {
        if (!filled_slots_.take(timeout_ms)) {
            return false;
        }
        {
            std::lock_guard<Mutex> lock(mutex_);
            out = std::move(buffer_[tail_]);
            tail_ = (tail_ + 1) % capacity_;
            --size_;
        }
        free_slots_.give();
        return true;
    }

    size_t size() const {
        std::lock_guard<Mutex> lock(mutex_);
        return size_;
    }

    size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    std::vector<T> buffer_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
    mutable Mutex mutex_;
    Semaphore free_slots_;
    Semaphore filled_slots_;
};

}  // namespace wisp::os
