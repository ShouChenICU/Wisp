#include "wisp/core/edge_inbox.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "wisp/core/log.hpp"

namespace wisp::core::edge_detail {

namespace {

constexpr const char* kTag = "core";
// The overflow warning: the first drop, then every 64th, as the bus's.
constexpr uint32_t kOverflowLogInterval = 64;

[[noreturn]] void fatal(const char* what, const char* name) {
    WLOG_E(kTag, "%s (%s)", what, name);
#if !defined(ESP_PLATFORM)
    // The log has no console sink in host tests; say why on stderr too.
    std::fprintf(stderr, "wisp: %s (%s)\n", what, name);
    std::fflush(stderr);
#endif
    std::abort();
}

}  // namespace

InboxCore::InboxCore(const char* name, Dispatcher& on, size_t capacity, size_t edge_size,
                     void (*run)(DispatchSlot&))
    : name_(name), on_(on), edge_size_(edge_size), capacity_(capacity) {
    ring_ = static_cast<unsigned char*>(os::alloc(capacity * edge_size, os::Mem::Large));
    if (ring_ == nullptr) {
        // As os::LargeAllocator: an inbox has no way to fail.
        fatal("EdgeInbox ring allocation failed", name_);
    }
    slot_.run = run;
    slot_.owner = this;
    on_.attach();
}

InboxCore::~InboxCore() { os::dealloc(ring_); }

void InboxCore::close() {
    if (on_.on_dispatch_thread() && in_run_) {
        // The run returns into an inbox that no longer exists; there is no
        // safe way on, on any build.
        fatal("EdgeInbox destroyed from its own handler", name_);
    }
    // Off the dispatcher's thread this waits for a run in progress; on it
    // (from another slot's run) no run of this slot is in progress, so it
    // never returns false.
    if (!on_.cancel(slot_)) {
        fatal("EdgeInbox slot still running at destruction", name_);
    }
    on_.detach();
}

void InboxCore::push_bytes(const void* edge) {
    uint32_t dropped = 0;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (count_ == capacity_) {
            head_ = (head_ + 1) % capacity_;
            --count_;
            dropped = overflows_.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        std::memcpy(ring_ + ((head_ + count_) % capacity_) * edge_size_, edge, edge_size_);
        // Only the empty-to-non-empty push schedules: while edges wait, a
        // run is queued, or is in progress and queues the next one in
        // end_run(), which checks under this same lock.
        if (count_++ == 0) {
            on_.schedule(slot_);
        }
    }
    // Outside the lock: logging can be slow and takes the log's own lock.
    if (dropped != 0 && (dropped == 1 || dropped % kOverflowLogInterval == 0)) {
        WLOG_W(kTag, "edge inbox %s overflowed (%u edges dropped)", name_,
               static_cast<unsigned>(dropped));
    }
}

size_t InboxCore::begin_run() {
    in_run_ = true;
    std::lock_guard<os::Mutex> lock(mu_);
    // Read once: edges pushed during this run wait for the next one, so a
    // handler that keeps pushing cannot hold the dispatcher.
    return count_;
}

bool InboxCore::pop(void* out) {
    std::lock_guard<os::Mutex> lock(mu_);
    if (count_ == 0) {
        return false;
    }
    std::memcpy(out, ring_ + head_ * edge_size_, edge_size_);
    head_ = (head_ + 1) % capacity_;
    --count_;
    return true;
}

void InboxCore::end_run() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (count_ != 0) {
            // Queued behind whatever else is ready. Refused (dropped) only
            // while the destructor cancels the slot, which discards the
            // edges anyway.
            on_.schedule(slot_);
        }
    }
    in_run_ = false;
}

}  // namespace wisp::core::edge_detail
