#pragma once

// TlsGate: lets at most one TLS handshake run at a time.
//
// Why: a handshake is the largest transient use of RAM an HTTPS request
// makes (about 20 KB on the reference board, mostly the TLS library's
// contexts and handshake state; where the platform puts them is its own
// choice, see docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md), and
// the internal-RAM budget allows for exactly one (wisp/app/ram_budget.hpp).
// Several tasks start HTTPS requests on their own (the model connectivity
// test on the admin httpd, the scene HTTP proxy's worker), so every
// HttpClient that does TLS on the device holds TlsGate::process() from
// before it connects until the handshake is done (EspHttpClient: around
// esp_http_client_open()), and never during the rest of the transfer: an
// established session costs only its record buffers, and holding the
// gate for a whole download would make the next request wait for it.
//
// The wait is bounded by the caller (its request deadline); a caller that
// does not get the gate in time fails its request as a timeout, and one
// whose request is cancelled meanwhile stops waiting (Hold's give_up). The
// simulator's client does not use it: curl does connect, handshake and
// transfer in one call, and the host has no internal-RAM limit.

#include <cstdint>
#include <functional>

#include "wisp/os/os.hpp"

namespace wisp::net {

class TlsGate {
public:
    TlsGate();

    TlsGate(const TlsGate&) = delete;
    TlsGate& operator=(const TlsGate&) = delete;

    static constexpr uint32_t kGiveUpPollMs = 20;

    // The one gate every client in the process shares.
    static TlsGate& process();

    // Holds the gate for its lifetime if it got it within `timeout_ms`
    // (os::kForever: no limit).
    class Hold {
    public:
        Hold(TlsGate& gate, uint32_t timeout_ms);
        // Same, but also stops waiting soon after `give_up` returns true
        // (asked every kGiveUpPollMs while waiting): a request cancelled
        // or past its deadline does not sit in the queue for the gate.
        Hold(TlsGate& gate, uint32_t timeout_ms, const std::function<bool()>& give_up);
        ~Hold();

        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

        bool held() const { return held_; }

    private:
        TlsGate& gate_;
        bool held_ = false;
    };

private:
    os::Semaphore sem_;
};

}  // namespace wisp::net
