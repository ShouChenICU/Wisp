#include <limits>
#include <new>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wisp/os/isr_queue.hpp"
#include "wisp/os/os.hpp"

namespace wisp::os {

struct IsrQueue::Impl {
    Impl(size_t queue_capacity, IsrQueueRecord* queue_slots)
        : capacity(queue_capacity),
          slots(queue_slots),
          signal(xSemaphoreCreateBinaryStatic(&signal_storage)) {}

    bool enqueue(const IsrQueueRecord& item, bool from_isr) noexcept;

    mutable portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    size_t capacity;
    IsrQueueRecord* slots;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;
    uint32_t dropped_count = 0;
    bool wake_pending = false;
    bool produced_since_resync = false;
    StaticSemaphore_t signal_storage{};
    SemaphoreHandle_t signal;
};

bool IRAM_ATTR IsrQueue::Impl::enqueue(const IsrQueueRecord& item, bool from_isr) noexcept {
    bool accepted = false;
    if (from_isr) {
        portENTER_CRITICAL_ISR(&lock);
    } else {
        portENTER_CRITICAL(&lock);
    }
    produced_since_resync = true;
    if (size == capacity) {
        ++dropped_count;
    } else {
        slots[tail] = item;
        tail = (tail + 1) % capacity;
        ++size;
        accepted = true;
    }
    if (from_isr) {
        portEXIT_CRITICAL_ISR(&lock);
    } else {
        portEXIT_CRITICAL(&lock);
    }

    return accepted;
}

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
    if (impl->signal == nullptr) {
        impl->~Impl();
        dealloc(impl);
        dealloc(slots);
        return Result<std::unique_ptr<IsrQueue>>::err(ErrorCode::NoMemory,
                                                      "unable to create ISR queue signal");
    }

    void* queue_memory = alloc(sizeof(IsrQueue), Mem::Fast);
    if (queue_memory == nullptr) {
        vSemaphoreDelete(impl->signal);
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
        vSemaphoreDelete(impl_->signal);
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

bool IRAM_ATTR IsrQueue::EdgeBinding::push_edge(void* context, bool pressed,
                                                uint64_t timestamp_us) noexcept {
    auto* binding = static_cast<EdgeBinding*>(context);
    return binding->queue->push_from_isr({binding->source, pressed ? 1u : 0u, timestamp_us});
}

bool IRAM_ATTR IsrQueue::push_from_isr(const IsrQueueRecord& item) noexcept {
    if (!impl_->enqueue(item, true)) return false;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(impl_->signal, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
    return true;
}

bool IsrQueue::push(const IsrQueueRecord& item) noexcept {
    if (!impl_->enqueue(item, false)) return false;
    xSemaphoreGive(impl_->signal);
    return true;
}

bool IsrQueue::pop(IsrQueueRecord& item, uint32_t timeout_ms) {
    const uint64_t start_ms = now_ms();
    for (;;) {
        portENTER_CRITICAL(&impl_->lock);
        if (impl_->size > 0) {
            item = impl_->slots[impl_->head];
            impl_->head = (impl_->head + 1) % impl_->capacity;
            --impl_->size;
            portEXIT_CRITICAL(&impl_->lock);
            return true;
        }
        if (impl_->wake_pending) {
            impl_->wake_pending = false;
            portEXIT_CRITICAL(&impl_->lock);
            return false;
        }
        portEXIT_CRITICAL(&impl_->lock);

        TickType_t ticks = portMAX_DELAY;
        if (timeout_ms != kForever) {
            uint64_t elapsed_ms = now_ms() - start_ms;
            if (elapsed_ms >= timeout_ms) {
                return false;
            }
            uint32_t remaining_ms = timeout_ms - static_cast<uint32_t>(elapsed_ms);
            ticks = static_cast<TickType_t>(
                (static_cast<uint64_t>(remaining_ms) * configTICK_RATE_HZ + 999) / 1000);
            if (ticks == 0) {
                ticks = 1;
            }
        }
        if (xSemaphoreTake(impl_->signal, ticks) != pdTRUE && timeout_ms != kForever) {
            return false;
        }
    }
}

void IsrQueue::wake() {
    portENTER_CRITICAL(&impl_->lock);
    impl_->wake_pending = true;
    portEXIT_CRITICAL(&impl_->lock);
    xSemaphoreGive(impl_->signal);
}

uint32_t IsrQueue::begin_resync() {
    portENTER_CRITICAL(&impl_->lock);
    impl_->head = impl_->tail;
    impl_->size = 0;
    impl_->produced_since_resync = false;
    const uint32_t count = impl_->dropped_count;
    portEXIT_CRITICAL(&impl_->lock);
    return count;
}

bool IsrQueue::resync_unchanged() const {
    portENTER_CRITICAL(&impl_->lock);
    const bool unchanged = !impl_->produced_since_resync;
    portEXIT_CRITICAL(&impl_->lock);
    return unchanged;
}

uint32_t IsrQueue::dropped() const {
    portENTER_CRITICAL(&impl_->lock);
    uint32_t count = impl_->dropped_count;
    portEXIT_CRITICAL(&impl_->lock);
    return count;
}

}  // namespace wisp::os
