#include "wisp/net/request_watch.hpp"

#include <mutex>
#include <new>
#include <utility>

namespace wisp::net {

os::LargeUnique<RequestWatch> RequestWatch::create() {
    // The constructor is private, so not os::make_large().
    void* mem = os::alloc(sizeof(RequestWatch), os::Mem::Large);
    if (mem == nullptr) return nullptr;
    os::LargeUnique<RequestWatch> watch(::new (mem) RequestWatch());
    RequestWatch* self = watch.get();
    watch->timer_ = os::Timer::create("wisp.http.watch", kTickMs, true, [self] { self->tick(); });
    if (!watch->timer_) return nullptr;
    return watch;
}

RequestWatch::~RequestWatch() { disarm(); }

uint32_t RequestWatch::arm(uint32_t timeout_ms) {
    uint32_t ticket = 0;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (++ticket_ == 0) ++ticket_;
        ticket = ticket_;
        armed_ = true;
        reason_ = Reason::None;
        stop_ = nullptr;
        deadline_ = os::now_ms() + timeout_ms;
    }
    // Only starts the timer (a no-op while it runs): never waits.
    timer_->start();
    return ticket;
}

void RequestWatch::attach(std::function<void()> stop) {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!armed_) return;
        if (reason_ == Reason::None) {
            stop_ = std::move(stop);
            return;
        }
    }
    // Already fired: the client is about to use what stop() ends, so end
    // it now, on the client's own thread.
    if (stop) stop();
}

void RequestWatch::abort(uint32_t ticket) {
    std::lock_guard<os::Mutex> lock(mu_);
    if (armed_ && ticket == ticket_ && reason_ == Reason::None) reason_ = Reason::Aborted;
}

void RequestWatch::disarm() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        armed_ = false;
        stop_ = nullptr;
    }
    // Waits for a tick in flight, and with it a stop() that tick runs.
    // Not under mu_: the tick takes it.
    if (timer_) timer_->stop();
}

RequestWatch::Reason RequestWatch::reason() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return reason_;
}

void RequestWatch::tick() {
    std::function<void()> stop;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!armed_) return;
        if (reason_ == Reason::None && os::now_ms() >= deadline_) reason_ = Reason::Deadline;
        if (reason_ == Reason::None) return;
        stop = std::move(stop_);
        stop_ = nullptr;
    }
    // Outside mu_, so abort() and reason() never wait for it; disarm()
    // waits for this callback as a whole.
    if (stop) stop();
}

}  // namespace wisp::net
