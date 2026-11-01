#pragma once

// Portable, header-only bounded queue built only from wisp::os::Mutex and
// wisp::os::Semaphore, so any port that implements os.hpp gets Queue for
// free without writing its own ring buffer.

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "wisp/os/os.hpp"

namespace wisp::os {

// A fixed-capacity FIFO ring buffer, safe for any number of concurrent
// producers and consumers calling push()/pop(). push_drop_oldest() is
// intended for a single-producer overflow policy (e.g. an event queue that
// drops its oldest entry rather than blocking); concurrent callers of
// push_drop_oldest() (or mixed with push()) on the same queue instance are
// not supported.
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
          filled_slots_(static_cast<uint32_t>(capacity), 0) {}

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
        bool dropped = false;
        if (!free_slots_.take(0)) {
            // Full: evict the oldest entry. filled_slots_ is known to be
            // positive here (we just observed the queue full), so this
            // take() cannot block.
            filled_slots_.take(0);
            {
                std::lock_guard<Mutex> lock(mutex_);
                tail_ = (tail_ + 1) % capacity_;
                --size_;
            }
            dropped = true;
        }
        {
            std::lock_guard<Mutex> lock(mutex_);
            buffer_[head_] = std::move(item);
            head_ = (head_ + 1) % capacity_;
            ++size_;
        }
        filled_slots_.give();
        return dropped;
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
