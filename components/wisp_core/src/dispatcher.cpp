#include "wisp/core/dispatcher.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {
// How often an off-thread cancel() looks again for the end of a run in
// progress. Runs do memory work only, so the wait is short; polling keeps
// the slot free of any per-slot wait object.
constexpr uint32_t kCancelPollMs = 1;
}  // namespace

void Dispatcher::attach() { attached_.fetch_add(1, std::memory_order_relaxed); }

void Dispatcher::detach() { attached_.fetch_sub(1, std::memory_order_relaxed); }

uint32_t Dispatcher::attached() const { return attached_.load(std::memory_order_relaxed); }

void Dispatcher::check_detached(const char* who) const {
    uint32_t n = attached();
    if (n == 0) {
        return;
    }
    char what[96];
    std::snprintf(what, sizeof(what), "%s destroyed with %u attached slot%s", who,
                  static_cast<unsigned>(n), n == 1 ? "" : "s");
    detail::misuse(what);
}

bool SlotQueue::push(DispatchSlot& slot) {
    std::lock_guard<os::Mutex> lock(mu_);
    if ((slot.flags & (DispatchSlot::kQueued | DispatchSlot::kCancelled)) != 0) {
        return false;
    }
    bool was_empty = (head_ == nullptr);
    slot.flags |= DispatchSlot::kQueued;
    slot.next = nullptr;
    if (tail_ != nullptr) {
        tail_->next = &slot;
    } else {
        head_ = &slot;
    }
    tail_ = &slot;
    ++queued_;
    return was_empty;
}

DispatchSlot* SlotQueue::take() {
    std::lock_guard<os::Mutex> lock(mu_);
    DispatchSlot* slot = head_;
    if (slot == nullptr) {
        return nullptr;
    }
    head_ = slot->next;
    if (head_ == nullptr) {
        tail_ = nullptr;
    }
    slot->next = nullptr;
    slot->flags =
        static_cast<uint8_t>((slot->flags & ~DispatchSlot::kQueued) | DispatchSlot::kRunning);
    --queued_;
    running_ = slot;
    return slot;
}

void SlotQueue::done(DispatchSlot& slot) {
    void (*finish)(DispatchSlot&) = nullptr;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        running_ = nullptr;
        slot.flags = static_cast<uint8_t>(slot.flags & ~DispatchSlot::kRunning);
        if ((slot.flags & DispatchSlot::kCancelled) != 0) {
            slot.flags = 0;
            finish = slot.finish;
        }
    }
    // Outside the lock, and the last touch of the slot: finish() frees it.
    if (finish != nullptr) {
        finish(slot);
    }
}

bool SlotQueue::cancel(DispatchSlot& slot, bool on_dispatch_thread) {
    while (true) {
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if ((slot.flags & DispatchSlot::kQueued) != 0) {
                unlink_locked(slot);
            }
            if (running_ != &slot) {
                slot.flags = 0;
                return true;
            }
            if (on_dispatch_thread) {
                // Only the slot's own run can be in progress on the
                // dispatch thread: the dispatcher still touches the slot
                // when the run returns, so done() hands it to finish().
                slot.flags |= DispatchSlot::kCancelled;
                return false;
            }
        }
        // A run of this slot is in progress on the dispatcher's thread;
        // it may queue the slot again, which the next pass unlinks.
        os::sleep_ms(kCancelPollMs);
    }
}

size_t SlotQueue::pending() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return queued_ + (running_ != nullptr ? 1 : 0);
}

void SlotQueue::unlink_locked(DispatchSlot& slot) {
    DispatchSlot* prev = nullptr;
    for (DispatchSlot* it = head_; it != nullptr; prev = it, it = it->next) {
        if (it != &slot) {
            continue;
        }
        if (prev != nullptr) {
            prev->next = it->next;
        } else {
            head_ = it->next;
        }
        if (tail_ == it) {
            tail_ = prev;
        }
        it->next = nullptr;
        it->flags = static_cast<uint8_t>(it->flags & ~DispatchSlot::kQueued);
        --queued_;
        return;
    }
}

namespace detail {

void misuse(const char* what) {
#if defined(ESP_PLATFORM)
    // As WISP_ASSERT_FLASH_THREAD does on the device: report and keep
    // running; the host builds' abort is what catches the bug in tests.
    WLOG_E("core", "misuse: %s", what);
#else
    std::fprintf(stderr, "wisp: misuse: %s\n", what);
    std::fflush(stderr);
    std::abort();
#endif
}

}  // namespace detail

}  // namespace wisp::core
