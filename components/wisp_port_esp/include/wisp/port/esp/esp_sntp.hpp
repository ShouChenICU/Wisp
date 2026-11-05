#pragma once

// The ESP port's net::Sntp (connectivity spec 5.6): one SNTP request per
// query() over a UDP socket of its own (lwIP), checked with
// sntp_packet.hpp. Chosen over lwIP's SNTP app, which runs on the tcpip
// thread on its own schedule and sets the clock itself: here the portable
// TimeSyncService decides when to ask, which server, and what to do with
// the answer. Blocking; TimeSyncService calls it from the "wisp.ntp"
// task, which never touches flash -- nor does anything here.
//
// Platform::set_wall_clock is set_wall_clock() from wall_clock.hpp.

#include <cstdint>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/net/time_sync.hpp"

namespace wisp::port::esp {

class EspSntp : public net::Sntp {
public:
    // Resolves `host` (IPv4 only), sends one request to its port 123 and
    // waits up to `timeout_ms` for a valid reply, ignoring any other
    // datagram. Fails with Timeout when none comes, NotFound when the name
    // does not resolve, IoError on a socket error.
    Result<int64_t> query(const std::string& host, uint32_t timeout_ms) override;
};

}  // namespace wisp::port::esp
