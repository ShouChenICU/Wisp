#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <semaphore>

#include "wisp/os/isr_queue.hpp"
#include "wisp/os/os.hpp"

namespace wisp::os {

struct IsrQueue::Impl {
    Impl(size_t queue_capacity, IsrQueueRecord* queue_slots)
        : capacity(queue_capacity), slots(queue_slots), signal(0) {}

    void lock_state() const noexcept {
        while (state_lock.test_and_set(std::memory_order_acquire)) {
        }
    }

    void unlock_state() const noexcept { state_lock.clear(std::memory_order_release); }

    bool enqueue(const IsrQueueRecord& item) noexcept {
        lock_state();
        if (size == capacity) {
            ++dropped_count;
            unlock_state();
            return false;
        }
        slots[tail] = item;
        tail = (tail + 1) % capacity;
        ++size;
        unlock_state();
        signal.release();
        return true;
    }

    // The producer cannot take a condition-variable mutex from ISR context.
    mutable std::atomic_flag state_lock = ATOMIC_FLAG_INIT;
    size_t capacity;
    IsrQueueRecord* slots;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;
    uint32_t dropped_count = 0;
    bool wake_pending = false;
    std::counting_semaphore<> signal;
};

Result<std::unique_ptr<IsrQueue>> IsrQueue::create(size_t capacity) {
    if (capacity == 0 || capacity > std::numeric_limits<size_t>::max() / sizeof(IsrQueueRecord) ||
        capacity >= static_cast<size_t>(std::counting_semaphore<>::max())) {
        return Result<std::unique_ptr<IsrQueue>>::err(ErrorCode::InvalidArgument,
                                                      "invalid ISR queue capacity");
    }

    auto* slots = static_cast<IsrQueueRecord*>(alloc(capacity * sizeof(IsrQueueRecord), Mem::Fast));
    if (slots == nullptr) {
        return Result<std::unique_ptr<IsrQueue>>::err(ErrorCode::NoMemory,
                                                      "unable to allocate ISR queue slots");
    }

    void* impl_memory = alloc(sizeof(Impl), Mem::Fast);
    if (impl_memory == nullptr) {
        dealloc(slots);
        return Result<std::unique_ptr<IsrQueue>>::err(ErrorCode::NoMemory,
                                                      "unable to allocate ISR queue state");
    }
    auto* impl = new (impl_memory) Impl(capacity, slots);

    void* queue_memory = alloc(sizeof(IsrQueue), Mem::Fast);
    if (queue_memory == nullptr) {
        impl->~Impl();
        dealloc(impl);
        dealloc(slots);
        return Result<std::unique_ptr<IsrQueue>>::err(ErrorCode::NoMemory,
                                                      "unable to allocate ISR queue handle");
    }
    auto* queue = new (queue_memory) IsrQueue(impl);
    return Result<std::unique_ptr<IsrQueue>>::ok(std::unique_ptr<IsrQueue>(queue));
}

IsrQueue::~IsrQueue() {
    if (impl_ != nullptr) {
        IsrQueueRecord* slots = impl_->slots;
        impl_->~Impl();
        dealloc(impl_);
        dealloc(slots);
    }
}

void IsrQueue::operator delete(void* ptr) noexcept { dealloc(ptr); }

bool IsrQueue::push_from_isr(const IsrQueueRecord& item) noexcept { return impl_->enqueue(item); }

bool IsrQueue::push(const IsrQueueRecord& item) noexcept { return impl_->enqueue(item); }

bool IsrQueue::pop(IsrQueueRecord& item, uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (timeout_ms == kForever) {
            impl_->signal.acquire();
        } else if (timeout_ms == 0) {
            if (!impl_->signal.try_acquire()) {
                return false;
            }
        } else if (!impl_->signal.try_acquire_until(deadline)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
        }

        impl_->lock_state();
        if (impl_->size > 0) {
            item = impl_->slots[impl_->head];
            impl_->head = (impl_->head + 1) % impl_->capacity;
            --impl_->size;
            impl_->unlock_state();
            return true;
        }
        if (impl_->wake_pending) {
            impl_->wake_pending = false;
            impl_->unlock_state();
            return false;
        }
        impl_->unlock_state();
    }
}

void IsrQueue::wake() {
    impl_->lock_state();
    const bool should_signal = !impl_->wake_pending;
    impl_->wake_pending = true;
    impl_->unlock_state();
    if (should_signal) {
        impl_->signal.release();
    }
}

uint32_t IsrQueue::dropped() const {
    impl_->lock_state();
    uint32_t count = impl_->dropped_count;
    impl_->unlock_state();
    return count;
}

}  // namespace wisp::os
