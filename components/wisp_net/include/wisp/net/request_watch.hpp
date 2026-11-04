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
// watch runs it once, at the deadline or after abort().
//
// One watch serves one request at a time and is reused: arm() starts the
// next request, disarm() ends it. It owns one periodic os::Timer, made
// once by create() (which can fail: device timers take internal RAM), so
// a request costs no allocation. stop() always runs on that timer's task,
// at the tick after the deadline or after abort(), never on the thread
// that calls abort(): a slow stop() cannot stall a caller such as the UI
// thread cancelling a scene. The only exception is attach() after the
// watch has fired, which runs stop() at once on the client's own thread.
//
// Lifetime of what stop() touches: once disarm() returns, stop() is
// neither running nor ever called again (disarm() stops the timer, which
// waits for a tick in flight). A client calls disarm() before it releases
// what stop() uses (closes the socket, whose descriptor could otherwise
// be reused under it).
//
// stop() must be short and must not touch flash: on the ESP port every
// os::Timer shares one dispatch task.

#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/os/os.hpp"

namespace wisp::net {

class RequestWatch {
public:
    enum class Reason : uint8_t { None, Deadline, Aborted };

    // How often the timer checks the deadline and pending aborts while a
    // request is armed; also how late stop() may run after either.
    static constexpr uint32_t kTickMs = 50;

    // nullptr if the timer cannot be made.
    static std::unique_ptr<RequestWatch> create();
    ~RequestWatch();  // disarm()

    RequestWatch(const RequestWatch&) = delete;
    RequestWatch& operator=(const RequestWatch&) = delete;

    // Starts watching a request (ending any earlier one, as disarm()):
    // Deadline `timeout_ms` from now. Returns the request's ticket for
    // abort().
    uint32_t arm(uint32_t timeout_ms);
    // What ends the blocked call. If the watch has already fired, runs it
    // now (on the caller's thread); when not armed, never.
    void attach(std::function<void()> stop);
    // Fires with Aborted if `ticket` is still the armed request; from any
    // thread, without waiting for stop() (see the file header).
    void abort(uint32_t ticket);
    // See the file header. Idempotent.
    void disarm();
    // Why the last armed request's watch fired; None if it has not.
    Reason reason() const;

private:
    RequestWatch() = default;
    void tick();

    // Never held while stop() runs.
    mutable os::Mutex mu_;
    std::function<void()> stop_;
    Reason reason_ = Reason::None;
    bool armed_ = false;
    uint32_t ticket_ = 0;
    uint64_t deadline_ = 0;
    std::unique_ptr<os::Timer> timer_;
};

}  // namespace wisp::net
