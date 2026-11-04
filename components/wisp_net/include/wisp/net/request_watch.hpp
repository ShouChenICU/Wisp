#pragma once

// RequestWatch: ends an HTTP request that is blocked inside a client call,
// at its deadline or when another thread aborts it.
//
// Why: a client's timeout may apply to each low-level read, and a library
// call can loop over many of them (esp_http_client's header fetch and
// body read do), so a server that trickles bytes can keep one call alive
// far past any deadline checked between calls. The client attach()es
// what interrupts its blocked call (on the device: shutdown() of the
// request's socket, which makes every pending and later read fail); the
// watch runs it once, from its timer at the deadline (arm()) or from
// abort().
//
// Lifetime of what stop() touches: stop() runs under the watch's lock,
// and disarm() takes that lock and stops the timer, so once disarm()
// returns stop() is neither running nor ever called again. A client
// calls disarm() before it releases what stop() uses (closes the socket,
// whose descriptor could otherwise be reused under it).
//
// stop() runs on the timer's task (or the abort()ing thread) and must be
// short and must not touch flash: on the ESP port every os::Timer shares
// one dispatch task.

#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/os/os.hpp"

namespace wisp::net {

class RequestWatch {
public:
    enum class Reason : uint8_t { None, Deadline, Aborted };

    RequestWatch();
    ~RequestWatch();  // disarm()

    RequestWatch(const RequestWatch&) = delete;
    RequestWatch& operator=(const RequestWatch&) = delete;

    // Fires with Deadline `timeout_ms` from now (at once for 0). At most
    // once; a watch that was disarmed stays disarmed.
    void arm(uint32_t timeout_ms);
    // What ends the blocked call. If the watch has already fired, runs it
    // now (on the caller's thread); after disarm(), never.
    void attach(std::function<void()> stop);
    // Fires with Aborted, from any thread.
    void abort();
    // See the file header. Idempotent.
    void disarm();
    // Why it fired; None if it has not.
    Reason reason() const;

private:
    void fire(Reason why);

    mutable os::Mutex mu_;
    std::function<void()> stop_;
    Reason reason_ = Reason::None;
    bool disarmed_ = false;
    std::unique_ptr<os::Timer> timer_;
};

}  // namespace wisp::net
