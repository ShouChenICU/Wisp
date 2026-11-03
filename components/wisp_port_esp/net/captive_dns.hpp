#pragma once

// The setup hotspot's catch-all DNS responder (connectivity spec 4.3):
// a small task answering every query that reaches UDP port 53 on the
// hotspot's address with that address (dns_reply.hpp builds the
// answers), so phones joining the hotspot detect a captive portal and
// open the setup page. Private to wisp_port_esp; owned by EspWifi, which
// starts it with the hotspot and stops it with the hotspot.

#include <atomic>
#include <cstdint>
#include <memory>

#include "wisp/base/result.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::esp {

class CaptiveDns {
public:
    CaptiveDns() = default;
    ~CaptiveDns();

    CaptiveDns(const CaptiveDns&) = delete;
    CaptiveDns& operator=(const CaptiveDns&) = delete;

    // Binds UDP port 53 on `ip` (host byte order) and starts answering
    // with `ip`. A no-op while already running.
    Result<void> start(uint32_t ip);
    // Stops the task; returns once it has exited, which takes at most one
    // receive timeout (kPollMs).
    void stop();

    static constexpr int kPollMs = 200;

private:
    void run(int sock, uint32_t ip);

    std::unique_ptr<os::Task> task_;
    std::atomic<bool> stop_{false};
};

}  // namespace wisp::port::esp
