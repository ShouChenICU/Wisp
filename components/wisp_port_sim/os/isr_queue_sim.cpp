#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>

#include "wisp/os/isr_queue.hpp"
#include "wisp/os/os.hpp"

namespace wisp::os {

struct IsrQueue::Impl {
    Impl(size_t queue_capacity, IsrQueueRecord* queue_slots)
        : capacity(queue_capacity), slots(queue_slots) {}

    bool enqueue(const IsrQueueRecord& item) {
        std::lock_guard<std::mutex> lock(mutex);
        if (size == capacity) {
            ++dropped_count;
            return false;
        }
        slots[tail] = item;
        tail = (tail + 1) % capacity;
        ++size;
        condition.notify_one();
        return true;
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    size_t capacity;
    IsrQueueRecord* slots;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;
    uint32_t dropped_count = 0;
    bool wake_pending = false;
};

Result<std::unique_ptr<IsrQueue>> IsrQueue::create(size_t capacity) {
    if (capacity == 0 || capacity > std::numeric_limits<size_t>::max() / sizeof(IsrQueueRecord)) {
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

std::unique_ptr<IsrQueue::EdgeBinding> IsrQueue::bind_edge(uint32_t source) {
    void* memory = alloc(sizeof(EdgeBinding), Mem::Fast);
    if (memory == nullptr) return nullptr;
    return std::unique_ptr<EdgeBinding>(new (memory) EdgeBinding{this, source});
}

void IsrQueue::EdgeBinding::operator delete(void* ptr) noexcept { dealloc(ptr); }

bool IsrQueue::EdgeBinding::push_edge(void* context, bool pressed, uint64_t timestamp_us) noexcept {
    auto* binding = static_cast<EdgeBinding*>(context);
    return binding->queue->push_from_isr({binding->source, pressed ? 1u : 0u, timestamp_us});
}

bool IsrQueue::push_from_isr(const IsrQueueRecord& item) noexcept { return impl_->enqueue(item); }

bool IsrQueue::push(const IsrQueueRecord& item) noexcept { return impl_->enqueue(item); }

bool IsrQueue::pop(IsrQueueRecord& item, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const auto ready = [this] { return impl_->size > 0 || impl_->wake_pending; };

    if (timeout_ms == kForever) {
        impl_->condition.wait(lock, ready);
    } else if (!impl_->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
        return false;
    }

    if (impl_->size > 0) {
        item = impl_->slots[impl_->head];
        impl_->head = (impl_->head + 1) % impl_->capacity;
        --impl_->size;
        return true;
    }

    impl_->wake_pending = false;
    return false;
}

void IsrQueue::wake() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->wake_pending = true;
    }
    impl_->condition.notify_one();
}

uint32_t IsrQueue::dropped() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->dropped_count;
}

}  // namespace wisp::os
